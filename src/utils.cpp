#include "utils.hpp"

#include <Windows.h>

#include <cwctype>
#include <stdexcept>
#include <string>
#include <string_view>

namespace s3cmd {

int caseicmp(const char16_t* lhs, const char16_t* rhs)
{
    return ncaseicmp(lhs, rhs, std::u16string_view::npos);
}

int ncaseicmp(const char16_t* lhs, const char16_t* rhs, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto left = std::towlower(lhs[i]);
        const auto right = std::towlower(rhs[i]);
        if (left != right)
            return left < right ? -1 : 1;
        if (lhs[i] == u'\0')
            break;
    }
    return 0;
}

std::u16string to_utf16(std::string_view text)
{
    if (text.empty())
        return {};

    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (size == 0)
        throw std::runtime_error("Invalid UTF-8 provided");

    std::u16string result(static_cast<std::size_t>(size), u'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), win32_string(result.data()), size) == 0)
    {
        throw std::runtime_error("Invalid UTF-8 provided");
    }
    return result;
}

std::string to_utf8(std::u16string_view text)
{
    if (text.empty())
        return {};

    const auto size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, win32_string(text.data()),
                            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0)
        throw std::runtime_error("Invalid UTF-16 provided");

    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, win32_string(text.data()),
                            static_cast<int>(text.size()), result.data(), size, nullptr,
                            nullptr) == 0)
    {
        throw std::runtime_error("Invalid UTF-16 provided");
    }
    return result;
}

} // namespace s3cmd
