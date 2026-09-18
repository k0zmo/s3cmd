#pragma once

#include "fsplugin.h" // FILETIME

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>

// Forward declarations
namespace std::filesystem {

class path;
} // namespace std::filesystem

namespace s3cmd {

struct BucketInfo
{
    std::string region;
    FILETIME created{};
};
using BucketMap = std::map<std::string, BucketInfo, std::less<>>;

class ProfileConfig
{
public:
    explicit ProfileConfig(std::string profile)
      : profile_{std::move(profile)}
    {
    }

    BucketMap registered_buckets() const;
    bool has_discovered_buckets() const;
    void set_discovered_buckets(BucketMap buckets) const;
    std::string bucket_region(std::string_view bucket) const;
    bool register_bucket(std::string_view bucket, std::string_view region) const;
    bool unregister_bucket(std::string_view bucket) const;

private:
    std::string profile_;
};

// Returns a path to config directory of the plugin
const std::filesystem::path& config_directory_path();

bool is_dry_run();
bool prefer_sso_device_code();
bool is_imds_enabled();
std::string aws_log_level();

// Test only exposure
void reset_runtime_config();

} // namespace s3cmd
