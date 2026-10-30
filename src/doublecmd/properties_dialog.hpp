#pragma once

#include "doublecmd/extension.hpp"
#include "properties.hpp"

#include <atomic>
#include <exception>
#include <functional>
#include <future>
#include <string>

namespace s3cmd {

class PropertiesDialog
{
public:
    using PrepareFn = std::function<void(S3Properties&)>;
    using LoadFn = std::function<S3Properties(S3Properties, const std::atomic<bool>&)>;
    using PresignFn = std::function<std::string(unsigned)>;

    PropertiesDialog(S3Properties properties, PrepareFn prepare, LoadFn load, PresignFn presign);
    void show(HWND owner);

private:
    static std::intptr_t dialog_proc(std::uintptr_t dialog, const char* item, std::intptr_t message,
                                     std::intptr_t wparam, std::intptr_t lparam) noexcept;
    void set_text(std::uintptr_t dialog, const char* item, const std::string& value);
    void update(std::uintptr_t dialog);
    void start_loading(std::uintptr_t dialog);
    void poll(std::uintptr_t dialog);
    void generate(std::uintptr_t dialog);
    void close(std::uintptr_t dialog);
    void finish_work();

    S3Properties properties_;
    PrepareFn prepare_;
    LoadFn load_;
    PresignFn presign_;
    dc::DialogApi api_;
    std::atomic<bool> cancelled_{};
    std::future<S3Properties> loading_;
    std::future<std::string> signing_;
    std::exception_ptr failure_;
    bool initialized_{};
    bool started_{};
    bool preparing_{};
    bool prepared_{};
    bool closing_{};
    bool host_closed_{};
};

} // namespace s3cmd
