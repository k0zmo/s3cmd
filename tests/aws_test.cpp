#include "s3.hpp"
#include "utils.hpp"

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3ClientConfiguration.h>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

struct AwsSession
{
    AwsSession() { Aws::InitAPI(options); }
    ~AwsSession() { Aws::ShutdownAPI(options); }

    Aws::SDKOptions options;
};

struct PluginSession
{
    PluginSession() { s3cmd::initialize(0, nullptr, nullptr, nullptr); }
    ~PluginSession() { s3cmd::shutdown(); }
};

class TemporaryAppData
{
public:
    TemporaryAppData()
        : path_(std::filesystem::temp_directory_path() /
                (L"s3cmd-aws-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                 std::to_wstring(GetTickCount64())))
    {
        wchar_t* previous{};
        std::size_t size{};
        if (_wdupenv_s(&previous, &size, L"APPDATA") != 0)
            throw std::runtime_error("Cannot read APPDATA");
        if (previous)
        {
            old_ = previous;
            std::free(previous);
        }

        std::filesystem::create_directories(path_);
        if (_wputenv_s(L"APPDATA", path_.c_str()) != 0)
            throw std::runtime_error("Cannot set APPDATA");
        s3cmd::reset_config();
    }

    ~TemporaryAppData()
    {
        _wputenv_s(L"APPDATA", old_.c_str());
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

private:
    std::filesystem::path path_;
    std::wstring old_;
};

Aws::S3::S3Client make_client(const char* profile, const char* region)
{
    Aws::S3::S3ClientConfiguration configuration(profile);
    configuration.region = region;

    Aws::Client::ClientConfiguration::CredentialProviderConfiguration credentials_configuration;
    credentials_configuration.profile = profile;
    credentials_configuration.region = region;
    auto credentials = Aws::MakeShared<Aws::Auth::DefaultAWSCredentialsProviderChain>(
        "s3cmd-test", credentials_configuration);
    return Aws::S3::S3Client(credentials, nullptr, configuration);
}

std::optional<std::string> environment(const char* name)
{
    const auto size = GetEnvironmentVariableA(name, nullptr, 0);
    if (size == 0)
        return std::nullopt;

    std::string value(size, '\0');
    const auto written = GetEnvironmentVariableA(name, value.data(), size);
    if (written == 0 || written >= size)
        throw std::runtime_error("Cannot read test environment");
    value.resize(written);
    return value;
}

} // namespace

TEST_CASE("ListBuckets matches the declared AWS permission", "[integration]")
{
    const auto profile = environment("S3CMD_TEST_PROFILE");
    const auto region = environment("S3CMD_TEST_REGION");
    const auto expected = environment("S3CMD_TEST_LIST_BUCKETS");
    if (!profile || !region || !expected)
        SKIP("Set S3CMD_TEST_PROFILE, S3CMD_TEST_REGION, and S3CMD_TEST_LIST_BUCKETS");

    REQUIRE((*expected == "allowed" || *expected == "denied"));

    AwsSession session;
    const auto outcome = make_client(profile->c_str(), region->c_str()).ListBuckets();
    if (*expected == "allowed")
    {
        REQUIRE(outcome.IsSuccess());
    }
    else
    {
        REQUIRE_FALSE(outcome.IsSuccess());
        CHECK(outcome.GetError().GetResponseCode() == Aws::Http::HttpResponseCode::FORBIDDEN);
    }
}

TEST_CASE("a bucket region can be discovered from its name", "[integration]")
{
    const auto profile = environment("S3CMD_TEST_PROFILE");
    const auto bucket = environment("S3CMD_TEST_BUCKET");
    const auto region = environment("S3CMD_TEST_REGION");
    if (!profile || !bucket || !region)
        SKIP("Set S3CMD_TEST_PROFILE, S3CMD_TEST_BUCKET, and S3CMD_TEST_REGION");

    PluginSession session;
    CHECK(s3cmd::discover_bucket_region(*profile, *bucket) == *region);
}

TEST_CASE("a bucket can be entered using its own region", "[integration]")
{
    const auto profile = environment("S3CMD_TEST_PROFILE");
    const auto bucket = environment("S3CMD_TEST_BUCKET");
    const auto region = environment("S3CMD_TEST_REGION");
    const auto list_buckets = environment("S3CMD_TEST_LIST_BUCKETS");
    const auto object = environment("S3CMD_TEST_OBJECT");
    if (!profile || !bucket || !region || !list_buckets || !object)
        SKIP("Set S3CMD_TEST_PROFILE, S3CMD_TEST_BUCKET, S3CMD_TEST_REGION, and "
             "S3CMD_TEST_LIST_BUCKETS, and S3CMD_TEST_OBJECT");

    REQUIRE((*list_buckets == "allowed" || *list_buckets == "denied"));
    PluginSession session;

    REQUIRE_FALSE(object->empty());
    REQUIRE(object->front() != '/');
    REQUIRE(object->back() != '/');

    std::vector<std::string_view> components;
    for (std::string_view remaining = *object; !remaining.empty();)
    {
        const auto separator = remaining.find('/');
        const auto component = remaining.substr(0, separator);
        REQUIRE_FALSE(component.empty());
        components.push_back(component);
        if (separator == remaining.npos)
            break;
        remaining.remove_prefix(separator + 1);
    }

    TemporaryAppData app_data;
    if (*list_buckets == "denied")
        REQUIRE(s3cmd::register_bucket(*profile, *bucket, *region));

    auto profile_path = L"\\" + s3cmd::utf8_to_wide(*profile);
    const auto bucket_name = s3cmd::utf8_to_wide(*bucket);
    WIN32_FIND_DATAW entry{};
    auto handle = s3cmd::find_first(profile_path.data(), &entry);
    REQUIRE(handle != INVALID_HANDLE_VALUE);

    bool found = false;
    do
    {
        found = found || bucket_name == entry.cFileName;
    } while (s3cmd::find_next(handle, &entry));
    s3cmd::find_close(handle);
    REQUIRE(found);

    auto directory = profile_path + L"\\" + bucket_name;
    for (std::size_t index = 0; index < components.size(); ++index)
    {
        const auto expected = s3cmd::utf8_to_wide(components[index]);
        handle = s3cmd::find_first(directory.data(), &entry);
        REQUIRE(handle != INVALID_HANDLE_VALUE);

        found = false;
        DWORD attributes{};
        do
        {
            if (expected == entry.cFileName)
            {
                found = true;
                attributes = entry.dwFileAttributes;
            }
        } while (s3cmd::find_next(handle, &entry));
        s3cmd::find_close(handle);

        REQUIRE(found);
        if (index + 1 < components.size())
        {
            REQUIRE((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
            directory += L"\\" + expected;
        }
    }
}
