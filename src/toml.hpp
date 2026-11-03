#pragma once

#include <toml++/toml.hpp>

#include <filesystem>
#include <optional>

namespace s3cmd {

std::optional<toml::table> read_document(const std::filesystem::path& file_path);
bool write_document(const toml::table& document, const std::filesystem::path& file_path);

} // namespace s3cmd
