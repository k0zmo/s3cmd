#include "log.hpp"

#ifdef _WIN32
#  include "utils.hpp"
#  include "platform.hpp"
#  include <Windows.h>
#else
#  include <cstdio>
#endif

#include <iterator>
#include <string>
#include <string_view>

namespace s3cmd {

void vlog(std::string_view format_str, fmt::format_args args)
{
    std::string buf;
    fmt::vformat_to(std::back_inserter(buf), format_str, args);
    buf.push_back('\n');
#ifdef _WIN32
    OutputDebugStringW(win32_string(to_utf16(buf).c_str()));
#else
    std::fwrite(buf.data(), 1, buf.size(), stderr);
#endif
}

} // namespace s3cmd
