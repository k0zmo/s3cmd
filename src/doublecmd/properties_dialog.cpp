#include "doublecmd/properties_dialog.hpp"
#include "properties_lfm.hpp"

#include <chrono>
#include <iterator>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace s3cmd {
namespace {

constexpr unsigned expirations[] = {900, 3600, 86400};

} // namespace

PropertiesDialog::PropertiesDialog(S3Properties properties, PrepareFn prepare, LoadFn load,
                                    PresignFn presign)
    : properties_(std::move(properties)),
      prepare_(std::move(prepare)),
      load_(std::move(load)),
      presign_(std::move(presign))
{
}

void PropertiesDialog::set_text(std::uintptr_t dialog, const char* item, const std::string& value)
{
    api_.send_message(dialog, item, dc::dm_set_text,
                      reinterpret_cast<std::intptr_t>(value.c_str()), 0);
}

void PropertiesDialog::update(std::uintptr_t dialog)
{
    api_.send_message(dialog, "TagsGroup", dc::dm_show_item,
                       properties_.kind != PropertiesKind::prefix, 0);
    api_.send_message(dialog, "ShareGroup", dc::dm_show_item,
                       properties_.kind == PropertiesKind::object, 0);
    set_text(dialog, "PropertiesAddress", properties_.address);
    set_text(dialog, "PropertiesRegion", properties_.region.empty() ? "Unknown" : properties_.region);
    set_text(dialog, "PropertiesDetails", properties_.details);
    set_text(dialog, "TagStatus", properties_.tag_status);
    std::string tags;
    for (const auto& [key, value] : properties_.tags)
        tags += key + " = " + value + '\n';
    set_text(dialog, "PropertiesTags", tags);
    const bool can_sign = prepared_ && properties_.kind == PropertiesKind::object &&
                          !closing_ && !signing_.valid();
    api_.send_message(dialog, "GenerateButton", dc::dm_enable, can_sign, 0);
    api_.send_message(dialog, "LinkExpiry", dc::dm_enable, can_sign, 0);
}

void PropertiesDialog::start_loading(std::uintptr_t dialog)
{
    started_ = true;
    // Prefixes have no independent metadata or tags. Their initial snapshot is complete.
    if (properties_.kind == PropertiesKind::prefix)
        return;

    preparing_ = true;
    try
    {
        // Credential refresh and SSO prompts must run on the host's UI thread.
        prepare_(properties_);
        prepared_ = true;
        if (!closing_ && !host_closed_)
        {
            loading_ = std::async(std::launch::async,
                [this, properties = properties_]() mutable {
                    return load_(std::move(properties), cancelled_);
                });
        }
    }
    catch (const std::exception& error)
    {
        properties_.details = error.what();
        properties_.tag_status = "Properties unavailable.";
    }
    preparing_ = false;
    if (!host_closed_)
        update(dialog);
}

void PropertiesDialog::poll(std::uintptr_t dialog)
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
            set_text(dialog, "PropertiesUrl", url);
            set_text(dialog, "LinkStatus", "Link generated.");
        }
        catch (const std::exception& error)
        {
            set_text(dialog, "LinkStatus", error.what());
        }
        update(dialog);
    }
    if (closing_ && !preparing_ && !loading_.valid() && !signing_.valid())
    {
        api_.send_message(dialog, "PropertiesTimer", dc::dm_enable, 0, 0);
        api_.send_message(dialog, nullptr, dc::dm_close, dc::id_cancel, 0);
    }
}

void PropertiesDialog::generate(std::uintptr_t dialog)
{
    if (!prepared_ || properties_.kind != PropertiesKind::object || closing_ || signing_.valid())
        return;
    const auto selected = api_.send_message(dialog, "LinkExpiry", dc::dm_list_get_index, 0, 0);
    if (selected < 0 || static_cast<std::size_t>(selected) >= std::size(expirations))
        return;
    set_text(dialog, "PropertiesUrl", "");
    set_text(dialog, "LinkStatus", "Generating link...");
    signing_ = std::async(std::launch::async, [this, selected] {
        return presign_(expirations[selected]);
    });
    update(dialog);
}

void PropertiesDialog::close(std::uintptr_t dialog)
{
    closing_ = true;
    cancelled_ = true;
    set_text(dialog, nullptr, "S3 Properties - Closing...");
    api_.send_message(dialog, "CloseButton", dc::dm_enable, 0, 0);
    update(dialog);
    poll(dialog);
}

std::intptr_t DCPCALL PropertiesDialog::dialog_proc(
    std::uintptr_t dialog, const char* item, std::intptr_t message,
    std::intptr_t wparam, std::intptr_t) noexcept
{
    PropertiesDialog* state{};
    try
    {
        const auto api = dc::dialog_api();
        if (!api.send_message)
            return 0;
        state = reinterpret_cast<PropertiesDialog*>(
            api.send_message(dialog, nullptr, dc::dm_get_dialog_data, 0, 0));
        if (!state)
            return 0;
        if (message == dc::dn_close)
        {
            state->host_closed_ = true;
            state->cancelled_ = true;
            return 0;
        }
        if (state->host_closed_)
            return 0;

        const std::string_view name = item ? item : "";
        if (message == dc::dn_init_dialog)
        {
            state->api_.send_message(dialog, "LinkExpiry", dc::dm_list_set_index, 1, 0);
            state->update(dialog);
            state->initialized_ = true;
        }
        else if (state->initialized_)
        {
            if (message == dc::dn_timer && name == "PropertiesTimer")
            {
                if (!state->started_ && !state->closing_)
                    state->start_loading(dialog);
                if (!state->host_closed_)
                    state->poll(dialog);
            }
            else if (message == dc::dn_click && name == "CloseButton")
                state->close(dialog);
            else if (message == dc::dn_key_down && wparam &&
                     *reinterpret_cast<std::uint16_t*>(wparam) == 27)
            {
                *reinterpret_cast<std::uint16_t*>(wparam) = 0;
                state->close(dialog);
            }
            else if (!state->closing_ && message == dc::dn_click && name == "GenerateButton")
                state->generate(dialog);
            else if (!state->closing_ && message == dc::dn_change && name == "LinkExpiry")
            {
                state->set_text(dialog, "PropertiesUrl", "");
                state->set_text(dialog, "LinkStatus", "");
            }
        }
    }
    catch (...)
    {
        // Do not unwind C++ exceptions through the host's Pascal event handler.
        if (state)
        {
            state->failure_ = std::current_exception();
            state->cancelled_ = true;
            state->api_.send_message(dialog, nullptr, dc::dm_close, dc::id_cancel, 0);
        }
    }
    return 0;
}

void PropertiesDialog::finish_work()
{
    // DN_CLOSE is a notification, not a veto: the window manager can dismiss
    // the form immediately. Join before returning to keep callback captures,
    // the SDK lease, and the plugin code alive until both workers finish.
    cancelled_ = true;
    if (loading_.valid())
        loading_.wait();
    if (signing_.valid())
        signing_.wait();
}

void PropertiesDialog::show(HWND)
{
    api_ = dc::dialog_api();
    if (!api_.show_dialog || !api_.send_message)
        throw std::runtime_error("Double Commander does not provide the Properties dialog API.");
    try
    {
        api_.show_dialog(const_cast<char*>(embedded_dialogs::properties),
                          sizeof(embedded_dialogs::properties) - 1, dialog_proc,
                          dc::db_lfm, this, nullptr);
    }
    catch (...)
    {
        finish_work();
        throw;
    }
    finish_work();
    if (failure_)
        std::rethrow_exception(failure_);
    if (!initialized_)
        throw std::runtime_error("Cannot open the Properties dialog.");
}

} // namespace s3cmd
