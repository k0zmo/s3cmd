#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

// Forward declarations
namespace Aws::Config {
    class Profile;
}

namespace s3cmd {

class PluginHost;

class SsoLoginFailed : public std::runtime_error
{
public:
    explicit SsoLoginFailed(std::string message);
};

class SsoLoginCancelled : public SsoLoginFailed
{
public:
    explicit SsoLoginCancelled(std::string_view profile);
};

void perform_sso_login(PluginHost& plugin_host,
                       const Aws::Config::Profile& profile);

} // namespace s3cmd
