#pragma once

#include <Windows.h>

#include <atomic>
#include <functional>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace s3cmd {

enum class PropertiesKind { unknown, bucket, object, prefix };

struct S3Properties
{
    PropertiesKind kind{PropertiesKind::unknown};
    std::string address;
    std::string region;
    std::string details{"Loading properties..."};
    std::vector<std::pair<std::string, std::string>> tags;
    std::string tag_status{"Loading tags..."};
};

class PropertiesDialog
{
public:
    using PrepareFn = std::function<void()>;
    using LoadFn = std::function<S3Properties(const std::atomic<bool>&)>;
    using PresignFn = std::function<std::string(unsigned)>;

    PropertiesDialog(S3Properties properties, PrepareFn prepare, LoadFn load, PresignFn presign);
    void show(HWND owner);

private:
    static INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam);
    void update(HWND dialog);
    void poll(HWND dialog);
    void close(HWND dialog);
    void copy(HWND dialog, int control);

    S3Properties properties_;
    PrepareFn prepare_;
    LoadFn load_;
    PresignFn presign_;
    std::atomic<bool> cancelled_{};
    std::future<S3Properties> loading_;
    std::future<std::string> signing_;
    bool prepared_{};
    bool closing_{};
};

} // namespace s3cmd
