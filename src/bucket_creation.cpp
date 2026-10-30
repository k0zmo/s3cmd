#include "bucket_creation.hpp"

#include <algorithm>
#include <fmt/format.h>

namespace s3cmd {

std::string build_bucket_name(std::string_view prefix, const BucketCreationOptions& options)
{
    return !options.account_regional
               ? std::string(prefix)
               // For example: mybucket-123456789012-us-east-1-an
               // Other variant: mybucket--123456789012--us-east-1--an
               : fmt::format("{}-{}-{}-an", prefix, options.account_id, options.region);
}

std::string validate_bucket_name(std::string_view prefix, const BucketCreationOptions& options)
{
    if (options.region.empty() || options.region.size() > 63 ||
        options.region.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") !=
            options.region.npos)
    {
        return "Enter an AWS region code, such as eu-central-1.";
    }

    if (options.account_regional)
    {
        if (options.account_id.size() != 12 ||
            options.account_id.find_first_not_of("0123456789") != options.account_id.npos)
        {
            return "The AWS account ID is unavailable.";
        }

        if (options.region == "me-south-1" || options.region == "me-central-1")
            return "Account regional buckets are unavailable in this region.";
    }

    const auto name = build_bucket_name(prefix, options);
    if (prefix.empty() || name.size() < 3 || name.size() > 63)
        return "The full bucket name must contain 3 to 63 characters.";

    const auto alphanumeric = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    if (!alphanumeric(prefix.front()) || !alphanumeric(prefix.back()) ||
        name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789.-") != name.npos ||
        name.find("..") != name.npos)
    {
        return "Use lowercase letters, digits, dots, or hyphens. Begin and end with a letter or "
               "digit.";
    }

    for (const auto reserved : {"xn--", "sthree-", "amzn-s3-demo-"})
    {
        if (name.starts_with(reserved))
            return "This bucket name uses a reserved prefix.";
    }

    for (const auto reserved :
         {"-s3alias", "--ol-s3", ".mrap", "--x-s3", "--table-s3", "--vector-s3"})
    {
        if (name.ends_with(reserved))
            return "This bucket name uses a reserved suffix.";
    }

    if (!options.account_regional && name.ends_with("-an"))
        return "The -an suffix requires the Account regional namespace.";

    if (std::count(name.begin(), name.end(), '.') == 3 &&
        name.find_first_not_of("0123456789.") == name.npos)
    {
        return "The bucket name cannot be an IP address.";
    }

    return {};
}

} // namespace s3cmd
