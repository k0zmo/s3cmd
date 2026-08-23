#pragma once

#include <string>

namespace s3cmd {

std::wstring utf8_to_wide(std::string_view text);
std::string wide_to_utf8(std::wstring_view text);

} // namespace s3cmd
