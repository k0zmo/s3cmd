#pragma once

#include <string>
#include <string_view>

namespace s3cmd {

struct BucketCreationOptions
{
    std::string bucket_name;
    std::string region;
    bool account_regional{};
    std::string account_id;
};

inline constexpr std::string_view bucket_regions[] = {
    "af-south-1",     "ap-east-1",      "ap-east-2",      "ap-northeast-1", "ap-northeast-2",
    "ap-northeast-3", "ap-south-1",     "ap-south-2",     "ap-southeast-1", "ap-southeast-2",
    "ap-southeast-3", "ap-southeast-4", "ap-southeast-5", "ap-southeast-6", "ap-southeast-7",
    "ca-central-1",   "ca-west-1",      "cn-north-1",     "cn-northwest-1", "eu-central-1",
    "eu-central-2",   "eu-north-1",     "eu-south-1",     "eu-south-2",     "eu-west-1",
    "eu-west-2",      "eu-west-3",      "eusc-de-east-1", "il-central-1",   "me-central-1",
    "me-south-1",     "mx-central-1",   "sa-east-1",      "us-east-1",      "us-east-2",
    "us-gov-east-1",  "us-gov-west-1",  "us-west-1",      "us-west-2"};

std::string build_bucket_name(std::string_view prefix, const BucketCreationOptions& options);
std::string validate_bucket_name(std::string_view prefix, const BucketCreationOptions& options);

} // namespace s3cmd
