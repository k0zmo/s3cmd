#include "icons.hpp"
#include "core.hpp"

#include <fmt/xchar.h>

#ifdef _WIN32
#  include "win32/resource.h"
extern "C" IMAGE_DOS_HEADER __ImageBase;
#else
#  include "icons_data.hpp"
#  include <array>
#  include <cstdint>
#  include <span>
#endif

#include <algorithm>

namespace s3cmd {

namespace {

enum class IconType
{
    none,
    profile,
    bucket
};

#ifdef _WIN32
HICON load_icon(int resource_id, int size)
{
    return static_cast<HICON>(LoadImageW(reinterpret_cast<HINSTANCE>(&__ImageBase),
                                         MAKEINTRESOURCEW(resource_id), IMAGE_ICON, size, size,
                                         LR_DEFAULTCOLOR));
}
#else
std::uint32_t read_le32(const unsigned char* data)
{
    return std::uint32_t{data[0]} | (std::uint32_t{data[1]} << 8) | (std::uint32_t{data[2]} << 16) |
           (std::uint32_t{data[3]} << 24);
}

std::uint32_t read_be32(const unsigned char* data)
{
    return (std::uint32_t{data[0]} << 24) | (std::uint32_t{data[1]} << 16) |
           (std::uint32_t{data[2]} << 8) | std::uint32_t{data[3]};
}

std::span<unsigned char> load_icon(std::span<unsigned char> data, std::uintptr_t requested_size)
{
    constexpr std::array<unsigned char, 8> png_signature{0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    std::span<unsigned char> selected;
    std::uint32_t selected_size{};
    if (data.size() < 6)
        return selected;

    const auto count = unsigned{data[4]} | (unsigned{data[5]} << 8);
    for (unsigned i = 0; i < count; ++i)
    {
        const auto entry_offset = 6 + i * 16;
        if (entry_offset + 16 > data.size())
            break;
        const auto length = read_le32(data.data() + entry_offset + 8);
        const auto offset = read_le32(data.data() + entry_offset + 12);
        if (offset > data.size() || length > data.size() - offset || length < 24)
            continue;
        const auto image = data.subspan(offset, length);
        if (!std::equal(png_signature.begin(), png_signature.end(), image.begin()))
            continue;

        // These ICOs contain PNGs. Read their actual dimensions: some generators
        // write 256 for every size in the ICO directory.
        const auto size = read_be32(image.data() + 16);
        if (selected.empty() || (selected_size < requested_size && size > selected_size) ||
            (size >= requested_size && size < selected_size))
        {
            selected = image;
            selected_size = size;
        }
    }
    return selected;
}
#endif

} // namespace

int extract_custom_icon(char16_t* remote_name, int extract_flags, TWfxIcon* icon)
{
    if (remote_name == nullptr || icon == nullptr)
        return FS_ICON_USEDEFAULT;

    const auto type = [&] {
        const auto path = s3cmd::RemotePathView::make(remote_name);
        if (path.bucket.empty())
        {
            // We still want to have a default icon for "one level up" entry
            if (!path.profile.empty() && path.profile != u"..")
                return IconType::profile;
        }
        else if (path.key.empty() && path.bucket != u"..")
        {
            return IconType::bucket;
        }
        return IconType::none;
    }();

    if (type == IconType::none)
        return FS_ICON_USEDEFAULT;

#ifdef _WIN32
    const auto size = (extract_flags & FS_ICONFLAG_SMALL) != 0 ? 16 : 48;
    const auto resource_id = type == IconType::profile ? IDI_PROFILE : IDI_BUCKET;
    *icon = load_icon(resource_id, size);
    if (*icon == nullptr)
        return FS_ICON_USEDEFAULT;
#else
    // Double Commander supplies the requested pixel size here. For binary icons
    // the returned Size is the byte count instead.
    const auto size = icon->Size != 0                            ? icon->Size
                      : (extract_flags & FS_ICONFLAG_SMALL) != 0 ? 16u
                                                                 : 48u;
    const auto data = type == IconType::profile ? std::span<unsigned char>{embedded_icons::profile}
                                                : std::span<unsigned char>{embedded_icons::bucket};
    const auto image = load_icon(data, size);
    if (image.empty())
        return FS_ICON_USEDEFAULT;
    icon->Pointer = image.data();
    icon->Size = image.size();
    icon->Format = FS_ICON_FORMAT_BINARY;
    icon->Free = nullptr;
#endif

    // Return icon name remote_name to improve caching strategy of the calling app.
    // We also append the size since at least double commander doesn't key on it
    const auto name =
        fmt::format(u"s3cmd.{}.{}", type == IconType::profile ? u"profile" : u"bucket", size);
    std::copy_n(name.c_str(), name.size() + 1, remote_name);
#ifdef _WIN32
    return FS_ICON_EXTRACTED_DESTROY;
#else
    // The PNG bytes live in the module; Double Commander must not free them.
    return FS_ICON_EXTRACTED;
#endif
}

} // namespace s3cmd
