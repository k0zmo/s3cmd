#pragma once

#include <charconv>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace s3cmd {

int caseicmp(const wchar_t* lhs, const wchar_t* rhs);
int ncaseicmp(const wchar_t* lhs, const wchar_t* rhs, std::size_t count);

std::wstring to_wide(std::string_view text);
std::string to_utf8(std::wstring_view text);

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
