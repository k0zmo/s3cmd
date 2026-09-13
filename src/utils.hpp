#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace s3cmd {

int caseicmp(const wchar_t* lhs, const wchar_t* rhs);
int ncaseicmp(const wchar_t* lhs, const wchar_t* rhs, std::size_t count);

std::wstring to_wide(std::string_view text);
std::string to_utf8(std::wstring_view text);

} // namespace s3cmd
