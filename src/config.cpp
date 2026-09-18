#include "config.hpp"

#include <toml++/toml.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
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
    };

    bool dry_run{false};
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
        // Deserialize TOML document into RuntimeConfig.
        runtime_config->dry_run = (*document)["settings"]["DryRun"].value_or(false);
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
    // Serialize our config to TOML document and write it to disk.
    toml::table document;
    document.emplace("settings", toml::table{{"DryRun", dry_run},
                                             {"AwsLogLevel", aws_log_level},
                                             {"PreferSsoDeviceCode", prefer_sso_device_code},
                                             {"EnableIMDS", enable_imds}});

    toml::table profile_tables;
    for (const auto& [profile_name, profile] : profiles)
    {
        if (profile.registered_buckets.empty())
            continue;

        toml::table bucket_map;
        for (const auto& [bucket_name, bucket_info] : profile.registered_buckets)
            bucket_map.emplace(bucket_name, bucket_info.region);

        toml::table profile_table;
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
        if (_wdupenv_s(&app_data, &size, L"APPDATA") != 0 || !app_data || !*app_data)
        {
            std::free(app_data);
            throw std::runtime_error("APPDATA is not set");
        }
        std::unique_ptr<wchar_t, decltype(&std::free)> releaser(app_data, &std::free);
        return std::filesystem::path(releaser.get()) / L"s3cmd";
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

bool is_dry_run()
{
    std::scoped_lock lock(config_mtx);
    return RuntimeConfig::get().dry_run;
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
    RuntimeConfig::get().profiles[profile_].discovered_buckets = std::move(buckets);
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

} // namespace s3cmd
