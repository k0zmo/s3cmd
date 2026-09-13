#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "utils.hpp"

#include <Windows.h>

#include <cwchar>
#include <stdexcept>
#include <string>
#include <string_view>

namespace s3cmd {

int caseicmp(const wchar_t* lhs, const wchar_t* rhs)
{
#ifdef _WIN32
    return _wcsicmp(lhs, rhs);
#else
    return wcscasecmp(lhs, rhs);
#endif
}

int ncaseicmp(const wchar_t* lhs, const wchar_t* rhs, std::size_t count)
{
#ifdef _WIN32
    return _wcsnicmp(lhs, rhs, count);
#else
    return wcsncasecmp(lhs, rhs, count);
#endif
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
