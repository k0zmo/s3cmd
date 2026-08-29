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

    // Splits totalcmd's path (i.e. \\{{profile}}\{{bucket}}\{{key}}) into profile, bucket and key
    static RemotePath make(std::wstring_view path);

    // Returns key as an S3 directory prefix, adding a trailing '/' when needed.
    // It assumes `key` refers to a directory, thus this function should only be called for S3
    // directory operations
    std::string directory_prefix() const;

    bool operator==(const RemotePath&) const = default;
};

struct BucketInfo
{
    std::string region;
    FILETIME created{};
};
using BucketMap = std::map<std::string, BucketInfo, std::less<>>;

// Function called only in tests
void reset_config();

class ProfileConfig
{
public:
    explicit ProfileConfig(std::string profile)
      : profile_{std::move(profile)}
    {
    }

    BucketMap registered_buckets() const;
    void set_discovered_buckets(BucketMap buckets) const;
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
HANDLE find_first(const wchar_t* path, WIN32_FIND_DATAW* find_data);
bool find_next(HANDLE handle, WIN32_FIND_DATAW* find_data);
int find_close(HANDLE handle);
void status_info(const wchar_t* remote_directory, int start_end, int operation);
int get_file(const wchar_t* remote_name, const wchar_t* local_name, int copy_flags,
             const RemoteInfoStruct* info);
int put_file(const wchar_t* local_name, const wchar_t* remote_name, int copy_flags);
bool delete_file(const wchar_t* remote_name);
bool make_directory(const wchar_t* remote_name);
bool remove_directory(const wchar_t* remote_name);
int rename_or_move(const wchar_t* old_name, const wchar_t* new_name, bool move, bool overwrite,
                   const RemoteInfoStruct* info);
void get_default_root_name(char* name, int max_length);
int content_get_supported_field(int field_index, char* field_name, char* units, int max_length);
int content_get_value(const wchar_t* file_name, int field_index, void* field_value, int max_length);

} // namespace s3cmd
