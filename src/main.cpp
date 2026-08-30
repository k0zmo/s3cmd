#include "s3.hpp"
#include "icons.hpp"

#define WFX_EXPORT extern "C"

WFX_EXPORT int __stdcall FsInitW(int number, tProgressProcW progress, tLogProcW log,
                                 tRequestProcW request)
{
    return s3cmd::initialize(number, progress, log, request);
}

WFX_EXPORT HANDLE __stdcall FsFindFirstW(wchar_t* path, WIN32_FIND_DATAW* find_data)
{
    return s3cmd::find_first(path, find_data);
}

WFX_EXPORT BOOL __stdcall FsFindNextW(HANDLE handle, WIN32_FIND_DATAW* find_data)
{
    return s3cmd::find_next(handle, find_data);
}

WFX_EXPORT int __stdcall FsFindClose(HANDLE handle)
{
    return s3cmd::find_close(handle);
}

WFX_EXPORT void __stdcall FsStatusInfoW(wchar_t* remote_directory, int start_end, int operation)
{
    s3cmd::status_info(remote_directory, start_end, operation);
}

WFX_EXPORT int __stdcall FsGetFileW(wchar_t* remote_name, wchar_t* local_name, int copy_flags,
                                    RemoteInfoStruct* info)
{
    return s3cmd::get_file(remote_name, local_name, copy_flags, info);
}

WFX_EXPORT int __stdcall FsPutFileW(wchar_t* local_name, wchar_t* remote_name, int copy_flags)
{
    return s3cmd::put_file(local_name, remote_name, copy_flags);
}

WFX_EXPORT int __stdcall FsGetBackgroundFlags()
{
    return BG_DOWNLOAD | BG_UPLOAD;
}

WFX_EXPORT BOOL __stdcall FsDeleteFileW(wchar_t* remote_name)
{
    return s3cmd::delete_file(remote_name);
}

WFX_EXPORT BOOL __stdcall FsMkDirW(wchar_t* remote_name)
{
    return s3cmd::make_directory(remote_name);
}

WFX_EXPORT BOOL __stdcall FsRemoveDirW(wchar_t* remote_name)
{
    return s3cmd::remove_directory(remote_name);
}

WFX_EXPORT int __stdcall FsRenMovFileW(wchar_t* old_name, wchar_t* new_name, BOOL move,
                                       BOOL overwrite, RemoteInfoStruct* info)
{
    return s3cmd::rename_or_move(old_name, new_name, move, overwrite, info);
}

WFX_EXPORT void __stdcall FsGetDefRootName(char* name, int max_length)
{
    s3cmd::get_default_root_name(name, max_length);
}

WFX_EXPORT int __stdcall FsExtractCustomIconW(wchar_t* remote_name, int extract_flags,
                                              HICON* icon)
{
    return s3cmd::extract_custom_icon(remote_name, extract_flags, icon);
}

WFX_EXPORT int __stdcall FsContentGetSupportedField(int field_index, char* field_name, char* units,
                                                    int max_length)
{
    return s3cmd::content_get_supported_field(field_index, field_name, units, max_length);
}

WFX_EXPORT int __stdcall FsContentGetValueW(wchar_t* file_name, int field_index, int, void* value,
                                            int max_length, int)
{
    return s3cmd::content_get_value(file_name, field_index, value, max_length);
}

WFX_EXPORT void __stdcall FsContentPluginUnloading()
{
    s3cmd::shutdown();
}
