#pragma once

#include <toml++/toml.hpp>

#include <optional>

namespace std::filesystem {

class path;
} // namespace std::filesystem

namespace s3cmd {

std::optional<toml::table> read_document(const std::filesystem::path& file_path);
bool write_document(const toml::table& document, const std::filesystem::path& file_path);

} // namespace s3cmd
