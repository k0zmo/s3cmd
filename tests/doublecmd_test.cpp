#include "doublecmd/bucket_creation_dialog.hpp"
#include "doublecmd/extension.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace dc = s3cmd::dc;

// Exercise the C++/host boundary, including synchronous change notifications
// and the single host-owned buffer returned by DM_GETTEXT.
struct DialogDriver
{
    struct Control
    {
        std::string text;
        std::vector<std::string> items;
        std::intptr_t index{-1};
        bool enabled{true};
    };

    explicit DialogDriver(std::function<void(DialogDriver&)> exercise = {})
        : exercise(std::move(exercise))
    {
        active = this;
        dc::StartupInfo info{};
        info.struct_size = sizeof(info);
        info.send_dialog_message = send;
        info.dialog_box_param = show;
        dc::initialize_extension(&info);
    }

    ~DialogDriver()
    {
        dc::finalize_extension();
        active = nullptr;
    }

    void event(const char* item, std::intptr_t message)
    {
        callback(reinterpret_cast<std::uintptr_t>(this), item, message, 0, 0);
    }

    void edit(const char* item, std::string value)
    {
        controls[item].text = std::move(value);
        event(item, dc::dn_change);
    }

    void select_namespace(std::intptr_t index)
    {
        controls["BucketNamespace"].index = index;
        event("BucketNamespace", dc::dn_change);
    }

    static std::uintptr_t DCPCALL show(void*, std::uint32_t, dc::DialogProc callback,
                                      std::uint32_t flags, void* user_data, void* reserved)
    {
        auto& driver = *active;
        CHECK(flags == dc::db_lfm);
        CHECK(reserved == nullptr);
        driver.callback = callback;
        driver.user_data = user_data;
        driver.event("BucketCreation", dc::dn_init_dialog);
        if (!driver.closed && driver.exercise)
            driver.exercise(driver);
        return driver.result == dc::id_ok;
    }

    static std::intptr_t DCPCALL send(std::uintptr_t handle, const char* item,
                                      std::intptr_t message, std::intptr_t wparam,
                                      std::intptr_t)
    {
        auto& driver = *reinterpret_cast<DialogDriver*>(handle);
        if (message == dc::dm_get_dialog_data)
            return reinterpret_cast<std::intptr_t>(driver.user_data);
        if (message == dc::dm_close)
        {
            driver.closed = true;
            driver.result = wparam;
            return 0;
        }
        if (message == dc::dm_redraw)
            return 0;

        auto& control = driver.controls[item];
        switch (message)
        {
        case dc::dm_get_text:
            if (driver.fail_get_text)
                return -1;
            driver.host_text = control.text;
            return reinterpret_cast<std::intptr_t>(driver.host_text.c_str());
        case dc::dm_set_text:
            if (control.text != reinterpret_cast<const char*>(wparam))
            {
                control.text = reinterpret_cast<const char*>(wparam);
                driver.event(item, dc::dn_change);
            }
            return 0;
        case dc::dm_enable:
        {
            const auto previous = control.enabled;
            if (wparam != -1)
                control.enabled = wparam != 0;
            return previous;
        }
        case dc::dm_list_get_index:
            return control.index;
        case dc::dm_list_set_index:
            control.index = wparam;
            driver.event(item, dc::dn_change);
            return 0;
        case dc::dm_list_add_string:
            control.items.emplace_back(reinterpret_cast<const char*>(wparam));
            return control.items.size() - 1;
        default:
            FAIL("Unexpected host message: " << message);
            return 0;
        }
    }

    static inline DialogDriver* active{};
    std::function<void(DialogDriver&)> exercise;
    std::map<std::string, Control> controls;
    std::string host_text;
    dc::DialogProc callback{};
    void* user_data{};
    std::intptr_t result{};
    bool closed{};
    bool fail_get_text{};
};

} // namespace

TEST_CASE("Double Commander bucket creation lets invalid initial names be corrected", "[unit]")
{
    std::string initial = "Invalid_Name";
    SECTION("invalid characters") {}
    SECTION("overlong name") { initial.assign(80, 'a'); }

    DialogDriver driver([&](DialogDriver& host) {
        CHECK(host.controls["BucketPrefix"].text == initial);
        CHECK(host.controls["BucketRegion"].text == "eu-central-1");
        CHECK(host.controls["BucketNamespace"].index == 0);
        CHECK_FALSE(host.controls["CreateButton"].enabled);
        CHECK_FALSE(host.controls["BucketError"].text.empty());
        host.event("CreateButton", dc::dn_click);
        CHECK_FALSE(host.closed);
        host.edit("BucketPrefix", "corrected-bucket");
        CHECK(host.controls["CreateButton"].enabled);
        CHECK(host.controls["BucketError"].text.empty());
        CHECK(host.controls["BucketPreview"].text == "corrected-bucket");
        host.edit("BucketRegion", "future-region-1");
        CHECK(host.controls["CreateButton"].enabled);
        host.event("CreateButton", dc::dn_click);
    });
    s3cmd::BucketCreationDialog dialog(initial, "eu-central-1", [](std::string_view) {
        FAIL("A global bucket must not look up the account ID");
        return "";
    });
    const auto options = dialog.show();
    REQUIRE(options);
    CHECK(options->bucket_name == "corrected-bucket");
    CHECK(options->region == "future-region-1");
    CHECK_FALSE(options->account_regional);
}

TEST_CASE("Double Commander account regional creation previews names and validates changes", "[unit]")
{
    int lookups{};
    DialogDriver driver([&](DialogDriver& host) {
        host.select_namespace(1);
        CHECK(lookups == 1);
        CHECK(host.controls["BucketPreview"].text == "my-bucket-123456789012-eu-central-1-an");
        CHECK(host.controls["CreateButton"].enabled);
        host.edit("BucketRegion", "me-central-1");
        CHECK_FALSE(host.controls["CreateButton"].enabled);
        host.edit("BucketRegion", "");
        CHECK_FALSE(host.controls["CreateButton"].enabled);
        host.edit("BucketRegion", "us-west-2");
        CHECK(host.controls["BucketPreview"].text == "my-bucket-123456789012-us-west-2-an");
        host.event("BucketRegion", dc::dn_kill_focus);
        CHECK(lookups == 1);
        host.event("CreateButton", dc::dn_click);
    });
    s3cmd::BucketCreationDialog dialog("my-bucket", "eu-central-1", [&](std::string_view region) {
        ++lookups;
        CHECK(region == "eu-central-1");
        CHECK_FALSE(driver.controls["CreateButton"].enabled);
        return "123456789012";
    });
    const auto options = dialog.show();
    REQUIRE(options);
    CHECK(options->account_regional);
    CHECK(options->account_id == "123456789012");
    CHECK(options->region == "us-west-2");
    CHECK(options->bucket_name == "my-bucket-123456789012-us-west-2-an");
}

TEST_CASE("Double Commander shows account lookup failures and permits a retry", "[unit]")
{
    std::vector<std::string> regions;
    DialogDriver driver([&](DialogDriver& host) {
        host.select_namespace(1);
        CHECK(host.controls["BucketError"].text == "STS unavailable");
        CHECK_FALSE(host.controls["CreateButton"].enabled);
        host.event("CreateButton", dc::dn_click);
        CHECK_FALSE(host.closed);
        host.edit("BucketRegion", "us-west-2");
        host.event("BucketRegion", dc::dn_kill_focus);
        CHECK(host.controls["BucketError"].text.empty());
        CHECK(host.controls["CreateButton"].enabled);
        host.event("CreateButton", dc::dn_click);
    });
    s3cmd::BucketCreationDialog dialog("my-bucket", "eu-central-1", [&](std::string_view region) {
        regions.emplace_back(region);
        if (regions.size() == 1)
            throw std::runtime_error("STS unavailable");
        return "123456789012";
    });
    const auto options = dialog.show();
    REQUIRE(options);
    CHECK(regions == std::vector<std::string>{"eu-central-1", "us-west-2"});
    CHECK(options->bucket_name == "my-bucket-123456789012-us-west-2-an");
}

TEST_CASE("Double Commander bucket creation can be canceled", "[unit]")
{
    bool click_cancel = false;
    SECTION("close window") {}
    SECTION("cancel button") { click_cancel = true; }
    DialogDriver driver([&](DialogDriver& host) {
        if (click_cancel)
            host.event("CancelButton", dc::dn_click);
        // Returning without accepting simulates closing the window or Escape.
    });
    s3cmd::BucketCreationDialog dialog("my-bucket", "eu-central-1", {});
    CHECK_FALSE(dialog.show());
}

TEST_CASE("Double Commander callback errors are reported outside the host callback", "[unit]")
{
    DialogDriver driver;
    driver.fail_get_text = true;
    s3cmd::BucketCreationDialog dialog("my-bucket", "eu-central-1", {});
    REQUIRE_THROWS_WITH(dialog.show(), "Cannot read the Create bucket dialog.");
    CHECK(driver.closed);
    CHECK(driver.result == dc::id_cancel);
}

TEST_CASE("Double Commander bucket creation requires a complete extension API", "[unit]")
{
    dc::StartupInfo info{};
    info.struct_size = sizeof(info);
    info.send_dialog_message = DialogDriver::send;
    info.dialog_box_param = DialogDriver::show;
    SECTION("host did not initialize extensions") { dc::finalize_extension(); }
    SECTION("older host record")
    {
        info.struct_size = offsetof(dc::StartupInfo, dialog_box_param);
        dc::initialize_extension(&info);
    }
    SECTION("missing dialog callback")
    {
        info.dialog_box_param = nullptr;
        dc::initialize_extension(&info);
    }
    SECTION("missing message callback")
    {
        info.send_dialog_message = nullptr;
        dc::initialize_extension(&info);
    }
    s3cmd::BucketCreationDialog dialog("my-bucket", "eu-central-1", {});
    CHECK_THROWS_WITH(dialog.show(), "Double Commander does not provide the Create bucket dialog API.");
    dc::finalize_extension();
}
