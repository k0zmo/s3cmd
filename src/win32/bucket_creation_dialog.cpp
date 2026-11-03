#include "win32/bucket_creation_dialog.hpp"
#include "win32/resource.h"
#include "platform.hpp"
#include "utils.hpp"

#include <array>
#include <stdexcept>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace s3cmd {

BucketCreationDialog::BucketCreationDialog(
    std::string_view prefix,
    std::string_view region,
    AccountIdLookupFn lookup_account)
    : prefix_(prefix),
      options_{{}, std::string(region), false, {}},
      lookup_account_(std::move(lookup_account))
{
}

void BucketCreationDialog::update(HWND dialog)
{
    const auto prefix_control = GetDlgItem(dialog, IDC_BUCKET_PREFIX);
    std::u16string prefix(GetWindowTextLengthW(prefix_control) + 1, u'\0');
    GetWindowTextW(prefix_control, win32_string(prefix.data()), static_cast<int>(prefix.size()));
    prefix_ = to_utf8(prefix.c_str());
    std::array<char16_t, 65> region{};
    GetDlgItemTextW(dialog, IDC_BUCKET_REGION, win32_string(region.data()), static_cast<int>(region.size()));
    options_.region = to_utf8(region.data());
    options_.account_regional = SendDlgItemMessageW(dialog, IDC_BUCKET_NAMESPACE, CB_GETCURSEL, 0, 0) == 1;
    auto name = options_.account_regional && options_.account_id.empty() ?
        std::string("Account ID unavailable") : build_bucket_name(prefix_, options_);
    SetDlgItemTextW(dialog, IDC_BUCKET_PREVIEW, win32_string(to_utf16(name).c_str()));
    const auto error = options_.account_regional && !lookup_error_.empty()
                           ? lookup_error_
                           : validate_bucket_name(prefix_, options_);
    SetDlgItemTextW(dialog, IDC_BUCKET_ERROR, win32_string(to_utf16(error).c_str()));
    EnableWindow(GetDlgItem(dialog, IDOK), error.empty());

    options_.bucket_name = std::move(name);
}

void BucketCreationDialog::lookup_account(HWND dialog)
{
    if (!options_.account_regional || !options_.account_id.empty())
        return;

    lookup_error_.clear();
    SetDlgItemTextW(dialog, IDC_BUCKET_ERROR, win32_string(u"Looking up the AWS account ID..."));
    UpdateWindow(dialog);
    const auto cursor = SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_WAIT)));
    try
    {
        options_.account_id = lookup_account_(options_.region);
    }
    catch (const std::exception& error)
    {
        lookup_error_ = error.what();
    }
    SetCursor(cursor);
}

INT_PTR CALLBACK BucketCreationDialog::dialog_proc(HWND dialog, UINT message, WPARAM wparam,
                                                   LPARAM lparam)
{
    auto* state = reinterpret_cast<BucketCreationDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG)
    {
        state = reinterpret_cast<BucketCreationDialog*>(lparam);
        SetDlgItemTextW(dialog, IDC_BUCKET_PREFIX, win32_string(to_utf16(state->prefix_).c_str()));
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SendDlgItemMessageW(dialog, IDC_BUCKET_NAMESPACE, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(u"Global"));
        SendDlgItemMessageW(dialog, IDC_BUCKET_NAMESPACE, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(u"Account regional"));
        SendDlgItemMessageW(dialog, IDC_BUCKET_NAMESPACE, CB_SETCURSEL, 0, 0);
        for (const auto region : bucket_regions)
        {
            SendDlgItemMessageW(dialog, IDC_BUCKET_REGION, CB_ADDSTRING, 0,
                                reinterpret_cast<LPARAM>(to_utf16(region).c_str()));
        }
        SendDlgItemMessageW(dialog, IDC_BUCKET_REGION, CB_LIMITTEXT, 63, 0);
        SetDlgItemTextW(dialog, IDC_BUCKET_REGION, win32_string(to_utf16(state->options_.region).c_str()));
        state->update(dialog);
        return TRUE;
    }

    if (!state || message != WM_COMMAND)
        return FALSE;

    switch (LOWORD(wparam))
    {
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;

    case IDOK:
        state->update(dialog);
        if (IsWindowEnabled(GetDlgItem(dialog, IDOK)))
            EndDialog(dialog, IDOK);
        return TRUE;

    case IDC_BUCKET_PREFIX:
        if (HIWORD(wparam) == EN_CHANGE)
            state->update(dialog);
        return TRUE;

    case IDC_BUCKET_NAMESPACE:
        if (HIWORD(wparam) == CBN_SELCHANGE)
        {
            state->update(dialog);
            state->lookup_account(dialog);
            state->update(dialog);
        }
        return TRUE;

    case IDC_BUCKET_REGION:
        if (HIWORD(wparam) == CBN_SELCHANGE)
        {
            const auto selected =
                SendDlgItemMessageW(dialog, IDC_BUCKET_REGION, CB_GETCURSEL, 0, 0);
            if (selected != CB_ERR)
            {
                std::array<char16_t, 65> region{};
                SendDlgItemMessageW(dialog, IDC_BUCKET_REGION, CB_GETLBTEXT, selected,
                                    reinterpret_cast<LPARAM>(region.data()));
                SetDlgItemTextW(dialog, IDC_BUCKET_REGION, win32_string(region.data()));
            }
            state->update(dialog);
        }
        else if (HIWORD(wparam) == CBN_EDITCHANGE)
        {
            state->update(dialog);
        }
        else if (HIWORD(wparam) == CBN_KILLFOCUS)
        {
            state->update(dialog);
            state->lookup_account(dialog);
            state->update(dialog);
        }
        return TRUE;

    default:
        return FALSE;
    }
}

std::optional<BucketCreationOptions> BucketCreationDialog::show()
{
    const auto result = DialogBoxParamW(reinterpret_cast<HINSTANCE>(&__ImageBase),
                                        MAKEINTRESOURCEW(IDD_BUCKET_CREATION), GetActiveWindow(),
                                        dialog_proc, reinterpret_cast<LPARAM>(this));
    if (result == -1)
        throw std::runtime_error("Cannot open the Create bucket dialog.");
    if (result != IDOK)
        return std::nullopt;
    return options_;
}

} // namespace s3cmd
