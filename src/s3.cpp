#include "s3.hpp"
#include "core.hpp"
#include "creds.hpp"
#include "fsplugin.h"
#include "log.hpp"
#include "sso.hpp"

#include <aws/core/AmazonWebServiceRequest.h>
#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/config/ConfigAndCredentialsCacheManager.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/internal/AWSHttpResourceClient.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/logging/FormattedLogSystem.h>
#include <aws/core/utils/logging/LogLevel.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSAllocator.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3ClientConfiguration.h>
#include <aws/s3/model/BucketLocationConstraint.h>
#include <aws/s3/model/CopyObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetBucketLocationRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListBucketsRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/PutObjectRequest.h>

#include <toml++/toml.hpp>

// FIXME: This should be removed
#include <Windows.h> // GetCurrentThreadId, SetLastError, GetTempFileNameW, MoveFileExW

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <debugapi.h>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <ios>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace s3cmd {

namespace {

constexpr std::uint64_t max_single_part_size = 5ULL * 1024 * 1024 * 1024;

class AwsLogSystem final : public Aws::Utils::Logging::FormattedLogSystem
{
public:
    using FormattedLogSystem::FormattedLogSystem;
    void Flush() override {}

    static Aws::Utils::Logging::LogLevel parse_log_level(std::string_view value)
    {
        using enum Aws::Utils::Logging::LogLevel;
        if (value == "Off")
            return Off;
        if (value == "Fatal")
            return Fatal;
        if (value == "Error")
            return Error;
        if (value == "Warn")
            return Warn;
        if (value == "Debug")
            return Debug;
        if (value == "Trace")
            return Trace;
        return Info;
    }

private:
    void ProcessFormattedStatement(Aws::String&& statement) override
    {
        statement.pop_back(); // FormattedLogSystem always appends a newline
        log("{}", statement);
    }
};

std::shared_mutex aws_lifecycle_mtx;
Aws::SDKOptions aws_options;
std::thread::id aws_init_thread_id{};
bool aws_initialized{};
bool imds_enabled{};
std::optional<PluginHost> plugin_host{};

// All operations on RuntimeConfig requires a `config_mtx` mutex to be held
struct RuntimeConfig
{
    static RuntimeConfig& get();

    // Mirror the current config on the disk, at `path()`
    bool flush_to_disk();

    struct ProfileSettings
    {
        BucketMap registered_buckets;
        BucketMap discovered_buckets;
    };

    bool dry_run{};
    bool prefer_sso_device_code{};
    bool enable_imds{};
    std::string aws_log_level{"Info"};
    std::map<std::string, ProfileSettings, std::less<>> profiles;

    static const std::filesystem::path& path()
    {
        static const std::filesystem::path value = s3cmd::config_directory_path() / "s3cmd.toml";
        return value;
    }
};

std::mutex config_mtx;
std::optional<RuntimeConfig> runtime_config;

// Total Commander recursively lists directories before calling FsRemoveDir. Profiles and buckets
// are virtual directories, so their delete operations must see an empty listing.
thread_local bool suppress_delete_listing{};

struct ClientEntry
{
    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> credentials;
    std::shared_ptr<Aws::S3::S3Client> client;
    bool uses_sso{};
    std::uint64_t sso_generation{};
};

std::mutex client_mutex;
using ClientKey = std::pair<std::string, std::string>; // (profile, region)
std::map<ClientKey, std::shared_ptr<ClientEntry>> clients;
std::mutex sso_login_mutex;
std::atomic<std::uint64_t> sso_generation{};

// AwsLease has two jobs:
// 1. Verify the SDK is initialized.
// 2. Hold a shared lifecycle lock for the entire AWS operation.
// Multiple operations can hold it concurrently, but shutdown() needs the exclusive lock.
struct AwsLease
{
    AwsLease() : lock(aws_lifecycle_mtx)
    {
        if (!aws_initialized)
            throw std::runtime_error("AWS SDK is not initialized");
    }

    AwsLease(const AwsLease&) = delete;
    AwsLease& operator=(const AwsLease&) = delete;

    std::shared_lock<std::shared_mutex> lock;
};

std::optional<toml::table> read_document(const std::filesystem::path& file_path)
{
    try
    {
        std::ifstream input{file_path};
        if (input)
            return toml::parse(input);
    }
    catch (const toml::parse_error&)
    {
    }
    return std::nullopt;
}

bool write_document(const toml::table& document, const std::filesystem::path& file_path)
{
    std::error_code ec;
    std::filesystem::create_directories(file_path.parent_path(), ec);
    if (ec)
        return false;

    std::ofstream output(file_path, std::ios::trunc);
    return output && (output << document) && output.flush();
}

RuntimeConfig& RuntimeConfig::get()
{
    if (runtime_config)
        return *runtime_config;
    runtime_config.emplace();

    if (auto document = read_document(path()))
    {
        // Deserialize TOML document into RuntimeConfig
        runtime_config->dry_run = (*document)["settings"]["DryRun"].value_or(false);
        runtime_config->prefer_sso_device_code =
            (*document)["settings"]["PreferSsoDeviceCode"].value_or(false);
        runtime_config->enable_imds = (*document)["settings"]["EnableIMDS"].value_or(false);
        runtime_config->aws_log_level =
            (*document)["settings"]["AwsLogLevel"].value_or("Info");

        if (const auto* profiles = document->get_as<toml::table>("profiles"))
        {
            for (const auto& [name, profile] : *profiles)
            {
                const auto* profile_table = profile.as_table();
                const auto* buckets =
                    profile_table ? profile_table->get_as<toml::table>("buckets") : nullptr;
                if (!buckets)
                    continue;
                auto& registered =
                    runtime_config->profiles[std::string(name.str())].registered_buckets;
                for (const auto& [bucket, region] : *buckets)
                {
                    if (const auto value = region.value<std::string>())
                        registered.emplace(bucket.str(), BucketInfo{*value});
                }
            }
        }
    }

    return *runtime_config;
}

bool RuntimeConfig::flush_to_disk()
{
    // Serialize our config to TOML document and write it to disk
    toml::table document;
    document.emplace("settings", toml::table{{"DryRun", dry_run},
                                             {"AwsLogLevel", aws_log_level},
                                             {"PreferSsoDeviceCode", prefer_sso_device_code},
                                             {"EnableIMDS", enable_imds}});

    toml::table profile_tables;
    for (const auto& [profile_name, profile] : profiles)
    {
        if (profile.registered_buckets.empty())
            continue;

        toml::table bucket_map;
        for (const auto& [bucket_name, bucket_info] : profile.registered_buckets)
        {
            bucket_map.emplace(bucket_name, bucket_info.region);
        }

        toml::table profile_table;
        profile_table.emplace("buckets", std::move(bucket_map));
        profile_tables.emplace(profile_name, std::move(profile_table));
    }
    if (!profile_tables.empty())
        document.emplace("profiles", std::move(profile_tables));

    return write_document(document, path());
}

// Factory function for creating a new S3Client.
// Credentials (whether SSO token has expired) are checked later
std::shared_ptr<ClientEntry> make_client(const Aws::S3::S3ClientConfiguration& configuration,
                                         const RemotePath& path)
{
    const auto generation = sso_generation.load(std::memory_order_relaxed);
    auto credentials_configuration = configuration;
    credentials_configuration.credentialProviderConfig.profile = path.profile;
    credentials_configuration.credentialProviderConfig.region = configuration.region;
    auto credentials = Aws::MakeShared<CredentialsProviderChain>("s3cmd", credentials_configuration);

    const auto profile = Aws::Config::GetCachedConfigProfile(path.profile);
    const bool uses_sso = profile.IsSsoSessionSet() || !profile.GetSsoStartUrl().empty();
    return std::make_shared<ClientEntry>(
        credentials,
        Aws::MakeShared<Aws::S3::S3Client>(/*allocationTag*/ "s3cmd", credentials, nullptr,
                                           configuration),
        uses_sso, generation);
}

std::shared_ptr<Aws::S3::S3Client> get_client(const RemotePath& path,
                                              std::string_view region_override = {})
{
    Aws::S3::S3ClientConfiguration configuration(path.profile.c_str(), !imds_enabled);
    if (!region_override.empty())
    {
        configuration.region = region_override;
    }
    else if (!path.bucket.empty())
    {
        // Get region for the client from the 'active' bucket's region
        const auto region = ProfileConfig(path.profile).bucket_region(path.bucket);
        if (!region.empty())
            configuration.region = region;
    }

    // Clients are cached and keyed by (profile, region)
    const ClientKey key{path.profile, configuration.region};

    auto entry = [&] {
        std::scoped_lock lock(client_mutex);
        if (const auto found = clients.find(key); found != clients.end())
            return found->second;
        return std::shared_ptr<ClientEntry>{};
    }();

    if (!entry)
    {
        auto candidate = make_client(configuration, path);

        std::scoped_lock lock(client_mutex);
        entry = clients.try_emplace(key, std::move(candidate)).first->second;
    }

    if (entry->uses_sso && entry->credentials->GetAWSCredentials().IsEmpty())
    {
        // Profile is SSO based but the credentials are expired and couldn't be refreshed
        std::scoped_lock login_lock(sso_login_mutex);

        // While this thread waits for sso_login_mutex another one might've
        // completed login and refreshed the shared SSO token (very uncommon situation).
        // Recreate a fresh client if a login completed since this entry was created.
        const bool login_required = [&] {
            if (entry->sso_generation == sso_generation.load(std::memory_order_relaxed))
                return true;
            // There was a succesful login after `entry` client was created
            entry = make_client(configuration, path);
            return entry->credentials->GetAWSCredentials().IsEmpty();
        }();
        if (login_required)
        {
            const auto profile = Aws::Config::GetCachedConfigProfile(path.profile);
            if (!profile.IsSsoSessionSet())
            {
                const auto message = std::format(
                    "AWS SSO profile '{}' uses legacy configuration. Built-in login needs a "
                    "sso_session entry that points to an [sso-session] section.\n\n"
                    "To update this profile, run:\naws configure sso --profile \"{}\"\n"
                    "When prompted, enter an SSO session name. Then retry the operation.\n\n"
                    "To keep the legacy configuration, sign in with AWS CLI instead:\n"
                    "aws sso login --profile \"{}\"\nThen retry the operation.",
                    path.profile, path.profile, path.profile);
                plugin_host->notify_message_box(PluginHost::message_box_type::msg_ok, L"Amazon S3",
                                                to_wide(message).c_str());
                throw SsoLoginFailed(message);
            }
            const auto prefer_device_code = [] {
                std::scoped_lock lock(config_mtx);
                return RuntimeConfig::get().prefer_sso_device_code;
            }();
            perform_sso_login(*plugin_host, profile, prefer_device_code);
            sso_generation.fetch_add(1, std::memory_order_relaxed);

            auto refreshed = make_client(configuration, path);
            if (refreshed->credentials->GetAWSCredentials().IsEmpty())
            {
                throw SsoLoginFailed(std::format(
                    "AWS SSO login for profile '{}' did not produce credentials", path.profile));
            }
            entry = std::move(refreshed);
        }
        std::scoped_lock lock(client_mutex);
        clients.insert_or_assign(key, entry);
    }
    return entry->client;
}

void log_error(std::string_view operation, std::string_view message)
{
    const auto text = std::format("{}: {}", operation, message);
    assert(plugin_host);
    plugin_host->notify_log(to_wide(text).data());
    log("[s3cmd] {}", text);
}

template <class Error>
void log_aws_error(std::string_view operation, const Error& error)
{
    log_error(operation, error.GetMessage());
}

void log_unexpected(std::string_view operation, const std::exception& error)
{
    log_error(operation, error.what());
}

void log_operation(std::string_view operation, const RemotePath& path, bool dry)
{
    log("[s3cmd] operation={} dry={} profile={} bucket={} key={}", operation, dry, path.profile,
        path.bucket, path.key);
}

void log_local_operation(std::string_view operation, const wchar_t* path, bool dry)
{
    log("[s3cmd] operation={} dry={} path={}", operation, dry, to_utf8(path));
}

bool report_progress(const wchar_t* source, const wchar_t* target, int percent)
{
    assert(plugin_host);
    return plugin_host->notify_progress(source, target, percent);
}

// Notifies totalcmd about the transfer progress. Used in both get and put operations
// Only calls the calback when value of the progress changes (i.e. from 34% to 35%).
// Naturally supports pause transfers when totalcmd blocks the `report_progress` function
// on their end
class TransferProgress
{
public:
    // Constructor for Get request
    TransferProgress(const wchar_t* source, const wchar_t* target,
                     Aws::S3::Model::GetObjectRequest& request, std::uint64_t transferred = 0)
        : TransferProgress{source, target, 0, request, transferred}
    {
        request.SetHeadersReceivedEventHandler(
            [this, transferred](const Aws::Http::HttpRequest*, Aws::Http::HttpResponse* response) {
                const auto& length = response->GetHeader(Aws::Http::CONTENT_LENGTH_HEADER);
                std::uint64_t remaining{};
                std::from_chars(length.data(), length.data() + length.size(), remaining);
                total_ = transferred + remaining;
            });
        request.SetDataReceivedEventHandler([this](const Aws::Http::HttpRequest*,
                                                   Aws::Http::HttpResponse*,
                                                   long long bytes) { add(bytes); });
    }

    // Constructor for Put request
    TransferProgress(const wchar_t* source, const wchar_t* target,
                     Aws::S3::Model::PutObjectRequest& request)
        : TransferProgress{source, target, static_cast<std::uint64_t>(request.GetContentLength()),
                           request}
    {
        request.SetDataSentEventHandler(
            [this](const Aws::Http::HttpRequest*, long long bytes) { add(bytes); });
    }

    bool is_canceled() const { return canceled_.load(); }

private:
    // Common constructor for both Get and Put requests
    template <typename Request>
    TransferProgress(const wchar_t* source, const wchar_t* target, std::uint64_t total,
                     Request& request, std::uint64_t transferred = 0)
        : source_{source},
          target_{target},
          total_{total},
          initial_transferred_{transferred},
          transferred_{transferred}
    {
        request.SetRequestRetryHandler(
            [this](const Aws::AmazonWebServiceRequest&) { transferred_ = initial_transferred_; });
        request.SetContinueRequestHandler(
            [this](const Aws::Http::HttpRequest*) { return !canceled_.load(); });
    }

    // Calculate percentage of the transfer. Never returns 100 as that's reserved for a completed transfer
    int transfer_percent(std::uint64_t transferred, std::uint64_t total)
    {
        return total == 0
                   ? 0
                   : std::min(99, static_cast<int>(std::min(transferred, total) * 100 / total));
    }

    // Called whenever `bytes` data is transferred. Recalculates a percentage and if it changed
    // since last time, notifies totalcmd about that.
    void add(long long bytes)
    {
        if (bytes > 0)
            transferred_ += static_cast<std::uint64_t>(bytes);
        const auto next = transfer_percent(transferred_, total_);
        if (next > percent_)
        {
            percent_ = next;
            if (report_progress(source_, target_, percent_))
                canceled_ = true;
        }
    }

    const wchar_t* source_;
    const wchar_t* target_;
    std::uint64_t total_{};
    const std::uint64_t initial_transferred_{};
    std::uint64_t transferred_{};
    int percent_{};
    std::atomic<bool> canceled_{};
};

// Checks whether an object at `path` exists
bool remote_exists(Aws::S3::S3Client& client, const RemotePath& path)
{
    log_operation("HeadObject", path, false);
    Aws::S3::Model::HeadObjectRequest request;
    request.SetBucket(path.bucket);
    request.SetKey(path.key);
    const auto outcome = client.HeadObject(request);
    if (outcome.IsSuccess())
        return true;
    if (outcome.GetError().GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND)
        return false;
    log_aws_error("HeadObject", outcome.GetError());
    throw std::runtime_error(outcome.GetError().GetMessage());
}

class FindState
{
public:
    explicit FindState(const RemotePath& path)
    {
        AwsLease lease;

        // Path is at "profile level", just list all the detected profiles
        if (path.profile.empty())
        {
            list_profiles();
        }
        // Path is at "bucket level", list all the buckets, discovered - if user has access, or registered otherwise
        else if (path.bucket.empty())
        {
            list_buckets(path);
        }
        else
        {
            list_objects(path);
        }
    }

    // Behaves as *input_iterator++
    bool dereference_move_next(WIN32_FIND_DATAW* data)
    {
        if (next_ >= entries_.size())
            return false;
        entries_[next_++].copy(data);
        return true;
    }

private:
    struct FindEntry
    {
        std::wstring name;
        std::uint64_t size{};
        FILETIME modified{};
        bool directory{};

        void copy(WIN32_FIND_DATAW* data) const
        {
            *data = {};
            data->dwFileAttributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            data->ftLastWriteTime = modified;
            data->nFileSizeLow = static_cast<DWORD>(size);
            data->nFileSizeHigh = static_cast<DWORD>(size >> 32);
            std::copy(name.begin(), name.end(), data->cFileName);
            data->cFileName[name.size()] = L'\0';
        }
    };

    static bool valid_entry_name(const std::wstring& name)
    {
        return !name.empty()              &&
            name != L"."                  &&
            name != L".."                 &&
            name.find(L'\\') == name.npos &&
            // cFileName has MAX_PATH slots, including the terminating null
            // This only limits displayed entry name, not the complete S3 key/path
            name.size() < MAX_PATH;
    }

    void append_entry(std::string_view name, bool directory, std::uint64_t size = 0,
                      FILETIME modified = {})
    {
        auto wide_name = to_wide(name);
        if (valid_entry_name(wide_name))
            entries_.push_back({std::move(wide_name), size, modified, directory});
    }

    FILETIME to_file_time(const Aws::Utils::DateTime& value)
    {
        constexpr std::int64_t windows_epoch_offset_ms = 11'644'473'600'000;
        const auto ticks =
            static_cast<std::uint64_t>(value.Millis() + windows_epoch_offset_ms) * 10'000;
        return {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    }

    void list_profiles()
    {
        std::set<std::string> profiles{"default"};
        const auto selected = Aws::Auth::GetConfigProfileName();
        profiles.emplace(selected);
        for (const auto& [name, ignored] : Aws::Config::GetCachedConfigProfiles())
            profiles.emplace(name);
        for (const auto& [name, ignored] : Aws::Config::GetCachedCredentialsProfiles())
            profiles.emplace(name);

        for (const auto& profile : profiles)
            append_entry(profile, true);
    }

    void list_buckets(const RemotePath& path)
    {
        BucketMap buckets;
        bool discovered{};
        // Force us-east-1 region because global endpoint returns the original bucket creation time
        // whereas regional replicas return their last metadata replication time in the CreationDate
        // field.
        auto client = get_client(path, "us-east-1");
        Aws::S3::Model::ListBucketsRequest request;
        request.SetMaxBuckets(10'000);
        log_operation("ListBuckets", path, false);
        for (;;)
        {
            const auto outcome = client->ListBuckets(request);
            if (!outcome.IsSuccess())
            {
                buckets = ProfileConfig(path.profile).registered_buckets();
                if (outcome.GetError().GetResponseCode() != Aws::Http::HttpResponseCode::FORBIDDEN)
                    log_aws_error("ListBuckets", outcome.GetError());
                break;
            }

            discovered = true;
            const auto& result = outcome.GetResult();
            for (const auto& bucket : result.GetBuckets())
            {
                buckets[bucket.GetName()] = {bucket.GetBucketRegion(),
                                             to_file_time(bucket.GetCreationDate())};
            }

            if (result.GetContinuationToken().empty())
                break;
            request.SetContinuationToken(result.GetContinuationToken());
        }

        // We either want to list discovered buckets, or registered, never both
        ProfileConfig(path.profile).set_discovered_buckets(discovered ? buckets : BucketMap{});

        if (!discovered)
            append_entry("_F7=register bucket.txt", false);
        for (const auto& [name, bucket] : buckets)
            append_entry(name, true, 0, bucket.created);
    }

    void list_objects(const RemotePath& path)
    {
        const auto prefix = path.directory_prefix();
        auto client = get_client(path);
        Aws::S3::Model::ListObjectsV2Request request;
        request.SetBucket(path.bucket);
        request.SetDelimiter("/");
        request.SetPrefix(prefix);
        log_operation("ListObjectsV2", {path.profile, path.bucket, prefix}, false);

        for (;;)
        {
            const auto outcome = client->ListObjectsV2(request);
            if (!outcome.IsSuccess())
            {
                log_aws_error("ListObjectsV2", outcome.GetError());
                throw std::runtime_error(outcome.GetError().GetMessage());
            }

            const auto& result = outcome.GetResult();
            for (const auto& common_prefix : result.GetCommonPrefixes())
            {
                const auto& value = common_prefix.GetPrefix();
                if (value.size() <= prefix.size() + 1)
                    continue;
                append_entry({value.data() + prefix.size(), value.size() - prefix.size() - 1},
                             true);
            }

            for (const auto& object : result.GetContents())
            {
                const auto& key = object.GetKey();
                if (key == prefix || key.size() <= prefix.size())
                {
                    continue;
                }
                append_entry({key.data() + prefix.size(), key.size() - prefix.size()}, false,
                             static_cast<std::uint64_t>(object.GetSize()),
                             to_file_time(object.GetLastModified()));
            }

            if (!result.GetIsTruncated())
                break;
            request.SetContinuationToken(result.GetNextContinuationToken());
        }
    }

private:
    std::vector<FindEntry> entries_; // Immutable after ctor as finished
    std::size_t next_{};
};

} // namespace

void reset_config()
{
    std::scoped_lock lock(config_mtx);
    runtime_config.reset();
}

bool is_dry_run()
{
    std::scoped_lock lock(config_mtx);
    return RuntimeConfig::get().dry_run;
}

BucketMap ProfileConfig::registered_buckets() const
{
    std::scoped_lock lock(config_mtx);
    const auto& config = RuntimeConfig::get();
    const auto profile = config.profiles.find(profile_);
    return profile == config.profiles.end() ? BucketMap{}
                                            : profile->second.registered_buckets;
}

bool ProfileConfig::has_discovered_buckets() const
{
    std::scoped_lock lock(config_mtx);
    const auto& config = RuntimeConfig::get();
    const auto profile = config.profiles.find(profile_);
    return profile != config.profiles.end() && !profile->second.discovered_buckets.empty();
}

void ProfileConfig::set_discovered_buckets(BucketMap buckets) const
{
    std::scoped_lock lock(config_mtx);
    auto& config = RuntimeConfig::get();
    config.profiles[profile_].discovered_buckets = std::move(buckets);
}

std::string ProfileConfig::bucket_region(std::string_view bucket) const
{
    std::scoped_lock lock(config_mtx);
    const auto& config = RuntimeConfig::get();
    const auto profile = config.profiles.find(profile_);
    if (profile == config.profiles.end())
        return {};

    if (const auto registered = profile->second.registered_buckets.find(bucket);
        registered != profile->second.registered_buckets.end())
    {
        return registered->second.region;
    }
    if (const auto discovered = profile->second.discovered_buckets.find(bucket);
        discovered != profile->second.discovered_buckets.end())
    {
        return discovered->second.region;
    }

    return {};
}

bool ProfileConfig::register_bucket(std::string_view bucket, std::string_view region) const
{
    std::scoped_lock lock(config_mtx);
    auto& config = RuntimeConfig::get();
    config.profiles[profile_].registered_buckets[std::string{bucket}] =
        BucketInfo{std::string{region}};
    return config.flush_to_disk();
}

bool ProfileConfig::unregister_bucket(std::string_view bucket) const
{
    std::scoped_lock lock(config_mtx);
    auto& config = RuntimeConfig::get();
    auto& registered = config.profiles[profile_].registered_buckets;
    if (auto it = registered.find(bucket); it != registered.end())
    {
        registered.erase(it);
        return config.flush_to_disk();
    }
    return false;
}

std::string discover_bucket_region(std::string_view profile, std::string_view bucket)
{
    AwsLease lease;
    auto client = get_client({std::string(profile), {}, {}});
    Aws::S3::Model::GetBucketLocationRequest request;
    request.SetBucket(std::string(bucket));
    log("[s3cmd] operation=GetBucketLocation profile={} bucket={}", profile, bucket);
    const auto outcome = client->GetBucketLocation(request);
    if (!outcome.IsSuccess())
        return {};

    const auto location = outcome.GetResult().GetLocationConstraint();
    if (location == Aws::S3::Model::BucketLocationConstraint::NOT_SET)
        return "us-east-1";
    if (location == Aws::S3::Model::BucketLocationConstraint::EU)
        return "eu-west-1";
    const auto region =
        Aws::S3::Model::BucketLocationConstraintMapper::GetNameForBucketLocationConstraint(
            location);
    return region;
}

int initialize(int number, tProgressProcW progress, tLogProcW log, tRequestProcW request)
{
    s3cmd::log("[s3cmd] Initialize called, plugin={}, thread={}", number, GetCurrentThreadId());

    std::unique_lock lock(aws_lifecycle_mtx);
    if (!aws_initialized)
    {
        {
            std::scoped_lock config_lock(config_mtx);
            aws_options.loggingOptions.logLevel =
                AwsLogSystem::parse_log_level(RuntimeConfig::get().aws_log_level);
            imds_enabled = RuntimeConfig::get().enable_imds;
        }
        aws_options.loggingOptions.logger_create_fn = [] {
            return Aws::MakeShared<AwsLogSystem>("s3cmd", aws_options.loggingOptions.logLevel);
        };
        Aws::InitAPI(aws_options);
        if (!imds_enabled)
        {
            // SDK SSO token and container providers construct default configurations internally.
            // Configure their shared metadata client before any providers or worker threads exist.
            Aws::Internal::CleanupEC2MetadataClient();
            Aws::Client::ClientConfiguration::CredentialProviderConfiguration config{};
            config.imdsConfig.disableImds = true;
            Aws::Internal::InitEC2MetadataClient(config);
        }
        aws_init_thread_id = std::this_thread::get_id();
        aws_initialized = true;
    }
    else
    {
        assert(aws_init_thread_id == std::this_thread::get_id());
    }

    plugin_host.emplace(number, progress, log, request);
    return 0;
}

void shutdown()
{
    log("[s3cmd] Shutdown called, thread={}", GetCurrentThreadId());

    std::unique_lock lock(aws_lifecycle_mtx);
    reset_config();
    if (!aws_initialized)
        return;

    const auto same_thread = aws_init_thread_id == std::this_thread::get_id();
    assert(same_thread);
    if (!same_thread)
        return;

    {
        std::scoped_lock clients_lock(client_mutex);
        clients.clear();
    }
    Aws::ShutdownAPI(aws_options);
    aws_initialized = false;
    aws_init_thread_id = std::thread::id{};
}

HANDLE find_first(const wchar_t* path, WIN32_FIND_DATAW* find_data)
{
    if (suppress_delete_listing)
    {
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }

    try
    {
        auto state = std::make_unique<FindState>(RemotePath::make(path));
        if (!state->dereference_move_next(find_data))
        {
            SetLastError(ERROR_NO_MORE_FILES);
            return INVALID_HANDLE_VALUE;
        }
        return state.release();
    }
    catch (const SsoLoginCancelled& error)
    {
        log_unexpected("FsFindFirstW", error);
        SetLastError(ERROR_CANCELLED);
        return INVALID_HANDLE_VALUE;
    }
    catch (const SsoLoginFailed& error)
    {
        log_unexpected("FsFindFirstW", error);
        SetLastError(ERROR_LOGON_FAILURE);
        return INVALID_HANDLE_VALUE;
    }
    catch (const std::exception& error)
    {
        log_unexpected("FsFindFirstW", error);
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
}

bool find_next(HANDLE handle, WIN32_FIND_DATAW* find_data)
{
    if (!handle || handle == INVALID_HANDLE_VALUE)
        return false;
    return static_cast<FindState*>(handle)->dereference_move_next(find_data);
}

int find_close(HANDLE handle)
{
    if (handle != INVALID_HANDLE_VALUE)
        delete static_cast<FindState*>(handle);
    return 0;
}

void status_info(const wchar_t* remote_directory, int start_end, int operation)
{
    if (operation != FS_STATUS_OP_DELETE)
        return;
    if (start_end == FS_STATUS_END)
    {
        suppress_delete_listing = false;
        return;
    }

    const auto path = RemotePathView::make(remote_directory);
    suppress_delete_listing = path.bucket.empty();
}

int get_file(const wchar_t* remote_name, const wchar_t* local_name, int copy_flags,
             const RemoteInfoStruct* info)
try
{
    const auto local = std::filesystem::path(local_name);
    const auto resume = (copy_flags & FS_COPYFLAGS_RESUME) != 0;
    std::error_code error;
    const auto local_exists = std::filesystem::exists(local, error);
    if (error)
        return FS_FILE_WRITEERROR;
    if (!resume && (copy_flags & FS_COPYFLAGS_OVERWRITE) == 0 && local_exists)
        return std::filesystem::is_regular_file(local, error) && !error
                   ? FS_FILE_EXISTSRESUMEALLOWED
                   : FS_FILE_EXISTS;

    std::uint64_t offset{};
    if (resume)
    {
        if (!std::filesystem::is_regular_file(local, error) || error)
            return FS_FILE_NOTSUPPORTED;
        offset = std::filesystem::file_size(local, error);
        if (error)
            return FS_FILE_WRITEERROR;
    }
    if (report_progress(remote_name, local_name, 0))
        return FS_FILE_USERABORT;

    const auto path = RemotePath::make(remote_name);
    if (path.profile.empty() || path.bucket.empty() || path.key.empty())
        return FS_FILE_NOTFOUND;

    const auto is_dry = is_dry_run();
    log_operation("GetObject", path, is_dry);
    if (is_dry)
    {
        if ((copy_flags & FS_COPYFLAGS_MOVE) != 0)
            log_operation("DeleteObject", path, true);
        report_progress(remote_name, local_name, 100);
        return FS_FILE_OK;
    }

    std::uint64_t remote_size{};
    {
        AwsLease lease;
        auto client = get_client(path);

        Aws::String e_tag;
        if (resume)
        {
            // ponytail: the existing prefix is trusted; persist object identity if provenance checks matter.
            log_operation("HeadObject", path, false);
            Aws::S3::Model::HeadObjectRequest request;
            request.SetBucket(path.bucket);
            request.SetKey(path.key);
            const auto outcome = client->HeadObject(request);
            if (!outcome.IsSuccess())
            {
                log_aws_error("HeadObject", outcome.GetError());
                return outcome.GetError().GetResponseCode() ==
                               Aws::Http::HttpResponseCode::NOT_FOUND
                           ? FS_FILE_NOTFOUND
                           : FS_FILE_READERROR;
            }

            const auto length = outcome.GetResult().GetContentLength();
            if (length < 0)
                return FS_FILE_READERROR;
            remote_size = static_cast<std::uint64_t>(length);
            const auto listed_size = info ? static_cast<std::uint64_t>(info->SizeLow) |
                                                (static_cast<std::uint64_t>(info->SizeHigh) << 32)
                                          : remote_size;
            if (listed_size != remote_size)
                return FS_FILE_READERROR;
            if (offset > remote_size)
                return FS_FILE_NOTSUPPORTED;
            e_tag = outcome.GetResult().GetETag();
        }

        if (!resume || offset < remote_size)
        {
            Aws::S3::Model::GetObjectRequest request;
            request.SetBucket(path.bucket.c_str());
            request.SetKey(path.key.c_str());
            if (resume)
            {
                const auto range = std::format("bytes={}-", offset);
                request.SetRange(range.c_str());
                if (!e_tag.empty())
                    request.SetIfMatch(e_tag);
            }
            request.SetResponseStreamFactory([local, offset] {
                auto stream = Aws::New<std::fstream>("s3cmd");
                if (offset == 0)
                {
                    stream->open(local, std::ios::out | std::ios::binary | std::ios::trunc);
                }
                else
                {
                    std::error_code error;
                    std::filesystem::resize_file(local, offset, error);
                    if (!error)
                    {
                        stream->open(local, std::ios::in | std::ios::out | std::ios::binary);
                        stream->seekp(static_cast<std::streamoff>(offset));
                    }
                }
                return stream;
            });

            TransferProgress progress{remote_name, local_name, request, offset};
            const auto outcome = client->GetObject(request);
            if (!outcome.IsSuccess())
            {
                if (progress.is_canceled())
                    return FS_FILE_USERABORT;
                log_aws_error("GetObject", outcome.GetError());
                return outcome.GetError().GetResponseCode() ==
                               Aws::Http::HttpResponseCode::NOT_FOUND
                           ? FS_FILE_NOTFOUND
                           : FS_FILE_READERROR;
            }
        }
    }

    if (resume)
    {
        std::filesystem::resize_file(local, remote_size, error);
        if (error)
            return FS_FILE_WRITEERROR;
    }
    if ((copy_flags & FS_COPYFLAGS_MOVE) != 0 && !delete_file(remote_name))
        return FS_FILE_WRITEERROR;

    report_progress(remote_name, local_name, 100);
    return FS_FILE_OK;
}
catch (const SsoLoginCancelled&)
{
    return FS_FILE_USERABORT;
}
catch (const std::exception& error)
{
    log_unexpected("FsGetFileW", error);
    return FS_FILE_READERROR;
}

int put_file(const wchar_t* local_name, const wchar_t* remote_name, int copy_flags)
try
{
    if ((copy_flags & FS_COPYFLAGS_RESUME) != 0)
        return FS_FILE_NOTSUPPORTED;

    std::error_code error;
    const auto size = std::filesystem::file_size(local_name, error);
    if (error)
        return FS_FILE_READERROR;
    // FIXME: single-part upload stops at S3's 5 GiB limit
    if (size > max_single_part_size)
        return FS_FILE_NOTSUPPORTED;
    if (report_progress(local_name, remote_name, 0))
        return FS_FILE_USERABORT;

    const auto path = RemotePath::make(remote_name);
    if (path.profile.empty() || path.bucket.empty() || path.key.empty())
        return FS_FILE_WRITEERROR;

    const auto is_dry = is_dry_run();
    log_operation("PutObject", path, is_dry);
    if (is_dry)
    {
        if ((copy_flags & FS_COPYFLAGS_MOVE) != 0)
            log_local_operation("DeleteLocalFile", local_name, true);
        report_progress(local_name, remote_name, 100);
        return FS_FILE_OK;
    }

    {
        AwsLease lease;
        auto client = get_client(path);

        auto body = Aws::MakeShared<std::fstream>("s3cmd", std::filesystem::path(local_name),
                                                  std::ios::in | std::ios::binary);
        if (!body->is_open())
            return FS_FILE_READERROR;

        Aws::S3::Model::PutObjectRequest request;
        request.SetBucket(path.bucket);
        request.SetKey(path.key);
        request.SetBody(body);
        request.SetContentLength(static_cast<long long>(size));
        if ((copy_flags & FS_COPYFLAGS_OVERWRITE) == 0)
            request.SetIfNoneMatch("*");

        TransferProgress progress{local_name, remote_name, request};
        const auto outcome = client->PutObject(request);
        if (!outcome.IsSuccess())
        {
            if (progress.is_canceled())
                return FS_FILE_USERABORT;
            log_aws_error("PutObject", outcome.GetError());
            return outcome.GetError().GetResponseCode() ==
                           Aws::Http::HttpResponseCode::PRECONDITION_FAILED
                       ? FS_FILE_EXISTS
                       : FS_FILE_WRITEERROR;
        }
    }

    if ((copy_flags & FS_COPYFLAGS_MOVE) != 0)
    {
        log_local_operation("DeleteLocalFile", local_name, false);
        std::filesystem::remove(local_name, error);
        if (error)
        {
            log_error("Delete local source", std::format("error {}", error.value()));
            return FS_FILE_READERROR;
        }
    }

    report_progress(local_name, remote_name, 100);
    return FS_FILE_OK;
}
catch (const SsoLoginCancelled&)
{
    return FS_FILE_USERABORT;
}
catch (const std::exception& error)
{
    log_unexpected("FsPutFileW", error);
    return FS_FILE_WRITEERROR;
}

bool delete_file(const wchar_t* remote_name)
try
{
    const auto path = RemotePath::make(remote_name);
    if (path.profile.empty() || path.bucket.empty() || path.key.empty())
        return false;

    const auto is_dry = is_dry_run();
    log_operation("DeleteObject", path, is_dry);
    if (is_dry)
    {
        return true;
    }

    AwsLease lease;
    auto client = get_client(path);
    Aws::S3::Model::DeleteObjectRequest request;
    request.SetBucket(path.bucket);
    request.SetKey(path.key);
    const auto outcome = client->DeleteObject(request);
    if (!outcome.IsSuccess())
        log_aws_error("DeleteObject", outcome.GetError());
    return outcome.IsSuccess();
}
catch (const std::exception& error)
{
    log_unexpected("FsDeleteFileW", error);
    return false;
}

bool make_directory(const wchar_t* remote_name)
try
{
    const auto path = RemotePath::make(remote_name);
    if (path.profile.empty() || path.bucket.empty())
        return false;

    if (path.key.empty())
    {
        const ProfileConfig profile(path.profile);

        // Don't allow bucket registration is the profile already lists remote buckets.
        // Also, check for already registered bucket
        if (profile.has_discovered_buckets() ||
            profile.registered_buckets().contains(path.bucket))
        {
            return false;
        }

        // Try to discover a bucket region
        auto region = discover_bucket_region(path.profile, path.bucket);
        if (region.empty())
        {
            // Can't do, let's ask the user to provide it manually,
            // with profile's default region being the default option
            {
                AwsLease lease;
                const Aws::S3::S3ClientConfiguration configuration{path.profile.c_str(), true};
                region = configuration.region;
            }

            assert(plugin_host);
            if (plugin_host->is_notify_message_box_available())
            {
                auto value = to_wide(region);
                value.resize(64); // Max length for out value for the user
                if (!plugin_host->notify_message_box_result(
                        PluginHost::message_box_type::other, L"Register S3 bucket",
                        std::format(L"Region for AWS profile '{}:", to_wide(path.profile)).c_str(),
                        /*out*/ value))
                {
                    return false;
                }

                region = to_utf8(value.data());
            }

            // Still no region, early-return with an error
            if (region.empty())
                return false;
        }
        const auto is_dry = is_dry_run();
        log_operation("RegisterBucket", path, is_dry);
        if (is_dry)
        {
            return true;
        }
        return profile.register_bucket(path.bucket, region);
    }

    const RemotePath marker{path.profile, path.bucket, path.directory_prefix()};
    const auto is_dry = is_dry_run();
    log_operation("PutObject", marker, is_dry);
    if (is_dry)
    {
        return true;
    }

    AwsLease lease;
    auto client = get_client(path);
    Aws::S3::Model::PutObjectRequest request;
    request.SetBucket(path.bucket);
    request.SetKey(path.directory_prefix());
    request.SetBody(Aws::MakeShared<Aws::StringStream>("s3cmd"));
    request.SetContentLength(0);
    const auto outcome = client->PutObject(request);
    if (!outcome.IsSuccess())
        log_aws_error("PutObject directory marker", outcome.GetError());
    return outcome.IsSuccess();
}
catch (const std::exception& error)
{
    log_unexpected("FsMkDirW", error);
    return false;
}

bool remove_directory(const wchar_t* remote_name)
try
{
    const auto path = RemotePath::make(remote_name);
    if (path.profile.empty() || path.bucket.empty())
        return false;

    if (path.key.empty())
    {
        if (ProfileConfig(path.profile).has_discovered_buckets())
            return false;

        const auto is_dry = is_dry_run();
        log_operation("UnregisterBucket", path, is_dry);
        if (is_dry)
            return ProfileConfig(path.profile).registered_buckets().contains(path.bucket);
        return ProfileConfig(path.profile).unregister_bucket(path.bucket);
    }

    const auto prefix = path.directory_prefix();

    AwsLease lease;
    auto client = get_client(path);
    Aws::S3::Model::ListObjectsV2Request list;
    list.SetBucket(path.bucket);
    list.SetPrefix(prefix);
    list.SetMaxKeys(2);
    log_operation("ListObjectsV2", {path.profile, path.bucket, prefix}, false);
    const auto listed = client->ListObjectsV2(list);
    if (!listed.IsSuccess())
    {
        log_aws_error("ListObjectsV2", listed.GetError());
        return false;
    }

    bool marker_exists = false;
    for (const auto& object : listed.GetResult().GetContents())
    {
        if (object.GetKey() != prefix)
            return false;
        marker_exists = true;
    }
    if (!marker_exists)
        return true;

    const RemotePath marker{path.profile, path.bucket, prefix};
    const auto is_dry = is_dry_run();
    log_operation("DeleteObject", marker, is_dry);
    if (is_dry)
    {
        return true;
    }

    Aws::S3::Model::DeleteObjectRequest remove;
    remove.SetBucket(path.bucket);
    remove.SetKey(prefix);
    const auto removed = client->DeleteObject(remove);
    if (!removed.IsSuccess())
        log_aws_error("DeleteObject directory marker", removed.GetError());
    return removed.IsSuccess();
}
catch (const std::exception& error)
{
    log_unexpected("FsRemoveDirW", error);
    return false;
}

int rename_or_move(const wchar_t* old_name, const wchar_t* new_name,  bool move, bool overwrite,
                   const RemoteInfoStruct* info)
try
{
    if (info && info->SizeHigh == 0xFFFFFFFF)
        return FS_FILE_NOTSUPPORTED;
    const auto size = info ? (static_cast<std::uint64_t>(info->SizeHigh) << 32) | info->SizeLow : 0;
    // FIXME: CopyObject is limited to 5 GiB and only reports 0%/100%; use
    // multipart copy when larger objects or intermediate progress are needed.
    if (size > max_single_part_size)
        return FS_FILE_NOTSUPPORTED;
    if (report_progress(old_name, new_name, 0))
        return FS_FILE_USERABORT;

    const auto source = RemotePath::make(old_name);
    const auto target = RemotePath::make(new_name);
    if (source.profile.empty() || source.bucket.empty() || source.key.empty() ||
        target.profile.empty() || target.bucket.empty() || target.key.empty())
        return FS_FILE_NOTFOUND;

    const auto is_dry = is_dry_run();
    const auto copy_operation =
        std::format("CopyObject source={}/{}/{}", source.profile, source.bucket, source.key);
    log_operation(copy_operation, target, is_dry);
    if (is_dry)
    {
        if (move)
            log_operation("DeleteObject", source, true);
        report_progress(old_name, new_name, 100);
        return FS_FILE_OK;
    }

    AwsLease lease;
    auto target_client = get_client(target);
    if (!overwrite && remote_exists(*target_client, target))
        return FS_FILE_EXISTS;

    Aws::S3::Model::CopyObjectRequest copy;
    copy.SetBucket(target.bucket);
    copy.SetKey(target.key);
    auto copy_source = std::format("{}/{}", source.bucket, source.key);
    copy.SetCopySource(std::move(copy_source));
    const auto copy_outcome = target_client->CopyObject(copy);
    if (!copy_outcome.IsSuccess())
    {
        log_aws_error("CopyObject", copy_outcome.GetError());
        return copy_outcome.GetError().GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND
                   ? FS_FILE_NOTFOUND
                   : FS_FILE_WRITEERROR;
    }

    if (move)
    {
        auto source_client = get_client(source);
        log_operation("DeleteObject", source, false);
        Aws::S3::Model::DeleteObjectRequest remove;
        remove.SetBucket(source.bucket);
        remove.SetKey(source.key);
        const auto delete_outcome = source_client->DeleteObject(remove);
        if (!delete_outcome.IsSuccess())
        {
            log_aws_error("DeleteObject", delete_outcome.GetError());
            return FS_FILE_WRITEERROR;
        }
    }

    report_progress(old_name, new_name, 100);
    return FS_FILE_OK;
}
catch (const SsoLoginCancelled&)
{
    return FS_FILE_USERABORT;
}
catch (const std::exception& error)
{
    log_unexpected("FsRenMovFileW", error);
    return FS_FILE_WRITEERROR;
}

void get_default_root_name(char* name, int max_length)
{
    constexpr std::string_view root_name = "Amazon S3";
    if (!name || max_length <= 0)
        return;
    const auto length = std::min(root_name.size(), static_cast<std::size_t>(max_length - 1));
    std::memcpy(name, root_name.data(), length);
    name[length] = '\0';
}

int content_get_supported_field(int field_index, char* field_name, char* units, int max_length)
{
    if (field_index != 0)
        return ft_nomorefields;
    if (!field_name || !units || max_length <= 0)
        return ft_nomorefields;

    constexpr std::string_view name = "Region";
    const auto length = std::min(name.size(), static_cast<std::size_t>(max_length - 1));
    std::memcpy(field_name, name.data(), length);
    field_name[length] = '\0';
    units[0] = '\0';
    return ft_string;
}

int content_get_value(const wchar_t* file_name, int field_index, void* field_value, int max_length)
{
    if (field_index != 0)
        return ft_nosuchfield;
    if (!file_name || !field_value || max_length < static_cast<int>(sizeof(wchar_t)))
        return ft_fileerror;

    const auto path = RemotePathView::make(file_name);
    // Region is only available at buckets view
    if (path.profile.empty() || path.bucket.empty() || !path.key.empty())
        return ft_fieldempty;

    // Get region from a cached config
    const auto region = ProfileConfig(to_utf8(path.profile)).bucket_region(to_utf8(path.bucket));
    if (region.empty())
        return ft_fieldempty;

    const auto value = to_wide(region);
    auto* output = static_cast<wchar_t*>(field_value);
    const auto capacity = static_cast<std::size_t>(max_length) / sizeof(wchar_t);
    const auto length = std::min(value.size(), capacity - 1);
    std::copy_n(value.data(), length, output);
    output[length] = L'\0';
    return ft_stringw;
}

} // namespace s3cmd
