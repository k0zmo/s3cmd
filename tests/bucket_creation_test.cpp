#include "bucket_creation.hpp"

#include <catch2/catch_test_macros.hpp>

#include <string>

TEST_CASE("bucket creation validates global names and editable region codes", "[unit]")
{
    s3cmd::BucketCreationOptions options{{}, "future-region-1", false, {}};
    for (const auto* name : {"my-bucket", "abc", "bucket.with.dots", "123"})
    {
        CHECK(s3cmd::validate_bucket_name(name, options).empty());
        CHECK(s3cmd::build_bucket_name(name, options) == name);
    }
    CHECK(s3cmd::validate_bucket_name(std::string(63, 'a'), options).empty());
    for (const auto* name : {"", "ab", "Invalid_Name", "-bucket", "bucket-", "two..dots",
                             "192.168.1.1", "xn--bucket", "sthree-bucket", "amzn-s3-demo-bucket",
                             "bucket-s3alias", "bucket--ol-s3", "bucket.mrap", "bucket--x-s3",
                             "bucket--table-s3", "bucket--vector-s3", "bucket-an"})
    {
        INFO(name);
        CHECK_FALSE(s3cmd::validate_bucket_name(name, options).empty());
    }
    CHECK_FALSE(s3cmd::validate_bucket_name(std::string(64, 'a'), options).empty());
    for (const auto* region : {"", "EU-CENTRAL-1", "eu_central_1", "eu-central-1/"})
    {
        options.region = region;
        CHECK_FALSE(s3cmd::validate_bucket_name("my-bucket", options).empty());
    }
}

TEST_CASE("account regional bucket validation uses the complete generated name", "[unit]")
{
    s3cmd::BucketCreationOptions options{{}, "eu-central-1", true, "123456789012"};
    CHECK(s3cmd::build_bucket_name("my-bucket", options) ==
          "my-bucket-123456789012-eu-central-1-an");
    CHECK(s3cmd::validate_bucket_name("my-bucket", options).empty());
    const auto suffix_size = s3cmd::build_bucket_name("", options).size();
    CHECK(s3cmd::validate_bucket_name(std::string(63 - suffix_size, 'a'), options).empty());
    CHECK_FALSE(s3cmd::validate_bucket_name(std::string(64 - suffix_size, 'a'), options).empty());
    CHECK_FALSE(s3cmd::validate_bucket_name("", options).empty());
    for (const auto* account : {"", "123", "12345678901a"})
    {
        options.account_id = account;
        CHECK_FALSE(s3cmd::validate_bucket_name("my-bucket", options).empty());
    }
    options.account_id = "123456789012";
    for (const auto* region : {"me-south-1", "me-central-1"})
    {
        options.region = region;
        CHECK_FALSE(s3cmd::validate_bucket_name("my-bucket", options).empty());
    }
}
