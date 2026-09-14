#include "content_range.hpp"
#include "utils.hpp"

namespace s3cmd {

bool ContentRange::matches(std::uint64_t expected_first, std::uint64_t expected_size,
                           std::uint64_t content_length) const
{
    return first == expected_first && first <= last && size != 0 && size == expected_size &&
           last == size - 1 && content_length == size - first;
}

std::optional<ContentRange> parse_content_range(std::string_view value)
{
    if (!value.starts_with("bytes "))
        return std::nullopt;
    value.remove_prefix(6);

    const auto dash = value.find('-');
    const auto slash = value.find('/');
    if (dash == value.npos || slash == value.npos || dash > slash)
        return std::nullopt;

    const auto first = parse_number<std::uint64_t>(value.substr(0, dash));
    const auto last = parse_number<std::uint64_t>(value.substr(dash + 1, slash - dash - 1));
    const auto size = parse_number<std::uint64_t>(value.substr(slash + 1));
    if (!first || !last || !size)
        return std::nullopt;
    return ContentRange{*first, *last, *size};
}

} // namespace s3cmd
