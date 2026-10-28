#include "config.hpp"
#include "toml.hpp"
#include "prereq.hpp"

#include <toml++/toml.hpp>

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stdexcept>

#ifdef _WIN32
#  include <memory>
#endif

namespace s3cmd {

namespace {

// All operations on RuntimeConfig require config_mtx to be held.
struct RuntimeConfig
{
    static RuntimeConfig& get();

    // Mirror the current config on disk at path().
    bool flush_to_disk();

    struct ProfileSettings
    {
        BucketMap registered_buckets;
        BucketMap discovered_buckets;
        bool has_discovered_buckets{};
        bool bucket_registration_hint_shown{};
    };

    bool prefer_sso_device_code{false};
    bool enable_imds{false};
    std::string aws_log_level{"Info"};
    std::map<std::string, ProfileSettings, std::less<>> profiles;

    static const std::filesystem::path& path()
    {
        static const std::filesystem::path value = config_directory_path() / "s3cmd.toml";
        return value;
    }
};

std::mutex config_mtx;
std::optional<RuntimeConfig> runtime_config;

RuntimeConfig& RuntimeConfig::get()
{
    if (runtime_config)
        return *runtime_config;
    runtime_config.emplace();

    if (auto document = read_document(path()))
    {
        // Deserialize TOML document into RuntimeConfig.
        runtime_config->prefer_sso_device_code =
            (*document)["settings"]["PreferSsoDeviceCode"].value_or(false);
        runtime_config->enable_imds = (*document)["settings"]["EnableIMDS"].value_or(false);
        runtime_config->aws_log_level =
            (*document)["settings"]["AwsLogLevel"].value_or("Info");

        if (const auto* profiles = document->get_as<toml::table>("profiles"))
        {
            for (const auto& [name, profile] : *profiles)
            {
                const auto* profile_table = profile.as_table();
                if (!profile_table)
                    continue;
                auto& settings = runtime_config->profiles[std::string(name.str())];
                settings.bucket_registration_hint_shown =
                    (*profile_table)["BucketRegistrationHintShown"].value_or(false);
                const auto* buckets = profile_table->get_as<toml::table>("buckets");
                if (!buckets)
                    continue;
                auto& registered = settings.registered_buckets;
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
    // Serialize our config to TOML document and write it to disk.
    toml::table document;
    document.emplace("settings", toml::table{{"AwsLogLevel", aws_log_level},
                                             {"PreferSsoDeviceCode", prefer_sso_device_code},
                                             {"EnableIMDS", enable_imds}});

    toml::table profile_tables;
    for (const auto& [profile_name, profile] : profiles)
    {
        if (profile.registered_buckets.empty() && !profile.bucket_registration_hint_shown)
            continue;

        toml::table bucket_map;
        for (const auto& [bucket_name, bucket_info] : profile.registered_buckets)
            bucket_map.emplace(bucket_name, bucket_info.region);

        toml::table profile_table;
        if (profile.bucket_registration_hint_shown)
            profile_table.emplace("BucketRegistrationHintShown", true);
        profile_table.emplace("buckets", std::move(bucket_map));
        profile_tables.emplace(profile_name, std::move(profile_table));
    }
    if (!profile_tables.empty())
        document.emplace("profiles", std::move(profile_tables));

    return write_document(document, path());
}

} // namespace

const std::filesystem::path& config_directory_path()
{
    static std::filesystem::path value = [] {
#ifdef _WIN32
        wchar_t* app_data{};
        std::size_t size{};
        // _wdupenv_s allocates a correctly sized UTF-16 copy.
        if (_wdupenv_s(&app_data, &size, win32_string(u"APPDATA")) != 0 || !app_data || !*app_data)
        {
            std::free(app_data);
            throw std::runtime_error("APPDATA is not set");
        }
        std::unique_ptr<wchar_t, decltype(&std::free)> releaser(app_data, &std::free);
        return std::filesystem::path(releaser.get()) / u"s3cmd";
#else
        if (const auto* config_home = std::getenv("XDG_CONFIG_HOME"); config_home && *config_home)
            return std::filesystem::path(config_home) / "s3cmd";
        if (const auto* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".config" / "s3cmd";
        throw std::runtime_error("XDG_CONFIG_HOME and HOME are not set");
#endif
    }();
    return value;
}

bool prefer_sso_device_code()
{
    std::scoped_lock lock(config_mtx);
    return RuntimeConfig::get().prefer_sso_device_code;
}

bool is_imds_enabled()
{
    std::scoped_lock lock(config_mtx);
    return RuntimeConfig::get().enable_imds;
}

std::string aws_log_level()
{
    std::scoped_lock lock(config_mtx);
    return RuntimeConfig::get().aws_log_level;
}

void reset_runtime_config()
{
    std::scoped_lock lock(config_mtx);
    runtime_config.reset();
}

BucketMap ProfileConfig::registered_buckets() const
{
    std::scoped_lock lock(config_mtx);
    const auto& config = RuntimeConfig::get();
    const auto profile = config.profiles.find(profile_);
    return profile == config.profiles.end() ? BucketMap{}
                                            : profile->second.registered_buckets;
}

bool ProfileConfig::take_bucket_registration_hint() const
{
    std::scoped_lock lock(config_mtx);
    auto& config = RuntimeConfig::get();
    auto& profile = config.profiles[profile_];
    if (profile.bucket_registration_hint_shown || !profile.registered_buckets.empty())
        return false;
    profile.bucket_registration_hint_shown = true;
    config.flush_to_disk();
    return true;
}

bool ProfileConfig::has_discovered_buckets() const
{
    std::scoped_lock lock(config_mtx);
    const auto& config = RuntimeConfig::get();
    const auto profile = config.profiles.find(profile_);
    return profile != config.profiles.end() && profile->second.has_discovered_buckets;
}

void ProfileConfig::set_discovered_buckets(BucketMap buckets) const
{
    std::scoped_lock lock(config_mtx);
    auto& profile = RuntimeConfig::get().profiles[profile_];
    profile.has_discovered_buckets = !buckets.empty();
    profile.discovered_buckets = std::move(buckets);
}

void ProfileConfig::cache_bucket_region(const std::string& bucket, std::string region) const
{
    std::scoped_lock lock(config_mtx);
    auto& profile = RuntimeConfig::get().profiles[profile_];
    profile.discovered_buckets[bucket] = BucketInfo{std::move(region)};
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

bool ProfileConfig::register_bucket(const std::string& bucket, std::string region) const
{
    std::scoped_lock lock(config_mtx);
    auto& config = RuntimeConfig::get();
    config.profiles[profile_].registered_buckets[bucket] = BucketInfo{std::move(region)};
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

bool ProfileConfig::forget_bucket(std::string_view bucket) const
{
    std::scoped_lock lock(config_mtx);
    auto& config = RuntimeConfig::get();
    auto& profile = config.profiles[profile_];
    const std::string name(bucket);
    profile.discovered_buckets.erase(name);
    return !profile.registered_buckets.erase(name) || config.flush_to_disk();
}

} // namespace s3cmd
