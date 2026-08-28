#pragma once

#include "fsplugin.h"

#include <map>
#include <string>
#include <string_view>

namespace s3cmd {

struct RemotePath
{
    std::string profile;
    std::string bucket;
    std::string key;

    bool operator==(const RemotePath&) const = default;
};

struct BucketInfo
{
    std::string region;
    FILETIME created{};
};

using BucketMap = std::map<std::string, BucketInfo>;

RemotePath parse_remote_path(std::wstring_view path);
std::string directory_prefix(const RemotePath& path);

// Function called only in tests
void reset_config();

class ProfileConfig
{
public:
    explicit ProfileConfig(std::string_view profile);

    BucketMap registered_buckets() const;
    BucketMap cached_bucket_regions() const;
    bool cache_bucket_regions(const BucketMap& buckets) const;
    std::string bucket_region(std::string_view bucket) const;
    bool register_bucket(std::string_view bucket, std::string_view region) const;
    bool unregister_bucket(std::string_view bucket) const;

private:
    std::string profile_;
};

std::string discover_bucket_region(std::string_view profile, std::string_view bucket);

// Plugin API implementation
int initialize(int number, tProgressProcW progress, tLogProcW log, tRequestProcW request);
void shutdown();
HANDLE find_first(wchar_t* path, WIN32_FIND_DATAW* find_data);
BOOL find_next(HANDLE handle, WIN32_FIND_DATAW* find_data);
int find_close(HANDLE handle);
void status_info(wchar_t* remote_directory, int start_end, int operation);
int get_file(wchar_t* remote_name, wchar_t* local_name, int copy_flags, RemoteInfoStruct* info);
int put_file(wchar_t* local_name, wchar_t* remote_name, int copy_flags);
BOOL delete_file(const wchar_t* remote_name);
BOOL make_directory(wchar_t* remote_name);
BOOL remove_directory(wchar_t* remote_name);
int rename_or_move(wchar_t* old_name, wchar_t* new_name, BOOL move, BOOL overwrite,
                   RemoteInfoStruct* info);
void get_default_root_name(char* name, int max_length);
int content_get_supported_field(int field_index, char* field_name, char* units, int max_length);
int content_get_value(wchar_t* file_name, int field_index, void* field_value, int max_length);

} // namespace s3cmd
