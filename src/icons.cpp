#include "icons.hpp"
#include "core.hpp"
#include "fsplugin.h"
#include "win32/resource.h"

#include <Windows.h>
#include <fmt/xchar.h>

#include <algorithm>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace s3cmd {

namespace {

HICON load_icon(int resource_id, int size)
{
    return static_cast<HICON>(LoadImageW(reinterpret_cast<HINSTANCE>(&__ImageBase),
                                         MAKEINTRESOURCEW(resource_id), IMAGE_ICON, size, size,
                                         LR_DEFAULTCOLOR));
}

} // namespace

int extract_custom_icon(char16_t* remote_name, int extract_flags, HICON* icon)
{
    if (remote_name == nullptr || icon == nullptr)
        return FS_ICON_USEDEFAULT;

    const auto resource_id = [&] {
        const auto path = s3cmd::RemotePathView::make(remote_name);
        if (path.bucket.empty())
        {
            // We still want to have a default icon for "one level up" entry
            if (!path.profile.empty() && path.profile != u"..")
                return IDI_PROFILE;
        }
        else if (path.key.empty() && path.bucket != u"..")
        {
            return IDI_BUCKET;
        }
        return 0;
    }();

    if (resource_id == 0)
        return FS_ICON_USEDEFAULT;

    const auto size = (extract_flags & FS_ICONFLAG_SMALL) != 0 ? 16 : 48;
    *icon = load_icon(resource_id, size);
    if (*icon == nullptr)
        return FS_ICON_USEDEFAULT;

    // Return icon name remote_name to improve caching strategy of the calling app.
    // We also append the size since at least double commander doesn't key on it
    const auto name = fmt::format(u"s3cmd.{}.{}",
                                  resource_id == IDI_PROFILE ? u"profile" : u"bucket", size);
    std::copy_n(name.c_str(), name.size() + 1, remote_name);
    return FS_ICON_EXTRACTED_DESTROY;
}

} // namespace s3cmd
