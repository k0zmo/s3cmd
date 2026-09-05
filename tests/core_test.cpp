#include "s3.hpp"
#include "core.hpp"

#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/logging/LogLevel.h>
#include <aws/core/utils/logging/LogSystemInterface.h>
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

int request_count{};
int request_type{};
bool request_accept{true};
std::wstring request_text;
std::wstring log_text;

BOOL __stdcall capture_request(int, int type, wchar_t*, wchar_t* text, wchar_t*, int)
{
    ++request_count;
    request_type = type;
    request_text = text ? text : L"";
    return request_accept;
}

void __stdcall capture_log(int, int type, wchar_t* text)
{
    if (type == MSGTYPE_IMPORTANTERROR)
        log_text = text ? text : L"";
}

struct TemporaryEnvironment
{
    TemporaryEnvironment(const char* variable_name, const char* value) : name(variable_name)
    {
        char* previous{};
        std::size_t size{};
        if (_dupenv_s(&previous, &size, name.c_str()) != 0)
            throw std::runtime_error("Cannot read environment variable");
        if (previous)
        {
            old = previous;
            std::free(previous);
            existed = true;
        }
        if (_putenv_s(name.c_str(), value ? value : "") != 0)
            throw std::runtime_error("Cannot set environment variable");
    }

    ~TemporaryEnvironment() { _putenv_s(name.c_str(), existed ? old.c_str() : ""); }

    std::string name;
    std::string old;
    bool existed{};
};

struct PluginSession
{
    PluginSession(int number = 0, tProgressProcW progress = nullptr, tLogProcW log = nullptr,
                  tRequestProcW request = nullptr)
    {
        s3cmd::initialize(number, progress, log, request);
    }

    ~PluginSession() { s3cmd::shutdown(); }
};

struct TemporaryConfig
{
    TemporaryConfig()
        : root(std::filesystem::temp_directory_path() /
               (L"s3cmd-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()))),
          path(root / L"s3cmd" / L"s3cmd.toml")
    {
        wchar_t* previous{};
        std::size_t size{};
        if (_wdupenv_s(&previous, &size, L"APPDATA") != 0)
            throw std::runtime_error("Cannot read APPDATA");
        if (previous)
        {
            old_app_data = previous;
            std::free(previous);
        }
        std::filesystem::create_directories(root);
        if (_wputenv_s(L"APPDATA", root.c_str()) != 0)
            throw std::runtime_error("Cannot set APPDATA");
    }

    ~TemporaryConfig()
    {
        _wputenv_s(L"APPDATA", old_app_data.c_str());
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void reset()
    {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        if (error)
            throw std::runtime_error("Cannot reset temporary config");
        std::filesystem::create_directories(root);
        s3cmd::reset_config();
    }

    std::filesystem::path root;
    std::filesystem::path path;
    std::wstring old_app_data;
};

TemporaryConfig& temporary_config()
{
    static TemporaryConfig config;
    config.reset();
    return config;
}

} // namespace

TEST_CASE("AWS SDK lifecycle is idempotent on its owner thread", "[unit]")
{
    s3cmd::initialize(0, nullptr, nullptr, nullptr);
    s3cmd::initialize(0, nullptr, nullptr, nullptr);
    s3cmd::shutdown();
    s3cmd::shutdown();
    SUCCEED();
}

TEST_CASE("AWS log level comes from configuration", "[unit]")
{
    auto& config = temporary_config();

    SECTION("default")
    {
        PluginSession session;
        REQUIRE(Aws::Utils::Logging::GetLogSystem());
        CHECK(Aws::Utils::Logging::GetLogSystem()->GetLogLevel() ==
              Aws::Utils::Logging::LogLevel::Info);
    }

    SECTION("configured")
    {
        std::filesystem::create_directories(config.path.parent_path());
        std::ofstream(config.path) << "[settings]\nAwsLogLevel = \"Trace\"\n";
        PluginSession session;
        REQUIRE(Aws::Utils::Logging::GetLogSystem());
        CHECK(Aws::Utils::Logging::GetLogSystem()->GetLogLevel() ==
              Aws::Utils::Logging::LogLevel::Trace);
    }
}

TEST_CASE("remote paths identify profile, bucket, and object", "[unit]")
{
    CHECK(s3cmd::RemotePath::make(L"\\") == s3cmd::RemotePath{});
    CHECK(s3cmd::RemotePath::make(L"\\work") == s3cmd::RemotePath{"work", {}, {}});
    CHECK(s3cmd::RemotePath::make(L"\\work\\my-bucket") ==
          s3cmd::RemotePath{"work", "my-bucket", {}});
    CHECK(s3cmd::RemotePath::make(L"\\work\\my-bucket\\one\\two.txt") ==
          s3cmd::RemotePath{"work", "my-bucket", "one/two.txt"});
    CHECK(s3cmd::RemotePath::make(L"\\\\work\\my-bucket\\one\\\\") ==
          s3cmd::RemotePath{"work", "my-bucket", "one"});
}

TEST_CASE("remote path views borrow the original wide path", "[unit]")
{
    using s3cmd::RemotePathView;
    CHECK(RemotePathView::make({}) == RemotePathView{});
    CHECK(RemotePathView::make(L"\\\\") == RemotePathView{});
    CHECK(RemotePathView::make(L"\\work\\") == RemotePathView{L"work", {}, {}});
    CHECK(RemotePathView::make(L"\\work\\bucket\\") ==
          RemotePathView{L"work", L"bucket", {}});
    CHECK(RemotePathView::make(L"\\work\\..\\") == RemotePathView{L"work", L"..", {}});
    CHECK(RemotePathView::make(L"\\\\work\\bucket\\one\\\\") ==
          RemotePathView{L"work", L"bucket", L"one"});

    const std::wstring path = L"\\w\u00f3rk\\bucket\\one\\two.txt";
    const auto view = RemotePathView::make(path);
    CHECK(view == RemotePathView{L"w\u00f3rk", L"bucket", L"one\\two.txt"});
    CHECK(view.profile.data() == path.data() + 1);
    CHECK(view.bucket.data() == path.data() + 6);
    CHECK(view.key.data() == path.data() + 13);
    CHECK(s3cmd::RemotePath::make(path) ==
          s3cmd::RemotePath{"w\xc3\xb3rk", "bucket", "one/two.txt"});
}

TEST_CASE("virtual deletes skip recursive directory listings", "[unit]")
{
    temporary_config();
    const s3cmd::ProfileConfig config("work");
    REQUIRE(config.register_bucket("bucket", "eu-central-1"));

    wchar_t root[] = L"\\";
    wchar_t profile[] = L"\\work";
    wchar_t bucket[] = L"\\work\\bucket";
    WIN32_FIND_DATAW entry{};

    s3cmd::status_info(root, FS_STATUS_START, FS_STATUS_OP_DELETE);
    SetLastError(ERROR_SUCCESS);
    CHECK(s3cmd::find_first(profile, &entry) == INVALID_HANDLE_VALUE);
    CHECK(GetLastError() == ERROR_NO_MORE_FILES);
    CHECK_FALSE(s3cmd::remove_directory(profile));
    s3cmd::status_info(root, FS_STATUS_END, FS_STATUS_OP_DELETE);

    s3cmd::status_info(profile, FS_STATUS_START, FS_STATUS_OP_DELETE);
    SetLastError(ERROR_SUCCESS);
    CHECK(s3cmd::find_first(bucket, &entry) == INVALID_HANDLE_VALUE);
    CHECK(GetLastError() == ERROR_NO_MORE_FILES);
    CHECK(s3cmd::remove_directory(bucket));
    CHECK_FALSE(config.registered_buckets().contains("bucket"));
    s3cmd::status_info(profile, FS_STATUS_END, FS_STATUS_OP_DELETE);

    REQUIRE(config.register_bucket("discovered-bucket", "eu-central-1"));
    config.set_discovered_buckets({{"discovered-bucket", {"us-west-2"}}});
    wchar_t discovered_bucket[] = L"\\work\\discovered-bucket";

    s3cmd::status_info(profile, FS_STATUS_START, FS_STATUS_OP_DELETE);
    CHECK_FALSE(s3cmd::remove_directory(discovered_bucket));
    CHECK(config.registered_buckets().contains("discovered-bucket"));
    s3cmd::status_info(profile, FS_STATUS_END, FS_STATUS_OP_DELETE);
}

TEST_CASE("directory prefixes use S3 separators", "[unit]")
{
    CHECK(s3cmd::RemotePath{"work", "bucket", ""}.directory_prefix().empty());
    CHECK(s3cmd::RemotePath{"work", "bucket", "one/two"}.directory_prefix() == "one/two/");
    CHECK(s3cmd::RemotePath{"work", "bucket", "one/two/"}.directory_prefix() == "one/two/");
}

TEST_CASE("bucket registry only unregisters registered buckets", "[unit]")
{
    auto& ini = temporary_config();
    const s3cmd::ProfileConfig profile("work");

    REQUIRE(profile.register_bucket("private-bucket", "eu-central-1"));

    std::ifstream file(ini.path);
    REQUIRE(file);
    bool found_profile_buckets{};
    for (std::string line; std::getline(file, line);)
        found_profile_buckets |= line == "[profiles.work.buckets]";
    CHECK(found_profile_buckets);

    s3cmd::reset_config();

    const auto registered = profile.registered_buckets();
    REQUIRE(registered.contains("private-bucket"));
    CHECK(registered.at("private-bucket").region == "eu-central-1");
    REQUIRE(profile.unregister_bucket("private-bucket"));
    CHECK(profile.registered_buckets().empty());
    CHECK_FALSE(profile.unregister_bucket("private-bucket"));
}

TEST_CASE("discovered bucket regions remain in memory", "[unit]")
{
    auto& ini = temporary_config();
    const s3cmd::ProfileConfig profile("work");
    const s3cmd::BucketMap discovered{
        {"ireland-bucket", {"eu-west-1"}},
        {"singapore-bucket", {"ap-southeast-1"}},
    };

    profile.set_discovered_buckets(discovered);
    CHECK(profile.bucket_region("ireland-bucket") == "eu-west-1");
    CHECK(profile.bucket_region("singapore-bucket") == "ap-southeast-1");
    CHECK(profile.registered_buckets().empty());
    CHECK_FALSE(std::filesystem::exists(ini.path));
}

TEST_CASE("bucket registration is disabled when buckets were discovered", "[unit]")
{
    temporary_config();
    const s3cmd::ProfileConfig profile("work");
    profile.set_discovered_buckets({{"discovered-bucket", {"eu-west-1"}}});
    PluginSession session;

    wchar_t bucket[] = L"\\work\\new-bucket";
    CHECK_FALSE(s3cmd::make_directory(bucket));
    CHECK_FALSE(profile.registered_buckets().contains("new-bucket"));
}

TEST_CASE("bucket registration rejects an already registered bucket", "[unit]")
{
    temporary_config();
    const s3cmd::ProfileConfig profile("work");
    REQUIRE(profile.register_bucket("registered-bucket", "eu-central-1"));
    PluginSession session;

    wchar_t bucket[] = L"\\work\\registered-bucket";
    CHECK_FALSE(s3cmd::make_directory(bucket));
}

TEST_CASE("registered regions override discovered regions", "[unit]")
{
    temporary_config();
    const s3cmd::ProfileConfig profile("work");
    profile.set_discovered_buckets({{"shared-bucket", {"us-west-2"}}});
    CHECK(profile.bucket_region("shared-bucket") == "us-west-2");

    REQUIRE(profile.register_bucket("shared-bucket", "eu-west-1"));
    CHECK(profile.bucket_region("shared-bucket") == "eu-west-1");
}

TEST_CASE("Region is exposed as a Total Commander content field", "[unit]")
{
    temporary_config();
    const s3cmd::ProfileConfig profile("work");
    profile.set_discovered_buckets({{"owned-bucket", {"ap-southeast-1"}}});

    char name[32]{};
    char units[32]{};
    CHECK(s3cmd::content_get_supported_field(0, name, units, sizeof(name)) == ft_string);
    CHECK(std::string_view(name) == "Region");
    CHECK(std::string_view(units).empty());
    CHECK(s3cmd::content_get_supported_field(1, name, units, sizeof(name)) == ft_nomorefields);

    wchar_t bucket_path[] = L"\\work\\owned-bucket";
    wchar_t value[32]{};
    {
        PluginSession session;
        CHECK(s3cmd::content_get_value(bucket_path, 0, value, sizeof(value)) == ft_stringw);
        CHECK(std::wstring_view(value) == L"ap-southeast-1");

        s3cmd::reset_config();
        CHECK(s3cmd::content_get_value(bucket_path, 0, value, sizeof(value)) == ft_fieldempty);

        wchar_t object_path[] = L"\\work\\owned-bucket\\file.txt";
        CHECK(s3cmd::content_get_value(object_path, 0, value, sizeof(value)) == ft_fieldempty);
    }

    PluginSession session;
    CHECK(s3cmd::content_get_value(bucket_path, 0, value, sizeof(value)) == ft_fieldempty);
}

TEST_CASE("runtime dry run completes S3 operations without side effects", "[unit]")
{
    auto& ini = temporary_config();
    std::filesystem::create_directories(ini.path.parent_path());
    {
        std::ofstream file(ini.path);
        REQUIRE(file);
        file << "[settings]\nDryRun = true\n";
    }
    const s3cmd::ProfileConfig profile("work");
    REQUIRE(profile.register_bucket("registered-bucket", "eu-central-1"));
    PluginSession session;

    wchar_t object[] = L"\\work\\bucket\\file.txt";
    wchar_t directory[] = L"\\work\\bucket\\folder";
    wchar_t copy[] = L"\\work\\bucket\\copy.txt";
    wchar_t registered_bucket[] = L"\\work\\registered-bucket";
    wchar_t discovered_bucket[] = L"\\work\\discovered-bucket";
    const auto local = std::filesystem::temp_directory_path() /
                       (L"s3cmd-dry-run-" + std::to_wstring(GetCurrentProcessId()));
    auto local_name = local.wstring();
    const auto upload = ini.root / L"upload.txt";
    {
        std::ofstream file(upload);
        REQUIRE(file);
        file << "move source";
    }
    auto upload_name = upload.wstring();

    CHECK(s3cmd::get_file(object, local_name.data(), FS_COPYFLAGS_MOVE, nullptr) == FS_FILE_OK);
    CHECK_FALSE(std::filesystem::exists(local));
    CHECK(s3cmd::put_file(upload_name.data(), object, FS_COPYFLAGS_MOVE) == FS_FILE_OK);
    CHECK(std::filesystem::exists(upload));
    CHECK(s3cmd::delete_file(object));
    CHECK(s3cmd::make_directory(directory));
    CHECK(s3cmd::rename_or_move(object, copy, TRUE, TRUE, nullptr) == FS_FILE_OK);
    CHECK(s3cmd::remove_directory(registered_bucket));
    CHECK_FALSE(s3cmd::remove_directory(discovered_bucket));
    CHECK(profile.registered_buckets().contains("registered-bucket"));
}

TEST_CASE("expired SSO session asks before starting browser login", "[unit]")
{
    auto& ini = temporary_config();
    const auto config = ini.root / L"config";
    const auto credentials = ini.root / L"credentials";
    {
        std::ofstream file(config);
        REQUIRE(file);
        file << "[profile expired]\n"
                "sso_session = expired\n"
                "sso_account_id = 111122223333\n"
                "sso_role_name = ReadOnly\n"
                "region = eu-central-1\n"
                "[sso-session expired]\n"
                "sso_start_url = https://example.awsapps.com/start\n"
                "sso_region = eu-central-1\n"
                "sso_registration_scopes = sso:account:access\n";
    }
    std::ofstream(credentials).close();

    const auto config_name = config.string();
    const auto credentials_name = credentials.string();
    TemporaryEnvironment config_file("AWS_CONFIG_FILE", config_name.c_str());
    TemporaryEnvironment credentials_file("AWS_SHARED_CREDENTIALS_FILE", credentials_name.c_str());
    TemporaryEnvironment access_key("AWS_ACCESS_KEY_ID", nullptr);
    TemporaryEnvironment secret_key("AWS_SECRET_ACCESS_KEY", nullptr);
    TemporaryEnvironment session_token("AWS_SESSION_TOKEN", nullptr);
    TemporaryEnvironment disable_metadata("AWS_EC2_METADATA_DISABLED", "true");

    request_count = 0;
    request_type = 0;
    request_accept = false;
    request_text.clear();
    log_text.clear();
    PluginSession session(7, nullptr, capture_log, capture_request);

    wchar_t path[] = L"\\expired";
    WIN32_FIND_DATAW entry{};
    CHECK(s3cmd::find_first(path, &entry) == INVALID_HANDLE_VALUE);
    CHECK(GetLastError() == ERROR_LOGON_FAILURE);
    CHECK(request_count == 1);
    CHECK(request_type == RT_MsgYesNo);
    CHECK(request_text.find(L"Start browser login") != std::wstring::npos);
    CHECK(request_text.find(L"aws sso login") == std::wstring::npos);
    CHECK(log_text.find(L"was cancelled") != std::wstring::npos);

    CHECK(s3cmd::find_first(path, &entry) == INVALID_HANDLE_VALUE);
    CHECK(request_count == 2);
    request_accept = true;
}
