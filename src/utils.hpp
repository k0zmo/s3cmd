#pragma once

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace s3cmd {

int caseicmp(const char16_t* lhs, const char16_t* rhs);
int ncaseicmp(const char16_t* lhs, const char16_t* rhs, std::size_t count);

std::u16string to_utf16(std::string_view text);
std::string to_utf8(std::u16string_view text);

template <typename T>
std::optional<T> parse_number(std::string_view text)
{
    if (text.empty())
        return std::nullopt;
    T value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        return std::nullopt;
    return value;
}

} // namespace s3cmd
