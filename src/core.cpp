#include "core.hpp"
#include "fsplugin.h"
#include "utils.hpp"

#include <algorithm>
#include <array>
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

bool PluginHost::notify_progress(const char16_t* source, const char16_t* target, int percent_done)
{
    if (!progress_proc_)
        return false;
    return progress_proc_(plugin_number_,
                          const_cast<char16_t*>(source),
                          const_cast<char16_t*>(target),
                          percent_done);
}

void PluginHost::notify_log(const char16_t* message)
{
    if (!log_proc_)
        return;
    log_proc_(plugin_number_, MSGTYPE_IMPORTANTERROR, const_cast<char16_t*>(message));
}

bool PluginHost::notify_message_box(message_box_type type,
                                    const char16_t* title,
                                    const char16_t* text)
{
    if (!request_proc_)
        return false;
    std::array<char16_t, 1> ignored;
    return request_proc_(plugin_number_,
                         static_cast<int>(type),
                         const_cast<char16_t*>(title),
                         const_cast<char16_t*>(text),
                         ignored.data(),
                         static_cast<int>(ignored.size()));
}

bool PluginHost::notify_message_box_result(message_box_type type,
                                           const char16_t* title,
                                           const char16_t* text,
                                           std::u16string& out)
{
    if (!request_proc_)
        return false;
    return request_proc_(plugin_number_,
                         static_cast<int>(type),
                         const_cast<char16_t*>(title),
                         const_cast<char16_t*>(text),
                         out.data(),
                         static_cast<int>(out.size()));
}

RemotePathView RemotePathView::make(std::u16string_view path) noexcept
{
    constexpr std::u16string_view separators = u"\\/";

    // Strip the root separator and any trailing directory separators.
    while (!path.empty() && separators.find(path.front()) != separators.npos)
        path.remove_prefix(1);
    while (!path.empty() && separators.find(path.back()) != separators.npos)
        path.remove_suffix(1);

    // Check for the profile
    const auto profile_end = path.find_first_of(separators);
    const auto profile = path.substr(0, profile_end);
    if (profile_end == std::u16string_view::npos)
        return {profile, {}, {}};

    // Split the remaining into the bucket and the key
    path.remove_prefix(profile_end + 1);
    const auto bucket_end = path.find_first_of(separators);
    const auto bucket = path.substr(0, bucket_end);
    const auto key =
        bucket_end == std::u16string_view::npos ? std::u16string_view{} : path.substr(bucket_end + 1);

    return {profile, bucket, key};
}

RemotePath RemotePath::make(std::u16string_view path)
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
