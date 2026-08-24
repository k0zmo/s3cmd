#include "s3.hpp"
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

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <cstdint>
#include <cstring>
#include <debugapi.h>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace s3cmd {
namespace {
constexpr std::uint64_t max_single_part_size = 5ULL * 1024 * 1024 * 1024;

int plugin_number{};
tProgressProcW progress_proc{};
tLogProcW log_proc{};
tRequestProcW request_proc{};

std::shared_mutex aws_lifecycle_mutex;
std::mutex client_mutex;
std::mutex registry_mutex;
Aws::SDKOptions aws_options;
DWORD aws_init_thread_id{};
bool aws_initialized{};

enum class DeleteMode
{
    normal,
    unregister_buckets,
    protect_profiles,
};

thread_local DeleteMode delete_mode = DeleteMode::normal;

// AwsLease has two jobs:
// 1. Verify the SDK is initialized.
// 2. Hold a shared lifecycle lock for the entire AWS operation.
// Multiple operations can hold it concurrently, but shutdown() needs the exclusive lock.
struct AwsLease
{
    AwsLease() : lock(aws_lifecycle_mutex)
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

struct FindEntry
{
    std::wstring name;
    std::uint64_t size{};
    FILETIME modified{};
    bool directory{};
};

struct FindState
{
    std::vector<FindEntry> entries;
    std::size_t next{};
};

struct ClientEntry
{
    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> credentials;
    std::shared_ptr<Aws::S3::S3Client> client;
    bool uses_sso{};
};

using ClientKey = std::pair<std::string, std::string>; // (profile, region)
std::map<ClientKey, std::shared_ptr<ClientEntry>> clients;

const std::filesystem::path bucket_registry_file = [] {
    const auto size = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
    if (size == 0)
        throw std::runtime_error("APPDATA is not set");

    std::wstring app_data(size, L'\0');
    const auto written = GetEnvironmentVariableW(L"APPDATA", app_data.data(), size);
    if (written == 0 || written >= size)
        throw std::runtime_error("Cannot read APPDATA");
    app_data.resize(written);
    return std::filesystem::path(std::move(app_data)) / L"s3cmd" / L"buckets.ini";
}();

RemotePath parse_remote_path_impl(std::wstring_view path)
{
    while (!path.empty() && path.front() == L'\\')
        path.remove_prefix(1);
    while (!path.empty() && path.back() == L'\\')
        path.remove_suffix(1);

    const auto profile_end = path.find(L'\\');
    const auto profile = path.substr(0, profile_end);
    if (profile_end == std::wstring_view::npos)
        return {wide_to_utf8(profile), {}, {}};

    path.remove_prefix(profile_end + 1);
    const auto bucket_end = path.find(L'\\');
    const auto bucket = path.substr(0, bucket_end);
    const auto key =
        bucket_end == std::wstring_view::npos ? std::wstring_view{} : path.substr(bucket_end + 1);

    auto key_utf8 = wide_to_utf8(key);
    std::ranges::replace(key_utf8, '\\', '/');
    return {wide_to_utf8(profile), wide_to_utf8(bucket), std::move(key_utf8)};
}

std::string directory_prefix_impl(const RemotePath& path)
{
    if (path.key.empty())
        return {};
    return path.key.back() == '/' ? path.key : path.key + '/';
}

FILETIME to_file_time(const Aws::Utils::DateTime& value)
{
    constexpr std::int64_t windows_epoch_offset_ms = 11'644'473'600'000;
    const auto ticks =
        static_cast<std::uint64_t>(value.Millis() + windows_epoch_offset_ms) * 10'000;
    return {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
}

bool valid_entry_name(const std::wstring& name)
{
    return !name.empty() && name != L"." && name != L".." && name.find(L'\\') == name.npos &&
           name.size() < MAX_PATH;
}

bool is_dry_run()
{
    return GetPrivateProfileIntW(L"settings", L"DryRun", 0, bucket_registry_file.c_str()) != 0;
}

std::wstring bucket_section(std::string_view profile)
{
    return L"buckets." + utf8_to_wide(profile);
}

std::wstring hidden_section(std::string_view profile)
{
    return L"hidden." + utf8_to_wide(profile);
}

std::wstring region_section(std::string_view profile)
{
    return L"regions." + utf8_to_wide(profile);
}

std::vector<std::pair<std::wstring, std::wstring>>
    read_ini_section(const std::filesystem::path& file, const std::wstring& section)
{
    std::vector<wchar_t> buffer(1024);

    for (;;)
    {
        const auto length = GetPrivateProfileSectionW(
            section.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()), file.c_str());
        if (length < buffer.size() - 2)
            break;
        buffer.resize(buffer.size() * 2);
    }

    std::vector<std::pair<std::wstring, std::wstring>> values;
    for (const wchar_t* item = buffer.data(); *item; item += std::wcslen(item) + 1)
    {
        const std::wstring_view entry(item);
        const auto separator = entry.find(L'=');
        if (separator != entry.npos)
            values.emplace_back(entry.substr(0, separator), entry.substr(separator + 1));
    }
    return values;
}

BucketMap registered_buckets_impl(const std::filesystem::path& file, std::string_view profile)
{
    std::scoped_lock lock(registry_mutex);
    BucketMap buckets;
    for (const auto& [bucket, region] : read_ini_section(file, bucket_section(profile)))
        buckets.emplace(wide_to_utf8(bucket), BucketInfo{wide_to_utf8(region)});
    return buckets;
}

std::set<std::string> hidden_buckets_impl(const std::filesystem::path& file,
                                          std::string_view profile)
{
    std::scoped_lock lock(registry_mutex);
    std::set<std::string> buckets;
    for (const auto& [bucket, ignored] : read_ini_section(file, hidden_section(profile)))
        buckets.emplace(wide_to_utf8(bucket));
    return buckets;
}

BucketMap cached_bucket_regions_impl(const std::filesystem::path& file, std::string_view profile)
{
    std::scoped_lock lock(registry_mutex);
    BucketMap buckets;
    for (const auto& [bucket, region] : read_ini_section(file, region_section(profile)))
        buckets.emplace(wide_to_utf8(bucket), BucketInfo{wide_to_utf8(region)});
    return buckets;
}

bool cache_bucket_regions_impl(const std::filesystem::path& file, std::string_view profile,
                               const BucketMap& buckets)
{
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
        return false;

    const auto section = region_section(profile);
    std::scoped_lock lock(registry_mutex);
    if (!WritePrivateProfileStringW(section.c_str(), nullptr, nullptr, file.c_str()))
        return false;

    for (const auto& [bucket, info] : buckets)
    {
        if (info.region.empty())
            continue;
        const auto name = utf8_to_wide(bucket);
        const auto region = utf8_to_wide(info.region);
        if (!WritePrivateProfileStringW(section.c_str(), name.c_str(), region.c_str(),
                                        file.c_str()))
            return false;
    }
    return true;
}

bool register_bucket_impl(const std::filesystem::path& file, std::string_view profile,
                          std::string_view bucket, std::string_view region)
{
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
        return false;

    const auto profile_section = bucket_section(profile);
    const auto ignored_section = hidden_section(profile);
    const auto bucket_name = utf8_to_wide(bucket);
    const auto bucket_region = utf8_to_wide(region);

    std::scoped_lock lock(registry_mutex);
    const auto written = WritePrivateProfileStringW(profile_section.c_str(), bucket_name.c_str(),
                                                    bucket_region.c_str(), file.c_str()) != FALSE;
    const auto unhidden = WritePrivateProfileStringW(ignored_section.c_str(), bucket_name.c_str(),
                                                     nullptr, file.c_str()) != FALSE;
    return written && unhidden;
}

bool unregister_bucket_impl(const std::filesystem::path& file, std::string_view profile,
                            std::string_view bucket)
{
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
        return false;

    const auto profile_section = bucket_section(profile);
    const auto ignored_section = hidden_section(profile);
    const auto bucket_name = utf8_to_wide(bucket);

    std::scoped_lock lock(registry_mutex);
    const auto removed = WritePrivateProfileStringW(profile_section.c_str(), bucket_name.c_str(),
                                                    nullptr, file.c_str()) != FALSE;
    const auto hidden = WritePrivateProfileStringW(ignored_section.c_str(), bucket_name.c_str(),
                                                   L"1", file.c_str()) != FALSE;
    return removed && hidden;
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
        const auto region = bucket_region(bucket_registry_file, path.profile, path.bucket);
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
            auto title = std::wstring(L"Amazon S3");
            auto message = utf8_to_wide(error.what());
            std::array<wchar_t, 1> ignored{};
            request_proc(plugin_number, RT_MsgOK, title.data(), message.data(), ignored.data(),
                         static_cast<int>(ignored.size()));
        }
        throw error;
    }
    return entry->client;
}

void log_message(int type, std::wstring message)
{
    if (log_proc)
        log_proc(plugin_number, type, message.data());
    OutputDebugStringW(std::format(L"[s3cmd] {}", message).c_str());
}

void log_error(std::string_view operation, std::string_view message)
{
    auto text = utf8_to_wide(operation);
    text += L": ";
    text += utf8_to_wide(message);
    log_message(MSGTYPE_IMPORTANTERROR, std::move(text));
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
    const auto message = std::format(L"[s3cmd] operation={} dry={} profile={} bucket={} key={}\n",
                                     utf8_to_wide(operation), dry, utf8_to_wide(path.profile),
                                     utf8_to_wide(path.bucket), utf8_to_wide(path.key));
    OutputDebugStringW(message.c_str());
}

void log_local_operation(std::string_view operation, const wchar_t* path, bool dry)
{
    const auto message = std::format(L"[s3cmd] operation={} dry={} path={}\n",
                                     utf8_to_wide(operation), dry, path);
    OutputDebugStringW(message.c_str());
}

void fill_find_data(const FindEntry& entry, WIN32_FIND_DATAW* data)
{
    *data = {};
    data->dwFileAttributes = entry.directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    data->ftLastWriteTime = entry.modified;
    data->nFileSizeLow = static_cast<DWORD>(entry.size);
    data->nFileSizeHigh = static_cast<DWORD>(entry.size >> 32);
    std::copy(entry.name.begin(), entry.name.end(), data->cFileName);
    data->cFileName[entry.name.size()] = L'\0';
}

void append_entry(std::vector<FindEntry>& entries, std::string_view name, bool directory,
                  std::uint64_t size = 0, FILETIME modified = {})
{
    auto wide_name = utf8_to_wide(name);
    if (valid_entry_name(wide_name))
        entries.push_back({std::move(wide_name), size, modified, directory});
}

std::vector<FindEntry> list_entries(const RemotePath& path)
{
    AwsLease lease;
    std::vector<FindEntry> entries;

    if (path.profile.empty())
    {
        std::set<std::string> profiles{"default"};
        const auto selected = Aws::Auth::GetConfigProfileName();
        profiles.emplace(selected.data(), selected.size());
        for (const auto& [name, ignored] : Aws::Config::GetCachedConfigProfiles())
            profiles.emplace(name.data(), name.size());
        for (const auto& [name, ignored] : Aws::Config::GetCachedCredentialsProfiles())
            profiles.emplace(name.data(), name.size());

        for (const auto& profile : profiles)
            append_entry(entries, profile, true);
        return entries;
    }

    if (path.bucket.empty())
    {
        auto registered = registered_buckets(bucket_registry_file, path.profile);
        const auto hidden = hidden_buckets(bucket_registry_file, path.profile);

        BucketMap discovered;
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
                discovered.clear();
                if (outcome.GetError().GetResponseCode() != Aws::Http::HttpResponseCode::FORBIDDEN)
                    log_aws_error("ListBuckets", outcome.GetError());
                break;
            }

            const auto& result = outcome.GetResult();
            for (const auto& bucket : result.GetBuckets())
            {
                const std::string name(bucket.GetName().data(), bucket.GetName().size());
                const auto& region = bucket.GetBucketRegion();
                discovered[name] = {
                    {region.data(), region.size()},
                    to_file_time(bucket.GetCreationDate()),
                };
            }

            if (result.GetContinuationToken().empty())
                break;
            request.SetContinuationToken(result.GetContinuationToken());
        }

        if (discovery_succeeded && !cache_bucket_regions(bucket_registry_file, path.profile, discovered))
            log_error("ListBuckets", "Cannot cache bucket regions");

        const auto buckets = merge_buckets(std::move(registered), hidden, discovered);
        for (const auto& [name, bucket] : buckets)
            append_entry(entries, name, true, 0, bucket.created);
        return entries;
    }

    const auto prefix = directory_prefix(path);
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
            append_entry(entries, {value.data() + prefix.size(), value.size() - prefix.size() - 1},
                         true);
        }

        for (const auto& object : result.GetContents())
        {
            const auto& key = object.GetKey();
            if (std::string_view(key.data(), key.size()) == prefix || key.size() <= prefix.size())
                continue;
            append_entry(entries, {key.data() + prefix.size(), key.size() - prefix.size()}, false,
                         static_cast<std::uint64_t>(object.GetSize()),
                         to_file_time(object.GetLastModified()));
        }

        if (!result.GetIsTruncated())
            break;
        request.SetContinuationToken(result.GetNextContinuationToken());
    }

    return entries;
}

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

bool report_progress(const wchar_t* source, const wchar_t* target, int percent)
{
    return progress_proc && progress_proc(plugin_number, const_cast<wchar_t*>(source),
                                          const_cast<wchar_t*>(target), percent) != 0;
}

class TransferProgress
{
public:
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
    template <typename Request>
    TransferProgress(const wchar_t* source, const wchar_t* target,
                     std::uint64_t total, Request& request)
        : source_{source}, target_{target}, total_{total}
    {
        set_common_handlers(request);
    }

    template <typename Request>
    void set_common_handlers(Request& request)
    {
        request.SetRequestRetryHandler(
            [this](const Aws::AmazonWebServiceRequest&) { transferred_ = 0; });
        request.SetContinueRequestHandler(
            [this](const Aws::Http::HttpRequest*) { return !canceled_.load(); });
    }

    int transfer_percent(std::uint64_t transferred, std::uint64_t total)
    {
        return total == 0
                   ? 0
                   : std::min(99, static_cast<int>(std::min(transferred, total) * 100 / total));
    }

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

int download_file(const wchar_t* remote_name, const wchar_t* local_name, int copy_flags)
{
    if ((copy_flags & FS_COPYFLAGS_RESUME) != 0)
        return FS_FILE_NOTSUPPORTED;
    if ((copy_flags & FS_COPYFLAGS_OVERWRITE) == 0 && std::filesystem::exists(local_name))
        return FS_FILE_EXISTS;
    if (report_progress(remote_name, local_name, 0))
        return FS_FILE_USERABORT;

    const auto path = parse_remote_path(remote_name);
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

int upload_file(const wchar_t* local_name, const wchar_t* remote_name, int copy_flags)
{
    if ((copy_flags & FS_COPYFLAGS_RESUME) != 0)
        return FS_FILE_NOTSUPPORTED;

    std::error_code error;
    const auto size = std::filesystem::file_size(local_name, error);
    if (error)
        return FS_FILE_READERROR;
    // ponytail: single-part upload stops at S3's 5 GiB limit; use TransferManager
    // when larger uploads are actually needed.
    if (size > max_single_part_size)
        return FS_FILE_NOTSUPPORTED;
    if (report_progress(local_name, remote_name, 0))
        return FS_FILE_USERABORT;

    const auto path = parse_remote_path(remote_name);
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

int copy_or_move(const wchar_t* old_name, const wchar_t* new_name, bool move, bool overwrite,
                 const RemoteInfoStruct* info)
{
    if (info && info->SizeHigh == 0xFFFFFFFF)
        return FS_FILE_NOTSUPPORTED;
    const auto size = info ? (static_cast<std::uint64_t>(info->SizeHigh) << 32) | info->SizeLow : 0;
    // ponytail: CopyObject is limited to 5 GiB and only reports 0%/100%; use
    // multipart copy when larger objects or intermediate progress are needed.
    if (size > max_single_part_size)
        return FS_FILE_NOTSUPPORTED;
    if (report_progress(old_name, new_name, 0))
        return FS_FILE_USERABORT;

    const auto source = parse_remote_path(old_name);
    const auto target = parse_remote_path(new_name);
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
    // CopyObjectRequest URL-encodes this value when serializing the header.
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
} // namespace

RemotePath parse_remote_path(std::wstring_view path)
{
    return parse_remote_path_impl(path);
}

std::string directory_prefix(const RemotePath& path)
{
    return directory_prefix_impl(path);
}

BucketMap registered_buckets(const std::filesystem::path& file, std::string_view profile)
{
    return registered_buckets_impl(file, profile);
}

std::set<std::string> hidden_buckets(const std::filesystem::path& file, std::string_view profile)
{
    return hidden_buckets_impl(file, profile);
}

BucketMap cached_bucket_regions(const std::filesystem::path& file, std::string_view profile)
{
    return cached_bucket_regions_impl(file, profile);
}

bool cache_bucket_regions(const std::filesystem::path& file, std::string_view profile,
                          const BucketMap& buckets)
{
    return cache_bucket_regions_impl(file, profile, buckets);
}

std::string bucket_region(const std::filesystem::path& file, std::string_view profile,
                          std::string_view bucket)
{
    const std::string bucket_name(bucket);
    const auto registered = registered_buckets(file, profile);
    if (const auto found = registered.find(bucket_name); found != registered.end())
        return found->second.region;

    const auto cached = cached_bucket_regions(file, profile);
    if (const auto found = cached.find(bucket_name); found != cached.end())
        return found->second.region;
    return {};
}

std::string discover_bucket_region(std::string_view profile, std::string_view bucket)
{
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

bool register_bucket(const std::filesystem::path& file, std::string_view profile,
                     std::string_view bucket, std::string_view region)
{
    return register_bucket_impl(file, profile, bucket, region);
}

bool unregister_bucket(const std::filesystem::path& file, std::string_view profile,
                       std::string_view bucket)
{
    return unregister_bucket_impl(file, profile, bucket);
}

BucketMap merge_buckets(BucketMap registered, const std::set<std::string>& hidden,
                        const BucketMap& discovered)
{
    for (const auto& [name, bucket] : discovered)
    {
        if (!hidden.contains(name))
        {
            auto& visible = registered[name];
            if (visible.region.empty())
                visible.region = bucket.region;
            visible.created = bucket.created;
        }
    }
    return registered;
}

int initialize(int number, tProgressProcW progress, tLogProcW log, tRequestProcW request)
{
    OutputDebugStringW(std::format(L"[s3cmd] Initialize called, plugin={}, thread={}", number,
                                   GetCurrentThreadId())
                           .c_str());

    std::unique_lock lock(aws_lifecycle_mutex);
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
    OutputDebugStringW(
        std::format(L"[s3cmd] Shutdown called, thread={}", GetCurrentThreadId()).c_str());

    std::unique_lock lock(aws_lifecycle_mutex);
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

HANDLE find_first(wchar_t* path, WIN32_FIND_DATAW* find_data)
{
    try
    {
        auto state = std::make_unique<FindState>();
        state->entries = list_entries(parse_remote_path(path));
        if (state->entries.empty())
        {
            SetLastError(ERROR_NO_MORE_FILES);
            return INVALID_HANDLE_VALUE;
        }

        fill_find_data(state->entries.front(), find_data);
        state->next = 1;
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

BOOL find_next(HANDLE handle, WIN32_FIND_DATAW* find_data)
{
    auto* state = static_cast<FindState*>(handle);
    if (!state || state->next >= state->entries.size())
        return FALSE;
    fill_find_data(state->entries[state->next++], find_data);
    return TRUE;
}

int find_close(HANDLE handle)
{
    if (handle != INVALID_HANDLE_VALUE)
        delete static_cast<FindState*>(handle);
    return 0;
}

void status_info(wchar_t* remote_directory, int start_end, int operation)
{
    if (operation != FS_STATUS_OP_DELETE)
        return;
    if (start_end == FS_STATUS_END)
    {
        delete_mode = DeleteMode::normal;
        return;
    }

    const auto path = parse_remote_path(remote_directory);
    delete_mode = path.profile.empty()
                      ? DeleteMode::protect_profiles
                      : path.bucket.empty() ? DeleteMode::unregister_buckets : DeleteMode::normal;
}

int get_file(wchar_t* remote_name, wchar_t* local_name, int copy_flags, RemoteInfoStruct*)
{
    try
    {
        return download_file(remote_name, local_name, copy_flags);
    }
    catch (const std::exception& error)
    {
        log_unexpected("FsGetFileW", error);
        return FS_FILE_READERROR;
    }
}

int put_file(wchar_t* local_name, wchar_t* remote_name, int copy_flags)
{
    try
    {
        return upload_file(local_name, remote_name, copy_flags);
    }
    catch (const std::exception& error)
    {
        log_unexpected("FsPutFileW", error);
        return FS_FILE_WRITEERROR;
    }
}

BOOL delete_file(const wchar_t* remote_name)
{
    try
    {
        if (delete_mode != DeleteMode::normal)
            return TRUE;

        const auto path = parse_remote_path(remote_name);
        if (path.profile.empty() || path.bucket.empty() || path.key.empty())
            return FALSE;

        const auto is_dry = is_dry_run();
        log_operation("DeleteObject", path, is_dry);
        if (is_dry)
        {
            return TRUE;
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
        return FALSE;
    }
}

BOOL make_directory(wchar_t* remote_name)
{
    try
    {
        const auto path = parse_remote_path(remote_name);
        if (path.profile.empty() || path.bucket.empty())
            return FALSE;

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
                    return FALSE;
            }
            const auto is_dry = is_dry_run();
            log_operation("RegisterBucket", path, is_dry);
            if (is_dry)
            {
                return TRUE;
            }
            return register_bucket(bucket_registry_file, path.profile, path.bucket, region);
        }

        const RemotePath marker{path.profile, path.bucket, directory_prefix(path)};
        const auto is_dry = is_dry_run();
        log_operation("PutObject", marker, is_dry);
        if (is_dry)
        {
            return TRUE;
        }

        AwsLease lease;
        auto client = get_client(path);
        Aws::S3::Model::PutObjectRequest request;
        request.SetBucket(path.bucket.c_str());
        request.SetKey(directory_prefix(path).c_str());
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
        return FALSE;
    }
}

BOOL remove_directory(wchar_t* remote_name)
{
    try
    {
        const auto path = parse_remote_path(remote_name);
        if (path.profile.empty() || path.bucket.empty())
            return FALSE;

        // Top-level profile and bucket directories are virtual links. During
        // recursive deletion, never pass the delete through to their S3 contents.
        if (delete_mode == DeleteMode::protect_profiles)
            return TRUE;
        if (delete_mode == DeleteMode::unregister_buckets)
        {
            if (!path.key.empty())
                return TRUE;
            const auto is_dry = is_dry_run();
            log_operation("UnregisterBucket", path, is_dry);
            if (is_dry)
            {
                return TRUE;
            }
            return unregister_bucket(bucket_registry_file, path.profile, path.bucket);
        }

        if (path.key.empty())
        {
            const auto is_dry = is_dry_run();
            log_operation("UnregisterBucket", path, is_dry);
            if (is_dry)
            {
                return TRUE;
            }
            return unregister_bucket(bucket_registry_file, path.profile, path.bucket);
        }

        const auto prefix = directory_prefix(path);

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
            return FALSE;
        }

        bool marker_exists = false;
        for (const auto& object : listed.GetResult().GetContents())
        {
            if (std::string_view(object.GetKey().data(), object.GetKey().size()) != prefix)
                return FALSE;
            marker_exists = true;
        }
        if (!marker_exists)
            return TRUE;

        const RemotePath marker{path.profile, path.bucket, prefix};
        const auto is_dry = is_dry_run();
        log_operation("DeleteObject", marker, is_dry);
        if (is_dry)
        {
            return TRUE;
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
        return FALSE;
    }
}

int rename_or_move(wchar_t* old_name, wchar_t* new_name, BOOL move, BOOL overwrite,
                   RemoteInfoStruct* info)
{
    try
    {
        return copy_or_move(old_name, new_name, move != FALSE, overwrite != FALSE, info);
    }
    catch (const std::exception& error)
    {
        log_unexpected("FsRenMovFileW", error);
        return FS_FILE_WRITEERROR;
    }
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

int content_get_value(wchar_t* file_name, int field_index, void* field_value, int max_length)
{
    if (field_index != 0)
        return ft_nosuchfield;
    if (!file_name || !field_value || max_length < static_cast<int>(sizeof(wchar_t)))
        return ft_fileerror;

    const auto path = parse_remote_path(file_name);
    if (path.profile.empty() || path.bucket.empty() || !path.key.empty())
        return ft_fieldempty;

    const auto region = bucket_region(bucket_registry_file, path.profile, path.bucket);
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
