#pragma once

#include "fsplugin.h"

#include <format>
#include <string>
#include <string_view>

namespace std::filesystem {

class path;
} // namespace std::filesystem

namespace s3cmd {

// Returns a path to config directory of the plugin
const std::filesystem::path& config_directory_path();

class PluginHost
{
public:
    PluginHost(int plugin_number,
               tProgressProcW progress_proc,
               tLogProcW log_proc,
               tRequestProcW request_proc);

    bool notify_progress(const wchar_t* source, const wchar_t* target, int percent);
    void notify_log(const wchar_t* message);

	enum class message_box_type : int
	{
		other,
		user_name,
		password,
		account,
		user_name_firewall,
		password_firewall,
		target_dir,
		url,
		msg_ok,
		msg_yes_no,
		msg_ok_cancel
	};

    bool is_notify_message_box_available() const { return request_proc_ != nullptr; }

    bool notify_message_box(message_box_type type,
                            const wchar_t* title,
                            const wchar_t* text);

    template <typename... Args>
    bool notify_message_box(message_box_type type,
                            const wchar_t* title,
                            std::wstring_view format_str,
                            const Args&... args)
    {
        return vnotify_message_box(type, title, format_str,
                                   std::make_wformat_args(args...));
    }

    bool notify_message_box_result(message_box_type type,
                                   const wchar_t* title,
                                   const wchar_t* text,
                                   std::wstring& out);

private:
    // Calls notify_message_box with formatter string as text
    bool vnotify_message_box(message_box_type type,
                             const wchar_t* title,
                             std::wstring_view format_str,
                             std::wformat_args args);

private:
    int plugin_number_;
    tProgressProcW progress_proc_;
    tLogProcW log_proc_;
    tRequestProcW request_proc_;
};

struct RemotePathView
{
    std::wstring_view profile;
    std::wstring_view bucket;
    std::wstring_view key;

    static RemotePathView make(std::wstring_view path) noexcept;

    bool operator==(const RemotePathView&) const = default;
};

struct RemotePath
{
    std::string profile;
    std::string bucket;
    std::string key;

    // Splits totalcmd's path (i.e. \\{{profile}}\{{bucket}}\{{key}}) into profile, bucket and key
    static RemotePath make(std::wstring_view path);

    // Returns key as an S3 directory prefix, adding a trailing '/' when needed.
    // It assumes `key` refers to a directory, thus this function should only be called for S3
    // directory operations
    std::string directory_prefix() const;

    bool operator==(const RemotePath&) const = default;
};

std::wstring to_wide(std::string_view text);
std::string to_utf8(std::wstring_view text);

} // namespace s3cmd
