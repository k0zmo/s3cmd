#pragma once

#include <string>
#include <utility>
#include <vector>

namespace s3cmd {

enum class PropertiesKind
{
    bucket,
    object,
    prefix
};

struct S3Properties
{
    PropertiesKind kind{PropertiesKind::bucket};
    std::string address;
    std::string region;
    std::string details{"Loading properties..."};
    std::vector<std::pair<std::string, std::string>> tags;
    std::string tag_status{"Loading tags..."};
};

} // namespace s3cmd
