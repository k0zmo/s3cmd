#include "doublecmd/bucket_creation_dialog.hpp"
#include "doublecmd/extension.hpp"
#include "doublecmd/properties_dialog.hpp"
#include "config.hpp"
#include "s3.hpp"
#include "test_helpers.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <httplib.h>

#include <cstddef>
#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
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
        bool visible{true};
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

    void tick() { event("PropertiesTimer", dc::dn_timer); }

    void pump_until(const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (!done() && !closed && std::chrono::steady_clock::now() < deadline)
        {
            tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(done());
    }

    void window_close()
    {
        event(nullptr, dc::dn_close);
        closed = true;
        result = dc::id_cancel;
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
        driver.wrong_thread |= std::this_thread::get_id() != driver.caller;
        if (driver.closed)
            ++driver.late_ui_calls;
        if (message == dc::dm_get_dialog_data)
            return reinterpret_cast<std::intptr_t>(driver.user_data);
        if (message == dc::dm_close)
        {
            driver.window_close();
            driver.result = wparam;
            return 0;
        }
        if (message == dc::dm_redraw)
            return 0;

        auto& control = driver.controls[item ? item : ""];
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
        case dc::dm_show_item:
            control.visible = wparam != 0;
            return 0;
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
    std::thread::id caller{std::this_thread::get_id()};
    bool wrong_thread{};
    unsigned late_ui_calls{};
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

TEST_CASE("Double Commander Properties shows metadata appropriate to the item kind", "[unit]")
{
    auto kind = s3cmd::PropertiesKind::object;
    SECTION("object") {}
    SECTION("bucket") { kind = s3cmd::PropertiesKind::bucket; }
    SECTION("prefix") { kind = s3cmd::PropertiesKind::prefix; }
    const auto caller = std::this_thread::get_id();
    bool prepared_on_caller{};
    std::atomic<bool> loaded_on_worker{};
    DialogDriver driver([&](DialogDriver& host) {
        CHECK(host.controls["PropertiesAddress"].text == "s3://bucket/résumé #?.txt");
        CHECK(host.controls["PropertiesDetails"].text == "Size: 42 bytes");
        CHECK(host.controls["PropertiesRegion"].text == "Unknown");
        CHECK(host.controls["TagsGroup"].visible == (kind != s3cmd::PropertiesKind::prefix));
        CHECK(host.controls["ShareGroup"].visible == (kind == s3cmd::PropertiesKind::object));
        CHECK_FALSE(host.controls["GenerateButton"].enabled);
        CHECK_FALSE(prepared_on_caller);
        host.tick();
        if (kind != s3cmd::PropertiesKind::prefix)
        {
            host.pump_until([&] { return host.controls["TagStatus"].text == "Loaded"; });
            CHECK(host.controls["PropertiesRegion"].text == "eu-central-1");
            CHECK(host.controls["PropertiesTags"].text == "project = demo\nowner = żółć\n");
            CHECK(host.controls["GenerateButton"].enabled ==
                  (kind == s3cmd::PropertiesKind::object));
        }
        host.event("CloseButton", dc::dn_click);
    });
    s3cmd::S3Properties properties;
    properties.kind = kind;
    properties.address = "s3://bucket/résumé #?.txt";
    properties.details = "Size: 42 bytes";
    s3cmd::PropertiesDialog dialog(properties,
        [&](s3cmd::S3Properties& state) {
            prepared_on_caller = std::this_thread::get_id() == caller;
            state.region = "eu-central-1";
        },
        [&](s3cmd::S3Properties state, const std::atomic<bool>&) {
            loaded_on_worker = std::this_thread::get_id() != caller && state.region == "eu-central-1";
            state.tags = {{"project", "demo"}, {"owner", "żółć"}};
            state.tag_status = "Loaded";
            return state;
        }, {});
    dialog.show(nullptr);
    CHECK(driver.closed);
    CHECK(prepared_on_caller == (kind != s3cmd::PropertiesKind::prefix));
    CHECK(loaded_on_worker == (kind != s3cmd::PropertiesKind::prefix));
    CHECK_FALSE(driver.wrong_thread);
    CHECK(driver.late_ui_calls == 0);
}

TEST_CASE("Double Commander Properties can share while tags load and change link expiry", "[unit]")
{
    const auto caller = std::this_thread::get_id();
    std::atomic<bool> loading{}, cancelled{}, signed_on_worker{};
    std::vector<unsigned> expirations;
    DialogDriver driver([&](DialogDriver& host) {
        host.tick();
        host.pump_until([&] { return loading.load(); });
        CHECK(host.controls["TagStatus"].text == "Loading tags...");
        CHECK(host.controls["GenerateButton"].enabled);
        host.event("GenerateButton", dc::dn_click);
        CHECK_FALSE(host.controls["GenerateButton"].enabled);
        CHECK_FALSE(host.controls["LinkExpiry"].enabled);
        host.event("GenerateButton", dc::dn_click); // Ignore duplicate commands while signing.
        host.pump_until([&] { return !host.controls["PropertiesUrl"].text.empty(); });
        CHECK(host.controls["PropertiesUrl"].text == "https://example.test/signed");
        CHECK(host.controls["TagStatus"].text == "Loading tags...");
        host.controls["LinkExpiry"].index = 2;
        host.event("LinkExpiry", dc::dn_change);
        CHECK(host.controls["PropertiesUrl"].text.empty());
        CHECK(host.controls["LinkStatus"].text.empty());
        host.event("GenerateButton", dc::dn_click);
        host.pump_until([&] { return !host.controls["PropertiesUrl"].text.empty(); });
        host.event("CloseButton", dc::dn_click);
        host.pump_until([&] { return host.closed; });
    });
    s3cmd::S3Properties properties;
    properties.kind = s3cmd::PropertiesKind::object;
    properties.address = "s3://bucket/file.txt";
    s3cmd::PropertiesDialog dialog(properties,
        [](s3cmd::S3Properties& state) { state.region = "eu-central-1"; },
        [&](s3cmd::S3Properties state, const std::atomic<bool>& stop) {
            loading = true;
            while (!stop)
                std::this_thread::yield();
            cancelled = true;
            return state;
        },
        [&](unsigned seconds) {
            signed_on_worker = std::this_thread::get_id() != caller;
            expirations.push_back(seconds);
            return "https://example.test/signed";
        });
    dialog.show(nullptr);
    CHECK(cancelled);
    CHECK(signed_on_worker);
    CHECK(expirations == std::vector<unsigned>{3600, 86400});
    CHECK_FALSE(driver.wrong_thread);
    CHECK(driver.late_ui_calls == 0);
}

TEST_CASE("Double Commander Properties displays failures and permits signing retries", "[unit]")
{
    bool prepare_fails{};
    SECTION("preparation fails") { prepare_fails = true; }
    SECTION("loading and first signing attempt fail") {}
    unsigned attempts{};
    std::atomic<unsigned> selected_expiry{};
    DialogDriver driver([&](DialogDriver& host) {
        host.tick();
        if (prepare_fails)
        {
            CHECK(host.controls["PropertiesDetails"].text == "Authentication failed.");
            CHECK_FALSE(host.controls["GenerateButton"].enabled);
            CHECK(host.controls["TagStatus"].text == "Properties unavailable.");
        }
        else
        {
            host.pump_until([&] { return host.controls["PropertiesDetails"].text == "Metadata failed."; });
            CHECK(host.controls["TagStatus"].text == "Tags unavailable.");
            CHECK(host.controls["GenerateButton"].enabled);
            host.controls["LinkExpiry"].index = -1;
            host.event("GenerateButton", dc::dn_click);
            CHECK(attempts == 0);
            host.controls["LinkExpiry"].index = 0;
            host.event("GenerateButton", dc::dn_click);
            host.pump_until([&] { return host.controls["LinkStatus"].text == "Signing failed."; });
            CHECK(host.controls["PropertiesUrl"].text.empty());
            CHECK(host.controls["GenerateButton"].enabled);
            host.event("GenerateButton", dc::dn_click);
            host.pump_until([&] { return !host.controls["PropertiesUrl"].text.empty(); });
        }
        host.event("CloseButton", dc::dn_click);
    });
    s3cmd::S3Properties properties;
    properties.kind = s3cmd::PropertiesKind::object;
    s3cmd::PropertiesDialog dialog(properties,
        [&](s3cmd::S3Properties&) {
            if (prepare_fails)
                throw std::runtime_error("Authentication failed.");
        },
        [](s3cmd::S3Properties, const std::atomic<bool>&) -> s3cmd::S3Properties {
            throw std::runtime_error("Metadata failed.");
        },
        [&](unsigned seconds) {
            selected_expiry = seconds;
            if (++attempts == 1)
                throw std::runtime_error("Signing failed.");
            return "https://example.test/signed";
        });
    dialog.show(nullptr);
    CHECK(attempts == (prepare_fails ? 0 : 2));
    CHECK(selected_expiry == (prepare_fails ? 0 : 900));
    CHECK(driver.closed);
    CHECK_FALSE(driver.wrong_thread);
}

TEST_CASE("Double Commander Properties finishes workers before returning after any close route", "[unit]")
{
    enum class Close { button, escape, window } route{Close::button};
    SECTION("close button") {}
    SECTION("Escape") { route = Close::escape; }
    SECTION("window manager") { route = Close::window; }
    std::atomic<bool> loading{}, signing{}, cancelled{}, load_finished{}, sign_finished{};
    DialogDriver driver([&](DialogDriver& host) {
        host.tick();
        host.pump_until([&] { return loading.load(); });
        host.event("GenerateButton", dc::dn_click);
        host.pump_until([&] { return signing.load(); });
        if (route == Close::window)
            host.window_close();
        else
        {
            if (route == Close::escape)
            {
                std::uint16_t key = 27;
                host.callback(reinterpret_cast<std::uintptr_t>(&host), nullptr, dc::dn_key_down,
                              reinterpret_cast<std::intptr_t>(&key), 0);
                CHECK(key == 0);
            }
            else
                host.event("CloseButton", dc::dn_click);
            host.pump_until([&] { return host.closed; });
        }
    });
    s3cmd::S3Properties properties;
    properties.kind = s3cmd::PropertiesKind::object;
    s3cmd::PropertiesDialog dialog(properties, [](s3cmd::S3Properties&) {},
        [&](s3cmd::S3Properties state, const std::atomic<bool>& stop) {
            loading = true;
            while (!stop)
                std::this_thread::yield();
            cancelled = true;
            load_finished = true;
            return state;
        },
        [&](unsigned) {
            signing = true;
            // Deliberately finish only after the dialog cancels the tag request.
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            while (!cancelled && std::chrono::steady_clock::now() < deadline)
                std::this_thread::yield();
            sign_finished = true;
            return "https://example.test/signed";
        });
    dialog.show(nullptr);
    CHECK(cancelled);
    CHECK(load_finished);
    CHECK(sign_finished);
    CHECK(driver.closed);
    CHECK_FALSE(driver.wrong_thread);
    CHECK(driver.late_ui_calls == 0);
}

TEST_CASE("Double Commander Properties handles close and timer events during credential prompts", "[unit]")
{
    bool window_close{};
    SECTION("close command while preparing") {}
    SECTION("window closes while preparing") { window_close = true; }
    unsigned prepares{}, loads{};
    DialogDriver driver([](DialogDriver& host) { host.tick(); });
    s3cmd::S3Properties properties;
    properties.kind = s3cmd::PropertiesKind::object;
    s3cmd::PropertiesDialog dialog(properties,
        [&](s3cmd::S3Properties&) {
            ++prepares;
            driver.tick(); // An SSO prompt can pump the host event loop.
            if (window_close)
                driver.window_close();
            else
            {
                driver.event("CloseButton", dc::dn_click);
                CHECK_FALSE(driver.closed);
            }
        },
        [&](s3cmd::S3Properties state, const std::atomic<bool>&) { ++loads; return state; }, {});
    dialog.show(nullptr);
    CHECK(prepares == 1);
    CHECK(loads == 0);
    CHECK(driver.closed);
    CHECK(driver.late_ui_calls == 0);
}

TEST_CASE("Double Commander Properties closes before preparation and reports a missing host API", "[unit]")
{
    SECTION("closed before the first timer event")
    {
        DialogDriver driver([](DialogDriver& host) { host.event("CloseButton", dc::dn_click); });
        s3cmd::PropertiesDialog dialog({}, {}, {}, {});
        CHECK_NOTHROW(dialog.show(nullptr));
        CHECK(driver.closed);
    }
    SECTION("no extension callbacks")
    {
        dc::finalize_extension();
        s3cmd::PropertiesDialog dialog({}, {}, {}, {});
        CHECK_THROWS_WITH(dialog.show(nullptr), "Double Commander does not provide the Properties dialog API.");
    }
}

TEST_CASE("Double Commander S3 Properties uses cached metadata, loads tags, and signs the real object", "[unit]")
{
    using s3cmd::test::TemporaryEnvironment;
    s3cmd::test::temporary_config();
    REQUIRE(s3cmd::ProfileConfig("work").register_bucket("bucket", "eu-central-1"));
    bool bucket{}, folder{}, denied{}, no_tags{}, nested_linux{};
    SECTION("object") {}
    SECTION("object with denied tags") { denied = true; }
    SECTION("bucket") { bucket = true; }
    SECTION("bucket with no tags") { bucket = no_tags = true; }
    SECTION("folder") { folder = true; }
    SECTION("nested object with Linux separators") { nested_linux = true; }
    const std::string object_prefix = nested_linux ? "one/two/" : "";
    const std::string object_key = object_prefix + "r\xc3\xa9sum\xc3\xa9 #?.txt";
    std::atomic<unsigned> tag_requests{}, unexpected_requests{};
    httplib::Server server;
    server.set_pre_routing_handler([&](const httplib::Request& request, httplib::Response& response) {
        if (request.method == "GET" && request.has_param("tagging"))
        {
            ++tag_requests;
            if (request.path != (bucket ? "/bucket" : "/bucket/" + object_key))
                ++unexpected_requests;
            if (denied || no_tags)
            {
                response.status = denied ? 403 : 404;
                const std::string code = denied ? "AccessDenied" : "NoSuchTagSet";
                response.set_content("<Error><Code>" + code + "</Code><Message>" + code +
                                     "</Message></Error>", "application/xml");
            }
            else
                response.set_content("<Tagging><TagSet><Tag><Key>project</Key><Value>demo</Value>"
                                     "</Tag></TagSet></Tagging>", "application/xml");
        }
        else if (request.method == "GET" && request.path == "/bucket" &&
                 request.has_param("list-type"))
        {
            response.set_content("<ListBucketResult><CommonPrefixes><Prefix>folder/</Prefix>"
                                 "</CommonPrefixes><Contents><Key>" + object_key + "</Key>"
                                 "<Size>42</Size><ETag>\"listed\"</ETag><StorageClass>STANDARD_IA"
                                 "</StorageClass><LastModified>2026-10-23T12:00:00.000Z</LastModified>"
                                 "</Contents></ListBucketResult>", "application/xml");
        }
        else
        {
            ++unexpected_requests;
            response.status = 403;
        }
        return httplib::Server::HandlerResponse::Handled;
    });
    const auto port = server.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    std::jthread worker([&](std::stop_token stop) {
        std::stop_callback shutdown(stop, [&] { server.stop(); });
        server.listen_after_bind();
    });
    server.wait_until_ready();
    const auto endpoint = "http://127.0.0.1:" + std::to_string(port);
    TemporaryEnvironment url("AWS_ENDPOINT_URL_S3", endpoint.c_str());
    TemporaryEnvironment endpoints("AWS_IGNORE_CONFIGURED_ENDPOINT_URLS", "false");
    TemporaryEnvironment access("AWS_ACCESS_KEY_ID", "test");
    TemporaryEnvironment secret("AWS_SECRET_ACCESS_KEY", "test");
    TemporaryEnvironment token("AWS_SESSION_TOKEN", "test-session");
    s3cmd::test::PluginSession session;
    if (!bucket)
    {
        WfxFindData data{};
        const auto listing = s3cmd::find_first(
            nested_linux ? u"/work/bucket/one/two" : u"\\work\\bucket", &data);
        REQUIRE(listing != INVALID_HANDLE_VALUE);
        s3cmd::find_close(listing);
    }
    DialogDriver driver([&](DialogDriver& host) {
        const auto expected_address = bucket ? "s3://bucket/" : folder ? "s3://bucket/folder/"
                                               : "s3://bucket/" + object_key;
        CHECK(host.controls["PropertiesAddress"].text == expected_address);
        CHECK(host.controls["PropertiesRegion"].text == "eu-central-1");
        CHECK(host.controls["ShareGroup"].visible == (!bucket && !folder));
        CHECK(host.controls["TagsGroup"].visible == !folder);
        host.tick();
        if (!folder)
        {
            host.pump_until([&] { return host.controls["TagStatus"].text != "Loading tags..."; });
            if (denied)
                CHECK(host.controls["TagStatus"].text.find("AccessDenied") != std::string::npos);
            else if (no_tags)
                CHECK(host.controls["TagStatus"].text == "No tags.");
            else
                CHECK(host.controls["PropertiesTags"].text == "project = demo\n");
        }
        if (!bucket && !folder)
        {
            const auto& details = host.controls["PropertiesDetails"].text;
            CHECK(details.find("42 bytes") != details.npos);
            CHECK(details.find("2026-10-23T12:00:00Z") != details.npos);
            CHECK(details.find("STANDARD_IA") != details.npos);
            CHECK(details.find("listed") != details.npos);
            host.event("GenerateButton", dc::dn_click);
            host.pump_until([&] { return !host.controls["PropertiesUrl"].text.empty(); });
            const auto& link = host.controls["PropertiesUrl"].text;
            CHECK(link.starts_with(endpoint + "/bucket/" + object_prefix +
                                   "r%C3%A9sum%C3%A9%20%23%3F.txt?"));
            CHECK(link.find("X-Amz-Expires=3600") != link.npos);
            CHECK(link.find("X-Amz-Security-Token=test-session") != link.npos);
            CHECK(link.find("eu-central-1") != link.npos);
            CHECK(link.find("X-Amz-Signature=") != link.npos);
        }
        host.event("CloseButton", dc::dn_click);
    });
    std::u16string path = u"\\work\\bucket";
    if (nested_linux)
        path = u"/work/bucket/one/two/r\u00e9sum\u00e9 #?.txt";
    else if (!bucket)
        path += folder ? u"\\folder" : u"\\r\u00e9sum\u00e9 #?.txt";
    CHECK(s3cmd::execute_file(path.data(), u"properties") == FS_EXEC_OK);
    CHECK(driver.closed);
    CHECK(tag_requests == (folder ? 0 : 1));
    CHECK(unexpected_requests == 0);
    CHECK(driver.late_ui_calls == 0);
    CHECK_FALSE(driver.wrong_thread);
}
