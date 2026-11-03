#pragma once

#include "fsplugin.h"

#include <string>

namespace s3cmd {

// Function called only in tests
void reset_config();

std::string discover_bucket_region(const std::string& profile, const std::string& bucket);

// Plugin API implementation
int initialize(int number, tProgressProcW progress, tLogProcW log, tRequestProcW request);
void shutdown();
HANDLE find_first(const char16_t* path, WfxFindData* find_data);
bool find_next(HANDLE handle, WfxFindData* find_data);
int find_close(HANDLE handle);
void status_info(const char16_t* remote_directory, int start_end, int operation);
int execute_file(char16_t* remote_name, const char16_t* verb, HWND owner = nullptr);
int get_file(const char16_t* remote_name, const char16_t* local_name, int copy_flags,
             const RemoteInfoStruct* info);
int put_file(const char16_t* local_name, const char16_t* remote_name, int copy_flags);
bool delete_file(const char16_t* remote_name);
bool make_directory(const char16_t* remote_name);
bool remove_directory(const char16_t* remote_name);
int rename_or_move(const char16_t* old_name, const char16_t* new_name, bool move, bool overwrite,
                   const RemoteInfoStruct* info);
void get_default_root_name(char* name, int max_length);
int content_get_supported_field(int field_index, char* field_name, char* units, int max_length);
int content_get_value(const char16_t* file_name, int field_index, void* field_value, int max_length);

} // namespace s3cmd
