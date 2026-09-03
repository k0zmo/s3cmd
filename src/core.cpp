#include "core.hpp"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace s3cmd {

const std::filesystem::path& config_directory_path()
{
    static std::filesystem::path value = [] {
#ifdef _WIN32
        wchar_t* app_data{};
        std::size_t size{};
        // _wdupenv_s allocates a correctly sized UTF-16 copy.
        if (_wdupenv_s(&app_data, &size, L"APPDATA") != 0 || !app_data || !*app_data)
        {
            std::free(app_data);
            throw std::runtime_error("APPDATA is not set");
        }
        std::unique_ptr<wchar_t, decltype(&std::free)> releaser(app_data, &std::free);
        return std::filesystem::path(releaser.get()) / L"s3cmd";
#else
        if (const auto* config_home = std::getenv("XDG_CONFIG_HOME"); config_home && *config_home)
            return std::filesystem::path(config_home) / "s3cmd";
        if (const auto* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".config" / "s3cmd";
        throw std::runtime_error("XDG_CONFIG_HOME and HOME are not set");
#endif
    }();
    return value;
}

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

std::wstring to_wide(std::string_view text)
{
    if (text.empty())
        return {};

    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
    if (size == 0)
        throw std::runtime_error("Invalid UTF-8 provided");

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size) == 0)
    {
        throw std::runtime_error("Invalid UTF-8 provided");
    }
    return result;
}

std::string to_utf8(std::wstring_view text)
{
    if (text.empty())
        return {};

    const auto size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0)
        throw std::runtime_error("Invalid UTF-16 provided");

    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                            static_cast<int>(text.size()), result.data(), size, nullptr,
                            nullptr) == 0)
    {
        throw std::runtime_error("Invalid UTF-16 provided");
    }
    return result;
}

} // namespace s3cmd
