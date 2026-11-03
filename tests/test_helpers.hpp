#pragma once

#include "s3.hpp"
#include "platform.hpp"
#include "utils.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>

namespace s3cmd::test {

struct TemporaryEnvironment
{
    TemporaryEnvironment(const char* variable_name, const char* value) : name(variable_name)
    {
#ifdef _WIN32
        const auto wide_name = to_utf16(name);
        wchar_t* previous{};
        std::size_t size{};
        if (_wdupenv_s(&previous, &size, win32_string(wide_name.c_str())) != 0)
            throw std::runtime_error("Cannot read environment variable " + name);
        std::unique_ptr<wchar_t, decltype(&std::free)> releaser(previous, &std::free);
        if (previous)
        {
            old = to_utf8(utf16_string(previous));
            existed = true;
        }
#else
        if (const auto* previous = std::getenv(name.c_str()))
        {
            old = previous;
            existed = true;
        }
#endif
        set(value);
    }

    ~TemporaryEnvironment() { set_value(existed ? old.c_str() : nullptr); }

    TemporaryEnvironment(const TemporaryEnvironment&) = delete;
    TemporaryEnvironment& operator=(const TemporaryEnvironment&) = delete;

    void set(const char* value)
    {
        if (set_value(value) != 0)
            throw std::runtime_error("Cannot set environment variable " + name);
    }

private:
    int set_value(const char* value) const
    {
#ifdef _WIN32
        const auto wide_name = to_utf16(name);
        const auto wide_value = to_utf16(value ? value : "");
        return _wputenv_s(win32_string(wide_name.c_str()), win32_string(wide_value.c_str()));
#else
        return value ? ::setenv(name.c_str(), value, 1) : ::unsetenv(name.c_str());
#endif
    }

    std::string name;
    std::string old;
    bool existed{};
};

struct PluginSession
{
    PluginSession(int number = 0, tProgressProcW progress = nullptr, tLogProcW log = nullptr,
                  tRequestProcW request = nullptr)
    {
        initialize(number, progress, log, request);
    }

    ~PluginSession() { shutdown(); }
};

inline std::filesystem::path create_temporary_directory()
{
    const auto base = std::filesystem::temp_directory_path() /
                      ("s3cmd-" + std::to_string(
                          std::chrono::steady_clock::now().time_since_epoch().count()));
    for (unsigned attempt = 0; ; ++attempt)
    {
        auto candidate = base;
        candidate += "-" + std::to_string(attempt);
        if (std::filesystem::create_directory(candidate))
            return candidate;
    }
}

struct TemporaryConfig
{
    TemporaryConfig()
        : root(create_temporary_directory()),
          path(root / "s3cmd" / "s3cmd.toml"),
#ifdef _WIN32
          config_home("APPDATA", to_utf8(root.u16string()).c_str())
#else
          config_home("XDG_CONFIG_HOME", root.string().c_str())
#endif
    {
    }

    ~TemporaryConfig()
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void reset()
    {
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        reset_config();
    }

    std::filesystem::path root;
    std::filesystem::path path;
    TemporaryEnvironment config_home;
};

inline TemporaryConfig& temporary_config()
{
    static TemporaryConfig config;
    config.reset();
    return config;
}

} // namespace s3cmd::test
