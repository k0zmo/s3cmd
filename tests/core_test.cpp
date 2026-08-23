#include "s3.hpp"

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
std::wstring request_text;
std::wstring log_text;

BOOL __stdcall capture_request(int, int type, wchar_t*, wchar_t* text, wchar_t*, int)
{
    ++request_count;
    request_type = type;
    request_text = text ? text : L"";
    return TRUE;
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

struct ResetCallbacks
{
    ~ResetCallbacks() { s3cmd::initialize(0, nullptr, nullptr, nullptr); }
};

struct TemporaryIni
{
    TemporaryIni()
        : root(std::filesystem::temp_directory_path() /
               (L"s3cmd-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                std::to_wstring(GetTickCount64()))),
          path(root / L"s3cmd" / L"buckets.ini")
    {
        const auto old_size = GetEnvironmentVariableW(L"APPDATA", nullptr, 0);
        if (old_size != 0)
        {
            old_app_data.resize(old_size, L'\0');
            const auto written = GetEnvironmentVariableW(L"APPDATA", old_app_data.data(), old_size);
            if (written == 0 || written >= old_size)
                throw std::runtime_error("Cannot save APPDATA");
            old_app_data.resize(written);
        }
        std::filesystem::create_directories(root);
        if (!SetEnvironmentVariableW(L"APPDATA", root.c_str()))
            throw std::runtime_error("Cannot set APPDATA");
    }

    ~TemporaryIni()
    {
        SetEnvironmentVariableW(L"APPDATA", old_app_data.empty() ? nullptr : old_app_data.c_str());
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    std::filesystem::path root;
    std::filesystem::path path;
    std::wstring old_app_data;
};

} // namespace

TEST_CASE("remote paths identify profile, bucket, and object", "[unit]")
{
    CHECK(s3cmd::parse_remote_path(L"\\") == s3cmd::RemotePath{});
    CHECK(s3cmd::parse_remote_path(L"\\work") == s3cmd::RemotePath{"work", {}, {}});
    CHECK(s3cmd::parse_remote_path(L"\\work\\my-bucket") ==
          s3cmd::RemotePath{"work", "my-bucket", {}});
    CHECK(s3cmd::parse_remote_path(L"\\work\\my-bucket\\one\\two.txt") ==
          s3cmd::RemotePath{"work", "my-bucket", "one/two.txt"});
    CHECK(s3cmd::parse_remote_path(L"\\\\work\\my-bucket\\one\\\\") ==
          s3cmd::RemotePath{"work", "my-bucket", "one"});
}

TEST_CASE("directory prefixes use S3 separators", "[unit]")
{
    CHECK(s3cmd::directory_prefix({"work", "bucket", ""}).empty());
    CHECK(s3cmd::directory_prefix({"work", "bucket", "one/two"}) == "one/two/");
    CHECK(s3cmd::directory_prefix({"work", "bucket", "one/two/"}) == "one/two/");
}

TEST_CASE("bucket registry stores regions and hides removed buckets", "[unit]")
{
    TemporaryIni ini;

    REQUIRE(s3cmd::register_bucket(ini.path, "work", "private-bucket", "eu-central-1"));

    const auto registered = s3cmd::registered_buckets(ini.path, "work");
    REQUIRE(registered.contains("private-bucket"));
    CHECK(registered.at("private-bucket").region == "eu-central-1");
    CHECK(s3cmd::hidden_buckets(ini.path, "work").empty());

    REQUIRE(s3cmd::unregister_bucket(ini.path, "work", "private-bucket"));
    CHECK(s3cmd::registered_buckets(ini.path, "work").empty());
    CHECK(s3cmd::hidden_buckets(ini.path, "work").contains("private-bucket"));

    REQUIRE(s3cmd::register_bucket(ini.path, "work", "private-bucket", "us-east-1"));
    CHECK(s3cmd::registered_buckets(ini.path, "work").at("private-bucket").region == "us-east-1");
    CHECK(s3cmd::hidden_buckets(ini.path, "work").empty());
}

TEST_CASE("discovered bucket regions are cached separately from registrations", "[unit]")
{
    TemporaryIni ini;
    const s3cmd::BucketMap discovered{
        {"ireland-bucket", {"eu-west-1"}},
        {"singapore-bucket", {"ap-southeast-1"}},
    };

    REQUIRE(s3cmd::cache_bucket_regions(ini.path, "work", discovered));

    const auto cached = s3cmd::cached_bucket_regions(ini.path, "work");
    CHECK(cached.at("ireland-bucket").region == "eu-west-1");
    CHECK(cached.at("singapore-bucket").region == "ap-southeast-1");
    CHECK(s3cmd::registered_buckets(ini.path, "work").empty());
}

TEST_CASE("registered regions override discovered regions", "[unit]")
{
    TemporaryIni ini;
    REQUIRE(s3cmd::cache_bucket_regions(ini.path, "work", {{"shared-bucket", {"us-west-2"}}}));
    CHECK(s3cmd::bucket_region(ini.path, "work", "shared-bucket") == "us-west-2");

    REQUIRE(s3cmd::register_bucket(ini.path, "work", "shared-bucket", "eu-west-1"));
    CHECK(s3cmd::bucket_region(ini.path, "work", "shared-bucket") == "eu-west-1");
}

TEST_CASE("Region is exposed as a Total Commander content field", "[unit]")
{
    TemporaryIni ini;
    REQUIRE(s3cmd::cache_bucket_regions(ini.path, "work", {{"owned-bucket", {"ap-southeast-1"}}}));

    char name[32]{};
    char units[32]{};
    CHECK(s3cmd::content_get_supported_field(0, name, units, sizeof(name)) == ft_string);
    CHECK(std::string_view(name) == "Region");
    CHECK(std::string_view(units).empty());
    CHECK(s3cmd::content_get_supported_field(1, name, units, sizeof(name)) == ft_nomorefields);

    wchar_t bucket_path[] = L"\\work\\owned-bucket";
    wchar_t value[32]{};
    CHECK(s3cmd::content_get_value(bucket_path, 0, value, sizeof(value)) == ft_stringw);
    CHECK(std::wstring_view(value) == L"ap-southeast-1");

    wchar_t object_path[] = L"\\work\\owned-bucket\\file.txt";
    CHECK(s3cmd::content_get_value(object_path, 0, value, sizeof(value)) == ft_fieldempty);
}

TEST_CASE("registered buckets survive unavailable discovery", "[unit]")
{
    s3cmd::BucketMap registered{{"private-bucket", {"eu-central-1"}}};

    const auto visible = s3cmd::merge_buckets(std::move(registered), {}, {});

    REQUIRE(visible.contains("private-bucket"));
    CHECK(visible.at("private-bucket").region == "eu-central-1");
}

TEST_CASE("discovery adds visible buckets and preserves registered regions", "[unit]")
{
    FILETIME discovered_time{123, 456};
    s3cmd::BucketMap registered{{"private-bucket", {"eu-central-1"}}};
    const s3cmd::BucketMap discovered{
        {"private-bucket", {"us-west-1", discovered_time}},
        {"public-bucket", {"ap-southeast-1", discovered_time}},
        {"hidden-bucket", {"eu-west-2", discovered_time}},
    };

    const auto visible = s3cmd::merge_buckets(std::move(registered), {"hidden-bucket"}, discovered);

    REQUIRE(visible.size() == 2);
    CHECK(visible.contains("private-bucket"));
    CHECK(visible.contains("public-bucket"));
    CHECK_FALSE(visible.contains("hidden-bucket"));
    CHECK(visible.at("private-bucket").region == "eu-central-1");
    CHECK(visible.at("public-bucket").region == "ap-southeast-1");
    CHECK(visible.at("private-bucket").created.dwLowDateTime == 123);
    CHECK(visible.at("private-bucket").created.dwHighDateTime == 456);
}

TEST_CASE("runtime dry run completes S3 operations without side effects", "[unit]")
{
    TemporaryIni ini;
    std::filesystem::create_directories(ini.path.parent_path());
    REQUIRE(WritePrivateProfileStringW(L"settings", L"DryRun", L"1", ini.path.c_str()));

    wchar_t object[] = L"\\work\\bucket\\file.txt";
    wchar_t directory[] = L"\\work\\bucket\\folder";
    wchar_t copy[] = L"\\work\\bucket\\copy.txt";
    wchar_t registered_bucket[] = L"\\work\\registered-bucket";
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
    REQUIRE(s3cmd::register_bucket(ini.path, "work", "registered-bucket", "eu-central-1"));
    CHECK(s3cmd::remove_directory(registered_bucket));
    CHECK(s3cmd::registered_buckets(ini.path, "work").contains("registered-bucket"));
}

TEST_CASE("expired SSO session reports the login command on every attempt", "[unit]")
{
    TemporaryIni ini;
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
    request_text.clear();
    log_text.clear();
    s3cmd::initialize(7, nullptr, capture_log, capture_request);
    ResetCallbacks reset;

    wchar_t path[] = L"\\expired";
    WIN32_FIND_DATAW entry{};
    CHECK(s3cmd::find_first(path, &entry) == INVALID_HANDLE_VALUE);
    CHECK(GetLastError() == ERROR_LOGON_FAILURE);
    CHECK(request_count == 1);
    CHECK(request_type == RT_MsgOK);
    CHECK(request_text.find(L"aws sso login --profile expired") != std::wstring::npos);
    CHECK(log_text.find(L"aws sso login --profile expired") != std::wstring::npos);

    CHECK(s3cmd::find_first(path, &entry) == INVALID_HANDLE_VALUE);
    CHECK(request_count == 2);
}
