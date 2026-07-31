#include "s3.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {

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
