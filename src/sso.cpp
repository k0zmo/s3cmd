#include "sso.hpp"
#include "core.hpp"
#include "log.hpp"

// Windows defines GetObject as GetObjectA, including in the AWS JSON API.
#undef GetObject

#include <aws/core/auth/bearer-token-provider/SSOBearerTokenProvider.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/utils/Array.h>
#include <aws/core/utils/DateTime.h>
#include <aws/core/utils/HashingUtils.h>
#include <aws/core/utils/StringUtils.h>
#include <aws/core/utils/json/JsonSerializer.h>
#include <aws/core/utils/crypto/Factories.h>
#include <aws/core/utils/crypto/SecureRandom.h>
#include <aws/core/utils/memory/stl/AWSString.h>
#include <aws/sso-oidc/SSOOIDCClient.h>
#include <aws/sso-oidc/SSOOIDCErrors.h>
#include <aws/sso-oidc/model/CreateTokenRequest.h>
#include <aws/sso-oidc/model/CreateTokenResult.h>
#include <aws/sso-oidc/model/RegisterClientRequest.h>
#include <aws/sso-oidc/model/RegisterClientResult.h>
#include <aws/sso-oidc/model/StartDeviceAuthorizationRequest.h>

#include <httplib.h>

#include <shellapi.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <ios>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace s3cmd {

namespace {

constexpr const wchar_t* title = L"Amazon S3";

Aws::SSOOIDC::Model::RegisterClientResult
    register_sso_client(const std::filesystem::path& cache_directory, const Aws::String& start_url,
                        const Aws::String& region,
                        const Aws::SSOOIDC::Model::RegisterClientRequest& request,
                        const Aws::SSOOIDC::SSOOIDCClient& oidc)
{
    const auto key = Aws::Utils::Json::JsonValue()
                         .WithString("cache", "s3cmd-client-registration-v1")
                         .WithString("startUrl", start_url)
                         .WithString("region", region)
                         .WithString("request", request.SerializePayload())
                         .View()
                         .WriteCompact();

    const auto hash =
        Aws::Utils::HashingUtils::HexEncode(Aws::Utils::HashingUtils::CalculateSHA256(key));
    const auto path = cache_directory / (hash + ".json");
    {
        std::ifstream input(path);
        if (input)
        {
            const Aws::Utils::Json::JsonValue cached(input);
            const auto view = cached.View();
            if (cached.WasParseSuccessful() && view.IsObject() &&
                view.KeyExists("clientId") &&
                view.GetObject("clientId").IsString() &&
                view.KeyExists("clientSecret") &&
                view.GetObject("clientSecret").IsString() &&
                view.KeyExists("clientSecretExpiresAt") &&
                view.GetObject("clientSecretExpiresAt").IsIntegerType())
            {
                const auto clientId = view.GetString("clientId");
                const auto clientSecret = view.GetString("clientSecret");
                const auto clientSecretExpiresAt = view.GetInt64("clientSecretExpiresAt");

                if (!clientId.empty() && !clientSecret.empty() &&
                    clientSecretExpiresAt > Aws::Utils::DateTime::Now().Seconds())
                {
                    Aws::SSOOIDC::Model::RegisterClientResult result;
                    result.SetClientId(clientId);
                    result.SetClientSecret(clientSecret);
                    result.SetClientSecretExpiresAt(clientSecretExpiresAt);
                    return result;
                }
            }
        }
    }

    const auto registration = oidc.RegisterClient(request);
    if (!registration.IsSuccess())
    {
        throw SsoLoginFailed(std::format("AWS SSO client registration failed: {}",
                                         registration.GetError().GetMessage()));
    }

    const auto& result = registration.GetResult();
    const auto clientId = result.GetClientId();
    const auto clientSecret = result.GetClientSecret();
    const auto clientSecretExpiresAt = result.GetClientSecretExpiresAt();

    if (clientId.empty() || clientSecret.empty() ||
        clientSecretExpiresAt <= Aws::Utils::DateTime::Now().Seconds())
    {
        throw SsoLoginFailed("AWS SSO returned an invalid client registration");
    }

    const auto cached = Aws::Utils::Json::JsonValue()
                            .WithString("clientId", clientId)
                            .WithString("clientSecret", clientSecret)
                            .WithInt64("clientSecretExpiresAt", clientSecretExpiresAt);
    std::error_code error;
    std::filesystem::create_directories(cache_directory, error);
    if (error)
        throw SsoLoginFailed("Cannot create the AWS SSO registration cache directory");
    std::ofstream output(path, std::ios::trunc);
    output << cached.View().WriteCompact();
    output.close();
    if (!output)
        throw SsoLoginFailed("Cannot write the AWS SSO client registration cache");
    return result;
}

bool open_url(std::string_view url)
{
    const auto wide_url = to_wide(url);
    return reinterpret_cast<std::intptr_t>(ShellExecuteW(nullptr, L"open", wide_url.c_str(),
                                                         nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}

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

    SsoTokenWriter::Token cached;
    cached.accessToken = token.GetAccessToken();
    cached.expiresAt = Aws::Utils::DateTime::Now() + std::chrono::seconds(token.GetExpiresIn());
    cached.refreshToken = token.GetRefreshToken();
    cached.clientId = registration.GetClientId();
    cached.clientSecret = registration.GetClientSecret();
    cached.registrationExpiresAt =
        Aws::Utils::DateTime(static_cast<std::uint64_t>(registration.GetClientSecretExpiresAt()));
    cached.region = region;
    cached.startUrl = start_url;
    if (!SsoTokenWriter(profile_name).write(cached))
        throw SsoLoginFailed("Cannot write the AWS SSO token cache");
}

std::filesystem::path sso_cache_directory()
{
    return s3cmd::config_directory_path() / "sso" / "cache";
}

void perform_pkce_sso_login(PluginHost& plugin_host, std::string_view profile_name,
                            const Aws::String& start_url, const Aws::String& region,
                            Aws::SSOOIDC::SSOOIDCClient& oidc)
{
    constexpr std::string_view registered_redirect_uri = "http://127.0.0.1/oauth/callback";

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
    const auto registration =
        register_sso_client(sso_cache_directory(), start_url, region, register_request, oidc);

    const auto dns_suffix = region.starts_with("cn-") ? "amazonaws.com.cn" : "amazonaws.com";
    const auto authorization_url = std::format(
        "https://oidc.{}.{}/authorize?response_type=code&client_id={}&redirect_uri={}&state={}"
        "&code_challenge_method=S256&scopes={}&code_challenge={}",
        region,
        dns_suffix,
        Aws::Utils::StringUtils::URLEncode(registration.GetClientId()),
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
            !plugin_host.notify_message_box(
                PluginHost::message_box_type::msg_ok_cancel, title,
                L"The AWS SSO browser could not be opened automatically.\n\n"
                L"Open this URL manually:\n{}\n\n"
                L"Click OK to keep waiting or Cancel to use another login method.",
                to_wide(authorization_url)))
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
    token_request.SetClientId(registration.GetClientId());
    token_request.SetClientSecret(registration.GetClientSecret());
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

    write_sso_token(profile_name, start_url, region, registration, token.GetResult());
}

void perform_device_sso_login(PluginHost& plugin_host, std::string_view profile_name,
                              const Aws::String& start_url, const Aws::String& region,
                              Aws::SSOOIDC::SSOOIDCClient& oidc)
{
    Aws::SSOOIDC::Model::RegisterClientRequest register_request;
    register_request.SetClientName("s3cmd");
    register_request.SetClientType("public");
    register_request.AddGrantTypes("urn:ietf:params:oauth:grant-type:device_code");
    register_request.AddGrantTypes("refresh_token");
    register_request.AddScopes("sso:account:access");
    const auto registration =
        register_sso_client(sso_cache_directory(), start_url, region, register_request, oidc);

    Aws::SSOOIDC::Model::StartDeviceAuthorizationRequest start_request;
    start_request.SetClientId(registration.GetClientId());
    start_request.SetClientSecret(registration.GetClientSecret());
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
    if (!plugin_host.notify_message_box(
            PluginHost::message_box_type::msg_ok_cancel, title,
            L"Complete AWS SSO login for profile '{}' in your browser.\n\n"
            L"URL: {}\nCode: {}\n\nClick OK after AWS reports success.",
            to_wide(profile_name), to_wide(url), to_wide(device.GetUserCode())))
    {
        throw SsoLoginFailed(
            std::format("AWS SSO login for profile '{}' was cancelled", profile_name));
    }

    if (!browser_opened)
        log("[s3cmd] AWS SSO login: browser could not be opened automatically");

    Aws::SSOOIDC::Model::CreateTokenRequest token_request;
    token_request.SetClientId(registration.GetClientId());
    token_request.SetClientSecret(registration.GetClientSecret());
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
            write_sso_token(profile_name, start_url, region, registration,
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

} // namespace

void perform_sso_login(PluginHost& plugin_host, std::string_view profile_name,
                       const Aws::Config::Profile& profile)
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

    if (!plugin_host.is_notify_message_box_available())
    {
        throw SsoLoginFailed("AWS SSO browser login requires an interactive file manager");
    }

    if (!plugin_host.notify_message_box(
            PluginHost::message_box_type::msg_yes_no, title,
            L"AWS SSO credentials for profile '{}' are unavailable or expired.\n\n"
            L"Start browser login?",
            to_wide(profile_name)))
    {
        throw SsoLoginFailed(
            std::format("AWS SSO login for profile '{}' was cancelled", profile_name));
    }

    Aws::Client::ClientConfiguration configuration;
    configuration.region = region;
    Aws::SSOOIDC::SSOOIDCClient oidc(configuration);

    try
    {
        perform_pkce_sso_login(plugin_host, profile_name, start_url, region, oidc);
        return;
    }
    catch (const SsoLoginFailed& error)
    {
        log("[s3cmd] AWS SSO PKCE login failed: {}", error.what());
        if (!plugin_host.notify_message_box(PluginHost::message_box_type::msg_yes_no, title,
                                            L"AWS SSO browser login failed:\n{}\n\n"
                                            L"Try device-code login instead?",
                                            to_wide(error.what())))
        {
            throw;
        }
    }

    perform_device_sso_login(plugin_host, profile_name, start_url, region, oidc);
}

} // namespace s3cmd
