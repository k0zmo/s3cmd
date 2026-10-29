#include "utils.hpp"
#include "platform.hpp"

#ifdef _WIN32
#  include <Windows.h>
#else
#  include <iconv.h>
#endif

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

#ifdef _WIN32

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

#else

namespace {

template <typename Char>
std::basic_string<Char> convert_encoding(std::string_view text, std::size_t capacity,
                                         const char* from, const char* to, const char* error)
{
    if (text.empty())
        return {};

    std::basic_string<Char> result(capacity, Char{});
    const auto converter = iconv_open(to, from);
    if (converter == reinterpret_cast<iconv_t>(-1))
        throw std::runtime_error("Failed to initialize character encoding conversion");

    auto* input = const_cast<char*>(text.data());
    auto input_left = text.size();
    auto* output = reinterpret_cast<char*>(result.data());
    auto output_left = result.size() * sizeof(Char);
    const auto status = iconv(converter, &input, &input_left, &output, &output_left);
    iconv_close(converter);
    if (status != 0 || input_left != 0)
        throw std::runtime_error{error};

    result.resize(result.size() - output_left / sizeof(Char));
    return result;
}

} // namespace

std::u16string to_utf16(std::string_view text)
{
    return convert_encoding<char16_t>(text, text.size(), "UTF-8", "UTF-16LE",
                                      "Invalid UTF-8 provided");
}

std::string to_utf8(std::u16string_view text)
{
    if (text.size() > std::string{}.max_size() / 3)
        throw std::length_error("UTF-16 input is too large");

    const std::string_view bytes(reinterpret_cast<const char*>(text.data()),
                                 text.size() * sizeof(char16_t));
    return convert_encoding<char>(bytes, text.size() * 3, "UTF-16LE", "UTF-8",
                                  "Invalid UTF-16 provided");
}

#endif

} // namespace s3cmd
