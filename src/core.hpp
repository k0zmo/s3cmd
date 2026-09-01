#pragma once

#include <string>
#include <string_view>

namespace s3cmd {

struct RemotePathView
{
    std::wstring_view profile;
    std::wstring_view bucket;
    std::wstring_view key;

    static RemotePathView make(std::wstring_view path) noexcept;

    bool operator==(const RemotePathView&) const = default;
};

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

std::wstring to_wide(std::string_view text);
std::string to_utf8(std::wstring_view text);

} // namespace s3cmd
