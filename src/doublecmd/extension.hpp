#pragma once

#include "fsplugin.h"

#include <cstdint>

namespace s3cmd::dc {

// ABI prefix from Double Commander's sdk/extension.h and sdk/extension.pas:
// https://github.com/doublecmd/doublecmd/blob/master/sdk/extension.h

using DialogProc = std::intptr_t(DCPCALL*)(std::uintptr_t, const char*, std::intptr_t,
                                           std::intptr_t, std::intptr_t);
using InputBoxProc = BOOL(DCPCALL*)(char*, char*, BOOL, char*, int);
using MessageBoxProc = int(DCPCALL*)(char*, char*, std::int32_t);
using DialogBoxProc = BOOL(DCPCALL*)(std::intptr_t, std::uint32_t, DialogProc);
using DialogFileProc = BOOL(DCPCALL*)(char*, DialogProc);
using TranslateProc = int(DCPCALL*)(void*, const char*, const char*, char*, int);
using ChoiceBoxProc = int(DCPCALL*)(char*, char*, char**, int, int);
using DialogBoxParamProc = std::uintptr_t(DCPCALL*)(void*, std::uint32_t, DialogProc, std::uint32_t,
                                                    void*, void*);

#pragma pack(push, 1)
struct StartupInfo
{
    std::uint32_t struct_size;
    char plugin_dir[16384];
    char plugin_config_dir[16384];
    InputBoxProc input_box;
    MessageBoxProc message_box;
    DialogBoxProc dialog_box_lfm;
    DialogBoxProc dialog_box_lrs;
    DialogFileProc dialog_box_lfm_file;
    DialogProc send_dialog_message;
    void* translation;
    TranslateProc translate_string;
    std::uintptr_t version_api;
    ChoiceBoxProc message_choice_box;
    DialogBoxParamProc dialog_box_param;
    // The SDK record continues with optional services and reserved storage.
};
#pragma pack(pop)

enum : std::intptr_t
{
    dm_close = 1,
    dm_enable = 2,
    dm_get_dialog_data = 3,
    dm_get_text = 6,
    dm_redraw = 11,
    dm_set_text = 12,
    dm_list_get_index = 19,
    dm_list_set_index = 20,
    dm_list_add_string = 23,
    dn_click = 0x1001,
    dn_change = 0x1003,
    dn_init_dialog = 0x1005,
    dn_kill_focus = 0x1006,
    id_ok = 1,
    id_cancel = 2,
    db_lfm = 0
};

struct DialogApi
{
    DialogProc send_message{};
    DialogBoxParamProc show_dialog{};
};

void initialize_extension(const StartupInfo* info);
void finalize_extension();
DialogApi dialog_api();

} // namespace s3cmd::dc
