#include "s3.hpp"
#include "config.hpp"
#include "utils.hpp"
#include "platform.hpp"
#include "test_helpers.hpp"

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3ClientConfiguration.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using s3cmd::test::PluginSession;
using s3cmd::test::temporary_config;

namespace {

#ifdef _WIN32
constexpr char16_t remote_separator = u'\\';
#else
constexpr char16_t remote_separator = u'/';
#endif

struct AwsSession
{
    AwsSession() { Aws::InitAPI(options); }
    ~AwsSession() { Aws::ShutdownAPI(options); }

    Aws::SDKOptions options;
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
    const auto value = Aws::Environment::GetEnv(name);
    if (value.empty())
        return std::nullopt;
    return std::string{value.c_str()};
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

    temporary_config();
    PluginSession session;
    CHECK(s3cmd::discover_bucket_region(*profile, *bucket) == *region);
    CHECK(s3cmd::ProfileConfig(*profile).bucket_region(*bucket) == *region);
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
    temporary_config();
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

    if (*list_buckets == "denied")
        REQUIRE(s3cmd::ProfileConfig(*profile).register_bucket(*bucket, *region));

    auto profile_path = std::u16string(1, remote_separator) + s3cmd::to_utf16(*profile);
    const auto bucket_name = s3cmd::to_utf16(*bucket);
    auto directory = profile_path + remote_separator + bucket_name;

    // Total Commander may open a saved bucket path without listing its profile first.
    WfxFindData entry{};
    auto handle = s3cmd::find_first(directory.data(), &entry);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    s3cmd::find_close(handle);

    handle = s3cmd::find_first(profile_path.data(), &entry);
    REQUIRE(handle != INVALID_HANDLE_VALUE);

    bool found = false;
    do
    {
        found = found || bucket_name == entry.cFileName;
    } while (s3cmd::find_next(handle, &entry));
    s3cmd::find_close(handle);
    REQUIRE(found);

    for (std::size_t index = 0; index < components.size(); ++index)
    {
        const auto expected = s3cmd::to_utf16(components[index]);
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
            directory += remote_separator + expected;
        }
    }
}

TEST_CASE("Get rejects an untracked partial local file", "[integration]")
{
    const auto profile = environment("S3CMD_TEST_PROFILE");
    const auto bucket = environment("S3CMD_TEST_BUCKET");
    const auto region = environment("S3CMD_TEST_REGION");
    const auto list_buckets = environment("S3CMD_TEST_LIST_BUCKETS");
    const auto object = environment("S3CMD_TEST_OBJECT");
    if (!profile || !bucket || !region || !list_buckets || !object)
        SKIP("Set S3CMD_TEST_PROFILE, S3CMD_TEST_BUCKET, S3CMD_TEST_REGION, "
             "S3CMD_TEST_LIST_BUCKETS, and S3CMD_TEST_OBJECT");

    auto& config = temporary_config();
    REQUIRE((*list_buckets == "allowed" || *list_buckets == "denied"));
    REQUIRE(s3cmd::ProfileConfig(*profile).register_bucket(*bucket, *region));
    PluginSession session;

    auto remote = std::u16string(1, remote_separator) + s3cmd::to_utf16(*profile) +
                  remote_separator + s3cmd::to_utf16(*bucket) + remote_separator +
                  s3cmd::to_utf16(*object);
    std::replace(remote.begin(), remote.end(), u'/', remote_separator);
    const auto local = config.root / "resumed-download";
    auto local_name = local.u16string();

    REQUIRE(s3cmd::get_file(remote.data(), local_name.data(), 0, nullptr) == FS_FILE_OK);
    const auto size = std::filesystem::file_size(local);
    if (size == 0)
        SKIP("S3CMD_TEST_OBJECT must be non-empty");
    std::filesystem::resize_file(local, size / 2);

    RemoteInfoStruct info{};
    info.SizeLow = static_cast<DWORD>(size);
    info.SizeHigh = static_cast<DWORD>(size >> 32);
    CHECK(s3cmd::get_file(remote.data(), local_name.data(), 0, &info) == FS_FILE_EXISTS);
    CHECK(s3cmd::get_file(remote.data(), local_name.data(), FS_COPYFLAGS_RESUME, &info) ==
          FS_FILE_NOTSUPPORTED);
}
