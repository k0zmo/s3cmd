#include "log.hpp"
#include "utils.hpp"

#include <Windows.h>

#include <iterator>
#include <string>
#include <string_view>

namespace s3cmd {

void vlog(std::string_view format_str, fmt::format_args args)
{
    std::string buf;
    fmt::vformat_to(std::back_inserter(buf), format_str, args);
    buf.push_back('\n');
    OutputDebugStringW(win32_string(to_utf16(buf).c_str()));
}

} // namespace s3cmd
