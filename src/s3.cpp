#include "s3.hpp"
#include "log.hpp"
#include "utils.hpp"

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/config/ConfigAndCredentialsCacheManager.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSAllocator.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3ClientConfiguration.h>
#include <aws/s3/model/CopyObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetBucketLocationRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListBucketsRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <toml++/toml.hpp>

#include <Windows.h>

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
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace std::string_view_literals;

namespace s3cmd {

namespace {
constexpr std::uint64_t max_single_part_size = 5ULL * 1024 * 1024 * 1024;

int plugin_number{};
tProgressProcW progress_proc{};
tLogProcW log_proc{};
tRequestProcW request_proc{};

std::shared_mutex aws_lifecycle_mtx;
Aws::SDKOptions aws_options;
DWORD aws_init_thread_id{};
bool aws_initialized{};

// All operations on RuntimeConfig requires a `config_mtx` mutex to be held
struct RuntimeConfig
{
    static RuntimeConfig& get();

    // Mirror the current config on the disk, at `path()`
    bool flush_to_disk();

    struct ProfileSettings
    {
        BucketMap registered_buckets;
        BucketMap cached_bucket_regions;
    };

    bool dry_run{};
    std::map<std::string, ProfileSettings, std::less<>> profiles;

private:
    // Returns a path to the config file
    static const std::filesystem::path& path();
};

std::mutex config_mtx;
std::optional<RuntimeConfig> runtime_config;

enum class DeleteMode
{
    normal,
    unregister_buckets,
    protect_profiles,
};

thread_local DeleteMode delete_mode = DeleteMode::normal;

struct ClientEntry
{
    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> credentials;
    std::shared_ptr<Aws::S3::S3Client> client;
    bool uses_sso{};
};

std::mutex client_mutex;
using ClientKey = std::pair<std::string, std::string>; // (profile, region)
std::map<ClientKey, std::shared_ptr<ClientEntry>> clients;

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

class SsoLoginRequired : public std::runtime_error
{
public:
    explicit SsoLoginRequired(std::string_view profile)
        : std::runtime_error(std::format(
              "AWS SSO credentials for profile '{}' are unavailable or expired.\n\nRun:\naws "
              "sso login --profile {}\n\nThen retry the operation.",
              profile, profile))
    {
    }
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

        for (const auto kind : {"buckets"sv, "regions"sv})
        {
            const toml::table* profiles = document->get_as<toml::table>(kind);
            if (!profiles)
                continue;

            for (const auto& [name, profile] : *profiles)
            {
                if (!profile.is_table())
                    continue;
                auto& settings = runtime_config->profiles[std::string(name.str())];
                auto& fields = kind == "buckets"sv ? settings.registered_buckets
                                                   : settings.cached_bucket_regions;
                for (const auto& [bucket, region] : *profile.as_table())
                {
                    if (const auto value = region.value<std::string>())
                        fields.emplace(bucket.str(), BucketInfo{*value});
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
    document.emplace("settings", toml::table{{"DryRun", dry_run}});

    for (const auto kind : {"buckets"sv, "regions"sv})
    {
        toml::table section;

        for (const auto& [profile_name, profile] : profiles)
        {
            const auto& fields = kind == "buckets"sv ? profile.registered_buckets 
                                                     : profile.cached_bucket_regions;
            if (fields.empty())
                continue;

            toml::table bucket_map;
            for (const auto& [bucket_name, bucket_info] : fields)
            {
                bucket_map.emplace(bucket_name, bucket_info.region);
            }

            section.emplace(profile_name, std::move(bucket_map));
        }

        if (!section.empty())
            document.emplace(kind, std::move(section));
    }
    
    return write_document(document, path());
}

const std::filesystem::path& RuntimeConfig::path()
{
    static std::filesystem::path value = [] {
#ifdef _WIN32
        wchar_t* app_data{};
        std::size_t size{};
        // _wdupenv_s allocates a correctly sized UTF-16 copy.
        if (_wdupenv_s(&app_data, &size, L"APPDATA") != 0 || !app_data || !*app_data)
        {
            std::free(app_data);
            throw std::runtime_error("APPDATA is not set");
        }
        std::unique_ptr<wchar_t, decltype(&std::free)> releaser(app_data, &std::free);
        return std::filesystem::path(releaser.get()) / L"s3cmd" / L"s3cmd.toml";
#else
        if (const auto* config_home = std::getenv("XDG_CONFIG_HOME"); config_home && *config_home)
            return std::filesystem::path(config_home) / "s3cmd" / "s3cmd.toml";
        if (const auto* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".config" / "s3cmd" / "s3cmd.toml";
        throw std::runtime_error("XDG_CONFIG_HOME and HOME are not set");
#endif
    }();
    return value;
}

bool is_dry_run()
{
    std::scoped_lock lock(config_mtx);
    return RuntimeConfig::get().dry_run;
}

std::string profile_region(std::string_view profile)
{
    const Aws::S3::S3ClientConfiguration configuration(std::string(profile).c_str(), true);
    return {configuration.region.data(), configuration.region.size()};
}

std::shared_ptr<Aws::S3::S3Client> get_client(const RemotePath& path,
                                              std::string_view region_override = {})
{
    Aws::S3::S3ClientConfiguration configuration(path.profile.c_str());
    if (!region_override.empty())
    {
        configuration.region.assign(region_override.data(), region_override.size());
    }
    else if (!path.bucket.empty())
    {
        const auto region = ProfileConfig(path.profile).bucket_region(path.bucket);
        if (!region.empty())
            configuration.region = region.c_str();
    }

    const ClientKey key{path.profile, {configuration.region.data(), configuration.region.size()}};
    std::shared_ptr<ClientEntry> entry;
    {
        std::scoped_lock lock(client_mutex);
        if (const auto found = clients.find(key); found != clients.end())
            entry = found->second;
    }

    if (!entry)
    {
        Aws::Client::ClientConfiguration::CredentialProviderConfiguration credentials_configuration;
        credentials_configuration.profile = path.profile.c_str();
        credentials_configuration.region = configuration.region;
        auto credentials = Aws::MakeShared<Aws::Auth::DefaultAWSCredentialsProviderChain>(
            "s3cmd", credentials_configuration);

        const auto profile = Aws::Config::GetCachedConfigProfile(path.profile.c_str());
        auto candidate = std::make_shared<ClientEntry>(
            credentials,
            Aws::MakeShared<Aws::S3::S3Client>("s3cmd", credentials, nullptr, configuration),
            profile.IsSsoSessionSet() || !profile.GetSsoStartUrl().empty()
        );

        std::scoped_lock lock(client_mutex);
        entry = clients.try_emplace(key, std::move(candidate)).first->second;
    }

    if (entry->uses_sso && entry->credentials->GetAWSCredentials().IsEmpty())
    {
        // Expired SSO credentials
        const SsoLoginRequired error(path.profile);
        if (request_proc)
        {
            const auto title = std::wstring(L"Amazon S3");
            const auto message = utf8_to_wide(error.what());
            std::array<wchar_t, 1> ignored{};
            request_proc(plugin_number, RT_MsgOK, const_cast<wchar_t*>(title.data()),
                         const_cast<wchar_t*>(message.data()), ignored.data(),
                         static_cast<int>(ignored.size()));
        }
        throw error;
    }
    return entry->client;
}

void log_error(std::string_view operation, std::string_view message)
{
    const auto text = std::format("{}: {}", operation, message);
    if (log_proc)
        log_proc(plugin_number, MSGTYPE_IMPORTANTERROR, utf8_to_wide(text).data());

    log("[s3cmd] {}", text);
}

template <class Error>
void log_aws_error(std::string_view operation, const Error& error)
{
    log_error(operation, {error.GetMessage().data(), error.GetMessage().size()});
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
    log("[s3cmd] operation={} dry={} path={}", operation, dry, wide_to_utf8(path));
}

bool report_progress(const wchar_t* source, const wchar_t* target, int percent)
{
    return progress_proc && progress_proc(plugin_number, const_cast<wchar_t*>(source),
                                          const_cast<wchar_t*>(target), percent) != 0;
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
                     Aws::S3::Model::GetObjectRequest& request)
        : TransferProgress{source, target, 0, request}
    {
        request.SetHeadersReceivedEventHandler(
            [this](const Aws::Http::HttpRequest*, Aws::Http::HttpResponse* response) {
                const auto& length = response->GetHeader(Aws::Http::CONTENT_LENGTH_HEADER);
                std::from_chars(length.data(), length.data() + length.size(), total_);
            });
        request.SetDataReceivedEventHandler([this](const Aws::Http::HttpRequest*,
                                                   Aws::Http::HttpResponse*,
                                                   long long bytes) { add(bytes); });
    }

    // Constructor for Put request
    TransferProgress(const wchar_t* source, const wchar_t* target,
                     Aws::S3::Model::PutObjectRequest& request)
        : TransferProgress{source, target,
                           static_cast<std::uint64_t>(request.GetContentLength()),
                           request}
    {
        request.SetDataSentEventHandler(
            [this](const Aws::Http::HttpRequest*, long long bytes) { add(bytes); });
    }

    bool is_canceled() const { return canceled_.load(); }

private:
    // Common constructor for both Get and Put requests
    template <typename Request>
    TransferProgress(const wchar_t* source, const wchar_t* target,
                     std::uint64_t total, Request& request)
        : source_{source}, target_{target}, total_{total}
    {
        request.SetRequestRetryHandler(
            [this](const Aws::AmazonWebServiceRequest&) { transferred_ = 0; });
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
    std::uint64_t transferred_{};
    int percent_{};
    std::atomic<bool> canceled_{};
};

// Checks whether an object at `path` exists
bool remote_exists(Aws::S3::S3Client& client, const RemotePath& path)
{
    log_operation("HeadObject", path, false);
    Aws::S3::Model::HeadObjectRequest request;
    request.SetBucket(path.bucket.c_str());
    request.SetKey(path.key.c_str());
    const auto outcome = client.HeadObject(request);
    if (outcome.IsSuccess())
        return true;
    if (outcome.GetError().GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND)
        return false;
    log_aws_error("HeadObject", outcome.GetError());
    throw std::runtime_error(outcome.GetError().GetMessage().c_str());
}

// Show the user prompt, asking user the for region for the just manually registered bucket
bool request_region(std::string_view profile, std::string& region)
{
    if (!request_proc)
        return !region.empty();

    std::array<wchar_t, 128> value{};
    const auto default_region = utf8_to_wide(region);
    std::copy_n(default_region.data(), std::min(default_region.size(), value.size() - 1),
                value.data());

    std::wstring title = L"Register S3 bucket";
    std::wstring prompt = std::format(L"Region for AWS profile '{}:", utf8_to_wide(profile));
    if (!request_proc(plugin_number, RT_Other, title.data(), prompt.data(), value.data(),
                      static_cast<int>(value.size())))
        return false;

    region = wide_to_utf8(value.data());
    return !region.empty();
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
        auto wide_name = utf8_to_wide(name);
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
        profiles.emplace(selected.data(), selected.size());
        for (const auto& [name, ignored] : Aws::Config::GetCachedConfigProfiles())
            profiles.emplace(name.data(), name.size());
        for (const auto& [name, ignored] : Aws::Config::GetCachedCredentialsProfiles())
            profiles.emplace(name.data(), name.size());

        for (const auto& profile : profiles)
            append_entry(profile, true);
    }

    void list_buckets(const RemotePath& path)
    {
        BucketMap buckets;
        // Force us-east-1 region because global endpoint returns the original bucket creation time
        // whereas regional replicas return their last metadata replication time in the CreationDate
        // field.
        auto client = get_client(path, "us-east-1");
        Aws::S3::Model::ListBucketsRequest request;
        request.SetMaxBuckets(10'000);
        log_operation("ListBuckets", path, false);
        bool discovery_succeeded = true;
        for (;;)
        {
            const auto outcome = client->ListBuckets(request);
            if (!outcome.IsSuccess())
            {
                discovery_succeeded = false;
                buckets = ProfileConfig(path.profile).registered_buckets();
                if (outcome.GetError().GetResponseCode() != Aws::Http::HttpResponseCode::FORBIDDEN)
                    log_aws_error("ListBuckets", outcome.GetError());
                break;
            }

            const auto& result = outcome.GetResult();
            for (const auto& bucket : result.GetBuckets())
            {
                const std::string name(bucket.GetName().data(), bucket.GetName().size());
                const auto& region = bucket.GetBucketRegion();
                buckets[name] = {
                    {region.data(), region.size()},
                    to_file_time(bucket.GetCreationDate()),
                };
            }

            if (result.GetContinuationToken().empty())
                break;
            request.SetContinuationToken(result.GetContinuationToken());
        }

        if (discovery_succeeded && !ProfileConfig(path.profile).cache_bucket_regions(buckets))
        {
            log_error("ListBuckets", "Cannot cache bucket regions");
        }

        for (const auto& [name, bucket] : buckets)
            append_entry(name, true, 0, bucket.created);
    }

    void list_objects(const RemotePath& path)
    {
        const auto prefix = path.directory_prefix();
        auto client = get_client(path);
        Aws::S3::Model::ListObjectsV2Request request;
        request.SetBucket(path.bucket.c_str());
        request.SetDelimiter("/");
        request.SetPrefix(prefix.c_str());
        log_operation("ListObjectsV2", {path.profile, path.bucket, prefix}, false);

        for (;;)
        {
            const auto outcome = client->ListObjectsV2(request);
            if (!outcome.IsSuccess())
            {
                log_aws_error("ListObjectsV2", outcome.GetError());
                throw std::runtime_error(outcome.GetError().GetMessage().c_str());
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
                if (std::string_view(key.data(), key.size()) == prefix ||
                    key.size() <= prefix.size())
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

RemotePath RemotePath::make(std::wstring_view path)
{
    // Remove \ from the back and the front
    while (!path.empty() && path.front() == L'\\')
        path.remove_prefix(1);
    while (!path.empty() && path.back() == L'\\')
        path.remove_suffix(1);

    // Check for the profile
    const auto profile_end = path.find(L'\\');
    const auto profile = path.substr(0, profile_end);
    if (profile_end == std::wstring_view::npos)
        return {wide_to_utf8(profile), {}, {}};

    // Split the remaining into the bucket and the key
    path.remove_prefix(profile_end + 1);
    const auto bucket_end = path.find(L'\\');
    const auto bucket = path.substr(0, bucket_end);
    const auto key =
        bucket_end == std::wstring_view::npos ? std::wstring_view{} : path.substr(bucket_end + 1);

    auto key_utf8 = wide_to_utf8(key);
    std::ranges::replace(key_utf8, '\\', '/');
    return {wide_to_utf8(profile), wide_to_utf8(bucket), std::move(key_utf8)};
}

std::string RemotePath::directory_prefix() const
{
    if (key.empty())
        return {};
    return key.back() == '/' ? key : key + '/';
}

void reset_config()
{
    std::scoped_lock lock(config_mtx);
    runtime_config.reset();
}

std::string discover_bucket_region(std::string_view profile, std::string_view bucket)
{
    // FIXME path is needed only to log
    const RemotePath path{std::string(profile), std::string(bucket), {}};
    AwsLease lease;
    auto client = get_client({std::string(profile), {}, {}});
    Aws::S3::Model::GetBucketLocationRequest request;
    request.SetBucket(std::string(bucket).c_str());
    log_operation("GetBucketLocation", path, false);
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
    return {region.data(), region.size()};
}

BucketMap ProfileConfig::registered_buckets() const
{
    std::scoped_lock lock(config_mtx);
    const auto& config = RuntimeConfig::get();
    const auto profile = config.profiles.find(profile_);
    return profile == config.profiles.end() ? BucketMap{}
                                            : profile->second.registered_buckets;
}

BucketMap ProfileConfig::cached_bucket_regions() const
{
    std::scoped_lock lock(config_mtx);
    const auto& config = RuntimeConfig::get();
    const auto profile = config.profiles.find(profile_);
    return profile == config.profiles.end() ? BucketMap{}
                                            : profile->second.cached_bucket_regions;
}

bool ProfileConfig::cache_bucket_regions(const BucketMap& buckets) const
{
    std::scoped_lock lock(config_mtx);
    auto& config = RuntimeConfig::get();
    config.profiles[profile_].cached_bucket_regions = buckets;
    return config.flush_to_disk();
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
    if (const auto cached = profile->second.cached_bucket_regions.find(bucket);
        cached != profile->second.cached_bucket_regions.end())
    {
        return cached->second.region;
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

int initialize(int number, tProgressProcW progress, tLogProcW log, tRequestProcW request)
{
    s3cmd::log("[s3cmd] Initialize called, plugin={}, thread={}", number, GetCurrentThreadId());

    std::unique_lock lock(aws_lifecycle_mtx);
    if (!aws_initialized)
    {
        Aws::InitAPI(aws_options);
        aws_init_thread_id = GetCurrentThreadId();
        aws_initialized = true;
    }
    else
    {
        assert(aws_init_thread_id == GetCurrentThreadId());
    }

    plugin_number = number;
    progress_proc = progress;
    log_proc = log;
    request_proc = request;
    return 0;
}

void shutdown()
{
    log("[s3cmd] Shutdown called, thread={}", GetCurrentThreadId());

    std::unique_lock lock(aws_lifecycle_mtx);
    reset_config();
    if (!aws_initialized)
        return;

    const auto same_thread = aws_init_thread_id == GetCurrentThreadId();
    assert(same_thread);
    if (!same_thread)
        return;

    {
        std::scoped_lock clients_lock(client_mutex);
        clients.clear();
    }
    Aws::ShutdownAPI(aws_options);
    aws_initialized = false;
    aws_init_thread_id = 0;
}

HANDLE find_first(const wchar_t* path, WIN32_FIND_DATAW* find_data)
{
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
    catch (const SsoLoginRequired& error)
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
        delete_mode = DeleteMode::normal;
        return;
    }

    const auto path = RemotePath::make(remote_directory);
    delete_mode = path.profile.empty()
                      ? DeleteMode::protect_profiles
                      : path.bucket.empty() ? DeleteMode::unregister_buckets : DeleteMode::normal;
}

int get_file(const wchar_t* remote_name, const wchar_t* local_name, int copy_flags,
             [[maybe_unused]] const RemoteInfoStruct* info)
try
{
    if ((copy_flags & FS_COPYFLAGS_RESUME) != 0)
        return FS_FILE_NOTSUPPORTED;
    if ((copy_flags & FS_COPYFLAGS_OVERWRITE) == 0 && std::filesystem::exists(local_name))
        return FS_FILE_EXISTS;
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

    const auto local = std::filesystem::path(local_name);
    wchar_t temporary[MAX_PATH];
    if (GetTempFileNameW(local.parent_path().c_str(), L"s3c", 0, temporary) == 0)
        return FS_FILE_WRITEERROR;
    const auto partial = std::filesystem::path(temporary);
    std::error_code ignored;

    {
        AwsLease lease;
        auto client = get_client(path);
        Aws::S3::Model::GetObjectRequest request;
        request.SetBucket(path.bucket.c_str());
        request.SetKey(path.key.c_str());
        request.SetResponseStreamFactory([partial] {
            return Aws::New<std::fstream>("s3cmd", partial,
                                          std::ios::out | std::ios::binary | std::ios::trunc);
        });

        TransferProgress progress{remote_name, local_name, request};
        const auto outcome = client->GetObject(request);
        if (!outcome.IsSuccess())
        {
            std::filesystem::remove(partial, ignored);
            if (progress.is_canceled())
                return FS_FILE_USERABORT;
            log_aws_error("GetObject", outcome.GetError());
            return outcome.GetError().GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND
                       ? FS_FILE_NOTFOUND
                       : FS_FILE_READERROR;
        }
    }

    auto move_flags = MOVEFILE_WRITE_THROUGH;
    if ((copy_flags & FS_COPYFLAGS_OVERWRITE) != 0)
        move_flags |= MOVEFILE_REPLACE_EXISTING;
    if (!MoveFileExW(partial.c_str(), local_name, move_flags))
    {
        std::filesystem::remove(partial, ignored);
        return FS_FILE_WRITEERROR;
    }
    if ((copy_flags & FS_COPYFLAGS_MOVE) != 0 && !delete_file(remote_name))
        return FS_FILE_WRITEERROR;

    report_progress(remote_name, local_name, 100);
    return FS_FILE_OK;
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
        request.SetBucket(path.bucket.c_str());
        request.SetKey(path.key.c_str());
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
catch (const std::exception& error)
{
    log_unexpected("FsPutFileW", error);
    return FS_FILE_WRITEERROR;
}

bool delete_file(const wchar_t* remote_name)
try
{
    if (delete_mode != DeleteMode::normal)
        return true;

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
    request.SetBucket(path.bucket.c_str());
    request.SetKey(path.key.c_str());
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
        auto region = discover_bucket_region(path.profile, path.bucket);
        if (region.empty())
        {
            {
                AwsLease lease;
                region = profile_region(path.profile);
            }
            
            if (!request_region(path.profile, region))
                return false;
        }
        const auto is_dry = is_dry_run();
        log_operation("RegisterBucket", path, is_dry);
        if (is_dry)
        {
            return true;
        }
        return ProfileConfig(path.profile).register_bucket(path.bucket, region);
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
    request.SetBucket(path.bucket.c_str());
    request.SetKey(path.directory_prefix().c_str());
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

    // Top-level profile and bucket directories are virtual links. During
    // recursive deletion, never pass the delete through to their S3 contents.
    if (delete_mode == DeleteMode::protect_profiles)
        return true;
    if (delete_mode == DeleteMode::unregister_buckets)
    {
        if (!path.key.empty())
            return true;
    }

    if (path.key.empty())
    {
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
    list.SetBucket(path.bucket.c_str());
    list.SetPrefix(prefix.c_str());
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
        if (std::string_view(object.GetKey().data(), object.GetKey().size()) != prefix)
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
    remove.SetBucket(path.bucket.c_str());
    remove.SetKey(prefix.c_str());
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
    copy.SetBucket(target.bucket.c_str());
    copy.SetKey(target.key.c_str());
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
        remove.SetBucket(source.bucket.c_str());
        remove.SetKey(source.key.c_str());
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

    const auto path = RemotePath::make(file_name);
    // Region is only available at buckets view
    if (path.profile.empty() || path.bucket.empty() || !path.key.empty())
        return ft_fieldempty;

    // Get region from a cached config
    const auto region = ProfileConfig(path.profile).bucket_region(path.bucket);
    if (region.empty())
        return ft_fieldempty;

    const auto value = utf8_to_wide(region);
    auto* output = static_cast<wchar_t*>(field_value);
    const auto capacity = static_cast<std::size_t>(max_length) / sizeof(wchar_t);
    const auto length = std::min(value.size(), capacity - 1);
    std::copy_n(value.data(), length, output);
    output[length] = L'\0';
    return ft_stringw;
}

} // namespace s3cmd
