#include "log.hpp"
#include "core.hpp"

#include <Windows.h>

namespace s3cmd {

void vlog(std::string_view format_str, std::format_args args)
{
    std::string buf;
    std::vformat_to(std::back_inserter(buf), format_str, args);
    buf.push_back('\n');
    OutputDebugStringW(to_wide(buf).c_str());
}

} // namespace s3cmd
