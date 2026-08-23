#include "utils.hpp"

#include <Windows.h>

#include <stdexcept>

namespace s3cmd {

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

} // namespace s3cmd
