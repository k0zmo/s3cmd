#pragma once

#include <aws/s3/model/ObjectStorageClass.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace s3cmd {

// S3 object metadata that we get for free from ListObjectV2
struct ObjectMetadata
{
    Aws::S3::Model::ObjectStorageClass storage_class{};
    std::wstring etag;
};

template <typename String = std::wstring>
struct MetadataDirectory
{
    String profile;
    String bucket;
    String prefix; // Object name without the "filename"
};

template <typename L, typename R>
bool operator<(const MetadataDirectory<L>& left,
               const MetadataDirectory<R>& right) noexcept
{
    return std::tie(left.profile, left.bucket, left.prefix) <
           std::tie(right.profile, right.bucket, right.prefix);
}

using ObjectMetadataMap = std::map<std::wstring, ObjectMetadata, std::less<>>;

// Cache for object metadata with a configurable maximum size. The cache groups entries by
// directory as listing discover them together. It evicts one complete directory at a time.
class ObjectMetadataCache
{
public:
    explicit ObjectMetadataCache(std::size_t max_entries = 10'000)
        : max_entries_(max_entries)
    {
    }

    void add(MetadataDirectory<> key, ObjectMetadataMap objects)
    {
        const auto entries = objects.size() + 1; // +1 for directory record itself

        auto [entry, inserted] = directories_.try_emplace(std::move(key));
        if (!inserted)
            num_entries_ -= entry->second.objects.size() + 1;
        entry->second = {std::move(objects), ++generation_};
        num_entries_ += entries;

        while (num_entries_ > max_entries_ && directories_.size() > 1)
        {
            // We've exceeded the limit, get rid of the least recently used metadata directory
            const auto oldest = std::min_element(directories_.begin(), directories_.end(),
                                                 [](const auto& a, const auto& b) {
                                                     return a.second.last_used < b.second.last_used;
                                                 });
            num_entries_ -= oldest->second.objects.size() + 1;
            directories_.erase(oldest);
        }
    }

    template <typename String>
    const ObjectMetadata* find(const MetadataDirectory<String>& key,
                               std::wstring_view name)
    {
        const auto directory = directories_.find(key);
        if (directory == directories_.end())
            return nullptr;
        const auto object = directory->second.objects.find(name);
        if (object == directory->second.objects.end())
            return nullptr;
        directory->second.last_used = ++generation_;
        return &object->second;
    }

    void clear()
    {
        directories_.clear();
        num_entries_ = 0;
        generation_ = 0;
    }

    std::size_t retained_entries() const { return num_entries_; }

private:
    struct Directory
    {
        ObjectMetadataMap objects;
        std::uint64_t last_used{};
    };
    const std::size_t max_entries_;
    std::map<MetadataDirectory<>, Directory, std::less<>> directories_;
    std::size_t num_entries_{};
    std::uint64_t generation_{};
};

} // namespace s3cmd
