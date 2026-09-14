#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace s3cmd {

struct ContentRange
{
    std::uint64_t first{};
    std::uint64_t last{};
    std::uint64_t size{};

    bool matches(std::uint64_t expected_first, std::uint64_t expected_size,
                 std::uint64_t content_length) const;
};

std::optional<ContentRange> parse_content_range(std::string_view value);

} // namespace s3cmd
