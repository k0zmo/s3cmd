#pragma once

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

#else

#include "fsplugin.h"

inline constexpr DWORD ERROR_SUCCESS = 0;
inline constexpr DWORD ERROR_NOT_SUPPORTED = 50;
inline constexpr DWORD ERROR_NO_MORE_FILES = 18;
inline constexpr DWORD ERROR_CANCELLED = 1223;
inline constexpr DWORD ERROR_LOGON_FAILURE = 1326;
inline constexpr DWORD ERROR_PATH_NOT_FOUND = 3;

inline constexpr BOOL FALSE = 0;
inline constexpr BOOL TRUE = 1;

inline const HANDLE INVALID_HANDLE_VALUE = reinterpret_cast<HANDLE>(-1);

namespace s3cmd::detail {
inline thread_local DWORD last_error = ERROR_SUCCESS;
}

inline void SetLastError(DWORD error) noexcept
{
    s3cmd::detail::last_error = error;
}

inline DWORD GetLastError() noexcept
{
    return s3cmd::detail::last_error;
}

#endif