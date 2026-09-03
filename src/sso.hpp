#pragma once

#include <stdexcept>
#include <string>

// Forward declarations
namespace Aws::Config {
    class Profile;
}

namespace s3cmd {

class PluginHost;

class SsoLoginFailed : public std::runtime_error
{
public:
    explicit SsoLoginFailed(std::string message) : std::runtime_error(std::move(message)) {}
};

void perform_sso_login(PluginHost& plugin_host,
                       std::string_view profile_name,
                       const Aws::Config::Profile& profile);

} // namespace s3cmd
