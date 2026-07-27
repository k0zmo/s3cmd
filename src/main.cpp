#include "fsplugin.h"

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/config/ConfigAndCredentialsCacheManager.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSAllocator.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3ClientConfiguration.h>
#include <aws/s3/model/CopyObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/PutObjectRequest.h>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {
constexpr std::uint64_t max_single_part_size = 5ULL * 1024 * 1024 * 1024;

int plugin_number{};
tProgressProcW progress_proc{};
tLogProcW log_proc{};
tRequestProcW request_proc{};

std::mutex aws_mutex;
std::mutex registry_mutex;

enum class DeleteMode
{
    normal,
    unregister_buckets,
    protect_profiles,
};

thread_local DeleteMode delete_mode = DeleteMode::normal;

struct AwsSession
{
    AwsSession() : lock(aws_mutex) { Aws::InitAPI(options); }

    ~AwsSession() { Aws::ShutdownAPI(options); }

    AwsSession(const AwsSession&) = delete;
    AwsSession& operator=(const AwsSession&) = delete;

    // ponytail: serialized SDK sessions avoid unsafe overlapping global
    // InitAPI/ShutdownAPI calls; keep one process session if concurrency matters.
    std::unique_lock<std::mutex> lock;
    Aws::SDKOptions options;
};

struct RemotePath
{
    std::string profile;
    std::string bucket;
    std::string key;
};

struct BucketInfo
{
    std::string region;
    FILETIME created{};
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

std::wstring utf8_to_wide(std::string_view text)
{
    if (text.empty())
        return {};

    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (size == 0)
        throw std::runtime_error("S3 returned invalid UTF-8");

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size) == 0)
        throw std::runtime_error("S3 returned invalid UTF-8");
    return result;
}

std::string wide_to_utf8(std::wstring_view text)
{
    if (text.empty())
        return {};

    const auto size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0)
        throw std::runtime_error("Total Commander passed invalid UTF-16");

    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size, nullptr,
                            nullptr) == 0)
        throw std::runtime_error("Total Commander passed invalid UTF-16");
    return result;
}

RemotePath parse_remote_path(std::wstring_view path)
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
    auto key =
        bucket_end == std::wstring_view::npos ? std::wstring_view{} : path.substr(bucket_end + 1);

    auto key_utf8 = wide_to_utf8(key);
    std::ranges::replace(key_utf8, '\\', '/');
    return {wide_to_utf8(profile), wide_to_utf8(bucket), std::move(key_utf8)};
}

std::string directory_prefix(const RemotePath& path)
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

const std::filesystem::path& bucket_registry_file()
{
    static const auto path = [] {
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
    return path;
}

std::wstring bucket_section(std::string_view profile)
{
    return L"buckets." + utf8_to_wide(profile);
}

std::wstring hidden_section(std::string_view profile)
{
    return L"hidden." + utf8_to_wide(profile);
}

std::vector<std::pair<std::wstring, std::wstring>> read_ini_section(const std::wstring& section)
{
    std::vector<wchar_t> buffer(1024);
    const auto& file = bucket_registry_file();

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

std::map<std::string, BucketInfo> registered_buckets(std::string_view profile)
{
    std::scoped_lock lock(registry_mutex);
    std::map<std::string, BucketInfo> buckets;
    for (const auto& [bucket, region] : read_ini_section(bucket_section(profile)))
        buckets.emplace(wide_to_utf8(bucket), BucketInfo{wide_to_utf8(region)});
    return buckets;
}

std::set<std::string> hidden_buckets(std::string_view profile)
{
    std::scoped_lock lock(registry_mutex);
    std::set<std::string> buckets;
    for (const auto& [bucket, ignored] : read_ini_section(hidden_section(profile)))
        buckets.emplace(wide_to_utf8(bucket));
    return buckets;
}

bool register_bucket(std::string_view profile, std::string_view bucket, std::string_view region)
{
    std::error_code error;
    std::filesystem::create_directories(bucket_registry_file().parent_path(), error);
    if (error)
        return false;

    const auto profile_section = bucket_section(profile);
    const auto ignored_section = hidden_section(profile);
    const auto bucket_name = utf8_to_wide(bucket);
    const auto bucket_region = utf8_to_wide(region);
    const auto& file = bucket_registry_file();

    std::scoped_lock lock(registry_mutex);
    const auto written = WritePrivateProfileStringW(profile_section.c_str(), bucket_name.c_str(),
                                                    bucket_region.c_str(), file.c_str()) != FALSE;
    const auto unhidden = WritePrivateProfileStringW(ignored_section.c_str(), bucket_name.c_str(),
                                                     nullptr, file.c_str()) != FALSE;
    return written && unhidden;
}

bool unregister_bucket(std::string_view profile, std::string_view bucket)
{
    std::error_code error;
    std::filesystem::create_directories(bucket_registry_file().parent_path(), error);
    if (error)
        return false;

    const auto profile_section = bucket_section(profile);
    const auto ignored_section = hidden_section(profile);
    const auto bucket_name = utf8_to_wide(bucket);
    const auto& file = bucket_registry_file();

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

Aws::S3::S3Client make_client(const RemotePath& path)
{
    Aws::S3::S3ClientConfiguration configuration(path.profile.c_str());
    if (!path.bucket.empty())
    {
        const auto buckets = registered_buckets(path.profile);
        if (const auto bucket = buckets.find(path.bucket);
            bucket != buckets.end() && !bucket->second.region.empty())
            configuration.region = bucket->second.region.c_str();
    }

    Aws::Client::ClientConfiguration::CredentialProviderConfiguration credentials_configuration;
    credentials_configuration.profile = path.profile.c_str();
    credentials_configuration.region = configuration.region;
    auto credentials = Aws::MakeShared<Aws::Auth::DefaultAWSCredentialsProviderChain>(
        "s3cmd", credentials_configuration);
    return Aws::S3::S3Client(credentials, nullptr, configuration);
}

void log_message(int type, std::wstring message)
{
    if (log_proc)
        log_proc(plugin_number, type, message.data());
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
    AwsSession session;
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
        auto buckets = registered_buckets(path.profile);
        const auto hidden = hidden_buckets(path.profile);
        auto client = make_client(path);
        const auto outcome = client.ListBuckets();
        if (outcome.IsSuccess())
        {
            for (const auto& bucket : outcome.GetResult().GetBuckets())
            {
                const std::string name(bucket.GetName().data(), bucket.GetName().size());
                if (!hidden.contains(name))
                    buckets[name].created = to_file_time(bucket.GetCreationDate());
            }
        }
        else if (outcome.GetError().GetResponseCode() != Aws::Http::HttpResponseCode::FORBIDDEN)
        {
            log_aws_error("ListBuckets", outcome.GetError());
        }

        for (const auto& [name, bucket] : buckets)
            append_entry(entries, name, true, 0, bucket.created);
        return entries;
    }

    auto client = make_client(path);
    const auto prefix = directory_prefix(path);
    Aws::S3::Model::ListObjectsV2Request request;
    request.SetBucket(path.bucket.c_str());
    request.SetDelimiter("/");
    request.SetPrefix(prefix.c_str());

    for (;;)
    {
        const auto outcome = client.ListObjectsV2(request);
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

bool request_region(std::string_view profile, std::string& region)
{
    if (!request_proc)
        return !region.empty();

    std::array<wchar_t, 128> value{};
    const auto default_region = utf8_to_wide(region);
    std::copy_n(default_region.data(), std::min(default_region.size(), value.size() - 1),
                value.data());

    std::wstring title = L"Register S3 bucket";
    std::wstring prompt = L"Region for AWS profile '" + utf8_to_wide(profile) + L"':";
    if (!request_proc(plugin_number, RT_Other, title.data(), prompt.data(), value.data(),
                      static_cast<int>(value.size())))
        return false;

    region = wide_to_utf8(value.data());
    return !region.empty();
}

int get_file(const wchar_t* remote_name, const wchar_t* local_name, int copy_flags)
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

    const auto local = std::filesystem::path(local_name);
    wchar_t temporary[MAX_PATH];
    if (GetTempFileNameW(local.parent_path().c_str(), L"s3c", 0, temporary) == 0)
        return FS_FILE_WRITEERROR;
    const auto partial = std::filesystem::path(temporary);
    std::error_code ignored;

    {
        AwsSession session;
        auto client = make_client(path);
        Aws::S3::Model::GetObjectRequest request;
        request.SetBucket(path.bucket.c_str());
        request.SetKey(path.key.c_str());
        request.SetResponseStreamFactory([partial] {
            return Aws::New<std::fstream>("s3cmd", partial,
                                          std::ios::out | std::ios::binary | std::ios::trunc);
        });

        const auto outcome = client.GetObject(request);
        if (!outcome.IsSuccess())
        {
            std::filesystem::remove(partial, ignored);
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

    report_progress(remote_name, local_name, 100);
    return FS_FILE_OK;
}

int put_file(const wchar_t* local_name, const wchar_t* remote_name, int copy_flags)
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

    AwsSession session;
    auto client = make_client(path);

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
    const auto outcome = client.PutObject(request);
    if (!outcome.IsSuccess())
    {
        log_aws_error("PutObject", outcome.GetError());
        return outcome.GetError().GetResponseCode() ==
                       Aws::Http::HttpResponseCode::PRECONDITION_FAILED
                   ? FS_FILE_EXISTS
                   : FS_FILE_WRITEERROR;
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
    // ponytail: CopyObject is the useful one-call path; multipart copy belongs
    // here only when objects above 5 GiB are required.
    if (size > max_single_part_size)
        return FS_FILE_NOTSUPPORTED;
    if (report_progress(old_name, new_name, 0))
        return FS_FILE_USERABORT;

    const auto source = parse_remote_path(old_name);
    const auto target = parse_remote_path(new_name);
    if (source.profile.empty() || source.bucket.empty() || source.key.empty() ||
        target.profile.empty() || target.bucket.empty() || target.key.empty())
        return FS_FILE_NOTFOUND;

    AwsSession session;
    auto target_client = make_client(target);
    if (!overwrite && remote_exists(target_client, target))
        return FS_FILE_EXISTS;

    Aws::S3::Model::CopyObjectRequest copy;
    copy.SetBucket(target.bucket.c_str());
    copy.SetKey(target.key.c_str());
    auto copy_source = Aws::String(source.bucket.c_str()) + "/" +
                       Aws::Utils::StringUtils::URLEncode(source.key.c_str());
    copy.SetCopySource(std::move(copy_source));
    const auto copy_outcome = target_client.CopyObject(copy);
    if (!copy_outcome.IsSuccess())
    {
        log_aws_error("CopyObject", copy_outcome.GetError());
        return copy_outcome.GetError().GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND
                   ? FS_FILE_NOTFOUND
                   : FS_FILE_WRITEERROR;
    }

    if (move)
    {
        auto source_client = make_client(source);
        Aws::S3::Model::DeleteObjectRequest remove;
        remove.SetBucket(source.bucket.c_str());
        remove.SetKey(source.key.c_str());
        const auto delete_outcome = source_client.DeleteObject(remove);
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

#define WFX_EXPORT extern "C"

WFX_EXPORT int __stdcall FsInitW(int number, tProgressProcW progress, tLogProcW log,
                                 tRequestProcW request)
{
    plugin_number = number;
    progress_proc = progress;
    log_proc = log;
    request_proc = request;
    return 0;
}

WFX_EXPORT HANDLE __stdcall FsFindFirstW(wchar_t* path, WIN32_FIND_DATAW* find_data)
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
    catch (const std::exception& error)
    {
        log_unexpected("FsFindFirstW", error);
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
}

WFX_EXPORT BOOL __stdcall FsFindNextW(HANDLE handle, WIN32_FIND_DATAW* find_data)
{
    auto* state = static_cast<FindState*>(handle);
    if (!state || state->next >= state->entries.size())
        return FALSE;
    fill_find_data(state->entries[state->next++], find_data);
    return TRUE;
}

WFX_EXPORT int __stdcall FsFindClose(HANDLE handle)
{
    if (handle != INVALID_HANDLE_VALUE)
        delete static_cast<FindState*>(handle);
    return 0;
}

WFX_EXPORT void __stdcall FsStatusInfoW(wchar_t* remote_directory, int start_end, int operation)
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

WFX_EXPORT int __stdcall FsGetFileW(wchar_t* remote_name, wchar_t* local_name, int copy_flags,
                                    RemoteInfoStruct*)
{
    try
    {
        return get_file(remote_name, local_name, copy_flags);
    }
    catch (const std::exception& error)
    {
        log_unexpected("FsGetFileW", error);
        return FS_FILE_READERROR;
    }
}

WFX_EXPORT int __stdcall FsPutFileW(wchar_t* local_name, wchar_t* remote_name, int copy_flags)
{
    try
    {
        return put_file(local_name, remote_name, copy_flags);
    }
    catch (const std::exception& error)
    {
        log_unexpected("FsPutFileW", error);
        return FS_FILE_WRITEERROR;
    }
}

WFX_EXPORT BOOL __stdcall FsDeleteFileW(wchar_t* remote_name)
{
    try
    {
        if (delete_mode != DeleteMode::normal)
            return TRUE;

        const auto path = parse_remote_path(remote_name);
        if (path.profile.empty() || path.bucket.empty() || path.key.empty())
            return FALSE;

        AwsSession session;
        auto client = make_client(path);
        Aws::S3::Model::DeleteObjectRequest request;
        request.SetBucket(path.bucket.c_str());
        request.SetKey(path.key.c_str());
        const auto outcome = client.DeleteObject(request);
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

WFX_EXPORT BOOL __stdcall FsMkDirW(wchar_t* remote_name)
{
    try
    {
        const auto path = parse_remote_path(remote_name);
        if (path.profile.empty() || path.bucket.empty())
            return FALSE;

        if (path.key.empty())
        {
            std::string region;
            {
                AwsSession session;
                region = profile_region(path.profile);
            }
            return request_region(path.profile, region) &&
                   register_bucket(path.profile, path.bucket, region);
        }

        AwsSession session;
        auto client = make_client(path);
        Aws::S3::Model::PutObjectRequest request;
        request.SetBucket(path.bucket.c_str());
        request.SetKey(directory_prefix(path).c_str());
        request.SetBody(Aws::MakeShared<Aws::StringStream>("s3cmd"));
        request.SetContentLength(0);
        const auto outcome = client.PutObject(request);
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

WFX_EXPORT BOOL __stdcall FsRemoveDirW(wchar_t* remote_name)
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
            return path.key.empty() ? unregister_bucket(path.profile, path.bucket) : TRUE;

        if (path.key.empty())
            return unregister_bucket(path.profile, path.bucket);

        const auto prefix = directory_prefix(path);

        AwsSession session;
        auto client = make_client(path);
        Aws::S3::Model::ListObjectsV2Request list;
        list.SetBucket(path.bucket.c_str());
        list.SetPrefix(prefix.c_str());
        list.SetMaxKeys(2);
        const auto listed = client.ListObjectsV2(list);
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

        Aws::S3::Model::DeleteObjectRequest remove;
        remove.SetBucket(path.bucket.c_str());
        remove.SetKey(prefix.c_str());
        const auto removed = client.DeleteObject(remove);
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

WFX_EXPORT int __stdcall FsRenMovFileW(wchar_t* old_name, wchar_t* new_name, BOOL move,
                                       BOOL overwrite, RemoteInfoStruct* info)
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

WFX_EXPORT void __stdcall FsGetDefRootName(char* name, int max_length)
{
    constexpr std::string_view root_name = "Amazon S3";
    if (!name || max_length <= 0)
        return;
    const auto length = std::min(root_name.size(), static_cast<std::size_t>(max_length - 1));
    std::memcpy(name, root_name.data(), length);
    name[length] = '\0';
}

#ifdef S3CMD_SELF_CHECK
int main()
{
    const auto root = parse_remote_path(L"\\");
    assert(root.profile.empty() && root.bucket.empty() && root.key.empty());

    const auto profile = parse_remote_path(L"\\work");
    assert(profile.profile == "work" && profile.bucket.empty() && profile.key.empty());

    const auto bucket = parse_remote_path(L"\\work\\my-bucket");
    assert(bucket.profile == "work" && bucket.bucket == "my-bucket" && bucket.key.empty());

    const auto object = parse_remote_path(L"\\work\\my-bucket\\one\\two.txt");
    assert(object.profile == "work");
    assert(object.bucket == "my-bucket");
    assert(object.key == "one/two.txt");
    assert(directory_prefix({"work", "my-bucket", "one/two"}) == "one/two/");
    assert(directory_prefix({"work", "my-bucket", ""}).empty());

    const auto test_app_data = std::filesystem::temp_directory_path() /
                               (L"s3cmd-self-check-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    std::filesystem::remove_all(test_app_data, ignored);
    std::filesystem::create_directories(test_app_data);
    assert(SetEnvironmentVariableW(L"APPDATA", test_app_data.c_str()));

    assert(register_bucket("work", "private-bucket", "eu-central-1"));
    const auto registered = registered_buckets("work");
    assert(registered.at("private-bucket").region == "eu-central-1");
    assert(unregister_bucket("work", "private-bucket"));
    assert(registered_buckets("work").empty());
    assert(hidden_buckets("work").contains("private-bucket"));

    wchar_t profile_directory[] = L"\\work";
    FsStatusInfoW(profile_directory, FS_STATUS_START, FS_STATUS_OP_DELETE);
    assert(delete_mode == DeleteMode::unregister_buckets);
    FsStatusInfoW(profile_directory, FS_STATUS_END, FS_STATUS_OP_DELETE);
    assert(delete_mode == DeleteMode::normal);

    std::filesystem::remove_all(test_app_data, ignored);
}
#endif
