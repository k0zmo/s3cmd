#include "core.hpp"

#include <Windows.h>

#include <algorithm>
#include <stdexcept>

namespace s3cmd {

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
        return {to_utf8(profile), {}, {}};

    // Split the remaining into the bucket and the key
    path.remove_prefix(profile_end + 1);
    const auto bucket_end = path.find(L'\\');
    const auto bucket = path.substr(0, bucket_end);
    const auto key =
        bucket_end == std::wstring_view::npos ? std::wstring_view{} : path.substr(bucket_end + 1);

    auto key_utf8 = to_utf8(key);
    std::ranges::replace(key_utf8, '\\', '/');
    return {to_utf8(profile), to_utf8(bucket), std::move(key_utf8)};
}

std::string RemotePath::directory_prefix() const
{
    if (key.empty())
        return {};
    return key.back() == '/' ? key : key + '/';
}

std::wstring to_wide(std::string_view text)
{
    if (text.empty())
        return {};

    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (size == 0)
        throw std::runtime_error("Invalid UTF-8 provided");

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size) == 0)
    {
        throw std::runtime_error("Invalid UTF-8 provided");
    }
    return result;
}

std::string to_utf8(std::wstring_view text)
{
    if (text.empty())
        return {};

    const auto size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0)
        throw std::runtime_error("Invalid UTF-16 provided");

    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size, nullptr,
                            nullptr) == 0)
    {
        throw std::runtime_error("Invalid UTF-16 provided");
    }
    return result;
}

} // namespace s3cmd
