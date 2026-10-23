#include "bucket_creation_dialog.hpp"
#include "resource.h"
#include "utils.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <stdexcept>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace s3cmd {

namespace {

constexpr const wchar_t* regions[] = {
    L"af-south-1",     L"ap-east-1",      L"ap-east-2",      L"ap-northeast-1", L"ap-northeast-2",
    L"ap-northeast-3", L"ap-south-1",     L"ap-south-2",     L"ap-southeast-1", L"ap-southeast-2",
    L"ap-southeast-3", L"ap-southeast-4", L"ap-southeast-5", L"ap-southeast-6", L"ap-southeast-7",
    L"ca-central-1",   L"ca-west-1",      L"cn-north-1",     L"cn-northwest-1", L"eu-central-1",
    L"eu-central-2",   L"eu-north-1",     L"eu-south-1",     L"eu-south-2",     L"eu-west-1",
    L"eu-west-2",      L"eu-west-3",      L"eusc-de-east-1", L"il-central-1",   L"me-central-1",
    L"me-south-1",     L"mx-central-1",   L"sa-east-1",      L"us-east-1",      L"us-east-2",
    L"us-gov-east-1",  L"us-gov-west-1",  L"us-west-1",      L"us-west-2"};

std::string build_bucket_name(std::string_view prefix, const BucketCreationOptions& options)
{
    return !options.account_regional
               ? std::string(prefix)
               // For example: mybucket-123456789012-us-east-1-an
               // Other variant: mybucket--123456789012--us-east-1--an
               : std::format("{}-{}-{}-an", prefix, options.account_id, options.region);
}

std::string validate_bucket_name(std::string_view prefix, const BucketCreationOptions& options)
{
    if (options.region.empty() || options.region.size() > 63 ||
        options.region.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") !=
            options.region.npos)
    {
        return "Enter an AWS region code, such as eu-central-1.";
    }

    if (options.account_regional)
    {
        if (options.account_id.size() != 12 ||
            options.account_id.find_first_not_of("0123456789") != options.account_id.npos)
        {
            return "The AWS account ID is unavailable.";
        }

        if (options.region == "me-south-1" || options.region == "me-central-1")
            return "Account regional buckets are unavailable in this region.";
    }

    const auto name = build_bucket_name(prefix, options);
    if (prefix.empty() || name.size() < 3 || name.size() > 63)
        return "The full bucket name must contain 3 to 63 characters.";

    const auto alphanumeric = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    if (!alphanumeric(prefix.front()) || !alphanumeric(prefix.back()) ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") != name.npos ||
        name.find("..") != name.npos)
    {
        return "Use lowercase letters, digits, dots, or hyphens. Begin and end with a letter or digit.";
    }

    for (const auto reserved : {"xn--", "sthree-", "amzn-s3-demo-"})
    {
        if (name.starts_with(reserved))
            return "This bucket name uses a reserved prefix.";
    }

    for (const auto reserved : {"-s3alias", "--ol-s3", ".mrap", "--x-s3", "--table-s3", "--vector-s3"})
    {
        if (name.ends_with(reserved))
            return "This bucket name uses a reserved suffix.";
    }

    if (!options.account_regional && name.ends_with("-an"))
        return "The -an suffix requires the Account regional namespace.";

    if (std::count(name.begin(), name.end(), '.') == 3 &&
        name.find_first_not_of("0123456789.") == name.npos)
    {
        return "The bucket name cannot be an IP address.";
    }

    return {};
}

} // namespace

BucketCreationDialog::BucketCreationDialog(
    std::string_view prefix,
    std::string_view region,
    AccountIdLookupFn lookup_account)
    : prefix_(prefix),
      options_{{}, {}, std::string(region), false, {}},
      lookup_account_(std::move(lookup_account))
{
}

void BucketCreationDialog::update(HWND dialog)
{
    const auto prefix_control = GetDlgItem(dialog, IDC_BUCKET_PREFIX);
    std::wstring prefix(GetWindowTextLengthW(prefix_control) + 1, L'\0');
    GetWindowTextW(prefix_control, prefix.data(), static_cast<int>(prefix.size()));
    prefix_ = to_utf8(prefix.c_str());
    std::array<wchar_t, 65> region{};
    GetDlgItemTextW(dialog, IDC_BUCKET_REGION, region.data(), static_cast<int>(region.size()));
    options_.region = to_utf8(region.data());
    options_.account_regional = SendDlgItemMessageW(dialog, IDC_BUCKET_NAMESPACE, CB_GETCURSEL, 0, 0) == 1;
    auto name = options_.account_regional && options_.account_id.empty() ?
        std::string("Account ID unavailable") : build_bucket_name(prefix_, options_);
    SetDlgItemTextW(dialog, IDC_BUCKET_PREVIEW, to_wide(name).c_str());
    auto error = options_.account_regional && !lookup_error_.empty()
                     ? lookup_error_
                     : validate_bucket_name(prefix_, options_);
    SetDlgItemTextW(dialog, IDC_BUCKET_ERROR, to_wide(error).c_str());
    EnableWindow(GetDlgItem(dialog, IDOK), error.empty());

    options_.validation_error = std::move(error);
    options_.bucket_name = std::move(name);
}

void BucketCreationDialog::lookup_account(HWND dialog)
{
    if (!options_.account_regional || !options_.account_id.empty())
        return;

    lookup_error_.clear();
    SetDlgItemTextW(dialog, IDC_BUCKET_ERROR, L"Looking up the AWS account ID...");
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
        SetDlgItemTextW(dialog, IDC_BUCKET_PREFIX, to_wide(state->prefix_).c_str());
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SendDlgItemMessageW(dialog, IDC_BUCKET_NAMESPACE, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(L"Global"));
        SendDlgItemMessageW(dialog, IDC_BUCKET_NAMESPACE, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(L"Account regional"));
        SendDlgItemMessageW(dialog, IDC_BUCKET_NAMESPACE, CB_SETCURSEL, 0, 0);
        for (const auto region : regions)
        {
            SendDlgItemMessageW(dialog, IDC_BUCKET_REGION, CB_ADDSTRING, 0,
                                reinterpret_cast<LPARAM>(region));
        }
        SendDlgItemMessageW(dialog, IDC_BUCKET_REGION, CB_LIMITTEXT, 63, 0);
        SetDlgItemTextW(dialog, IDC_BUCKET_REGION, to_wide(state->options_.region).c_str());
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
                std::array<wchar_t, 65> region{};
                SendDlgItemMessageW(dialog, IDC_BUCKET_REGION, CB_GETLBTEXT, selected,
                                    reinterpret_cast<LPARAM>(region.data()));
                SetDlgItemTextW(dialog, IDC_BUCKET_REGION, region.data());
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
