#include "s3.hpp"
#include "config.hpp"
#include "creds.hpp"
#include "core.hpp"
#include "fsplugin.h"
#include "object_metadata.hpp"
#include "resume.hpp"
#include "utils.hpp"
#include "test_helpers.hpp"

#include <aws/core/auth/GeneralHTTPCredentialsProvider.h>
#include <aws/core/auth/SSOCredentialsProvider.h>
#include <aws/core/auth/STSCredentialsProvider.h>
#include <aws/core/internal/AWSHttpResourceClient.h>
#include <aws/core/platform/Environment.h>
#include <aws/core/utils/logging/AWSLogging.h>
#include <aws/core/utils/logging/LogLevel.h>
#include <aws/core/utils/logging/LogSystemInterface.h>
#include <catch2/catch_test_macros.hpp>
#include <httplib.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using s3cmd::test::PluginSession;
using s3cmd::test::TemporaryConfig;
using s3cmd::test::TemporaryEnvironment;
using s3cmd::test::temporary_config;

namespace {

int request_count{};
int request_type{};
bool request_accept{true};
std::u16string request_text;
std::vector<std::u16string> request_inputs;
std::u16string log_text;
std::atomic<bool> transfer_waiting{};
std::atomic<bool> transfer_canceled{};
std::thread::id transfer_caller{};
bool transfer_wrong_thread{};
std::vector<int> transfer_percentages;

int DCPCALL cancel_stalled_transfer(int, char16_t*, char16_t*, int percent)
{
    transfer_percentages.push_back(percent);
    transfer_wrong_thread |= std::this_thread::get_id() != transfer_caller;
    if (!transfer_waiting)
        return 0;
    transfer_canceled = true;
    return 1;
}

BOOL DCPCALL capture_request(int, int type, char16_t*, char16_t* text, char16_t*, int)
{
    ++request_count;
    request_type = type;
    request_text = text ? text : u"";
    return request_accept;
}

BOOL DCPCALL answer_request(int number, int type, char16_t* title, char16_t* text, char16_t* out,
                            int max_length)
{
    if (!capture_request(number, type, title, text, out, max_length) ||
        static_cast<std::size_t>(request_count) > request_inputs.size())
    {
        return FALSE;
    }
    const auto& input = request_inputs[request_count - 1];
    const auto length = std::min(input.size(), static_cast<std::size_t>(max_length - 1));
    std::copy_n(input.data(), length, out);
    out[length] = u'\0';
    return TRUE;
}

void DCPCALL capture_log(int, int type, char16_t* text)
{
    if (type == MSGTYPE_IMPORTANTERROR)
        log_text = text ? text : u"";
}

void write_resume_record(const TemporaryConfig& config, const std::filesystem::path& local,
                         std::u16string_view remote, std::string_view e_tag, std::uint64_t size)
{
    const auto key = s3cmd::to_utf8(std::filesystem::absolute(local).lexically_normal().u16string());
    std::filesystem::create_directories(config.path.parent_path());
    std::ofstream file(config.path.parent_path() / u"resume.toml");
    file << "[downloads.'" << key << "']\n"
         << "remote = '" << s3cmd::to_utf8(remote) << "'\n"
         << "etag = '" << e_tag << "'\n"
         << "size = " << size << '\n';
}

std::filesystem::path download_path(const std::filesystem::path& local)
{
    auto result = local;
    result += ".s3cmddownload";
    return result;
}

std::string read_file(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}

} // namespace

TEST_CASE("last error values persist and are isolated between threads", "[unit]")
{
    const auto previous = GetLastError();
    SetLastError(ERROR_NO_MORE_FILES);

    DWORD initial{}, written{}, repeated{};
    std::jthread worker([&] {
        initial = GetLastError();
        SetLastError(0xf0000000u);
        written = GetLastError();
        repeated = GetLastError();
    });
    worker.join();

    CHECK(initial == ERROR_SUCCESS);
    CHECK(written == 0xf0000000u);
    CHECK(repeated == written);
    CHECK(GetLastError() == ERROR_NO_MORE_FILES);
    SetLastError(previous);
}

TEST_CASE("Download commit preserves an unexpected destination", "[unit]")
{
    auto& config = temporary_config();
    const auto local = config.root / u"download";
    s3cmd::ResumeFile resume{local, "remote"};
    std::ofstream(local, std::ios::binary) << "existing";
    std::ofstream(resume.download_path(), std::ios::binary) << "replacement";

    CHECK_FALSE(resume.finish_download(11, false));
    CHECK(read_file(local) == "existing");
    CHECK(read_file(resume.download_path()) == "replacement");

    REQUIRE(resume.finish_download(11, true));
    CHECK(read_file(local) == "replacement");
    CHECK_FALSE(std::filesystem::exists(resume.download_path()));
}

TEST_CASE("Plugin initialization prunes invalid resume entries", "[unit]")
{
    auto& config = temporary_config();
    const auto valid = config.root / u"valid";
    const auto missing = config.root / u"missing";
    const auto complete = config.root / u"complete";
    s3cmd::ResumeFile valid_resume{valid, "remote"};
    s3cmd::ResumeFile missing_resume{missing, "remote"};
    s3cmd::ResumeFile complete_resume{complete, "remote"};
    std::ofstream(valid_resume.download_path(), std::ios::binary) << "part";
    std::ofstream(complete_resume.download_path(), std::ios::binary) << "complete";
    REQUIRE(valid_resume.write_resume_record({"etag", 8}));
    REQUIRE(missing_resume.write_resume_record({"etag", 8}));
    REQUIRE(complete_resume.write_resume_record({"etag", 8}));

    {
        PluginSession session;
        const auto document = read_file(config.path.parent_path() / u"resume.toml");
        const auto key = [](const std::filesystem::path& path) {
            return s3cmd::to_utf8(std::filesystem::absolute(path).lexically_normal().u16string());
        };
        CHECK(document.find(key(valid)) != std::string::npos);
        CHECK(document.find(key(missing)) == std::string::npos);
        CHECK(document.find(key(complete)) == std::string::npos);
        CHECK(std::filesystem::exists(valid_resume.download_path()));
        CHECK(std::filesystem::exists(complete_resume.download_path()));
    }
}

TEST_CASE("Get rejects an untracked local file without a resume state", "[unit]")
{
    auto& config = temporary_config();
    const auto local = config.root / u"download";
    std::ofstream(local, std::ios::binary) << "partial";
    PluginSession session;

    CHECK(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(), FS_COPYFLAGS_RESUME,
                          nullptr) == FS_FILE_NOTSUPPORTED);
    CHECK(read_file(local) == "partial");
}

TEST_CASE("Get discards HTTP error bodies before retry", "[unit]")
{
    auto& config = temporary_config();
    const auto local = config.root / u"download";
    auto duplicate_local = local.parent_path() / "." / local.filename();
#ifdef _WIN32
    duplicate_local.replace_filename(u"DOWNLOAD");
#endif
    std::string object = "prefix and the rest of the object";
    std::atomic<bool> reject{true};
    std::atomic<bool> stall{};
    std::atomic<bool> delay{};
    std::atomic<bool> ignore_range{};
    std::atomic<bool> probe_duplicate{};
    std::atomic<int> duplicate_result{-1};
    std::atomic<int> head_requests{};
    std::string if_match;
    transfer_percentages.clear();
    transfer_waiting = false;
    transfer_canceled = false;
    transfer_wrong_thread = false;
    transfer_caller = std::this_thread::get_id();
    httplib::Server server;
    const auto respond = [&](const httplib::Request& request, httplib::Response& response) {
        if (request.method == "HEAD")
            ++head_requests;
        if (probe_duplicate.exchange(false))
            duplicate_result =
                s3cmd::get_file(u"\\resume-test\\bucket\\object", duplicate_local.u16string().c_str(),
                                FS_COPYFLAGS_OVERWRITE, nullptr);
        if (delay)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (stall)
        {
            transfer_waiting = true;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!transfer_canceled && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        response.set_header("ETag", "\"test\"");
        if_match = request.get_header_value("If-Match");
        if (ignore_range)
        {
            // Keep httplib from repairing the intentionally invalid range response.
            const_cast<httplib::Request&>(request).ranges.clear();
            response.status = 200;
            response.set_content(object, "application/octet-stream");
            return;
        }
        if (request.method == "HEAD")
            response.set_content(object, "application/octet-stream");
        else if (reject)
        {
            response.status = 412;
            response.set_content("<Error><Code>PreconditionFailed</Code></Error>",
                                 "application/xml");
        }
        else if (delay)
            response.set_content_provider(
                object.size(), "application/octet-stream",
                [&](size_t offset, size_t length, httplib::DataSink& sink) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                    return sink.write(object.data() + offset, length);
                });
        else
            response.set_content(object, "application/octet-stream");
    };
    server.Get("/bucket/object", respond);
    server.Put("/bucket/object", respond);
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
    TemporaryEnvironment region("AWS_DEFAULT_REGION", "us-east-1");
    TemporaryEnvironment checksum("AWS_REQUEST_CHECKSUM_CALCULATION", "when_required");
    PluginSession session(0, cancel_stalled_transfer);

    const auto download = download_path(local);
    std::string prefix;
    int flags{};
    SECTION("new download") {}
    SECTION("concurrent download to the same destination is rejected")
    {
        reject = false;
        probe_duplicate = true;
        CHECK(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(),
                              FS_COPYFLAGS_OVERWRITE, nullptr) == FS_FILE_OK);
        CHECK(duplicate_result == FS_FILE_WRITEERROR);
        CHECK(read_file(local) == object);
        return;
    }
    SECTION("new download spans multiple response reads")
    {
        reject = false;
        object.assign(128 * 1024, 'x');
        REQUIRE(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(), 0, nullptr) ==
                FS_FILE_OK);
        CHECK(read_file(local) == object);
        return;
    }
    SECTION("equal size cannot be resumed without listing metadata")
    {
        reject = false;
        std::ofstream(download, std::ios::binary) << std::string(object.size(), 'x');
        CHECK(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(),
                              FS_COPYFLAGS_RESUME | FS_COPYFLAGS_MOVE,
                              nullptr) == FS_FILE_NOTSUPPORTED);
        CHECK(read_file(download) == std::string(object.size(), 'x'));
        return;
    }
    SECTION("ignored range leaves the prefix intact")
    {
        ignore_range = true;
        std::ofstream(download, std::ios::binary) << "prefix";
        write_resume_record(config, local, u"\\resume-test\\bucket\\object", "\"test\"",
                            object.size());
        CHECK(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(), FS_COPYFLAGS_RESUME,
                              nullptr) == FS_FILE_READERROR);
        CHECK(read_file(download) == "prefix");
        CHECK(head_requests == 0);
        return;
    }
    SECTION("resume percentage persists while GET is pending")
    {
        reject = false;
        delay = true;
        std::ofstream(local, std::ios::binary) << "existing target";
        std::ofstream(download, std::ios::binary) << object.substr(0, 6);
        write_resume_record(config, local, u"\\resume-test\\bucket\\object", "\"test\"",
                            object.size());
        RemoteInfoStruct info{};
        info.SizeLow = static_cast<DWORD>(object.size());
        const RemoteInfoStruct* listed = &info;
        SECTION("listed size available") {}
        SECTION("listing size unavailable")
        {
            listed = nullptr;
        }
        REQUIRE(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(),
                                FS_COPYFLAGS_RESUME, listed) == FS_FILE_OK);
        REQUIRE(transfer_percentages.size() >= 4);
        const auto expected = static_cast<int>(6 * 100 / object.size());
        CHECK(transfer_percentages.front() == expected);
        bool validated = true;
        int resumed_reports{};
        for (const auto percent : transfer_percentages)
        {
            if (percent == expected)
                ++resumed_reports;
            if (percent >= expected)
                validated = true;
            if (validated)
                CHECK(percent >= expected);
        }
        CHECK(resumed_reports >= 2);
        CHECK(transfer_percentages.back() == 100);
        CHECK(head_requests == 0);
        CHECK(read_file(local) == object);
        CHECK_FALSE(std::filesystem::exists(download));
        return;
    }
    SECTION("cancel while waiting for response headers")
    {
        stall = true;
        std::ofstream(local, std::ios::binary) << "existing target";
        CHECK(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(),
                              FS_COPYFLAGS_OVERWRITE, nullptr) == FS_FILE_USERABORT);
        CHECK(transfer_canceled);
        CHECK_FALSE(transfer_wrong_thread);
        CHECK(read_file(local) == "existing target");
        return;
    }
    SECTION("missing listing metadata skips the resume record")
    {
        reject = false;
        std::filesystem::create_directories(config.path.parent_path() / u"resume.toml");
        CHECK(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(),
                              FS_COPYFLAGS_OVERWRITE, nullptr) == FS_FILE_OK);
        CHECK_FALSE(std::filesystem::exists(download));
        return;
    }
    SECTION("resumed download")
    {
        prefix = object.substr(0, 6);
        flags = FS_COPYFLAGS_RESUME;
        std::ofstream(download, std::ios::binary) << prefix;
        write_resume_record(config, local, u"\\resume-test\\bucket\\object", "\"test\"",
                            object.size());
    }
    SECTION("cancel upload while waiting for response headers")
    {
        stall = true;
        std::ofstream(local, std::ios::binary) << object;
        CHECK(s3cmd::put_file(local.u16string().c_str(), u"\\resume-test\\bucket\\object", FS_COPYFLAGS_MOVE) ==
              FS_FILE_USERABORT);
        CHECK(transfer_canceled);
        CHECK_FALSE(transfer_wrong_thread);
        CHECK(std::filesystem::exists(local));
        return;
    }
    REQUIRE(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(), flags, nullptr) ==
            (flags == FS_COPYFLAGS_RESUME ? FS_FILE_NOTSUPPORTED : FS_FILE_READERROR));
    REQUIRE(read_file(download) == prefix);
    CHECK_FALSE(std::filesystem::exists(local));
    if (flags == FS_COPYFLAGS_RESUME)
        CHECK(if_match == "\"test\"");
    reject = false;
    REQUIRE(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(), FS_COPYFLAGS_OVERWRITE,
                            nullptr) == FS_FILE_OK);
    CHECK(read_file(local) == object);
    CHECK_FALSE(std::filesystem::exists(download));
}

TEST_CASE("Get resume state survives restart and rejects a changed object", "[unit]")
{
    auto& config = temporary_config();
    std::string object = "prefix and the rest of the object";
    std::string e_tag = "\"original\"";
    std::string if_match;
    bool interrupt = true;
    transfer_waiting = false;
    transfer_canceled = false;
    transfer_wrong_thread = false;
    transfer_caller = std::this_thread::get_id();

    httplib::Server server;
    server.Get("/bucket", [&](const httplib::Request&, httplib::Response& response) {
        response.set_content(
            "<ListBucketResult><IsTruncated>false</IsTruncated><Contents><Key>object</Key>"
            "<ETag>&quot;original&quot;</ETag><Size>" +
                std::to_string(object.size()) + "</Size></Contents></ListBucketResult>",
            "application/xml");
    });
    server.Get("/bucket/object", [&](const httplib::Request& request, httplib::Response& response) {
        if_match = request.get_header_value("If-Match");
        response.set_header("ETag", e_tag);
        if (!if_match.empty() && if_match != e_tag)
        {
            response.status = 412;
            response.set_content("<Error><Code>PreconditionFailed</Code></Error>",
                                 "application/xml");
            return;
        }
        if (!interrupt)
        {
            response.set_content(object, "application/octet-stream");
            return;
        }
        response.set_content_provider(
            object.size(), "application/octet-stream",
            [&](size_t offset, size_t length, httplib::DataSink& sink) {
                const auto count = std::min<std::size_t>(6, length);
                if (!sink.write(object.data() + offset, count))
                    return false;
                transfer_waiting = true;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!transfer_canceled && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                return false;
            });
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
    TemporaryEnvironment region("AWS_DEFAULT_REGION", "us-east-1");
    TemporaryEnvironment checksum("AWS_REQUEST_CHECKSUM_CALCULATION", "when_required");
    REQUIRE(s3cmd::ProfileConfig("resume-test").register_bucket("bucket", "us-east-1"));
    const auto local = config.root / u"download";
    const auto download = download_path(local);
    std::ofstream(local, std::ios::binary) << "existing target";

    {
        PluginSession session(0, cancel_stalled_transfer);
        char16_t directory[] = u"\\resume-test\\bucket";
        WfxFindData entry{};
        const auto handle = s3cmd::find_first(directory, &entry);
        REQUIRE(handle != INVALID_HANDLE_VALUE);
        s3cmd::find_close(handle);
        RemoteInfoStruct info{};
        info.SizeLow = entry.nFileSizeLow;
        info.SizeHigh = entry.nFileSizeHigh;
        CHECK(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(),
                              FS_COPYFLAGS_OVERWRITE, &info) == FS_FILE_USERABORT);
    }
    REQUIRE(read_file(local) == "existing target");
    REQUIRE(std::filesystem::file_size(download) == 6);

    interrupt = false;
    e_tag = "\"replacement\"";
    transfer_waiting = false;
    transfer_canceled = false;
    if_match.clear();
    RemoteInfoStruct info{};
    info.SizeLow = static_cast<DWORD>(object.size());
    {
        PluginSession session;
        CHECK(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(), 0, &info) ==
              FS_FILE_EXISTSRESUMEALLOWED);
        CHECK(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(), FS_COPYFLAGS_RESUME,
                              &info) == FS_FILE_NOTSUPPORTED);
        CHECK(if_match == "\"original\"");
        CHECK(read_file(local) == "existing target");
        CHECK(read_file(download) == "prefix");
        CHECK(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(), 0, &info) ==
              FS_FILE_EXISTS);
        CHECK(s3cmd::get_file(u"\\resume-test\\bucket\\object", local.u16string().c_str(),
                              FS_COPYFLAGS_OVERWRITE, &info) == FS_FILE_OK);
    }
    CHECK(read_file(local) == object);
    CHECK_FALSE(std::filesystem::exists(download));
    CHECK_FALSE(transfer_wrong_thread);
}

TEST_CASE("AWS SDK lifecycle is idempotent on its owner thread", "[unit]")
{
    temporary_config();
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

TEST_CASE("IMDS is opt-in without changing the environment", "[unit]")
{
    auto& config = temporary_config();
    TemporaryEnvironment metadata("AWS_EC2_METADATA_DISABLED", "original");
    bool enabled = false;
    std::string setting;
    SECTION("missing") {}
    SECTION("disabled")
    {
        setting = "EnableIMDS = false";
    }
    SECTION("invalid type")
    {
        setting = "EnableIMDS = \"true\"";
    }
    SECTION("enabled")
    {
        setting = "EnableIMDS = true";
        enabled = true;
    }
    std::filesystem::create_directories(config.path.parent_path());
    std::ofstream(config.path) << "[settings]\n" << setting << '\n';
    {
        PluginSession session;
        CHECK(Aws::Environment::GetEnv("AWS_EC2_METADATA_DISABLED") == "original");
        if (!enabled)
        {
            const auto metadata_client = Aws::Internal::GetEC2MetadataClient();
            REQUIRE(metadata_client);
            CHECK(metadata_client->GetCurrentRegion().empty());
            CHECK(metadata_client->GetDefaultCredentialsSecurely().empty());
        }
        REQUIRE(s3cmd::ProfileConfig("work").register_bucket("bucket", "eu-central-1"));
    }
    CHECK(Aws::Environment::GetEnv("AWS_EC2_METADATA_DISABLED") == "original");
    {
        PluginSession session;
        CHECK(Aws::Environment::GetEnv("AWS_EC2_METADATA_DISABLED") == "original");
        if (!enabled)
            CHECK(Aws::Internal::GetEC2MetadataClient()->GetCurrentRegion().empty());
    }
    CHECK(Aws::Environment::GetEnv("AWS_EC2_METADATA_DISABLED") == "original");
}

TEST_CASE("credential chain preserves provider order and gates IMDS", "[unit]")
{
    using namespace Aws::Auth;
    temporary_config();
    TemporaryEnvironment relative("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI", "");
    TemporaryEnvironment absolute("AWS_CONTAINER_CREDENTIALS_FULL_URI", "");
    TemporaryEnvironment metadata("AWS_EC2_METADATA_DISABLED", "false");
    PluginSession session;
    Aws::Client::ClientConfiguration config(
        Aws::Client::ClientConfigurationInitValues{.shouldDisableIMDS = true});
    std::size_t expected = 5;
    SECTION("disabled") {}
    SECTION("enabled")
    {
        config.disableIMDS = false;
        config.credentialProviderConfig.imdsConfig.disableImds = false;
        expected = 6;
    }
    SECTION("environment still disables IMDS")
    {
        config.disableIMDS = false;
        metadata.set("true");
    }
    SECTION("container credentials remain available")
    {
        relative.set("/credentials");
        expected = 6;
    }
    const s3cmd::CredentialsProviderChain chain(config);
    const auto& providers = chain.GetProviders();
    REQUIRE(providers.size() == expected);
    CHECK(dynamic_cast<EnvironmentAWSCredentialsProvider*>(providers[0].get()));
    CHECK(dynamic_cast<ProfileConfigFileAWSCredentialsProvider*>(providers[1].get()));
    CHECK(dynamic_cast<ProcessCredentialsProvider*>(providers[2].get()));
    CHECK(dynamic_cast<STSAssumeRoleWebIdentityCredentialsProvider*>(providers[3].get()));
    CHECK(dynamic_cast<SSOCredentialsProvider*>(providers[4].get()));
    if (expected == 6)
    {
        if (config.disableIMDS)
            CHECK(dynamic_cast<GeneralHTTPCredentialsProvider*>(providers[5].get()));
        else
            CHECK(dynamic_cast<InstanceProfileCredentialsProvider*>(providers[5].get()));
    }
}

TEST_CASE("UTF-16 strings preserve Unicode and reject malformed encoding", "[unit]")
{
    const std::u16string utf16 = u"r\u00e9sum\u00e9/\U0001f680";
    const std::string utf8 = "r\xc3\xa9sum\xc3\xa9/\xf0\x9f\x9a\x80";
    CHECK(s3cmd::to_utf16(utf8) == utf16);
    CHECK(s3cmd::to_utf8(utf16) == utf8);
    CHECK(s3cmd::to_utf16("").empty());
    CHECK(s3cmd::to_utf8(u"").empty());
    CHECK(s3cmd::to_utf8(s3cmd::to_utf16(std::string("a\0b", 3))) == std::string("a\0b", 3));
    CHECK_THROWS_AS(s3cmd::to_utf16("\xc0\xaf"), std::runtime_error);
    CHECK_THROWS_AS(s3cmd::to_utf8(std::u16string(1, 0xd800)), std::runtime_error);
    CHECK_THROWS_AS(s3cmd::to_utf8(std::u16string(1, 0xdc00)), std::runtime_error);
}

TEST_CASE("UTF-16 comparisons respect case, length, and terminators", "[unit]")
{
    CHECK(s3cmd::caseicmp(u"Open", u"OPEN") == 0);
    CHECK(s3cmd::caseicmp(u"", u"") == 0);
    CHECK(s3cmd::caseicmp(u"ab", u"ABC") < 0);
    CHECK(s3cmd::caseicmp(u"b", u"A") > 0);
    CHECK(s3cmd::ncaseicmp(u"Quote object", u"QUOTE ", 6) == 0);
    CHECK(s3cmd::ncaseicmp(u"a", u"b", 0) == 0);
    CHECK(s3cmd::ncaseicmp(u"a", u"ab", 2) < 0);
}

TEST_CASE("host dialogs format UTF-16 strings and numbers", "[unit]")
{
    s3cmd::PluginHost host(0, nullptr, nullptr, capture_request);
    request_count = 0;
    request_accept = true;
    CHECK(host.notify_message_box(s3cmd::PluginHost::message_box_type::msg_ok, u"s3cmd",
                                  u"{}: {}", u"\U0001f680", 42));
    CHECK(request_count == 1);
    CHECK(request_text == u"\U0001f680: 42");
}

TEST_CASE("remote paths identify profile, bucket, and object", "[unit]")
{
    CHECK(s3cmd::RemotePath::make(u"/") == s3cmd::RemotePath{});
    CHECK(s3cmd::RemotePath::make(u"/work") == s3cmd::RemotePath{"work", {}, {}});
    CHECK(s3cmd::RemotePath::make(u"/work/my-bucket") ==
          s3cmd::RemotePath{"work", "my-bucket", {}});
    CHECK(s3cmd::RemotePath::make(u"/work/my-bucket/one/two.txt") ==
          s3cmd::RemotePath{"work", "my-bucket", "one/two.txt"});
    CHECK(s3cmd::RemotePath::make(u"//work/my-bucket/one//") ==
          s3cmd::RemotePath{"work", "my-bucket", "one"});
    CHECK(s3cmd::RemotePath::make(u"\\") == s3cmd::RemotePath{});
    CHECK(s3cmd::RemotePath::make(u"\\work") == s3cmd::RemotePath{"work", {}, {}});
    CHECK(s3cmd::RemotePath::make(u"\\work\\my-bucket") ==
          s3cmd::RemotePath{"work", "my-bucket", {}});
    CHECK(s3cmd::RemotePath::make(u"\\work\\my-bucket\\one\\two.txt") ==
          s3cmd::RemotePath{"work", "my-bucket", "one/two.txt"});
    CHECK(s3cmd::RemotePath::make(u"\\\\work\\my-bucket\\one\\\\") ==
          s3cmd::RemotePath{"work", "my-bucket", "one"});
}

TEST_CASE("remote path views borrow the original UTF-16 path", "[unit]")
{
    using s3cmd::RemotePathView;
    CHECK(RemotePathView::make({}) == RemotePathView{});
    CHECK(RemotePathView::make(u"/") == RemotePathView{});
    CHECK(RemotePathView::make(u"//") == RemotePathView{});
    CHECK(RemotePathView::make(u"/work/") == RemotePathView{u"work", {}, {}});
    CHECK(RemotePathView::make(u"/work/bucket/") == RemotePathView{u"work", u"bucket", {}});
    CHECK(RemotePathView::make(u"//work/bucket/one//") ==
          RemotePathView{u"work", u"bucket", u"one"});
    CHECK(RemotePathView::make(u"\\\\") == RemotePathView{});
    CHECK(RemotePathView::make(u"\\work\\") == RemotePathView{u"work", {}, {}});
    CHECK(RemotePathView::make(u"\\work\\bucket\\") == RemotePathView{u"work", u"bucket", {}});
    CHECK(RemotePathView::make(u"\\work\\..\\") == RemotePathView{u"work", u"..", {}});
    CHECK(RemotePathView::make(u"\\\\work\\bucket\\one\\\\") ==
          RemotePathView{u"work", u"bucket", u"one"});

    const std::u16string path = u"\\w\u00f3rk\\bucket\\one\\two.txt";
    const auto view = RemotePathView::make(path);
    CHECK(view == RemotePathView{u"w\u00f3rk", u"bucket", u"one\\two.txt"});
    CHECK(view.profile.data() == path.data() + 1);
    CHECK(view.bucket.data() == path.data() + 6);
    CHECK(view.key.data() == path.data() + 13);
    CHECK(s3cmd::RemotePath::make(path) ==
          s3cmd::RemotePath{"w\xc3\xb3rk", "bucket", "one/two.txt"});

    const std::u16string linux_path = u"/w\u00f3rk/bucket/one/two.txt";
    const auto linux_view = RemotePathView::make(linux_path);
    CHECK(linux_view == RemotePathView{u"w\u00f3rk", u"bucket", u"one/two.txt"});
    CHECK(linux_view.profile.data() == linux_path.data() + 1);
    CHECK(linux_view.bucket.data() == linux_path.data() + 6);
    CHECK(linux_view.key.data() == linux_path.data() + 13);
    CHECK(s3cmd::RemotePath::make(linux_path) ==
          s3cmd::RemotePath{"w\xc3\xb3rk", "bucket", "one/two.txt"});
}

TEST_CASE("virtual deletes skip recursive directory listings", "[unit]")
{
    temporary_config();
    const s3cmd::ProfileConfig config("work");
    REQUIRE(config.register_bucket("bucket", "eu-central-1"));

    int delete_status{204};
    std::string error;
    bool discovered_only{};
    SECTION("deletes an empty registered bucket") {}
    SECTION("deletes an empty discovered bucket")
    {
        discovered_only = true;
    }
    SECTION("refuses a bucket containing objects")
    {
        delete_status = 409;
        error = "BucketNotEmpty";
    }
    SECTION("keeps registration when AWS denies deletion")
    {
        delete_status = 403;
        error = "AccessDenied";
    }
    if (discovered_only)
    {
        REQUIRE(config.unregister_bucket("bucket"));
        config.set_discovered_buckets({{"bucket", {"eu-central-1"}}});
    }

    std::atomic<int> bucket_deletes{}, object_deletes{}, unexpected_requests{};
    httplib::Server server;
    server.set_pre_routing_handler(
        [&](const httplib::Request& request, httplib::Response& response) {
            if (request.method == "DELETE" && request.path == "/bucket")
            {
                ++bucket_deletes;
                response.status = delete_status;
                if (!error.empty())
                    response.set_content("<Error><Code>" + error + "</Code><Message>" + error +
                                             "</Message></Error>",
                                         "application/xml");
            }
            else if (request.method == "DELETE" && request.path == "/bucket/file.txt")
            {
                ++object_deletes;
                response.status = 204;
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
    TemporaryEnvironment region("AWS_DEFAULT_REGION", "us-east-1");
    PluginSession session;

    char16_t root[] = u"\\";
    char16_t profile[] = u"\\work";
    char16_t bucket[] = u"\\work\\bucket";
    WfxFindData entry{};

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
    CHECK_FALSE(s3cmd::delete_file(u"\\work\\bucket\\file.txt"));
    CHECK_FALSE(s3cmd::remove_directory(u"\\work\\bucket\\folder"));
    CHECK(s3cmd::remove_directory(bucket) == (delete_status == 204));
    CHECK(bucket_deletes == 1);
    CHECK(object_deletes == 0);
    CHECK(unexpected_requests == 0);
    CHECK(config.registered_buckets().contains("bucket") ==
          (!discovered_only && delete_status != 204));
    if (delete_status == 204)
        CHECK(config.bucket_region("bucket").empty());
    s3cmd::status_info(profile, FS_STATUS_END, FS_STATUS_OP_DELETE);
    CHECK(s3cmd::delete_file(u"\\work\\bucket\\file.txt"));
    CHECK(object_deletes == 1);
}

TEST_CASE("directory prefixes use S3 separators", "[unit]")
{
    CHECK(s3cmd::RemotePath{"work", "bucket", ""}.directory_prefix().empty());
    CHECK(s3cmd::RemotePath{"work", "bucket", "one/two"}.directory_prefix() == "one/two/");
    CHECK(s3cmd::RemotePath{"work", "bucket", "one/two/"}.directory_prefix() == "one/two/");
}

TEST_CASE("remote files are delegated to the host for opening", "[unit]")
{
    char16_t object[] = u"\\work\\bucket\\file.txt";
    char16_t bucket[] = u"\\work\\bucket";
    char16_t named_object[] = u"\\work\\bucket\\_Add existing bucket_";
    CHECK(s3cmd::execute_file(object, u"open") == FS_EXEC_YOURSELF);
    CHECK(s3cmd::execute_file(object, u"OPEN") == FS_EXEC_YOURSELF);
    CHECK(s3cmd::execute_file(bucket, u"open") == FS_EXEC_OK);
    CHECK(s3cmd::execute_file(object, u"MODE I") == FS_EXEC_YOURSELF);
    CHECK(s3cmd::execute_file(named_object, u"open") == FS_EXEC_YOURSELF);
}

TEST_CASE("Add existing bucket opens a local registration prompt", "[unit]")
{
    temporary_config();
    request_count = 0;
    request_accept = true;
    request_inputs = {u"existing-bucket"};
    bool location_allowed{true};
    bool callback_available{true};
    bool expect_registration{true};
    SECTION("discovers the region and persists registration") {}
    SECTION("asks for the region when discovery fails")
    {
        location_allowed = false;
        request_inputs.push_back(u"eu-west-1");
    }
    SECTION("cancel leaves the registry unchanged")
    {
        request_accept = false;
        expect_registration = false;
    }
    SECTION("empty input leaves the registry unchanged")
    {
        request_inputs = {u""};
        expect_registration = false;
    }
    SECTION("a path cannot register a directory marker")
    {
        request_inputs = {u"existing-bucket/folder"};
        expect_registration = false;
    }
    SECTION("a missing callback fails without discovery")
    {
        callback_available = false;
        expect_registration = false;
    }

    std::atomic<int> location_requests{};
    std::atomic<bool> saw_location_query{};
    httplib::Server server;
    server.Get("/existing-bucket", [&](const httplib::Request& request,
                                       httplib::Response& response) {
        ++location_requests;
        saw_location_query = request.has_param("location");
        if (location_allowed)
            response.set_content("<LocationConstraint>eu-west-1</LocationConstraint>",
                                 "application/xml");
        else
        {
            response.status = 403;
            response.set_content("<Error><Code>AccessDenied</Code><Message>test</Message></Error>",
                                 "application/xml");
        }
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
    TemporaryEnvironment region("AWS_DEFAULT_REGION", "us-east-1");
    PluginSession session(0, nullptr, nullptr, callback_available ? answer_request : nullptr);

    char16_t remote_name[] = u"\\work\\_Add existing bucket_";
    CHECK(s3cmd::execute_file(remote_name, u"OPEN") ==
          (expect_registration ? FS_EXEC_SYMLINK : FS_EXEC_ERROR));
    CHECK(std::u16string_view(remote_name) ==
          (expect_registration ? u"\\work" : u"\\work\\_Add existing bucket_"));
    CHECK(request_count == (callback_available ? (location_allowed ? 1 : 2) : 0));
    CHECK(location_requests == (expect_registration ? 1 : 0));
    CHECK(saw_location_query == expect_registration);
    const s3cmd::ProfileConfig profile("work");
    const auto registered = profile.registered_buckets();
    if (expect_registration)
    {
        REQUIRE(registered.contains("existing-bucket"));
        CHECK(registered.at("existing-bucket").region == "eu-west-1");
        s3cmd::reset_config();
        CHECK(profile.registered_buckets().contains("existing-bucket"));
    }
    else
        CHECK(registered.empty());
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

TEST_CASE("directly discovered bucket regions remain in memory", "[unit]")
{
    temporary_config();
    const s3cmd::ProfileConfig profile("work");

    profile.cache_bucket_region("direct-bucket", "eu-west-2");

    CHECK(profile.bucket_region("direct-bucket") == "eu-west-2");
    CHECK_FALSE(profile.has_discovered_buckets());
}

TEST_CASE("bucket registration is disabled when buckets were discovered", "[unit]")
{
    temporary_config();
    const s3cmd::ProfileConfig profile("work");
    profile.set_discovered_buckets({{"discovered-bucket", {"eu-west-1"}}});

    request_count = 0;
    request_accept = true;
    request_inputs = {u"new-bucket"};
    PluginSession prompted(0, nullptr, nullptr, answer_request);
    char16_t action[] = u"\\work\\_Add existing bucket_";
    CHECK(s3cmd::execute_file(action, u"open") == FS_EXEC_ERROR);
    CHECK_FALSE(profile.registered_buckets().contains("new-bucket"));
}

TEST_CASE("bucket registration rejects an already registered bucket", "[unit]")
{
    temporary_config();
    const s3cmd::ProfileConfig profile("work");
    REQUIRE(profile.register_bucket("registered-bucket", "eu-central-1"));

    request_count = 0;
    request_accept = true;
    request_inputs = {u"registered-bucket"};
    PluginSession prompted(0, nullptr, nullptr, answer_request);
    char16_t action[] = u"\\work\\_Add existing bucket_";
    CHECK(s3cmd::execute_file(action, u"open") == FS_EXEC_ERROR);
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
    CHECK(s3cmd::content_get_supported_field(1, name, units, sizeof(name)) == ft_string);
    CHECK(std::string_view(name) == "Storage class");
    CHECK(s3cmd::content_get_supported_field(2, name, units, sizeof(name)) == ft_string);
    CHECK(std::string_view(name) == "ETag");
    CHECK(s3cmd::content_get_supported_field(3, name, units, sizeof(name)) == ft_nomorefields);

    char16_t bucket_path[] = u"\\work\\owned-bucket";
    char16_t value[32]{};
    {
        PluginSession session;
        CHECK(s3cmd::content_get_value(bucket_path, 0, value, sizeof(value)) == ft_stringw);
        CHECK(std::u16string_view(value) == u"ap-southeast-1");

        char16_t short_value[4]{u'?', u'?', u'?', u'?'};
        CHECK(s3cmd::content_get_value(bucket_path, 0, short_value, 7) == ft_stringw);
        CHECK(std::u16string_view(short_value) == u"ap");
        CHECK(short_value[3] == u'?');
        CHECK(s3cmd::content_get_value(bucket_path, 0, short_value, 1) == ft_fileerror);

        s3cmd::reset_config();
        CHECK(s3cmd::content_get_value(bucket_path, 0, value, sizeof(value)) == ft_fieldempty);

        char16_t object_path[] = u"\\work\\owned-bucket\\file.txt";
        CHECK(s3cmd::content_get_value(object_path, 0, value, sizeof(value)) == ft_fieldempty);
    }

    PluginSession session;
    CHECK(s3cmd::content_get_value(bucket_path, 0, value, sizeof(value)) == ft_fieldempty);
}

TEST_CASE("Object metadata retention is bounded by entry count", "[unit]")
{
    using namespace s3cmd;
    using Aws::S3::Model::ObjectStorageClass;
    const MetadataDirectory<> a{u"work", u"bucket", u"a\\"};
    const MetadataDirectory<> b{u"work", u"bucket", u"b\\"};
    const MetadataDirectory<> c{u"work", u"bucket", u"c\\"};
    const ObjectMetadataMap objects{{u"file.txt", {ObjectStorageClass::STANDARD, u"abc123"}}};
    ObjectMetadataCache cache(4);
    cache.add(a, objects);
    cache.add(b, objects);
    REQUIRE(cache.retained_entries() == 4);

    SECTION("a successful lookup retains the older directory")
    {
        REQUIRE(cache.find(a, u"file.txt"));
        cache.add(c, objects);
        CHECK(cache.find(b, u"file.txt") == nullptr);
        CHECK(cache.find(a, u"file.txt") != nullptr);
        CHECK(cache.find(c, u"file.txt") != nullptr);
        CHECK(cache.retained_entries() == 4);
        cache.add(b, objects);
        CHECK(cache.find(b, u"file.txt") != nullptr);
    }
    SECTION("misses do not update recency")
    {
        CHECK(cache.find(a, u"missing") == nullptr);
        cache.add(c, objects);
        CHECK(cache.find(a, u"file.txt") == nullptr);
        CHECK(cache.find(b, u"file.txt") != nullptr);
    }
    SECTION("replacement updates recency without accumulating entries")
    {
        cache.add(a, objects);
        CHECK(cache.retained_entries() == 4);
        cache.add(c, objects);
        CHECK(cache.find(b, u"file.txt") == nullptr);
        CHECK(cache.find(a, u"file.txt") != nullptr);
    }
    SECTION("an oversized directory stays complete until a new publication")
    {
        ObjectMetadataMap large{{u"one", {ObjectStorageClass::STANDARD, u"first"}},
                                {u"two", {ObjectStorageClass::STANDARD, u"second"}},
                                {u"three", {ObjectStorageClass::STANDARD, u"third"}},
                                {u"four", {ObjectStorageClass::STANDARD, u"fourth"}}};
        cache.add(c, std::move(large));
        CHECK(cache.retained_entries() == 5);
        CHECK(cache.find(a, u"file.txt") == nullptr);
        CHECK(cache.find(b, u"file.txt") == nullptr);
        CHECK(cache.find(c, u"one") != nullptr);
        CHECK(cache.find(c, u"two") != nullptr);
        cache.add(a, objects);
        CHECK(cache.find(c, u"one") == nullptr);
        CHECK(cache.retained_entries() == 2);
    }
    SECTION("empty replacements still consume one entry and clear old objects")
    {
        cache.add(a, {});
        CHECK(cache.find(a, u"file.txt") == nullptr);
        CHECK(cache.retained_entries() == 3);
        ObjectMetadataCache empty_cache(1);
        empty_cache.add(a, {});
        empty_cache.add(b, {});
        CHECK(empty_cache.retained_entries() == 1);
    }
    SECTION("clear resets accounting and permits reuse")
    {
        cache.clear();
        CHECK(cache.retained_entries() == 0);
        CHECK(cache.find(a, u"file.txt") == nullptr);
        cache.add(a, objects);
        CHECK(cache.retained_entries() == 2);
    }
}

TEST_CASE("Bucket listing shows a registration hint once per empty profile", "[unit]")
{
    temporary_config();
    bool listing_allowed{};
    bool registered{};
    bool callback_available{true};
    SECTION("empty fallback shows a persistent hint") {}
    SECTION("successful empty listing needs no hint")
    {
        listing_allowed = true;
    }
    SECTION("registered fallback needs no hint")
    {
        registered = true;
    }
    SECTION("missing callback does not consume the hint")
    {
        callback_available = false;
    }

    httplib::Server server;
    server.Get("/", [&](const httplib::Request&, httplib::Response& response) {
        if (listing_allowed)
            response.set_content("<ListAllMyBucketsResult><Buckets/></ListAllMyBucketsResult>",
                                 "application/xml");
        else
        {
            response.status = 403;
            response.set_content("<Error><Code>AccessDenied</Code><Message>test</Message></Error>",
                                 "application/xml");
        }
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
    TemporaryEnvironment region("AWS_DEFAULT_REGION", "us-east-1");
    if (registered)
        REQUIRE(s3cmd::ProfileConfig("work").register_bucket("private-bucket", "us-east-1"));

    request_count = 0;
    request_accept = true;
    {
        PluginSession session(0, nullptr, nullptr, callback_available ? capture_request : nullptr);
        for (int refresh = 0; refresh < 2; ++refresh)
        {
            WfxFindData entry{};
            const auto handle = s3cmd::find_first(u"\\work", &entry);
            if (!listing_allowed)
            {
                REQUIRE(handle != INVALID_HANDLE_VALUE);
                CHECK(std::u16string_view(entry.cFileName) == u"_Add existing bucket_");
                CHECK_FALSE(entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
                if (registered)
                {
                    REQUIRE(s3cmd::find_next(handle, &entry));
                    CHECK(std::u16string_view(entry.cFileName) == u"private-bucket");
                }
                CHECK_FALSE(s3cmd::find_next(handle, &entry));
                s3cmd::find_close(handle);
            }
            else
            {
                CHECK(handle == INVALID_HANDLE_VALUE);
                CHECK(GetLastError() == ERROR_NO_MORE_FILES);
            }
        }
        const bool expect_hint = !listing_allowed && !registered && callback_available;
        CHECK(request_count == (expect_hint ? 1 : 0));
        if (expect_hint)
        {
            CHECK(request_type == RT_MsgOK);
            CHECK(request_text.find(u"work") != request_text.npos);
            CHECK(request_text.find(u"_Add existing bucket_") != request_text.npos);
            CHECK(request_text.find(u"existing bucket") != request_text.npos);
        }
    }
    PluginSession restarted(0, nullptr, nullptr, capture_request);
    WfxFindData entry{};
    const auto handle = s3cmd::find_first(u"\\work", &entry);
    if (handle != INVALID_HANDLE_VALUE)
        s3cmd::find_close(handle);
    CHECK(request_count == (listing_allowed || registered ? 0 : 1));
    if (!listing_allowed && !registered)
    {
        const auto other = s3cmd::find_first(u"\\other", &entry);
        REQUIRE(other != INVALID_HANDLE_VALUE);
        s3cmd::find_close(other);
        CHECK(request_count == 2);
    }
}

TEST_CASE("ListObjectsV2 metadata is exposed as content fields", "[unit]")
{
    temporary_config();
    const s3cmd::ProfileConfig profile("work");
    REQUIRE(profile.register_bucket("owned-bucket", "us-east-1"));

    std::atomic<int> list_requests{};
    // 0: original, 1: empty, 2: two pages, 3: second page fails.
    std::atomic<int> listing_mode{};
    std::atomic<bool> saw_previous_snapshot{};
    httplib::Server server;
    server.Get("/owned-bucket", [&](const httplib::Request& request, httplib::Response& response) {
        ++list_requests;
        const auto mode = listing_mode.load();
        const auto second_page = request.has_param("continuation-token");
        if (second_page)
        {
            char16_t value[32]{};
            saw_previous_snapshot =
                s3cmd::content_get_value(u"\\work\\owned-bucket\\folder\\file.txt", 2, value,
                                         sizeof(value)) == ft_stringw &&
                std::u16string_view(value) == u"abc123";
        }
        if (mode == 3 && second_page)
        {
            response.status = 403;
            response.set_content("<Error><Code>AccessDenied</Code><Message>test</Message></Error>",
                                 "application/xml");
            return;
        }
        if (mode != 0)
        {
            std::string xml = "<ListBucketResult>";
            xml += mode >= 2 && !second_page
                       ? "<IsTruncated>true</IsTruncated><NextContinuationToken>next</"
                         "NextContinuationToken>"
                       : "<IsTruncated>false</IsTruncated>";
            if (mode >= 2)
            {
                xml += "<Contents><Key>folder/";
                xml += second_page ? "other.txt" : "file.txt";
                xml += "</Key><ETag>&quot;updated&quot;</ETag><Size>3</Size>"
                       "<StorageClass>STANDARD_IA</StorageClass></Contents>";
            }
            response.set_content(xml + "</ListBucketResult>", "application/xml");
            return;
        }
        response.set_content(
            R"xml(<?xml version="1.0" encoding="UTF-8"?>
<ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">
  <Name>owned-bucket</Name>
  <Prefix>folder/</Prefix>
  <Delimiter>/</Delimiter>
  <KeyCount>1</KeyCount>
  <MaxKeys>1000</MaxKeys>
  <IsTruncated>false</IsTruncated>
  <Contents>
    <Key>folder/file.txt</Key>
    <LastModified>2026-09-13T00:00:00.000Z</LastModified>
    <ETag>&quot;abc123&quot;</ETag>
    <Size>3</Size>
    <StorageClass>STANDARD</StorageClass>
  </Contents>
</ListBucketResult>)xml",
            "application/xml");
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
    TemporaryEnvironment region("AWS_DEFAULT_REGION", "us-east-1");
    PluginSession session;

    char16_t path[] = u"\\work\\owned-bucket\\folder";
    WfxFindData entry{};
    const auto handle = s3cmd::find_first(path, &entry);
    REQUIRE(handle != INVALID_HANDLE_VALUE);
    CHECK(std::u16string_view(entry.cFileName) == u"file.txt");
    s3cmd::find_close(handle);

    char16_t object_path[] = u"\\work\\owned-bucket\\folder\\file.txt";
    char16_t value[32]{};
    CHECK(s3cmd::content_get_value(object_path, 1, value, sizeof(value)) == ft_stringw);
    CHECK(std::u16string_view(value) == u"STANDARD");
    CHECK(s3cmd::content_get_value(object_path, 2, value, sizeof(value)) == ft_stringw);
    CHECK(std::u16string_view(value) == u"abc123");
    CHECK(list_requests == 1);

    SECTION("an empty listing removes previous metadata")
    {
        listing_mode = 1;
        CHECK(s3cmd::find_first(path, &entry) == INVALID_HANDLE_VALUE);
        CHECK(GetLastError() == ERROR_NO_MORE_FILES);
        CHECK(s3cmd::content_get_value(object_path, 2, value, sizeof(value)) == ft_fieldempty);
        CHECK(list_requests == 2);
    }
    SECTION("all pages are published together")
    {
        listing_mode = 2;
        const auto refreshed = s3cmd::find_first(path, &entry);
        REQUIRE(refreshed != INVALID_HANDLE_VALUE);
        s3cmd::find_close(refreshed);
        CHECK(saw_previous_snapshot);
        CHECK(s3cmd::content_get_value(object_path, 2, value, sizeof(value)) == ft_stringw);
        CHECK(std::u16string_view(value) == u"updated");
        CHECK(s3cmd::content_get_value(u"\\work\\owned-bucket\\folder\\other.txt", 1, value,
                                       sizeof(value)) == ft_stringw);
        CHECK(std::u16string_view(value) == u"STANDARD_IA");
        CHECK(list_requests == 3);
    }
    SECTION("a later page failure preserves the previous snapshot")
    {
        listing_mode = 3;
        CHECK(s3cmd::find_first(path, &entry) == INVALID_HANDLE_VALUE);
        CHECK(saw_previous_snapshot);
        CHECK(s3cmd::content_get_value(object_path, 2, value, sizeof(value)) == ft_stringw);
        CHECK(std::u16string_view(value) == u"abc123");
        CHECK(s3cmd::content_get_value(u"\\work\\owned-bucket\\folder\\other.txt", 2, value,
                                       sizeof(value)) == ft_fieldempty);
        CHECK(list_requests == 3);
    }
    SECTION("missing metadata stays empty until normal listing restores it")
    {
        s3cmd::reset_config();
        CHECK(s3cmd::content_get_value(object_path, 1, value, sizeof(value)) == ft_fieldempty);
        CHECK(s3cmd::content_get_value(object_path, 2, value, sizeof(value)) == ft_fieldempty);
        CHECK(list_requests == 1);
        const auto refreshed = s3cmd::find_first(path, &entry);
        REQUIRE(refreshed != INVALID_HANDLE_VALUE);
        s3cmd::find_close(refreshed);
        CHECK(s3cmd::content_get_value(object_path, 2, value, sizeof(value)) == ft_stringw);
        CHECK(std::u16string_view(value) == u"abc123");
        CHECK(list_requests == 2);
    }
    SECTION("shutdown clears metadata")
    {
        s3cmd::shutdown();
        CHECK(s3cmd::content_get_value(object_path, 2, value, sizeof(value)) == ft_fieldempty);
        CHECK(list_requests == 1);
    }
}

TEST_CASE("expired SSO profiles require supported login configuration", "[unit]")
{
    bool legacy{};
    SECTION("session profile asks before browser login") {}
    SECTION("legacy profile reports migration and external login")
    {
        legacy = true;
    }

    auto& ini = temporary_config();
    const auto config = ini.root / u"config";
    const auto credentials = ini.root / u"credentials";
    {
        std::ofstream file(config);
        REQUIRE(file);
        file << "[profile expired]\n"
                "sso_account_id = 111122223333\n"
                "sso_role_name = ReadOnly\n"
                "region = eu-central-1\n";
        if (!legacy)
            file << "sso_session = expired\n[sso-session expired]\n";
        file << "sso_start_url = https://example.awsapps.com/start\n"
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

    char16_t path[] = u"\\expired";
    WfxFindData entry{};
    CHECK(s3cmd::find_first(path, &entry) == INVALID_HANDLE_VALUE);
    CHECK(GetLastError() == (legacy ? (DWORD)ERROR_LOGON_FAILURE : (DWORD)ERROR_CANCELLED));
    CHECK(request_count == 1);
    if (legacy)
    {
        CHECK(request_type == RT_MsgOK);
        CHECK(request_text.find(u"legacy configuration") != std::u16string::npos);
        CHECK(request_text.find(u"aws configure sso --profile \"expired\"") != std::u16string::npos);
        CHECK(request_text.find(u"aws sso login --profile \"expired\"") != std::u16string::npos);
        CHECK(request_text.find(u"Start browser login") == std::u16string::npos);
        CHECK(log_text.find(request_text) != std::u16string::npos);
    }
    else
    {
        CHECK(request_type == RT_MsgYesNo);
        CHECK(request_text.find(u"Start browser login") != std::u16string::npos);
        CHECK(request_text.find(u"aws sso login") == std::u16string::npos);
        CHECK(log_text.find(u"was cancelled") != std::u16string::npos);
    }

    CHECK(s3cmd::find_first(path, &entry) == INVALID_HANDLE_VALUE);
    CHECK(request_count == 2);
    request_accept = true;
}
