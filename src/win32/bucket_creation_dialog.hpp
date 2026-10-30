#pragma once

#include "bucket_creation.hpp"

#include <Windows.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace s3cmd {

class BucketCreationDialog
{
public:
    using AccountIdLookupFn = std::function<std::string(std::string_view)>;

    BucketCreationDialog(std::string_view prefix,
                         std::string_view region,
                         AccountIdLookupFn lookup_account);

    std::optional<BucketCreationOptions> show();

private:
    static INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam);

    void update(HWND dialog);
    void lookup_account(HWND dialog);

    std::string prefix_;
    BucketCreationOptions options_;
    AccountIdLookupFn lookup_account_;
    std::string lookup_error_;
};

} // namespace s3cmd
