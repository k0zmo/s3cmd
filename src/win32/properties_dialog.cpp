#include "win32/properties_dialog.hpp"
#include "win32/resource.h"
#include "platform.hpp"
#include "utils.hpp"

#include <commctrl.h>

#include <chrono>
#include <cstring>
#include <stdexcept>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace s3cmd {

namespace {
constexpr UINT start_loading = WM_APP + 1;
constexpr unsigned expirations[] = {900, 3600, 86400};

void set_text(HWND dialog, int control, const std::string& text)
{
    SetDlgItemTextW(dialog, control, win32_string(to_utf16(text).c_str()));
}
} // namespace

PropertiesDialog::PropertiesDialog(S3Properties properties, PrepareFn prepare, LoadFn load,
                                   PresignFn presign)
    : properties_(std::move(properties)),
      prepare_(std::move(prepare)),
      load_(std::move(load)),
      presign_(std::move(presign))
{
}

void PropertiesDialog::update(HWND dialog)
{
    const bool prefix = properties_.kind == PropertiesKind::prefix;
    const bool sharing = properties_.kind == PropertiesKind::object;
    for (const auto control :
         {IDC_PROPERTIES_TAG_GROUP, IDC_PROPERTIES_TAGS, IDC_PROPERTIES_TAG_STATUS})
    {
        ShowWindow(GetDlgItem(dialog, control), prefix ? SW_HIDE : SW_SHOW);
    }
    for (const auto control :
         {IDC_PROPERTIES_LINK_GROUP, IDC_PROPERTIES_EXPIRY_LABEL, IDC_PROPERTIES_EXPIRY,
          IDC_PROPERTIES_GENERATE, IDC_PROPERTIES_URL, IDC_PROPERTIES_COPY_URL,
          IDC_PROPERTIES_LINK_STATUS, IDC_PROPERTIES_WARNING})
    {
        ShowWindow(GetDlgItem(dialog, control), sharing ? SW_SHOW : SW_HIDE);
    }

    RECT bounds{0, 0, 410, prefix ? 130 : sharing ? 318 : 218};
    MapDialogRect(dialog, &bounds);
    RECT close_bounds{347, (prefix ? 130 : sharing ? 318 : 218) - 21, 403, 0};
    MapDialogRect(dialog, &close_bounds);
    SetWindowPos(GetDlgItem(dialog, IDCANCEL), nullptr, close_bounds.left, close_bounds.top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    AdjustWindowRectEx(&bounds, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)), FALSE,
                       static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)));
    SetWindowPos(dialog, nullptr, 0, 0, bounds.right - bounds.left, bounds.bottom - bounds.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

    set_text(dialog, IDC_PROPERTIES_ADDRESS, properties_.address);
    set_text(dialog, IDC_PROPERTIES_REGION,
             properties_.region.empty() ? "Unknown" : properties_.region);
    set_text(dialog, IDC_PROPERTIES_DETAILS, properties_.details);
    set_text(dialog, IDC_PROPERTIES_TAG_STATUS, properties_.tag_status);
    const auto list = GetDlgItem(dialog, IDC_PROPERTIES_TAGS);
    ListView_DeleteAllItems(list);
    for (const auto& [key, value] : properties_.tags)
    {
        auto wide_key = to_utf16(key);
        auto wide_value = to_utf16(value);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = ListView_GetItemCount(list);
        item.pszText = win32_string(wide_key.data());
        const auto row = SendMessageW(list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&item));
        item.iSubItem = 1;
        item.pszText = win32_string(wide_value.data());
        SendMessageW(list, LVM_SETITEMTEXTW, row, reinterpret_cast<LPARAM>(&item));
    }
    const bool can_sign =
        prepared_ && properties_.kind == PropertiesKind::object && !closing_ && !signing_.valid();
    EnableWindow(GetDlgItem(dialog, IDC_PROPERTIES_GENERATE), can_sign);
    EnableWindow(GetDlgItem(dialog, IDC_PROPERTIES_EXPIRY), can_sign);
}

void PropertiesDialog::copy(HWND dialog, int control)
{
    const auto edit = GetDlgItem(dialog, control);
    const auto length = GetWindowTextLengthW(edit);
    if (!length)
        return;
    std::u16string text(static_cast<std::size_t>(length) + 1, u'\0');
    GetWindowTextW(edit, win32_string(text.data()), static_cast<int>(text.size()));
    const auto bytes = text.size() * sizeof(char16_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory)
        throw std::runtime_error("Cannot allocate clipboard text.");
    const auto buffer = GlobalLock(memory);
    if (!buffer)
    {
        GlobalFree(memory);
        throw std::runtime_error("Cannot access clipboard text.");
    }
    std::memcpy(buffer, text.data(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(dialog))
    {
        GlobalFree(memory);
        throw std::runtime_error("Cannot open the clipboard. Try again.");
    }
    const bool copied = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!copied)
    {
        GlobalFree(memory);
        throw std::runtime_error("Cannot copy text to the clipboard.");
    }
}

void PropertiesDialog::poll(HWND dialog)
{
    using namespace std::chrono_literals;
    if (loading_.valid() && loading_.wait_for(0s) == std::future_status::ready)
    {
        try
        {
            properties_ = loading_.get();
        }
        catch (const std::exception& error)
        {
            properties_.details = error.what();
            properties_.tag_status = "Tags unavailable.";
        }
        update(dialog);
    }
    if (signing_.valid() && signing_.wait_for(0s) == std::future_status::ready)
    {
        try
        {
            const auto url = signing_.get();
            set_text(dialog, IDC_PROPERTIES_URL, url);
            EnableWindow(GetDlgItem(dialog, IDC_PROPERTIES_COPY_URL), !url.empty());
            SetDlgItemTextW(dialog, IDC_PROPERTIES_LINK_STATUS, win32_string(u"Link generated."));
        }
        catch (const std::exception& error)
        {
            set_text(dialog, IDC_PROPERTIES_LINK_STATUS, error.what());
        }
        update(dialog);
    }
    if (closing_ && !loading_.valid() && !signing_.valid())
        EndDialog(dialog, IDCANCEL);
}

void PropertiesDialog::close(HWND dialog)
{
    // Keep the dialog and SDK alive until cancellation finishes, while pumping UI messages.
    closing_ = true;
    cancelled_ = true;
    SetWindowTextW(dialog, win32_string(u"S3 Properties - Closing..."));
    EnableWindow(GetDlgItem(dialog, IDCANCEL), FALSE);
    update(dialog);
    poll(dialog);
}

INT_PTR CALLBACK PropertiesDialog::dialog_proc(HWND dialog, UINT message, WPARAM wparam,
                                               LPARAM lparam)
{
    auto* state = reinterpret_cast<PropertiesDialog*>(GetWindowLongPtrW(dialog, DWLP_USER));
    try
    {
        if (message == WM_INITDIALOG)
        {
            state = reinterpret_cast<PropertiesDialog*>(lparam);
            SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
            const auto list = GetDlgItem(dialog, IDC_PROPERTIES_TAGS);
            ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            RECT bounds{};
            GetClientRect(list, &bounds);
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH;
            column.cx = (bounds.right - bounds.left) / 3;
            column.pszText = win32_string(const_cast<char16_t*>(u"Key"));
            SendMessageW(list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
            column.cx = (bounds.right - bounds.left) * 2 / 3 - GetSystemMetrics(SM_CXVSCROLL);
            column.pszText = win32_string(const_cast<char16_t*>(u"Value"));
            SendMessageW(list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
            for (const auto expiry : {u"15 minutes", u"1 hour", u"24 hours"})
            {
                SendDlgItemMessageW(dialog, IDC_PROPERTIES_EXPIRY, CB_ADDSTRING, 0,
                                    reinterpret_cast<LPARAM>(expiry));
            }
            SendDlgItemMessageW(dialog, IDC_PROPERTIES_EXPIRY, CB_SETCURSEL, 1, 0);
            EnableWindow(GetDlgItem(dialog, IDC_PROPERTIES_COPY_URL), FALSE);
            state->update(dialog);
            if (!SetTimer(dialog, 1, 50, nullptr))
                throw std::runtime_error("Cannot start the properties timer.");
            PostMessageW(dialog, start_loading, 0, 0);
            return TRUE;
        }
        if (!state)
            return FALSE;
        if (message == start_loading && !state->closing_)
        {
            RedrawWindow(dialog, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
            // Credential refresh and SSO prompts must run on the host's UI thread.
            state->prepare_(state->properties_);
            state->prepared_ = true;
            state->update(dialog);
            state->loading_ =
                std::async(std::launch::async, [state, properties = state->properties_]() mutable {
                    return state->load_(std::move(properties), state->cancelled_);
                });
            return TRUE;
        }
        if (message == WM_TIMER && wparam == 1)
        {
            state->poll(dialog);
            return TRUE;
        }
        if (message == WM_CLOSE ||
            (message == WM_COMMAND && (LOWORD(wparam) == IDCANCEL || LOWORD(wparam) == IDOK)))
        {
            state->close(dialog);
            return TRUE;
        }
        if (message == WM_COMMAND)
        {
            switch (LOWORD(wparam))
            {
            case IDC_PROPERTIES_COPY_ADDRESS:
                state->copy(dialog, IDC_PROPERTIES_ADDRESS);
                return TRUE;
            case IDC_PROPERTIES_COPY_URL:
                state->copy(dialog, IDC_PROPERTIES_URL);
                return TRUE;
            case IDC_PROPERTIES_EXPIRY:
                if (HIWORD(wparam) == CBN_SELCHANGE)
                {
                    SetDlgItemTextW(dialog, IDC_PROPERTIES_URL, win32_string(u""));
                    SetDlgItemTextW(dialog, IDC_PROPERTIES_LINK_STATUS, win32_string(u""));
                    EnableWindow(GetDlgItem(dialog, IDC_PROPERTIES_COPY_URL), FALSE);
                }
                return TRUE;
            case IDC_PROPERTIES_GENERATE:
            {
                if (!state->prepared_ || state->properties_.kind != PropertiesKind::object ||
                    state->closing_ || state->signing_.valid())
                    return TRUE;
                const auto selected =
                    SendDlgItemMessageW(dialog, IDC_PROPERTIES_EXPIRY, CB_GETCURSEL, 0, 0);
                if (selected < 0 || selected >= 3)
                    return TRUE;
                SetDlgItemTextW(dialog, IDC_PROPERTIES_URL, win32_string(u""));
                EnableWindow(GetDlgItem(dialog, IDC_PROPERTIES_COPY_URL), FALSE);
                SetDlgItemTextW(dialog, IDC_PROPERTIES_LINK_STATUS, win32_string(u"Generating link..."));
                state->signing_ = std::async(std::launch::async, [state, selected] {
                    return state->presign_(expirations[selected]);
                });
                state->update(dialog);
                return TRUE;
            }
            }
        }
    }
    catch (const std::exception& error)
    {
        set_text(dialog, IDC_PROPERTIES_DETAILS, error.what());
        SetDlgItemTextW(dialog, IDC_PROPERTIES_TAG_STATUS, win32_string(u"Properties unavailable."));
    }
    return FALSE;
}

void PropertiesDialog::show(HWND owner)
{
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    if (!InitCommonControlsEx(&controls))
        throw std::runtime_error("Cannot initialize the properties controls.");

    if (DialogBoxParamW(reinterpret_cast<HINSTANCE>(&__ImageBase), MAKEINTRESOURCEW(IDD_PROPERTIES),
                        owner, dialog_proc, reinterpret_cast<LPARAM>(this)) == -1)
    {
        throw std::runtime_error("Cannot open the Properties dialog.");
    }
}

} // namespace s3cmd
