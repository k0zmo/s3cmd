#pragma once

#include <string_view>

namespace s3cmd {

#ifdef _WIN32

static_assert(sizeof(wchar_t) == sizeof(char16_t));
static_assert(alignof(wchar_t) == alignof(char16_t));

inline const wchar_t* win32_string(const char16_t* text)
{
    return reinterpret_cast<const wchar_t*>(text);
}

inline wchar_t* win32_string(char16_t* text)
{
    return reinterpret_cast<wchar_t*>(text);
}

inline const char16_t* utf16_string(const wchar_t* text)
{
    return reinterpret_cast<const char16_t*>(text);
}
#endif

inline constexpr const std::string_view plugin_name = "S3";
inline constexpr const char16_t* title = u"s3cmd";

} // namespace s3cmd
