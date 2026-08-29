#pragma once

#include <format>
#include <string_view>

namespace s3cmd {

void vlog(std::string_view format_str, std::format_args args);

template <typename... Args>
void log(std::string_view format_str, const Args&... args)
{
    return vlog(format_str, std::make_format_args(args...));
}

} // namespace s3cmd
