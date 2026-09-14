#include "toml.hpp"

#include <toml++/toml.hpp>

#include <fstream>
#include <filesystem>
#include <optional>

namespace s3cmd {

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

} // namespace s3cmd
