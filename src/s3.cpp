#include "s3.hpp"
#include "core.hpp"
#include "log.hpp"

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/auth/bearer-token-provider/SSOBearerTokenProvider.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/config/ConfigAndCredentialsCacheManager.h>
#include <aws/core/http/HttpResponse.h>
#include <aws/core/utils/Array.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/crypto/Factories.h>
#include <aws/core/utils/crypto/SecureRandom.h>
#include <aws/core/utils/logging/FormattedLogSystem.h>
#include <aws/core/utils/memory/AWSMemory.h>
#include <aws/core/utils/memory/stl/AWSAllocator.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3ClientConfiguration.h>
#include <aws/s3/model/CopyObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/GetBucketLocationRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/ListBucketsRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/sso-oidc/SSOOIDCClient.h>
#include <aws/sso-oidc/SSOOIDCErrors.h>
#include <aws/sso-oidc/model/CreateTokenRequest.h>
#include <aws/sso-oidc/model/CreateTokenResult.h>
#include <aws/sso-oidc/model/RegisterClientRequest.h>
#include <aws/sso-oidc/model/RegisterClientResult.h>
#include <aws/sso-oidc/model/StartDeviceAuthorizationRequest.h>
#include <httplib.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <debugapi.h>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <set>
#include <shellapi.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace s3cmd {

namespace {
constexpr std::uint64_t max_single_part_size = 5ULL * 1024 * 1024 * 1024;

class AwsLogSystem final : public Aws::Utils::Logging::FormattedLogSystem
{
public:
    using FormattedLogSystem::FormattedLogSystem;
    void Flush() override {}

private:
    void ProcessFormattedStatement(Aws::String&& statement) override
    {
        statement.pop_back(); // FormattedLogSystem always appends a newline
        log("{}", statement);
    }
};

Aws::Utils::Logging::LogLevel parse_log_level(std::string_view value)
{
    using enum Aws::Utils::Logging::LogLevel;
    if (value == "Off")
        return Off;
    if (value == "Fatal")
        return Fatal;
    if (value == "Error")
        return Error;
    if (value == "Warn")
        return Warn;
    if (value == "Debug")
        return Debug;
    if (value == "Trace")
        return Trace;
    return Info;
}

int plugin_number{};
tProgressProcW progress_proc{};
tLogProcW log_proc{};
tRequestProcW request_proc{};

std::shared_mutex aws_lifecycle_mtx;
Aws::SDKOptions aws_options;
DWORD aws_init_thread_id{};
bool aws_initialized{};

// All operations on RuntimeConfig requires a `config_mtx` mutex to be held
struct RuntimeConfig
{
    static RuntimeConfig& get();

    // Mirror the current config on the disk, at `path()`
    bool flush_to_disk();

    struct ProfileSettings
    {
        BucketMap registered_buckets;
        BucketMap discovered_buckets;
    };

    bool dry_run{};
    std::string aws_log_level{"Info"};
    std::map<std::string, ProfileSettings, std::less<>> profiles;

private:
    // Returns a path to the config file
    static const std::filesystem::path& path();
};

std::mutex config_mtx;
std::optional<RuntimeConfig> runtime_config;

// Total Commander recursively lists directories before calling FsRemoveDir. Profiles and buckets
// are virtual directories, so their delete operations must see an empty listing.
thread_local bool suppress_delete_listing{};

struct ClientEntry
{
    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> credentials;
    std::shared_ptr<Aws::S3::S3Client> client;
    bool uses_sso{};
};

std::mutex client_mutex;
using ClientKey = std::pair<std::string, std::string>; // (profile, region)
std::map<ClientKey, std::shared_ptr<ClientEntry>> clients;
std::mutex sso_login_mutex;

// AwsLease has two jobs:
// 1. Verify the SDK is initialized.
// 2. Hold a shared lifecycle lock for the entire AWS operation.
// Multiple operations can hold it concurrently, but shutdown() needs the exclusive lock.
struct AwsLease
{
    AwsLease() : lock(aws_lifecycle_mtx)
    {
        if (!aws_initialized)
            throw std::runtime_error("AWS SDK is not initialized");
    }

    AwsLease(const AwsLease&) = delete;
    AwsLease& operator=(const AwsLease&) = delete;

    std::shared_lock<std::shared_mutex> lock;
};

class SsoLoginFailed : public std::runtime_error
{
public:
    explicit SsoLoginFailed(std::string message) : std::runtime_error(std::move(message)) {}
};

bool ask_sso_login(std::string_view message, int request_type)
{
    std::wstring title = L"Amazon S3";
    auto text = to_wide(message);
    std::array<wchar_t, 1> ignored{};
    return request_proc && request_proc(plugin_number, request_type, title.data(), text.data(),
                                        ignored.data(), static_cast<int>(ignored.size()));
}

bool open_url(std::string_view url)
{
    const auto wide_url = to_wide(url);
    return reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", wide_url.c_str(),
                                                         nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

class SsoTokenWriter : private Aws::Auth::SSOBearerTokenProvider
{
public:
    using Token = CachedSsoToken;

    explicit SsoTokenWriter(std::string_view profile)
        : SSOBearerTokenProvider(Aws::String{profile})
    {
    }

    bool write(const Token& token)
    {
        const auto profile_directory =
            Aws::Auth::ProfileConfigFileAWSCredentialsProvider::GetProfileDirectory();
        if (profile_directory.empty())
            return false;

        std::error_code error;
        std::filesystem::create_directories(
            std::filesystem::path(profile_directory) / "sso" / "cache", error);
        return !error && WriteAccessTokenFile(token);
    }
};

std::string base64_url(const Aws::Utils::ByteBuffer& buf)
{
    std::string base64 = Aws::Utils::HashingUtils::Base64Encode(buf);
    std::replace(base64.begin(), base64.end(), '+', '-');
    std::replace(base64.begin(), base64.end(), '/', '_');
    while (!base64.empty() && base64.back() == '=')
        base64.pop_back();
    return base64;
}

Aws::Utils::ByteBuffer random_bytes(std::size_t byte_count)
{
    Aws::Utils::ByteBuffer bytes(byte_count);
    const auto entropy = Aws::Utils::Crypto::CreateSecureRandomBytesImplementation();
    if (!entropy)
        throw SsoLoginFailed("Cannot initialize secure random generation for AWS SSO login");
    entropy->GetBytes(bytes.GetUnderlyingData(), bytes.GetLength());
    if (!*entropy)
        throw SsoLoginFailed("Cannot generate secure random data for AWS SSO login");
    return bytes;
}

void write_sso_token(std::string_view profile_name, const Aws::String& start_url,
                     const Aws::String& region,
                     const Aws::SSOOIDC::Model::RegisterClientResult& registration,
                     const Aws::SSOOIDC::Model::CreateTokenResult& token)
{
    SsoTokenWriter::Token cached;
    cached.accessToken = token.GetAccessToken();
    cached.expiresAt = Aws::Utils::DateTime::Now() + std::chrono::seconds(token.GetExpiresIn());
    cached.refreshToken = token.GetRefreshToken();
    cached.clientId = registration.GetClientId();
    cached.clientSecret = registration.GetClientSecret();
    cached.registrationExpiresAt = Aws::Utils::DateTime(
        static_cast<std::uint64_t>(registration.GetClientSecretExpiresAt()));
    cached.region = region;
    cached.startUrl = start_url;
    if (!SsoTokenWriter(profile_name).write(cached))
        throw SsoLoginFailed("Cannot write the AWS SSO token cache");
}

void perform_pkce_sso_login(std::string_view profile_name, const Aws::String& start_url,
                            const Aws::String& region, Aws::SSOOIDC::SSOOIDCClient& oidc)
{
    constexpr std::string_view registered_redirect_uri =
        "http://127.0.0.1/oauth/callback";

    const auto verifier = base64_url(random_bytes(48));
    const auto challenge = base64_url(Aws::Utils::HashingUtils::CalculateSHA256(verifier));
    const auto state = base64_url(random_bytes(32));

    struct Callback
    {
        std::mutex mutex;
        std::condition_variable ready;
        std::optional<std::string> code;
        std::optional<std::string> error;
    } callback;

    httplib::Server server;
    server.Get("/oauth/callback", [&](const httplib::Request& request, httplib::Response& response) {
        if (!request.has_param("state") || request.get_param_value("state") != state)
        {
            response.status = 400;
            response.set_content("Invalid AWS SSO login state.", "text/plain; charset=utf-8");
            return;
        }

        const auto succeeded = request.has_param("code");
        {
            std::scoped_lock lock(callback.mutex);
            if (callback.code || callback.error)
                return;
            if (succeeded)
                callback.code = request.get_param_value("code");
            else if (request.has_param("error_description"))
                callback.error = request.get_param_value("error_description");
            else if (request.has_param("error"))
                callback.error = request.get_param_value("error");
            else
                callback.error = "AWS SSO authorization returned no code";
        }
        response.set_content(
            succeeded
                ? "Your credentials have been shared successfully and can be used until your "
                  "session expires. You can now close this tab."
                : "AWS SSO login failed. You can close this tab.",
            "text/plain; charset=utf-8");
        callback.ready.notify_one();
    });

    const auto port = server.bind_to_any_port("127.0.0.1");
    if (port <= 0)
        throw SsoLoginFailed("Cannot start the AWS SSO browser callback server");
    const auto redirect_uri =
        std::format("http://127.0.0.1:{}/oauth/callback", port);

    Aws::SSOOIDC::Model::RegisterClientRequest register_request;
    register_request.SetClientName("s3cmd");
    register_request.SetClientType("public");
    register_request.AddRedirectUris(registered_redirect_uri);
    register_request.AddGrantTypes("authorization_code");
    register_request.AddGrantTypes("refresh_token");
    register_request.AddScopes("sso:account:access");
    register_request.SetIssuerUrl(start_url);
    const auto registration = oidc.RegisterClient(register_request);
    if (!registration.IsSuccess())
    {
        throw SsoLoginFailed(std::format("AWS SSO client registration failed: {}",
                                         registration.GetError().GetMessage()));
    }

    const auto dns_suffix = region.starts_with("cn-") ? "amazonaws.com.cn" : "amazonaws.com";
    const auto authorization_url = std::format(
        "https://oidc.{}.{}/authorize?response_type=code&client_id={}&redirect_uri={}&state={}"
        "&code_challenge_method=S256&scopes={}&code_challenge={}",
        region,
        dns_suffix,
        Aws::Utils::StringUtils::URLEncode(registration.GetResult().GetClientId()),
        Aws::Utils::StringUtils::URLEncode(redirect_uri),
        Aws::Utils::StringUtils::URLEncode(state),
        Aws::Utils::StringUtils::URLEncode("sso:account:access"),
        Aws::Utils::StringUtils::URLEncode(challenge));

    std::jthread listener([&] {
        if (!server.listen_after_bind())
        {
            std::scoped_lock lock(callback.mutex);
            if (!callback.code && !callback.error)
            {
                callback.error = "AWS SSO browser callback server stopped unexpectedly";
                callback.ready.notify_one();
            }
        }
    });

    std::optional<std::string> code;
    std::optional<std::string> callback_error;
    bool completed{};
    try
    {
        if (!open_url(authorization_url) &&
            !ask_sso_login(
                std::format("The AWS SSO browser could not be opened automatically.\n\n"
                            "Open this URL manually:\n{}\n\n"
                            "Click OK to keep waiting or Cancel to use another login method.",
                            authorization_url),
                RT_MsgOKCancel))
        {
            throw SsoLoginFailed(
                std::format("AWS SSO login for profile '{}' was cancelled", profile_name));
        }

        std::unique_lock lock(callback.mutex);
        completed = callback.ready.wait_for(lock, std::chrono::minutes(10), [&] {
            return callback.code.has_value() || callback.error.has_value();
        });
        code = callback.code;
        callback_error = callback.error;
    }
    catch (...)
    {
        server.stop();
        throw;
    }
    server.stop();
    listener.join();

    if (!completed)
    {
        throw SsoLoginFailed(std::format("AWS SSO login for profile '{}' timed out", profile_name));
    }
    if (callback_error)
    {
        throw SsoLoginFailed(
            std::format("AWS SSO browser authorization failed: {}", *callback_error));
    }

    Aws::SSOOIDC::Model::CreateTokenRequest token_request;
    token_request.SetClientId(registration.GetResult().GetClientId());
    token_request.SetClientSecret(registration.GetResult().GetClientSecret());
    token_request.SetGrantType("authorization_code");
    token_request.SetCode(*code);
    token_request.SetRedirectUri(redirect_uri);
    token_request.SetCodeVerifier(verifier);
    const auto token = oidc.CreateToken(token_request);
    if (!token.IsSuccess())
    {
        throw SsoLoginFailed(
            std::format("AWS SSO token request failed: {}", token.GetError().GetMessage()));
    }

    write_sso_token(profile_name, start_url, region, registration.GetResult(), token.GetResult());
}

void perform_device_sso_login(std::string_view profile_name, const Aws::String& start_url,
                              const Aws::String& region, Aws::SSOOIDC::SSOOIDCClient& oidc)
{
    Aws::SSOOIDC::Model::RegisterClientRequest register_request;
    register_request.SetClientName("s3cmd");
    register_request.SetClientType("public");
    register_request.AddGrantTypes("urn:ietf:params:oauth:grant-type:device_code");
    register_request.AddGrantTypes("refresh_token");
    register_request.AddScopes("sso:account:access");
    const auto registration = oidc.RegisterClient(register_request);
    if (!registration.IsSuccess())
    {
        throw SsoLoginFailed(std::format("AWS SSO client registration failed: {}",
                                         registration.GetError().GetMessage()));
    }

    Aws::SSOOIDC::Model::StartDeviceAuthorizationRequest start_request;
    start_request.SetClientId(registration.GetResult().GetClientId());
    start_request.SetClientSecret(registration.GetResult().GetClientSecret());
    start_request.SetStartUrl(start_url);
    const auto authorization = oidc.StartDeviceAuthorization(start_request);
    if (!authorization.IsSuccess())
    {
        throw SsoLoginFailed(std::format("AWS SSO device authorization failed: {}",
                                         authorization.GetError().GetMessage()));
    }

    const auto& device = authorization.GetResult();
    const auto& url = device.GetVerificationUriComplete().empty()
                          ? device.GetVerificationUri()
                          : device.GetVerificationUriComplete();
    const auto browser_opened = open_url(url);
    if (!ask_sso_login(
            std::format("Complete AWS SSO login for profile '{}' in your browser.\n\n"
                        "URL: {}\nCode: {}\n\nClick OK after AWS reports success.",
                        profile_name, url, device.GetUserCode()),
            RT_MsgOKCancel))
    {
        throw SsoLoginFailed(
            std::format("AWS SSO login for profile '{}' was cancelled", profile_name));
    }

    if (!browser_opened)
        log("[s3cmd] AWS SSO login: browser could not be opened automatically");

    Aws::SSOOIDC::Model::CreateTokenRequest token_request;
    token_request.SetClientId(registration.GetResult().GetClientId());
    token_request.SetClientSecret(registration.GetResult().GetClientSecret());
    token_request.SetGrantType("urn:ietf:params:oauth:grant-type:device_code");
    token_request.SetDeviceCode(device.GetDeviceCode());

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(device.GetExpiresIn());
    auto interval = std::max(1, device.GetInterval());
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto token = oidc.CreateToken(token_request);
        if (token.IsSuccess())
        {
            write_sso_token(profile_name, start_url, region, registration.GetResult(),
                            token.GetResult());
            return;
        }

        switch (token.GetError().GetErrorType())
        {
        case Aws::SSOOIDC::SSOOIDCErrors::AUTHORIZATION_PENDING:
            break;
        case Aws::SSOOIDC::SSOOIDCErrors::SLOW_DOWN:
            interval = std::min(interval + 5, 30);
            break;
        default:
            throw SsoLoginFailed(
                std::format("AWS SSO token request failed: {}", token.GetError().GetMessage()));
        }
        std::this_thread::sleep_for(std::chrono::seconds(interval));
    }

    throw SsoLoginFailed(std::format("AWS SSO login for profile '{}' timed out", profile_name));
}

void perform_sso_login(std::string_view profile_name, const Aws::Config::Profile& profile)
{
    assert(profile.IsSsoSessionSet());

    const auto& session = profile.GetSsoSession();
    const auto& start_url = session.GetSsoStartUrl();
    const auto& region = session.GetSsoRegion();
    if (start_url.empty() || region.empty())
    {
        throw SsoLoginFailed(std::format(
            "AWS SSO profile '{}' is missing sso_start_url or sso_region", profile_name));
    }

    if (!request_proc)
    {
        throw SsoLoginFailed("AWS SSO browser login requires an interactive file manager");
    }

    if (!ask_sso_login(
            std::format("AWS SSO credentials for profile '{}' are unavailable or expired.\n\n"
                        "Start browser login?",
                        profile_name),
            RT_MsgYesNo))
    {
        throw SsoLoginFailed(
            std::format("AWS SSO login for profile '{}' was cancelled", profile_name));
    }

    Aws::Client::ClientConfiguration configuration;
    configuration.region = region;
    Aws::SSOOIDC::SSOOIDCClient oidc(configuration);

    try
    {
        perform_pkce_sso_login(profile_name, start_url, region, oidc);
        return;
    }
    catch (const SsoLoginFailed& error)
    {
        log("[s3cmd] AWS SSO PKCE login failed: {}", error.what());
        if (!ask_sso_login(std::format("AWS SSO browser login failed:\n{}\n\n"
                                       "Try device-code login instead?",
                                       error.what()),
                           RT_MsgYesNo))
        {
            throw;
        }
    }

    perform_device_sso_login(profile_name, start_url, region, oidc);
}

std::optional<toml::table> read_document(const std::filesystem::path& file_path)
{
    try
    {
        std::ifstream input{file_path};
        if (input)
            return toml::parse(input);
    }
    catch (const toml::parse_error&)
    {
    }
    return std::nullopt;
}

bool write_document(const toml::table& document, const std::filesystem::path& file_path)
{
    std::error_code ec;
    std::filesystem::create_directories(file_path.parent_path(), ec);
    if (ec)
        return false;

    std::ofstream output(file_path, std::ios::trunc);
    return output && (output << document) && output.flush();
}

RuntimeConfig& RuntimeConfig::get()
{
    if (runtime_config)
        return *runtime_config;
    runtime_config.emplace();

    if (auto document = read_document(path()))
    {
        // Deserialize TOML document into RuntimeConfig
        runtime_config->dry_run = (*document)["settings"]["DryRun"].value_or(false);
        runtime_config->aws_log_level =
            (*document)["settings"]["AwsLogLevel"].value_or("Info");

        if (const auto* profiles = document->get_as<toml::table>("profiles"))
        {
            for (const auto& [name, profile] : *profiles)
            {
                const auto* profile_table = profile.as_table();
                const auto* buckets =
                    profile_table ? profile_table->get_as<toml::table>("buckets") : nullptr;
                if (!buckets)
                    continue;
                auto& registered =
                    runtime_config->profiles[std::string(name.str())].registered_buckets;
                for (const auto& [bucket, region] : *buckets)
                {
                    if (const auto value = region.value<std::string>())
                        registered.emplace(bucket.str(), BucketInfo{*value});
                }
            }
        }
    }

    return *runtime_config;
}

bool RuntimeConfig::flush_to_disk()
{
    // Serialize our config to TOML document and write it to disk
    toml::table document;
    document.emplace("settings",
                     toml::table{{"DryRun", dry_run}, {"AwsLogLevel", aws_log_level}});

    toml::table profile_tables;
    for (const auto& [profile_name, profile] : profiles)
    {
        if (profile.registered_buckets.empty())
            continue;

        toml::table bucket_map;
        for (const auto& [bucket_name, bucket_info] : profile.registered_buckets)
        {
            bucket_map.emplace(bucket_name, bucket_info.region);
        }

        toml::table profile_table;
        profile_table.emplace("buckets", std::move(bucket_map));
        profile_tables.emplace(profile_name, std::move(profile_table));
    }
    if (!profile_tables.empty())
        document.emplace("profiles", std::move(profile_tables));

    return write_document(document, path());
}

const std::filesystem::path& RuntimeConfig::path()
{
    static std::filesystem::path value = [] {
#ifdef _WIN32
        wchar_t* app_data{};
        std::size_t size{};
        // _wdupenv_s allocates a correctly sized UTF-16 copy.
        if (_wdupenv_s(&app_data, &size, L"APPDATA") != 0 || !app_data || !*app_data)
        {
            std::free(app_data);
            throw std::runtime_error("APPDATA is not set");
        }
        std::unique_ptr<wchar_t, decltype(&std::free)> releaser(app_data, &std::free);
        return std::filesystem::path(releaser.get()) / L"s3cmd" / L"s3cmd.toml";
#else
        if (const auto* config_home = std::getenv("XDG_CONFIG_HOME"); config_home && *config_home)
            return std::filesystem::path(config_home) / "s3cmd" / "s3cmd.toml";
        if (const auto* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".config" / "s3cmd" / "s3cmd.toml";
        throw std::runtime_error("XDG_CONFIG_HOME and HOME are not set");
#endif
    }();
    return value;
}

// Factory function for creating a new S3Client.
// Credentials (whether SSO token has expired) are checked later
std::shared_ptr<ClientEntry> make_client(const Aws::S3::S3ClientConfiguration& configuration,
                                         const RemotePath& path)
{
    Aws::Client::ClientConfiguration::CredentialProviderConfiguration credentials_configuration;
    credentials_configuration.profile = path.profile;
    credentials_configuration.region = configuration.region;
    auto credentials = Aws::MakeShared<Aws::Auth::DefaultAWSCredentialsProviderChain>(
        "s3cmd", credentials_configuration);

    const auto profile = Aws::Config::GetCachedConfigProfile(path.profile);
    const bool uses_sso = profile.IsSsoSessionSet() || !profile.GetSsoStartUrl().empty();
    return std::make_shared<ClientEntry>(
        credentials,
        Aws::MakeShared<Aws::S3::S3Client>(/*allocationTag*/ "s3cmd", credentials, nullptr,
                                           configuration),
        uses_sso);
}

std::shared_ptr<Aws::S3::S3Client> get_client(const RemotePath& path,
                                              std::string_view region_override = {})
{
    Aws::S3::S3ClientConfiguration configuration(path.profile.c_str());
    if (!region_override.empty())
    {
        configuration.region = region_override;
    }
    else if (!path.bucket.empty())
    {
        // Get region for the client from the 'active' bucket's region
        const auto region = ProfileConfig(path.profile).bucket_region(path.bucket);
        if (!region.empty())
            configuration.region = region;
    }

    // Clients are cached and keyed by (profile, region)
    const ClientKey key{path.profile, configuration.region};

    auto entry = [&] {
        std::scoped_lock lock(client_mutex);
        if (const auto found = clients.find(key); found != clients.end())
            return found->second;
        return std::shared_ptr<ClientEntry>{};
    }();

    if (!entry)
    {
        auto candidate = make_client(configuration, path);

        std::scoped_lock lock(client_mutex);
        entry = clients.try_emplace(key, std::move(candidate)).first->second;
    }

    if (entry->uses_sso && entry->credentials->GetAWSCredentials().IsEmpty())
    {
        const auto failed_entry = entry;

        // Profile is SSO based but the credentials are expired and couldn't be refreshed
        std::scoped_lock login_lock(sso_login_mutex);
        {
            // While this thread waits for sso_login_mutex another one might've
            // completed login and replace clients[key]. Hence we compare newEntry
            // with entry down below to detect this scenario and check for
            // possibly refreshed and valid credentials
            std::scoped_lock lock(client_mutex);
            entry = clients.at(key);
        }

        // Don't call GetAWSCredentials() twice on the same provider which could
        // potentially just repeat a failed refresh request.
        if (entry == failed_entry || entry->credentials->GetAWSCredentials().IsEmpty())
        {
            const auto profile = Aws::Config::GetCachedConfigProfile(path.profile);
            perform_sso_login(path.profile, profile);

            auto refreshed = make_client(configuration, path);
            if (refreshed->credentials->GetAWSCredentials().IsEmpty())
            {
                throw SsoLoginFailed(std::format(
                    "AWS SSO login for profile '{}' did not produce credentials", path.profile));
            }
            std::scoped_lock lock(client_mutex);
            entry = clients.insert_or_assign(key, std::move(refreshed)).first->second;
        }
    }
    return entry->client;
}

void log_error(std::string_view operation, std::string_view message)
{
    const auto text = std::format("{}: {}", operation, message);
    if (log_proc)
        log_proc(plugin_number, MSGTYPE_IMPORTANTERROR, to_wide(text).data());

    log("[s3cmd] {}", text);
}

template <class Error>
void log_aws_error(std::string_view operation, const Error& error)
{
    log_error(operation, error.GetMessage());
}

void log_unexpected(std::string_view operation, const std::exception& error)
{
    log_error(operation, error.what());
}

void log_operation(std::string_view operation, const RemotePath& path, bool dry)
{
    log("[s3cmd] operation={} dry={} profile={} bucket={} key={}", operation, dry, path.profile,
        path.bucket, path.key);
}

void log_local_operation(std::string_view operation, const wchar_t* path, bool dry)
{
    log("[s3cmd] operation={} dry={} path={}", operation, dry, to_utf8(path));
}

bool report_progress(const wchar_t* source, const wchar_t* target, int percent)
{
    return progress_proc && progress_proc(plugin_number, const_cast<wchar_t*>(source),
                                          const_cast<wchar_t*>(target), percent) != 0;
}

// Notifies totalcmd about the transfer progress. Used in both get and put operations
// Only calls the calback when value of the progress changes (i.e. from 34% to 35%).
// Naturally supports pause transfers when totalcmd blocks the `report_progress` function
// on their end
class TransferProgress
{
public:
    // Constructor for Get request
    TransferProgress(const wchar_t* source, const wchar_t* target,
                     Aws::S3::Model::GetObjectRequest& request)
        : TransferProgress{source, target, 0, request}
    {
        request.SetHeadersReceivedEventHandler(
            [this](const Aws::Http::HttpRequest*, Aws::Http::HttpResponse* response) {
                const auto& length = response->GetHeader(Aws::Http::CONTENT_LENGTH_HEADER);
                std::from_chars(length.data(), length.data() + length.size(), total_);
            });
        request.SetDataReceivedEventHandler([this](const Aws::Http::HttpRequest*,
                                                   Aws::Http::HttpResponse*,
                                                   long long bytes) { add(bytes); });
    }

    // Constructor for Put request
    TransferProgress(const wchar_t* source, const wchar_t* target,
                     Aws::S3::Model::PutObjectRequest& request)
        : TransferProgress{source, target,
                           static_cast<std::uint64_t>(request.GetContentLength()),
                           request}
    {
        request.SetDataSentEventHandler(
            [this](const Aws::Http::HttpRequest*, long long bytes) { add(bytes); });
    }

    bool is_canceled() const { return canceled_.load(); }

private:
    // Common constructor for both Get and Put requests
    template <typename Request>
    TransferProgress(const wchar_t* source, const wchar_t* target,
                     std::uint64_t total, Request& request)
        : source_{source}, target_{target}, total_{total}
    {
        request.SetRequestRetryHandler(
            [this](const Aws::AmazonWebServiceRequest&) { transferred_ = 0; });
        request.SetContinueRequestHandler(
            [this](const Aws::Http::HttpRequest*) { return !canceled_.load(); });
    }

    // Calculate percentage of the transfer. Never returns 100 as that's reserved for a completed transfer
    int transfer_percent(std::uint64_t transferred, std::uint64_t total)
    {
        return total == 0
                   ? 0
                   : std::min(99, static_cast<int>(std::min(transferred, total) * 100 / total));
    }

    // Called whenever `bytes` data is transferred. Recalculates a percentage and if it changed
    // since last time, notifies totalcmd about that.
    void add(long long bytes)
    {
        if (bytes > 0)
            transferred_ += static_cast<std::uint64_t>(bytes);
        const auto next = transfer_percent(transferred_, total_);
        if (next > percent_)
        {
            percent_ = next;
            if (report_progress(source_, target_, percent_))
                canceled_ = true;
        }
    }

    const wchar_t* source_;
    const wchar_t* target_;
    std::uint64_t total_{};
    std::uint64_t transferred_{};
    int percent_{};
    std::atomic<bool> canceled_{};
};

// Checks whether an object at `path` exists
bool remote_exists(Aws::S3::S3Client& client, const RemotePath& path)
{
    log_operation("HeadObject", path, false);
    Aws::S3::Model::HeadObjectRequest request;
    request.SetBucket(path.bucket);
    request.SetKey(path.key);
    const auto outcome = client.HeadObject(request);
    if (outcome.IsSuccess())
        return true;
    if (outcome.GetError().GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND)
        return false;
    log_aws_error("HeadObject", outcome.GetError());
    throw std::runtime_error(outcome.GetError().GetMessage());
}

class FindState
{
public:
    explicit FindState(const RemotePath& path)
    {
        AwsLease lease;

        // Path is at "profile level", just list all the detected profiles
        if (path.profile.empty())
        {
            list_profiles();
        }
        // Path is at "bucket level", list all the buckets, discovered - if user has access, or registered otherwise
        else if (path.bucket.empty())
        {
            list_buckets(path);
        }
        else
        {
            list_objects(path);
        }
    }

    // Behaves as *input_iterator++
    bool dereference_move_next(WIN32_FIND_DATAW* data)
    {
        if (next_ >= entries_.size())
            return false;
        entries_[next_++].copy(data);
        return true;
    }

private:
    struct FindEntry
    {
        std::wstring name;
        std::uint64_t size{};
        FILETIME modified{};
        bool directory{};

        void copy(WIN32_FIND_DATAW* data) const
        {
            *data = {};
            data->dwFileAttributes = directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            data->ftLastWriteTime = modified;
            data->nFileSizeLow = static_cast<DWORD>(size);
            data->nFileSizeHigh = static_cast<DWORD>(size >> 32);
            std::copy(name.begin(), name.end(), data->cFileName);
            data->cFileName[name.size()] = L'\0';
        }
    };

    static bool valid_entry_name(const std::wstring& name)
    {
        return !name.empty()              &&
            name != L"."                  &&
            name != L".."                 &&
            name.find(L'\\') == name.npos &&
            // cFileName has MAX_PATH slots, including the terminating null
            // This only limits displayed entry name, not the complete S3 key/path
            name.size() < MAX_PATH;
    }

    void append_entry(std::string_view name, bool directory, std::uint64_t size = 0,
                      FILETIME modified = {})
    {
        auto wide_name = to_wide(name);
        if (valid_entry_name(wide_name))
            entries_.push_back({std::move(wide_name), size, modified, directory});
    }

    FILETIME to_file_time(const Aws::Utils::DateTime& value)
    {
        constexpr std::int64_t windows_epoch_offset_ms = 11'644'473'600'000;
        const auto ticks =
            static_cast<std::uint64_t>(value.Millis() + windows_epoch_offset_ms) * 10'000;
        return {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
    }

    void list_profiles()
    {
        std::set<std::string> profiles{"default"};
        const auto selected = Aws::Auth::GetConfigProfileName();
        profiles.emplace(selected);
        for (const auto& [name, ignored] : Aws::Config::GetCachedConfigProfiles())
            profiles.emplace(name);
        for (const auto& [name, ignored] : Aws::Config::GetCachedCredentialsProfiles())
            profiles.emplace(name);

        for (const auto& profile : profiles)
            append_entry(profile, true);
    }

    void list_buckets(const RemotePath& path)
    {
        BucketMap buckets;
        bool discovered{};
        // Force us-east-1 region because global endpoint returns the original bucket creation time
        // whereas regional replicas return their last metadata replication time in the CreationDate
        // field.
        auto client = get_client(path, "us-east-1");
        Aws::S3::Model::ListBucketsRequest request;
        request.SetMaxBuckets(10'000);
        log_operation("ListBuckets", path, false);
        for (;;)
        {
            const auto outcome = client->ListBuckets(request);
            if (!outcome.IsSuccess())
            {
                buckets = ProfileConfig(path.profile).registered_buckets();
                if (outcome.GetError().GetResponseCode() != Aws::Http::HttpResponseCode::FORBIDDEN)
                    log_aws_error("ListBuckets", outcome.GetError());
                break;
            }

            discovered = true;
            const auto& result = outcome.GetResult();
            for (const auto& bucket : result.GetBuckets())
            {
                buckets[bucket.GetName()] = {bucket.GetBucketRegion(),
                                             to_file_time(bucket.GetCreationDate())};
            }

            if (result.GetContinuationToken().empty())
                break;
            request.SetContinuationToken(result.GetContinuationToken());
        }

        // We either want to list discovered buckets, or registered, never both
        ProfileConfig(path.profile).set_discovered_buckets(discovered ? buckets : BucketMap{});

        if (!discovered)
            append_entry("_F7=register bucket.txt", false);
        for (const auto& [name, bucket] : buckets)
            append_entry(name, true, 0, bucket.created);
    }

    void list_objects(const RemotePath& path)
    {
        const auto prefix = path.directory_prefix();
        auto client = get_client(path);
        Aws::S3::Model::ListObjectsV2Request request;
        request.SetBucket(path.bucket);
        request.SetDelimiter("/");
        request.SetPrefix(prefix);
        log_operation("ListObjectsV2", {path.profile, path.bucket, prefix}, false);

        for (;;)
        {
            const auto outcome = client->ListObjectsV2(request);
            if (!outcome.IsSuccess())
            {
                log_aws_error("ListObjectsV2", outcome.GetError());
                throw std::runtime_error(outcome.GetError().GetMessage());
            }

            const auto& result = outcome.GetResult();
            for (const auto& common_prefix : result.GetCommonPrefixes())
            {
                const auto& value = common_prefix.GetPrefix();
                if (value.size() <= prefix.size() + 1)
                    continue;
                append_entry({value.data() + prefix.size(), value.size() - prefix.size() - 1},
                             true);
            }

            for (const auto& object : result.GetContents())
            {
                const auto& key = object.GetKey();
                if (key == prefix || key.size() <= prefix.size())
                {
                    continue;
                }
                append_entry({key.data() + prefix.size(), key.size() - prefix.size()}, false,
                             static_cast<std::uint64_t>(object.GetSize()),
                             to_file_time(object.GetLastModified()));
            }

            if (!result.GetIsTruncated())
                break;
            request.SetContinuationToken(result.GetNextContinuationToken());
        }
    }

private:
    std::vector<FindEntry> entries_; // Immutable after ctor as finished
    std::size_t next_{};
};

} // namespace

void reset_config()
{
    std::scoped_lock lock(config_mtx);
    runtime_config.reset();
}

bool is_dry_run()
{
    std::scoped_lock lock(config_mtx);
    return RuntimeConfig::get().dry_run;
}

BucketMap ProfileConfig::registered_buckets() const
{
    std::scoped_lock lock(config_mtx);
    const auto& config = RuntimeConfig::get();
    const auto profile = config.profiles.find(profile_);
    return profile == config.profiles.end() ? BucketMap{}
                                            : profile->second.registered_buckets;
}

bool ProfileConfig::has_discovered_buckets() const
{
    std::scoped_lock lock(config_mtx);
    const auto& config = RuntimeConfig::get();
    const auto profile = config.profiles.find(profile_);
    return profile != config.profiles.end() && !profile->second.discovered_buckets.empty();
}

void ProfileConfig::set_discovered_buckets(BucketMap buckets) const
{
    std::scoped_lock lock(config_mtx);
    auto& config = RuntimeConfig::get();
    config.profiles[profile_].discovered_buckets = std::move(buckets);
}

std::string ProfileConfig::bucket_region(std::string_view bucket) const
{
    std::scoped_lock lock(config_mtx);
    const auto& config = RuntimeConfig::get();
    const auto profile = config.profiles.find(profile_);
    if (profile == config.profiles.end())
        return {};

    if (const auto registered = profile->second.registered_buckets.find(bucket);
        registered != profile->second.registered_buckets.end())
    {
        return registered->second.region;
    }
    if (const auto discovered = profile->second.discovered_buckets.find(bucket);
        discovered != profile->second.discovered_buckets.end())
    {
        return discovered->second.region;
    }

    return {};
}

bool ProfileConfig::register_bucket(std::string_view bucket, std::string_view region) const
{
    std::scoped_lock lock(config_mtx);
    auto& config = RuntimeConfig::get();
    config.profiles[profile_].registered_buckets[std::string{bucket}] =
        BucketInfo{std::string{region}};
    return config.flush_to_disk();
}

bool ProfileConfig::unregister_bucket(std::string_view bucket) const
{
    std::scoped_lock lock(config_mtx);
    auto& config = RuntimeConfig::get();
    auto& registered = config.profiles[profile_].registered_buckets;
    if (auto it = registered.find(bucket); it != registered.end())
    {
        registered.erase(it);
        return config.flush_to_disk();
    }
    return false;
}

std::string discover_bucket_region(std::string_view profile, std::string_view bucket)
{
    AwsLease lease;
    auto client = get_client({std::string(profile), {}, {}});
    Aws::S3::Model::GetBucketLocationRequest request;
    request.SetBucket(std::string(bucket));
    log("[s3cmd] operation=GetBucketLocation profile={} bucket={}", profile, bucket);
    const auto outcome = client->GetBucketLocation(request);
    if (!outcome.IsSuccess())
        return {};

    const auto location = outcome.GetResult().GetLocationConstraint();
    if (location == Aws::S3::Model::BucketLocationConstraint::NOT_SET)
        return "us-east-1";
    if (location == Aws::S3::Model::BucketLocationConstraint::EU)
        return "eu-west-1";
    const auto region =
        Aws::S3::Model::BucketLocationConstraintMapper::GetNameForBucketLocationConstraint(
            location);
    return region;
}

int initialize(int number, tProgressProcW progress, tLogProcW log, tRequestProcW request)
{
    s3cmd::log("[s3cmd] Initialize called, plugin={}, thread={}", number, GetCurrentThreadId());

    std::unique_lock lock(aws_lifecycle_mtx);
    if (!aws_initialized)
    {
        {
            std::scoped_lock config_lock(config_mtx);
            aws_options.loggingOptions.logLevel =
                parse_log_level(RuntimeConfig::get().aws_log_level);
        }
        aws_options.loggingOptions.logger_create_fn = [] {
            return Aws::MakeShared<AwsLogSystem>("s3cmd", aws_options.loggingOptions.logLevel);
        };
        Aws::InitAPI(aws_options);
        aws_init_thread_id = GetCurrentThreadId();
        aws_initialized = true;
    }
    else
    {
        assert(aws_init_thread_id == GetCurrentThreadId());
    }

    plugin_number = number;
    progress_proc = progress;
    log_proc = log;
    request_proc = request;
    return 0;
}

void shutdown()
{
    log("[s3cmd] Shutdown called, thread={}", GetCurrentThreadId());

    std::unique_lock lock(aws_lifecycle_mtx);
    reset_config();
    if (!aws_initialized)
        return;

    const auto same_thread = aws_init_thread_id == GetCurrentThreadId();
    assert(same_thread);
    if (!same_thread)
        return;

    {
        std::scoped_lock clients_lock(client_mutex);
        clients.clear();
    }
    Aws::ShutdownAPI(aws_options);
    aws_initialized = false;
    aws_init_thread_id = 0;
}

HANDLE find_first(const wchar_t* path, WIN32_FIND_DATAW* find_data)
{
    if (suppress_delete_listing)
    {
        SetLastError(ERROR_NO_MORE_FILES);
        return INVALID_HANDLE_VALUE;
    }

    try
    {
        auto state = std::make_unique<FindState>(RemotePath::make(path));
        if (!state->dereference_move_next(find_data))
        {
            SetLastError(ERROR_NO_MORE_FILES);
            return INVALID_HANDLE_VALUE;
        }
        return state.release();
    }
    catch (const SsoLoginFailed& error)
    {
        log_unexpected("FsFindFirstW", error);
        SetLastError(ERROR_LOGON_FAILURE);
        return INVALID_HANDLE_VALUE;
    }
    catch (const std::exception& error)
    {
        log_unexpected("FsFindFirstW", error);
        SetLastError(ERROR_PATH_NOT_FOUND);
        return INVALID_HANDLE_VALUE;
    }
}

bool find_next(HANDLE handle, WIN32_FIND_DATAW* find_data)
{
    if (!handle || handle == INVALID_HANDLE_VALUE)
        return false;
    return static_cast<FindState*>(handle)->dereference_move_next(find_data);
}

int find_close(HANDLE handle)
{
    if (handle != INVALID_HANDLE_VALUE)
        delete static_cast<FindState*>(handle);
    return 0;
}

void status_info(const wchar_t* remote_directory, int start_end, int operation)
{
    if (operation != FS_STATUS_OP_DELETE)
        return;
    if (start_end == FS_STATUS_END)
    {
        suppress_delete_listing = false;
        return;
    }

    const auto path = RemotePathView::make(remote_directory);
    suppress_delete_listing = path.bucket.empty();
}

int get_file(const wchar_t* remote_name, const wchar_t* local_name, int copy_flags,
             [[maybe_unused]] const RemoteInfoStruct* info)
try
{
    if ((copy_flags & FS_COPYFLAGS_RESUME) != 0)
        return FS_FILE_NOTSUPPORTED;
    if ((copy_flags & FS_COPYFLAGS_OVERWRITE) == 0 && std::filesystem::exists(local_name))
        return FS_FILE_EXISTS;
    if (report_progress(remote_name, local_name, 0))
        return FS_FILE_USERABORT;

    const auto path = RemotePath::make(remote_name);
    if (path.profile.empty() || path.bucket.empty() || path.key.empty())
        return FS_FILE_NOTFOUND;

    const auto is_dry = is_dry_run();
    log_operation("GetObject", path, is_dry);
    if (is_dry)
    {
        if ((copy_flags & FS_COPYFLAGS_MOVE) != 0)
            log_operation("DeleteObject", path, true);
        report_progress(remote_name, local_name, 100);
        return FS_FILE_OK;
    }

    const auto local = std::filesystem::path(local_name);
    wchar_t temporary[MAX_PATH];
    if (GetTempFileNameW(local.parent_path().c_str(), L"s3c", 0, temporary) == 0)
        return FS_FILE_WRITEERROR;
    const auto partial = std::filesystem::path(temporary);
    std::error_code ignored;

    {
        AwsLease lease;
        auto client = get_client(path);
        Aws::S3::Model::GetObjectRequest request;
        request.SetBucket(path.bucket);
        request.SetKey(path.key);
        request.SetResponseStreamFactory([partial] {
            return Aws::New<std::fstream>("s3cmd", partial,
                                          std::ios::out | std::ios::binary | std::ios::trunc);
        });

        TransferProgress progress{remote_name, local_name, request};
        const auto outcome = client->GetObject(request);
        if (!outcome.IsSuccess())
        {
            std::filesystem::remove(partial, ignored);
            if (progress.is_canceled())
                return FS_FILE_USERABORT;
            log_aws_error("GetObject", outcome.GetError());
            return outcome.GetError().GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND
                       ? FS_FILE_NOTFOUND
                       : FS_FILE_READERROR;
        }
    }

    auto move_flags = MOVEFILE_WRITE_THROUGH;
    if ((copy_flags & FS_COPYFLAGS_OVERWRITE) != 0)
        move_flags |= MOVEFILE_REPLACE_EXISTING;
    if (!MoveFileExW(partial.c_str(), local_name, move_flags))
    {
        std::filesystem::remove(partial, ignored);
        return FS_FILE_WRITEERROR;
    }
    if ((copy_flags & FS_COPYFLAGS_MOVE) != 0 && !delete_file(remote_name))
        return FS_FILE_WRITEERROR;

    report_progress(remote_name, local_name, 100);
    return FS_FILE_OK;
}
catch (const std::exception& error)
{
    log_unexpected("FsGetFileW", error);
    return FS_FILE_READERROR;
}

int put_file(const wchar_t* local_name, const wchar_t* remote_name, int copy_flags)
try
{
    if ((copy_flags & FS_COPYFLAGS_RESUME) != 0)
        return FS_FILE_NOTSUPPORTED;

    std::error_code error;
    const auto size = std::filesystem::file_size(local_name, error);
    if (error)
        return FS_FILE_READERROR;
    // FIXME: single-part upload stops at S3's 5 GiB limit
    if (size > max_single_part_size)
        return FS_FILE_NOTSUPPORTED;
    if (report_progress(local_name, remote_name, 0))
        return FS_FILE_USERABORT;

    const auto path = RemotePath::make(remote_name);
    if (path.profile.empty() || path.bucket.empty() || path.key.empty())
        return FS_FILE_WRITEERROR;

    const auto is_dry = is_dry_run();
    log_operation("PutObject", path, is_dry);
    if (is_dry)
    {
        if ((copy_flags & FS_COPYFLAGS_MOVE) != 0)
            log_local_operation("DeleteLocalFile", local_name, true);
        report_progress(local_name, remote_name, 100);
        return FS_FILE_OK;
    }

    {
        AwsLease lease;
        auto client = get_client(path);

        auto body = Aws::MakeShared<std::fstream>("s3cmd", std::filesystem::path(local_name),
                                                  std::ios::in | std::ios::binary);
        if (!body->is_open())
            return FS_FILE_READERROR;

        Aws::S3::Model::PutObjectRequest request;
        request.SetBucket(path.bucket);
        request.SetKey(path.key);
        request.SetBody(body);
        request.SetContentLength(static_cast<long long>(size));
        if ((copy_flags & FS_COPYFLAGS_OVERWRITE) == 0)
            request.SetIfNoneMatch("*");

        TransferProgress progress{local_name, remote_name, request};
        const auto outcome = client->PutObject(request);
        if (!outcome.IsSuccess())
        {
            if (progress.is_canceled())
                return FS_FILE_USERABORT;
            log_aws_error("PutObject", outcome.GetError());
            return outcome.GetError().GetResponseCode() ==
                           Aws::Http::HttpResponseCode::PRECONDITION_FAILED
                       ? FS_FILE_EXISTS
                       : FS_FILE_WRITEERROR;
        }
    }

    if ((copy_flags & FS_COPYFLAGS_MOVE) != 0)
    {
        log_local_operation("DeleteLocalFile", local_name, false);
        std::filesystem::remove(local_name, error);
        if (error)
        {
            log_error("Delete local source", std::format("error {}", error.value()));
            return FS_FILE_READERROR;
        }
    }

    report_progress(local_name, remote_name, 100);
    return FS_FILE_OK;
}
catch (const std::exception& error)
{
    log_unexpected("FsPutFileW", error);
    return FS_FILE_WRITEERROR;
}

bool delete_file(const wchar_t* remote_name)
try
{
    const auto path = RemotePath::make(remote_name);
    if (path.profile.empty() || path.bucket.empty() || path.key.empty())
        return false;

    const auto is_dry = is_dry_run();
    log_operation("DeleteObject", path, is_dry);
    if (is_dry)
    {
        return true;
    }

    AwsLease lease;
    auto client = get_client(path);
    Aws::S3::Model::DeleteObjectRequest request;
    request.SetBucket(path.bucket);
    request.SetKey(path.key);
    const auto outcome = client->DeleteObject(request);
    if (!outcome.IsSuccess())
        log_aws_error("DeleteObject", outcome.GetError());
    return outcome.IsSuccess();
}
catch (const std::exception& error)
{
    log_unexpected("FsDeleteFileW", error);
    return false;
}

bool make_directory(const wchar_t* remote_name)
try
{
    const auto path = RemotePath::make(remote_name);
    if (path.profile.empty() || path.bucket.empty())
        return false;

    if (path.key.empty())
    {
        const ProfileConfig profile(path.profile);

        // Don't allow bucket registration is the profile already lists remote buckets.
        // Also, check for already registered bucket
        if (profile.has_discovered_buckets() ||
            profile.registered_buckets().contains(path.bucket))
        {
            return false;
        }

        // Try to discover a bucket region
        auto region = discover_bucket_region(path.profile, path.bucket);
        if (region.empty())
        {
            // Can't do, let's ask the user to provide it manually,
            // with profile's default region being the default option
            {
                AwsLease lease;
                const Aws::S3::S3ClientConfiguration configuration{path.profile.c_str(), true};
                region = configuration.region;
            }

            if (request_proc)
            {
                std::array<wchar_t, 128> value{};
                const auto default_region = to_wide(region);
                std::copy_n(default_region.data(),
                            std::min(default_region.size(), value.size() - 1), value.data());

                std::wstring title = L"Register S3 bucket";
                std::wstring prompt =
                    std::format(L"Region for AWS profile '{}:", to_wide(path.profile));
                if (!request_proc(plugin_number, RT_Other, title.data(), prompt.data(),
                                  value.data(), static_cast<int>(value.size())))
                    return false;

                region = to_utf8(value.data());
            }

            // Still no region, early-return with an error
            if (region.empty())
                return false;
        }
        const auto is_dry = is_dry_run();
        log_operation("RegisterBucket", path, is_dry);
        if (is_dry)
        {
            return true;
        }
        return profile.register_bucket(path.bucket, region);
    }

    const RemotePath marker{path.profile, path.bucket, path.directory_prefix()};
    const auto is_dry = is_dry_run();
    log_operation("PutObject", marker, is_dry);
    if (is_dry)
    {
        return true;
    }

    AwsLease lease;
    auto client = get_client(path);
    Aws::S3::Model::PutObjectRequest request;
    request.SetBucket(path.bucket);
    request.SetKey(path.directory_prefix());
    request.SetBody(Aws::MakeShared<Aws::StringStream>("s3cmd"));
    request.SetContentLength(0);
    const auto outcome = client->PutObject(request);
    if (!outcome.IsSuccess())
        log_aws_error("PutObject directory marker", outcome.GetError());
    return outcome.IsSuccess();
}
catch (const std::exception& error)
{
    log_unexpected("FsMkDirW", error);
    return false;
}

bool remove_directory(const wchar_t* remote_name)
try
{
    const auto path = RemotePath::make(remote_name);
    if (path.profile.empty() || path.bucket.empty())
        return false;

    if (path.key.empty())
    {
        if (ProfileConfig(path.profile).has_discovered_buckets())
            return false;

        const auto is_dry = is_dry_run();
        log_operation("UnregisterBucket", path, is_dry);
        if (is_dry)
            return ProfileConfig(path.profile).registered_buckets().contains(path.bucket);
        return ProfileConfig(path.profile).unregister_bucket(path.bucket);
    }

    const auto prefix = path.directory_prefix();

    AwsLease lease;
    auto client = get_client(path);
    Aws::S3::Model::ListObjectsV2Request list;
    list.SetBucket(path.bucket);
    list.SetPrefix(prefix);
    list.SetMaxKeys(2);
    log_operation("ListObjectsV2", {path.profile, path.bucket, prefix}, false);
    const auto listed = client->ListObjectsV2(list);
    if (!listed.IsSuccess())
    {
        log_aws_error("ListObjectsV2", listed.GetError());
        return false;
    }

    bool marker_exists = false;
    for (const auto& object : listed.GetResult().GetContents())
    {
        if (object.GetKey() != prefix)
            return false;
        marker_exists = true;
    }
    if (!marker_exists)
        return true;

    const RemotePath marker{path.profile, path.bucket, prefix};
    const auto is_dry = is_dry_run();
    log_operation("DeleteObject", marker, is_dry);
    if (is_dry)
    {
        return true;
    }

    Aws::S3::Model::DeleteObjectRequest remove;
    remove.SetBucket(path.bucket);
    remove.SetKey(prefix);
    const auto removed = client->DeleteObject(remove);
    if (!removed.IsSuccess())
        log_aws_error("DeleteObject directory marker", removed.GetError());
    return removed.IsSuccess();
}
catch (const std::exception& error)
{
    log_unexpected("FsRemoveDirW", error);
    return false;
}

int rename_or_move(const wchar_t* old_name, const wchar_t* new_name,  bool move, bool overwrite,
                   const RemoteInfoStruct* info)
try
{
    if (info && info->SizeHigh == 0xFFFFFFFF)
        return FS_FILE_NOTSUPPORTED;
    const auto size = info ? (static_cast<std::uint64_t>(info->SizeHigh) << 32) | info->SizeLow : 0;
    // FIXME: CopyObject is limited to 5 GiB and only reports 0%/100%; use
    // multipart copy when larger objects or intermediate progress are needed.
    if (size > max_single_part_size)
        return FS_FILE_NOTSUPPORTED;
    if (report_progress(old_name, new_name, 0))
        return FS_FILE_USERABORT;

    const auto source = RemotePath::make(old_name);
    const auto target = RemotePath::make(new_name);
    if (source.profile.empty() || source.bucket.empty() || source.key.empty() ||
        target.profile.empty() || target.bucket.empty() || target.key.empty())
        return FS_FILE_NOTFOUND;

    const auto is_dry = is_dry_run();
    const auto copy_operation =
        std::format("CopyObject source={}/{}/{}", source.profile, source.bucket, source.key);
    log_operation(copy_operation, target, is_dry);
    if (is_dry)
    {
        if (move)
            log_operation("DeleteObject", source, true);
        report_progress(old_name, new_name, 100);
        return FS_FILE_OK;
    }

    AwsLease lease;
    auto target_client = get_client(target);
    if (!overwrite && remote_exists(*target_client, target))
        return FS_FILE_EXISTS;

    Aws::S3::Model::CopyObjectRequest copy;
    copy.SetBucket(target.bucket);
    copy.SetKey(target.key);
    auto copy_source = std::format("{}/{}", source.bucket, source.key);
    copy.SetCopySource(std::move(copy_source));
    const auto copy_outcome = target_client->CopyObject(copy);
    if (!copy_outcome.IsSuccess())
    {
        log_aws_error("CopyObject", copy_outcome.GetError());
        return copy_outcome.GetError().GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND
                   ? FS_FILE_NOTFOUND
                   : FS_FILE_WRITEERROR;
    }

    if (move)
    {
        auto source_client = get_client(source);
        log_operation("DeleteObject", source, false);
        Aws::S3::Model::DeleteObjectRequest remove;
        remove.SetBucket(source.bucket);
        remove.SetKey(source.key);
        const auto delete_outcome = source_client->DeleteObject(remove);
        if (!delete_outcome.IsSuccess())
        {
            log_aws_error("DeleteObject", delete_outcome.GetError());
            return FS_FILE_WRITEERROR;
        }
    }

    report_progress(old_name, new_name, 100);
    return FS_FILE_OK;
}
catch (const std::exception& error)
{
    log_unexpected("FsRenMovFileW", error);
    return FS_FILE_WRITEERROR;
}

void get_default_root_name(char* name, int max_length)
{
    constexpr std::string_view root_name = "Amazon S3";
    if (!name || max_length <= 0)
        return;
    const auto length = std::min(root_name.size(), static_cast<std::size_t>(max_length - 1));
    std::memcpy(name, root_name.data(), length);
    name[length] = '\0';
}

int content_get_supported_field(int field_index, char* field_name, char* units, int max_length)
{
    if (field_index != 0)
        return ft_nomorefields;
    if (!field_name || !units || max_length <= 0)
        return ft_nomorefields;

    constexpr std::string_view name = "Region";
    const auto length = std::min(name.size(), static_cast<std::size_t>(max_length - 1));
    std::memcpy(field_name, name.data(), length);
    field_name[length] = '\0';
    units[0] = '\0';
    return ft_string;
}

int content_get_value(const wchar_t* file_name, int field_index, void* field_value, int max_length)
{
    if (field_index != 0)
        return ft_nosuchfield;
    if (!file_name || !field_value || max_length < static_cast<int>(sizeof(wchar_t)))
        return ft_fileerror;

    const auto path = RemotePathView::make(file_name);
    // Region is only available at buckets view
    if (path.profile.empty() || path.bucket.empty() || !path.key.empty())
        return ft_fieldempty;

    // Get region from a cached config
    const auto region = ProfileConfig(to_utf8(path.profile)).bucket_region(to_utf8(path.bucket));
    if (region.empty())
        return ft_fieldempty;

    const auto value = to_wide(region);
    auto* output = static_cast<wchar_t*>(field_value);
    const auto capacity = static_cast<std::size_t>(max_length) / sizeof(wchar_t);
    const auto length = std::min(value.size(), capacity - 1);
    std::copy_n(value.data(), length, output);
    output[length] = L'\0';
    return ft_stringw;
}

} // namespace s3cmd
