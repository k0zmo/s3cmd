#pragma once

#include "bucket_creation.hpp"
#include "doublecmd/extension.hpp"

#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace s3cmd {

class BucketCreationDialog
{
public:
    using AccountIdLookupFn = std::function<std::string(std::string_view)>;

    BucketCreationDialog(std::string_view prefix, std::string_view region,
                         AccountIdLookupFn lookup_account);
    std::optional<BucketCreationOptions> show();

private:
    static std::intptr_t dialog_proc(std::uintptr_t dialog, const char* item, std::intptr_t message,
                                     std::intptr_t wparam, std::intptr_t lparam) noexcept;
    std::string text(std::uintptr_t dialog, const char* item);
    void set_text(std::uintptr_t dialog, const char* item, const std::string& value);
    bool update(std::uintptr_t dialog);
    void lookup_account(std::uintptr_t dialog);

    std::string prefix_;
    BucketCreationOptions options_;
    AccountIdLookupFn lookup_account_;
    std::string lookup_error_;
    dc::DialogApi api_;
    std::exception_ptr failure_;
    bool initialized_{};
    bool accepted_{};
    bool looking_up_{};
};

} // namespace s3cmd
