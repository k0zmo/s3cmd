#include "s3.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "test_helpers.hpp"
#include "platform.hpp"

#ifdef _WIN32
#  include "win32/bucket_creation_dialog.hpp"
#  include "win32/properties_dialog.hpp"
#  include "win32/resource.h"
#endif

#ifdef _WIN32
#  include <Windows.h> // GetCurrentProcessId, GetTickCount64, SetLastError, GetLastError
#  include <commctrl.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <httplib.h>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <stdexcept>
#include <thread>
#include <utility>

using s3cmd::test::PluginSession;
using s3cmd::test::TemporaryEnvironment;
using s3cmd::test::temporary_config;

namespace {

std::u16string dialog_text(HWND dialog, int control)
{
    const auto length = GetWindowTextLengthW(GetDlgItem(dialog, control));
    std::u16string text(static_cast<std::size_t>(length) + 1, u'\0');
    GetDlgItemTextW(dialog, control, win32_string(text.data()), static_cast<int>(text.size()));
    text.resize(length);
    return text;
}

struct PropertiesDriver
{
    explicit PropertiesDriver(std::function<void(HWND)> tick) : tick(std::move(tick))
    {
        active = this;
        hook = SetWindowsHookExW(WH_CBT,
            [](int code, WPARAM wparam, LPARAM lparam) -> LRESULT {
                const auto dialog = reinterpret_cast<HWND>(wparam);
                if (code == HCBT_ACTIVATE && GetDlgItem(dialog, IDC_PROPERTIES_ADDRESS))
                {
                    active->activated = true;
                    SetTimer(dialog, 99, 20, [](HWND window, UINT, UINT_PTR, DWORD) {
                        if (GetTickCount64() > active->deadline)
                        {
                            active->timed_out = true;
                            finish(window);
                        }
                        else
                            active->tick(window);
                    });
                }
                return CallNextHookEx(nullptr, code, wparam, lparam);
            }, nullptr, GetCurrentThreadId());
        if (!hook)
            throw std::runtime_error("Cannot install the properties test hook");
    }

    ~PropertiesDriver()
    {
        UnhookWindowsHookEx(hook);
        active = nullptr;
    }

    static void finish(HWND dialog)
    {
        KillTimer(dialog, 99);
        SendMessageW(dialog, WM_COMMAND, IDCANCEL, 0);
    }

    static inline PropertiesDriver* active{};
    std::function<void(HWND)> tick;
    HHOOK hook{};
    ULONGLONG deadline{GetTickCount64() + 5000};
    bool activated{};
    bool timed_out{};
};

}

TEST_CASE("bucket creation lets an invalid initial name be corrected", "[unit]")
{
    bool account_regional = false;
    std::string initial = "Invalid_Name";
    SECTION("global bucket") {}
    SECTION("account regional bucket")
    {
        account_regional = true;
    }
    SECTION("overlong initial name")
    {
        initial.assign(80, 'a');
    }

    bool exercised = false;
    static std::function<void(HWND)> exercise;
    exercise = [&](HWND dialog) {
        exercised = true;
        std::array<char16_t, 128> text{};
        GetDlgItemTextW(dialog, IDC_BUCKET_PREFIX, win32_string(text.data()), static_cast<int>(text.size()));
        CHECK(s3cmd::to_utf8(text.data()) == initial);
        CHECK_FALSE(IsWindowEnabled(GetDlgItem(dialog, IDOK)));
        CHECK(GetWindowTextLengthW(GetDlgItem(dialog, IDC_BUCKET_ERROR)) > 0);

        SetDlgItemTextW(dialog, IDC_BUCKET_PREFIX, win32_string(u"corrected-bucket"));
        CHECK(IsWindowEnabled(GetDlgItem(dialog, IDOK)));
        CHECK(GetWindowTextLengthW(GetDlgItem(dialog, IDC_BUCKET_ERROR)) == 0);
        if (account_regional)
        {
            SendDlgItemMessageW(dialog, IDC_BUCKET_NAMESPACE, CB_SETCURSEL, 1, 0);
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDC_BUCKET_NAMESPACE, CBN_SELCHANGE), 0);
        }
        GetDlgItemTextW(dialog, IDC_BUCKET_PREVIEW, win32_string(text.data()), static_cast<int>(text.size()));
        CHECK(std::u16string_view(text.data()) ==
              (account_regional ? u"corrected-bucket-123456789012-eu-central-1-an"
                                : u"corrected-bucket"));
        SendMessageW(dialog, WM_COMMAND, IDOK, 0);
    };
    const auto hook = SetWindowsHookExW(
        WH_CBT,
        [](int code, WPARAM wparam, LPARAM lparam) -> LRESULT {
            const auto dialog = reinterpret_cast<HWND>(wparam);
            if (code == HCBT_ACTIVATE && GetDlgItem(dialog, IDC_BUCKET_PREFIX))
                exercise(dialog);
            return CallNextHookEx(nullptr, code, wparam, lparam);
        },
        nullptr, GetCurrentThreadId());
    REQUIRE(hook != nullptr);
    s3cmd::BucketCreationDialog dialog(initial, "eu-central-1",
                                       [](std::string_view) { return "123456789012"; });
    const auto options = dialog.show();
    UnhookWindowsHookEx(hook);
    exercise = {};
    REQUIRE(exercised);
    REQUIRE(options.has_value());
    CHECK(
        options->bucket_name ==
        (account_regional ? "corrected-bucket-123456789012-eu-central-1-an" : "corrected-bucket"));
}


TEST_CASE("Properties supports sharing during tag loading and cancels on close", "[unit]")
{
    const auto caller = GetCurrentThreadId();
    bool prepared_on_caller{};
    std::atomic<bool> loaded_on_worker{}, snapshot_has_region{}, cancelled{};
    std::atomic<unsigned> expiration{};
    int stage{};
    PropertiesDriver driver([&](HWND dialog) {
        CHECK(dialog_text(dialog, IDC_PROPERTIES_ADDRESS) == u"s3://bucket/folder/file.txt");
        if (stage == 0 && IsWindowEnabled(GetDlgItem(dialog, IDC_PROPERTIES_GENERATE)))
        {
            CHECK(dialog_text(dialog, IDC_PROPERTIES_DETAILS) == u"Size: 42 bytes");
            SendMessageW(dialog, WM_COMMAND, IDC_PROPERTIES_GENERATE, 0);
            CHECK_FALSE(IsWindowEnabled(GetDlgItem(dialog, IDC_PROPERTIES_GENERATE)));
            ++stage;
        }
        else if (stage > 0 && !dialog_text(dialog, IDC_PROPERTIES_URL).empty())
        {
            CHECK(IsWindowEnabled(GetDlgItem(dialog, IDC_PROPERTIES_COPY_URL)));
            if (stage == 1)
            {
                CHECK(expiration == 3600);
                SendDlgItemMessageW(dialog, IDC_PROPERTIES_EXPIRY, CB_SETCURSEL, 2, 0);
                SendMessageW(dialog, WM_COMMAND,
                             MAKEWPARAM(IDC_PROPERTIES_EXPIRY, CBN_SELCHANGE), 0);
                CHECK(dialog_text(dialog, IDC_PROPERTIES_URL).empty());
                CHECK_FALSE(IsWindowEnabled(GetDlgItem(dialog, IDC_PROPERTIES_COPY_URL)));
                SendMessageW(dialog, WM_COMMAND, IDC_PROPERTIES_GENERATE, 0);
                ++stage;
            }
            else if (stage == 2)
            {
                CHECK(expiration == 86400);
                SendDlgItemMessageW(dialog, IDC_PROPERTIES_EXPIRY, CB_SETCURSEL, 0, 0);
                SendMessageW(dialog, WM_COMMAND,
                             MAKEWPARAM(IDC_PROPERTIES_EXPIRY, CBN_SELCHANGE), 0);
                SendMessageW(dialog, WM_COMMAND, IDC_PROPERTIES_GENERATE, 0);
                ++stage;
            }
            else
            {
                CHECK(expiration == 900);
                ++stage;
                PropertiesDriver::finish(dialog);
            }
        }
    });
    s3cmd::S3Properties properties;
    properties.kind = s3cmd::PropertiesKind::object;
    properties.address = "s3://bucket/folder/file.txt";
    properties.details = "Size: 42 bytes";
    s3cmd::PropertiesDialog dialog(properties,
        [&](s3cmd::S3Properties& state) {
            prepared_on_caller = GetCurrentThreadId() == caller;
            state.region = "eu-central-1";
        },
        [&](s3cmd::S3Properties snapshot, const std::atomic<bool>& stop) {
            loaded_on_worker = GetCurrentThreadId() != caller;
            snapshot_has_region = snapshot.region == "eu-central-1";
            while (!stop)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            cancelled = true;
            return snapshot;
        },
        [&](unsigned seconds) {
            expiration = seconds;
            return "https://example.test/file?signature=test";
        });
    dialog.show(nullptr);
    CHECK(driver.activated);
    CHECK_FALSE(driver.timed_out);
    CHECK(stage == 4);
    CHECK(prepared_on_caller);
    CHECK(loaded_on_worker);
    CHECK(snapshot_has_region);
    CHECK(cancelled);
}

TEST_CASE("Properties preserves the address and handles authentication and worker errors", "[unit]")
{
    bool prepare_fails{};
    SECTION("authentication failed") { prepare_fails = true; }
    SECTION("metadata and signing failed") {}
    int stage{};
    int attempts{};
    PropertiesDriver driver([&](HWND dialog) {
        CHECK(dialog_text(dialog, IDC_PROPERTIES_ADDRESS) == u"s3://bucket/file.txt");
        CHECK(IsWindowEnabled(GetDlgItem(dialog, IDC_PROPERTIES_COPY_ADDRESS)));
        if (prepare_fails)
        {
            if (dialog_text(dialog, IDC_PROPERTIES_DETAILS) == u"Authentication failed.")
            {
                CHECK_FALSE(IsWindowEnabled(GetDlgItem(dialog, IDC_PROPERTIES_GENERATE)));
                ++stage;
                PropertiesDriver::finish(dialog);
            }
        }
        else if (stage == 0 && dialog_text(dialog, IDC_PROPERTIES_DETAILS) == u"Metadata failed.")
        {
            CHECK(IsWindowEnabled(GetDlgItem(dialog, IDC_PROPERTIES_GENERATE)));
            CHECK(dialog_text(dialog, IDC_PROPERTIES_REGION) == u"eu-central-1");
            SendMessageW(dialog, WM_COMMAND, IDC_PROPERTIES_GENERATE, 0);
            ++stage;
        }
        else if (stage == 1 && dialog_text(dialog, IDC_PROPERTIES_LINK_STATUS) == u"Signing failed.")
        {
            CHECK_FALSE(IsWindowEnabled(GetDlgItem(dialog, IDC_PROPERTIES_COPY_URL)));
            CHECK(IsWindowEnabled(GetDlgItem(dialog, IDC_PROPERTIES_GENERATE)));
            SendMessageW(dialog, WM_COMMAND, IDC_PROPERTIES_GENERATE, 0);
            ++stage;
        }
        else if (stage == 2 && !dialog_text(dialog, IDC_PROPERTIES_URL).empty())
        {
            CHECK(IsWindowEnabled(GetDlgItem(dialog, IDC_PROPERTIES_COPY_URL)));
            ++stage;
            PropertiesDriver::finish(dialog);
        }
    });
    s3cmd::S3Properties properties;
    properties.kind = s3cmd::PropertiesKind::object;
    properties.address = "s3://bucket/file.txt";
    s3cmd::PropertiesDialog dialog(properties,
        [&](s3cmd::S3Properties& state) {
            if (prepare_fails)
                throw std::runtime_error("Authentication failed.");
            state.region = "eu-central-1";
        },
        [](s3cmd::S3Properties, const std::atomic<bool>&) -> s3cmd::S3Properties {
            throw std::runtime_error("Metadata failed.");
        },
        [&](unsigned) -> std::string {
            if (++attempts == 1)
                throw std::runtime_error("Signing failed.");
            return "https://example.test/signed";
        });
    dialog.show(nullptr);
    CHECK(driver.activated);
    CHECK_FALSE(driver.timed_out);
    CHECK(stage == (prepare_fails ? 1 : 3));
    CHECK(attempts == (prepare_fails ? 0 : 2));
}

TEST_CASE("Properties skips commands and profiles", "[unit]")
{
    temporary_config();
    PropertiesDriver driver([](HWND dialog) { PropertiesDriver::finish(dialog); });
    for (const auto name : {u"\\", u"\\work", u"\\work\\_Add existing bucket_"})
    {
        std::u16string path(name);
        CHECK(s3cmd::execute_file(path.data(), u"PROPERTIES") == FS_EXEC_OK);
    }
    CHECK_FALSE(driver.activated);
}

TEST_CASE("S3 Properties uses listed metadata, fetches tags, and signs the real address", "[unit]")
{
    temporary_config();
    REQUIRE(s3cmd::ProfileConfig("work").register_bucket("bucket", "eu-central-1"));
    bool bucket{}, folder{}, unlisted{}, uncached_file{}, tags_denied{}, no_tags{}, cancel{};
    SECTION("object") {}
    SECTION("object tags denied") { tags_denied = true; }
    SECTION("bucket tags") { bucket = true; }
    SECTION("bucket without tags") { bucket = no_tags = true; }
    SECTION("bucket tags denied") { bucket = tags_denied = true; }
    SECTION("listed virtual folder") { folder = true; }
    SECTION("unlisted virtual folder") { folder = unlisted = true; }
    SECTION("uncached file is treated as a folder") { folder = unlisted = uncached_file = true; }
    SECTION("closing during a tag request preserves listed metadata") { cancel = true; }

    const std::string object_key = "r\xc3\xa9sum\xc3\xa9 #?.txt";
    const std::u16string wide_key = u"r\u00e9sum\u00e9 #?.txt";
    std::atomic<int> tag_requests{}, unexpected_requests{};
    httplib::Server server;
    server.set_pre_routing_handler([&](const httplib::Request& request, httplib::Response& response) {
        if (request.method == "GET" && request.has_param("tagging"))
        {
            ++tag_requests;
            if (cancel)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (request.path != (bucket ? "/bucket" : "/bucket/" + object_key))
                ++unexpected_requests;
            if (tags_denied || no_tags)
            {
                response.status = tags_denied ? 403 : 404;
                const auto code = tags_denied ? "AccessDenied" : "NoSuchTagSet";
                response.set_content(std::string("<Error><Code>") + code + "</Code><Message>" +
                                     code + "</Message></Error>", "application/xml");
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
    PluginSession session;
    if (!bucket && !unlisted)
    {
        WfxFindData data{};
        const auto listing = s3cmd::find_first(u"\\work\\bucket", &data);
        REQUIRE(listing != INVALID_HANDLE_VALUE);
        s3cmd::find_close(listing);
    }
    bool checked{};
    bool generated{};
    PropertiesDriver driver([&](HWND dialog) {
        if (cancel)
        {
            if (tag_requests > 0)
            {
                const auto details = dialog_text(dialog, IDC_PROPERTIES_DETAILS);
                CHECK(details.find(u"42 bytes") != details.npos);
                CHECK(details.find(u"2026-10-23T12:00:00Z") != details.npos);
                CHECK(dialog_text(dialog, IDC_PROPERTIES_TAG_STATUS) == u"Loading tags...");
                checked = true;
                PropertiesDriver::finish(dialog);
            }
            return;
        }
        if (dialog_text(dialog, IDC_PROPERTIES_DETAILS) == u"Loading properties..." ||
            dialog_text(dialog, IDC_PROPERTIES_TAG_STATUS) == u"Loading tags...")
            return;
        if (!checked)
        {
            checked = true;
            const auto expected_address = bucket ? u"s3://bucket/" : uncached_file ?
                u"s3://bucket/" + wide_key + u"/" : folder ?
                u"s3://bucket/folder/" : u"s3://bucket/" + wide_key;
            CHECK(dialog_text(dialog, IDC_PROPERTIES_ADDRESS) == expected_address);
            CHECK(dialog_text(dialog, IDC_PROPERTIES_REGION) == u"eu-central-1");
            CHECK(IsWindowVisible(GetDlgItem(dialog, IDC_PROPERTIES_LINK_GROUP)) == (!bucket && !folder));
            CHECK(IsWindowVisible(GetDlgItem(dialog, IDC_PROPERTIES_TAG_GROUP)) == !folder);
            const auto tags = dialog_text(dialog, IDC_PROPERTIES_TAG_STATUS);
            if (tags_denied)
                CHECK(tags.find(u"AccessDenied") != tags.npos);
            else if (no_tags)
                CHECK(tags == u"No tags.");
            else if (!folder)
                CHECK(ListView_GetItemCount(GetDlgItem(dialog, IDC_PROPERTIES_TAGS)) == 1);
            if (!bucket && !folder)
            {
                const auto details = dialog_text(dialog, IDC_PROPERTIES_DETAILS);
                CHECK(details.find(u"42 bytes") != details.npos);
                CHECK(details.find(u"2026-10-23T12:00:00Z") != details.npos);
                CHECK(details.find(u"STANDARD_IA") != details.npos);
                CHECK(details.find(u"listed") != details.npos);
                CHECK(IsWindowEnabled(GetDlgItem(dialog, IDC_PROPERTIES_GENERATE)));
                SendMessageW(dialog, WM_COMMAND, IDC_PROPERTIES_GENERATE, 0);
            }
            else
            {
                CHECK_FALSE(IsWindowEnabled(GetDlgItem(dialog, IDC_PROPERTIES_GENERATE)));
                PropertiesDriver::finish(dialog);
            }
        }
        const auto link = s3cmd::to_utf8(dialog_text(dialog, IDC_PROPERTIES_URL));
        if (!link.empty())
        {
            generated = true;
            CHECK(link.starts_with(endpoint + "/bucket/r%C3%A9sum%C3%A9%20%23%3F.txt?"));
            CHECK(link.find("X-Amz-Expires=3600") != link.npos);
            CHECK(link.find("X-Amz-Security-Token=test-session") != link.npos);
            CHECK(link.find("eu-central-1") != link.npos);
            CHECK(link.find("X-Amz-Signature=") != link.npos);
            PropertiesDriver::finish(dialog);
        }
    });
    std::u16string path = u"\\work\\bucket";
    if (!bucket)
        path += u"\\" + (folder && !uncached_file ? std::u16string(u"folder") : wide_key);
    CHECK(s3cmd::execute_file(path.data(), u"properties") == FS_EXEC_OK);
    CHECK(driver.activated);
    CHECK_FALSE(driver.timed_out);
    CHECK(checked);
    CHECK(generated == (!bucket && !folder && !cancel));
    CHECK(tag_requests == (folder ? 0 : 1));
    CHECK(unexpected_requests == 0);
}
