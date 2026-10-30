#include "doublecmd/bucket_creation_dialog.hpp"
#include "bucket_creation_lfm.hpp"

#include <stdexcept>
#include <utility>

namespace s3cmd {

BucketCreationDialog::BucketCreationDialog(std::string_view prefix, std::string_view region,
                                           AccountIdLookupFn lookup_account)
    : prefix_(prefix),
      options_{{}, std::string(region), false, {}},
      lookup_account_(std::move(lookup_account))
{
}

std::string BucketCreationDialog::text(std::uintptr_t dialog, const char* item)
{
    const auto value = api_.send_message(dialog, item, dc::dm_get_text, 0, 0);
    if (value == -1)
        throw std::runtime_error("Cannot read the Create bucket dialog.");
    // The host owns this UTF-8 buffer and reuses it for the next DM_GETTEXT.
    return value ? reinterpret_cast<const char*>(value) : "";
}

void BucketCreationDialog::set_text(std::uintptr_t dialog, const char* item,
                                    const std::string& value)
{
    api_.send_message(dialog, item, dc::dm_set_text, reinterpret_cast<std::intptr_t>(value.c_str()), 0);
}

bool BucketCreationDialog::update(std::uintptr_t dialog)
{
    prefix_ = text(dialog, "BucketPrefix");
    const auto region = text(dialog, "BucketRegion");
    if (region != options_.region)
        lookup_error_.clear();
    options_.region = region;
    options_.account_regional =
        api_.send_message(dialog, "BucketNamespace", dc::dm_list_get_index, 0, 0) == 1;
    options_.bucket_name = build_bucket_name(prefix_, options_);
    set_text(dialog, "BucketPreview",
             options_.account_regional && options_.account_id.empty() ? "Account ID unavailable"
                                                                      : options_.bucket_name);
    const auto error = options_.account_regional && !lookup_error_.empty()
                           ? lookup_error_
                           : validate_bucket_name(prefix_, options_);
    set_text(dialog, "BucketError", error);
    api_.send_message(dialog, "CreateButton", dc::dm_enable, error.empty(), 0);
    return error.empty();
}

void BucketCreationDialog::lookup_account(std::uintptr_t dialog)
{
    if (!options_.account_regional || !options_.account_id.empty() || looking_up_)
        return;

    looking_up_ = true;
    lookup_error_.clear();
    set_text(dialog, "BucketError", "Looking up the AWS account ID...");
    api_.send_message(dialog, "CreateButton", dc::dm_enable, 0, 0);
    api_.send_message(dialog, nullptr, dc::dm_redraw, 0, 0);
    try
    {
        options_.account_id = lookup_account_(options_.region);
    }
    catch (const std::exception& error)
    {
        lookup_error_ = error.what();
    }
    looking_up_ = false;
}

std::intptr_t BucketCreationDialog::dialog_proc(std::uintptr_t dialog, const char* item,
                                                std::intptr_t message, std::intptr_t,
                                                std::intptr_t) noexcept
{
    BucketCreationDialog* state{};
    try
    {
        const auto api = dc::dialog_api();
        if (!api.send_message)
            return 0;
        state = reinterpret_cast<BucketCreationDialog*>(
            api.send_message(dialog, nullptr, dc::dm_get_dialog_data, 0, 0));
        if (!state)
            return 0;

        const std::string_view name = item ? item : "";
        if (message == dc::dn_init_dialog)
        {
            state->set_text(dialog, "BucketPrefix", state->prefix_);
            for (const auto region : bucket_regions)
            {
                state->api_.send_message(dialog, "BucketRegion", dc::dm_list_add_string,
                                         reinterpret_cast<std::intptr_t>(region.data()), 0);
            }
            state->set_text(dialog, "BucketRegion", state->options_.region);
            state->api_.send_message(dialog, "BucketNamespace", dc::dm_list_set_index, 0, 0);
            state->update(dialog);
            state->initialized_ = true;
        }
        else if (state->initialized_ && !state->looking_up_)
        {
            if (message == dc::dn_click && name == "CancelButton")
            {
                state->api_.send_message(dialog, nullptr, dc::dm_close, dc::id_cancel, 0);
            }
            else if (message == dc::dn_click && name == "CreateButton")
            {
                if (state->update(dialog))
                {
                    state->accepted_ = true;
                    state->api_.send_message(dialog, nullptr, dc::dm_close, dc::id_ok, 0);
                }
            }
            else if (message == dc::dn_change &&
                     (name == "BucketPrefix" || name == "BucketRegion" ||
                      name == "BucketNamespace"))
            {
                state->update(dialog);
                if (name == "BucketNamespace")
                {
                    state->lookup_account(dialog);
                    state->update(dialog);
                }
            }
            else if (message == dc::dn_kill_focus && name == "BucketRegion")
            {
                state->update(dialog);
                state->lookup_account(dialog);
                state->update(dialog);
            }
        }
    }
    catch (...)
    {
        // Never unwind a C++ exception through the host's Pascal callback.
        if (state)
        {
            state->failure_ = std::current_exception();
            state->api_.send_message(dialog, nullptr, dc::dm_close, dc::id_cancel, 0);
        }
    }
    return 0;
}

std::optional<BucketCreationOptions> BucketCreationDialog::show()
{
    api_ = dc::dialog_api();
    if (!api_.show_dialog || !api_.send_message)
        throw std::runtime_error("Double Commander does not provide the Create bucket dialog API.");

    initialized_ = false;
    accepted_ = false;
    failure_ = {};
    const auto result = api_.show_dialog(const_cast<char*>(embedded_dialogs::bucket_creation),
                                         sizeof(embedded_dialogs::bucket_creation) - 1, dialog_proc,
                                         dc::db_lfm, this, nullptr);
    if (failure_)
        std::rethrow_exception(failure_);
    if (!initialized_)
        throw std::runtime_error("Cannot open the Create bucket dialog.");
    if (!result || !accepted_)
        return std::nullopt;
    return options_;
}

} // namespace s3cmd
