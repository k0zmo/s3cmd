#pragma once

// Forward declarations
typedef struct HICON__* HICON;

namespace s3cmd {

int extract_custom_icon(wchar_t* remote_name, int extract_flags, HICON* icon);

} // namespace s3cmd
