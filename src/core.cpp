#include "core.hpp"
#include "fsplugin.h"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace s3cmd {

PluginHost::PluginHost(int plugin_number, tProgressProcW progress_proc, tLogProcW log_proc,
                       tRequestProcW request_proc)
    : plugin_number_(plugin_number),
      progress_proc_(progress_proc),
      log_proc_(log_proc),
      request_proc_(request_proc)
{

}

bool PluginHost::notify_progress(const wchar_t* source, const wchar_t* target, int percent_done)
{
    if (!progress_proc_)
        return false;
    return progress_proc_(plugin_number_,
                          const_cast<wchar_t*>(source),
                          const_cast<wchar_t*>(target),
                          percent_done);
}

void PluginHost::notify_log(const wchar_t* message)
{
    if (!log_proc_)
        return;
    log_proc_(plugin_number_, MSGTYPE_IMPORTANTERROR, const_cast<wchar_t*>(message));
}

bool PluginHost::notify_message_box(message_box_type type,
                                    const wchar_t* title,
                                    const wchar_t* text)
{
    if (!request_proc_)
        return false;
    std::array<wchar_t, 1> ignored;
    return request_proc_(plugin_number_,
                         static_cast<int>(type),
                         const_cast<wchar_t*>(title),
                         const_cast<wchar_t*>(text),
                         ignored.data(),
                         static_cast<int>(ignored.size()));
}

bool PluginHost::vnotify_message_box(message_box_type type,
                                    const wchar_t* title,
                                    std::wstring_view format_str,
                                    std::wformat_args args)
{
    std::wstring buf;
    std::vformat_to(std::back_inserter(buf), format_str, args);
    return notify_message_box(type, title, buf.c_str());
}

bool PluginHost::notify_message_box_result(message_box_type type,
                                           const wchar_t* title,
                                           const wchar_t* text,
                                           std::wstring& out)
{
    if (!request_proc_)
        return false;
    return request_proc_(plugin_number_,
                         static_cast<int>(type),
                         const_cast<wchar_t*>(title),
                         const_cast<wchar_t*>(text),
                         out.data(),
                         static_cast<int>(out.size()));
}

RemotePathView RemotePathView::make(std::wstring_view path) noexcept
{
    // Remove \ from the back and the front
    while (!path.empty() && path.front() == L'\\')
        path.remove_prefix(1);
    while (!path.empty() && path.back() == L'\\')
        path.remove_suffix(1);

    // Check for the profile
    const auto profile_end = path.find(L'\\');
    const auto profile = path.substr(0, profile_end);
    if (profile_end == std::wstring_view::npos)
        return {profile, {}, {}};

    // Split the remaining into the bucket and the key
    path.remove_prefix(profile_end + 1);
    const auto bucket_end = path.find(L'\\');
    const auto bucket = path.substr(0, bucket_end);
    const auto key =
        bucket_end == std::wstring_view::npos ? std::wstring_view{} : path.substr(bucket_end + 1);

    return {profile, bucket, key};
}

RemotePath RemotePath::make(std::wstring_view path)
{
    const auto view = RemotePathView::make(path);
    auto key_utf8 = to_utf8(view.key);
    std::ranges::replace(key_utf8, '\\', '/');
    return {to_utf8(view.profile), to_utf8(view.bucket), std::move(key_utf8)};
}

std::string RemotePath::directory_prefix() const
{
    if (key.empty())
        return {};
    return key.back() == '/' ? key : key + '/';
}

} // namespace s3cmd
