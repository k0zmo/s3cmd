#include "doublecmd/extension.hpp"

#include <cstddef>
#include <mutex>

namespace s3cmd::dc {
namespace {

std::mutex api_mutex;
DialogApi api;

} // namespace

void initialize_extension(const StartupInfo* info)
{
    std::scoped_lock lock(api_mutex);
    api = {};
    if (info && info->struct_size >=
                    offsetof(StartupInfo, dialog_box_param) + sizeof(info->dialog_box_param))
    {
        api = {info->send_dialog_message, info->dialog_box_param};
    }
}

void finalize_extension()
{
    std::scoped_lock lock(api_mutex);
    api = {};
}

DialogApi dialog_api()
{
    std::scoped_lock lock(api_mutex);
    return api;
}

} // namespace s3cmd::dc
