#pragma once

#include <Windows.h>

// Total Commander file-system plugin interface, version 2.1 (27.April.2010).
// The comments below are embedded from the corresponding pages in docs/.

// ids for FsGetFile
#define FS_FILE_OK 0
#define FS_FILE_EXISTS 1
#define FS_FILE_NOTFOUND 2
#define FS_FILE_READERROR 3
#define FS_FILE_WRITEERROR 4
#define FS_FILE_USERABORT 5
#define FS_FILE_NOTSUPPORTED 6
#define FS_FILE_EXISTSRESUMEALLOWED 7

#define FS_EXEC_OK 0
#define FS_EXEC_ERROR 1
#define FS_EXEC_YOURSELF -1
#define FS_EXEC_SYMLINK -2

#define FS_COPYFLAGS_OVERWRITE 1
#define FS_COPYFLAGS_RESUME 2
#define FS_COPYFLAGS_MOVE 4
#define FS_COPYFLAGS_EXISTS_SAMECASE 8
#define FS_COPYFLAGS_EXISTS_DIFFERENTCASE 16

// flags for tRequestProc
#define RT_Other 0
#define RT_UserName 1
#define RT_Password 2
#define RT_Account 3
#define RT_UserNameFirewall 4
#define RT_PasswordFirewall 5
#define RT_TargetDir 6
#define RT_URL 7
#define RT_MsgOK 8
#define RT_MsgYesNo 9
#define RT_MsgOKCancel 10

// flags for tLogProc
#define MSGTYPE_CONNECT 1
#define MSGTYPE_DISCONNECT 2
#define MSGTYPE_DETAILS 3
#define MSGTYPE_TRANSFERCOMPLETE 4
#define MSGTYPE_CONNECTCOMPLETE 5
#define MSGTYPE_IMPORTANTERROR 6
#define MSGTYPE_OPERATIONCOMPLETE 7

// flags for FsStatusInfo
#define FS_STATUS_START 0
#define FS_STATUS_END 1

#define FS_STATUS_OP_LIST 1
#define FS_STATUS_OP_GET_SINGLE 2
#define FS_STATUS_OP_GET_MULTI 3
#define FS_STATUS_OP_PUT_SINGLE 4
#define FS_STATUS_OP_PUT_MULTI 5
#define FS_STATUS_OP_RENMOV_SINGLE 6
#define FS_STATUS_OP_RENMOV_MULTI 7
#define FS_STATUS_OP_DELETE 8
#define FS_STATUS_OP_ATTRIB 9
#define FS_STATUS_OP_MKDIR 10
#define FS_STATUS_OP_EXEC 11
#define FS_STATUS_OP_CALCSIZE 12
#define FS_STATUS_OP_SEARCH 13
#define FS_STATUS_OP_SEARCH_TEXT 14
#define FS_STATUS_OP_SYNC_SEARCH 15
#define FS_STATUS_OP_SYNC_GET 16
#define FS_STATUS_OP_SYNC_PUT 17
#define FS_STATUS_OP_SYNC_DELETE 18
#define FS_STATUS_OP_GET_MULTI_THREAD 19
#define FS_STATUS_OP_PUT_MULTI_THREAD 20

#define FS_ICONFLAG_SMALL 1
#define FS_ICONFLAG_BACKGROUND 2

#define FS_ICON_USEDEFAULT 0
#define FS_ICON_EXTRACTED 1
#define FS_ICON_EXTRACTED_DESTROY 2
#define FS_ICON_DELAYED 3

#define FS_BITMAP_NONE 0
#define FS_BITMAP_EXTRACTED 1
#define FS_BITMAP_EXTRACT_YOURSELF 2
#define FS_BITMAP_EXTRACT_YOURSELF_ANDDELETE 3
#define FS_BITMAP_CACHE 256

// Save a password to the secure password store.
#define FS_CRYPT_SAVE_PASSWORD 1
// Load a password from the secure password store.
#define FS_CRYPT_LOAD_PASSWORD 2
// Load a password only if the master password has already been entered.
#define FS_CRYPT_LOAD_PASSWORD_NO_UI 3
// Copy the encrypted password to a new connection name.
#define FS_CRYPT_COPY_PASSWORD 4
// Move the password when renaming a connection.
#define FS_CRYPT_MOVE_PASSWORD 5
// Delete the password.
#define FS_CRYPT_DELETE_PASSWORD 6

// The user already has a master password defined.
#define FS_CRYPTOPT_MASTERPASS_SET 1

// Plugin supports downloads in background.
#define BG_DOWNLOAD 1
// Plugin supports uploads in background.
#define BG_UPLOAD 2
// Plugin requires a separate connection for background transfers; ask the user first.
#define BG_ASK_USER 4

/** Details of the remote file passed to FsGetFile and FsRenMovFile.
 * SizeLow/SizeHigh contain the 64-bit size; LastWriteTime and Attr describe
 * the remote file and may be copied to the local file. The parameter may be
 * ignored when these details are not needed.
 */
typedef struct
{
    DWORD SizeLow, SizeHigh;
    FILETIME LastWriteTime;
    int Attr;
} RemoteInfoStruct;

/** Parameters supplied by Total Commander through FsSetDefaultParams. */
typedef struct
{
    int size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;
    char DefaultIniName[MAX_PATH];
} FsDefaultParamStruct;

// ProgressProc is called during a transfer. Return 1 to abort, 0 to continue.
// SourceName and TargetName identify the current file; PercentDone is its
// percentage, not the percentage of the complete multi-file operation.
typedef int(__stdcall* tProgressProc)(int PluginNr, char* SourceName, char* TargetName,
                                      int PercentDone);
typedef int(__stdcall* tProgressProcW)(int PluginNr, WCHAR* SourceName, WCHAR* TargetName,
                                       int PercentDone);
// LogProc reports connection state and messages. MSGTYPE_CONNECT enables the
// Disconnect button and must be used if FsDisconnect is implemented.
typedef void(__stdcall* tLogProc)(int PluginNr, int MsgType, char* LogString);
typedef void(__stdcall* tLogProcW)(int PluginNr, int MsgType, WCHAR* LogString);

// RequestProc asks the user for input. RequestType selects a localized
// standard prompt; ReturnedText has room for maxlen characters.
typedef BOOL(__stdcall* tRequestProc)(int PluginNr, int RequestType, char* CustomTitle,
                                      char* CustomText, char* ReturnedText, int maxlen);
typedef BOOL(__stdcall* tRequestProcW)(int PluginNr, int RequestType, WCHAR* CustomTitle,
                                       WCHAR* CustomText, WCHAR* ReturnedText, int maxlen);
// CryptProc accesses Total Commander's secure password store. Mode is one of
// the FS_CRYPT_* values; maxlen includes the terminating null character.
typedef int(__stdcall* tCryptProc)(int PluginNr, int CryptoNr, int Mode, char* ConnectionName,
                                   char* Password, int maxlen);
typedef int(__stdcall* tCryptProcW)(int PluginNr, int CryptoNr, int Mode, WCHAR* ConnectionName,
                                    WCHAR* Password, int maxlen);

// FsInit is called when the plugin is loaded. Store the callbacks for later
// use and return 0; the return value is currently unused.
#ifdef __cplusplus
extern "C" {
#endif

int __stdcall FsInit(int PluginNr, tProgressProc pProgressProc, tLogProc pLogProc,
                     tRequestProc pRequestProc);
int __stdcall FsInitW(int PluginNr, tProgressProcW pProgressProcW, tLogProcW pLogProcW,
                      tRequestProcW pRequestProcW);
// Registers the secure-password callback. It is only needed when the plugin
// uses Total Commander's password store.
void __stdcall FsSetCryptCallback(tCryptProc pCryptProc, int CryptoNr, int Flags);
void __stdcall FsSetCryptCallbackW(tCryptProcW pCryptProcW, int CryptoNr, int Flags);
// Starts a directory listing. Path is a full backslash-separated plugin path;
// the root is a single backslash and never includes the displayed root name.
// Return a find handle, or INVALID_HANDLE_VALUE on failure.
HANDLE __stdcall FsFindFirst(char* Path, WIN32_FIND_DATA* FindData);
HANDLE __stdcall FsFindFirstW(WCHAR* Path, WIN32_FIND_DATAW* FindData);

// Continues a listing started by FsFindFirst. Return FALSE at end or on error.
BOOL __stdcall FsFindNext(HANDLE Hdl, WIN32_FIND_DATA* FindData);
BOOL __stdcall FsFindNextW(HANDLE Hdl, WIN32_FIND_DATAW* FindData);
// Ends a FsFindFirst/FsFindNext loop. The return value is currently unused;
// return 0.
int __stdcall FsFindClose(HANDLE Hdl);
// Creates the directory named by the full plugin path. Return TRUE on success.
BOOL __stdcall FsMkDir(char* Path);
BOOL __stdcall FsMkDirW(WCHAR* Path);
// Executes a remote file or shows its properties. Verb is typically open,
// properties, chmod, or quote. RemoteName="\\" and Verb="properties" opens
// the plugin configuration dialog.
int __stdcall FsExecuteFile(HWND MainWin, char* RemoteName, char* Verb);
int __stdcall FsExecuteFileW(HWND MainWin, WCHAR* RemoteName, WCHAR* Verb);
// Copies or moves a remote file. Move selects rename/move versus copy;
// OverWrite selects replacement of an existing target.
int __stdcall FsRenMovFile(char* OldName, char* NewName, BOOL Move, BOOL OverWrite,
                           RemoteInfoStruct* ri);
int __stdcall FsRenMovFileW(WCHAR* OldName, WCHAR* NewName, BOOL Move, BOOL OverWrite,
                            RemoteInfoStruct* ri);
// Downloads a remote file to a local path. CopyFlags is a combination of
// FS_COPYFLAGS_OVERWRITE, FS_COPYFLAGS_RESUME, and FS_COPYFLAGS_MOVE.
int __stdcall FsGetFile(char* RemoteName, char* LocalName, int CopyFlags, RemoteInfoStruct* ri);

int __stdcall FsGetFileW(WCHAR* RemoteName, WCHAR* LocalName, int CopyFlags, RemoteInfoStruct* ri);
// Uploads a local file to the plugin file system. CopyFlags uses the same
// overwrite/resume/move flags as FsGetFile.
int __stdcall FsPutFile(char* LocalName, char* RemoteName, int CopyFlags);
int __stdcall FsPutFileW(WCHAR* LocalName, WCHAR* RemoteName, int CopyFlags);
// Deletes a remote file. Return TRUE on success.
BOOL __stdcall FsDeleteFile(char* RemoteName);
BOOL __stdcall FsDeleteFileW(WCHAR* RemoteName);
// Removes a remote directory. Return TRUE on success.
BOOL __stdcall FsRemoveDir(char* RemoteName);
BOOL __stdcall FsRemoveDirW(WCHAR* RemoteName);
// Closes the connection identified by DisconnectRoot. Implement only when
// FsInit's log callback reports MSGTYPE_CONNECT.
BOOL __stdcall FsDisconnect(char* DisconnectRoot);
BOOL __stdcall FsDisconnectW(WCHAR* DisconnectRoot);
// Sets Windows file attributes. Implement only when supported.
BOOL __stdcall FsSetAttr(char* RemoteName, int NewAttr);
BOOL __stdcall FsSetAttrW(WCHAR* RemoteName, int NewAttr);
// Sets Windows file times. A null time leaves that time unchanged.
BOOL __stdcall FsSetTime(char* RemoteName, FILETIME* CreationTime, FILETIME* LastAccessTime,
                         FILETIME* LastWriteTime);
BOOL __stdcall FsSetTimeW(WCHAR* RemoteName, FILETIME* CreationTime, FILETIME* LastAccessTime,
                          FILETIME* LastWriteTime);
// Announces the start or end of an operation. Use it to allocate/free buffers
// or flush caches; it is optional when no such lifecycle work is needed.
void __stdcall FsStatusInfo(char* RemoteDir, int InfoStartEnd, int InfoOperation);
void __stdcall FsStatusInfoW(WCHAR* RemoteDir, int InfoStartEnd, int InfoOperation);
// Supplies the root name shown in Network Neighborhood. This is called during
// installation, before FsInit, and the name is not included in plugin paths.
void __stdcall FsGetDefRootName(char* DefRootName, int maxlen);
// Supplies a custom file/directory icon. The icon handle must be returned in
// TheIcon; the function may return an icon name for Total Commander to cache.
int __stdcall FsExtractCustomIcon(char* RemoteName, int ExtractFlags, HICON* TheIcon);
int __stdcall FsExtractCustomIconW(WCHAR* RemoteName, int ExtractFlags, HICON* TheIcon);
// Receives the interface version and suggested settings-file location after
// FsInit. Use a unique INI section because the file is shared by plugins.
void __stdcall FsSetDefaultParams(FsDefaultParamStruct* dps);

// Supplies a custom thumbnail bitmap. ReturnedBitmap must receive the bitmap
// handle; width and height are the maximum requested dimensions.
int __stdcall FsGetPreviewBitmap(char* RemoteName, int width, int height, HBITMAP* ReturnedBitmap);
int __stdcall FsGetPreviewBitmapW(WCHAR* RemoteName, int width, int height,
                                  HBITMAP* ReturnedBitmap);
// Identifies a temporary-panel plugin. Do not export this for normal plugins.
BOOL __stdcall FsLinksToLocalFiles(void);
// Converts a temporary-panel path to a local file path. Do not export this for
// normal plugins; temporary-panel implementations must be thread-safe.
BOOL __stdcall FsGetLocalName(char* RemoteName, int maxlen);
BOOL __stdcall FsGetLocalNameW(WCHAR* RemoteName, int maxlen);

// ************************** content plugin extension ****************************
// The content functions are optional. FieldIndex starts at zero and field
// enumeration ends when ft_nomorefields is returned.

//
#define ft_nomorefields 0

#define ft_numeric_32 1
#define ft_numeric_64 2
#define ft_numeric_floating 3
#define ft_date 4
#define ft_time 5
#define ft_boolean 6
#define ft_multiplechoice 7
#define ft_string 8
#define ft_fulltext 9
#define ft_datetime 10
// Should only be returned by the Unicode function.
#define ft_stringw 11

// for FsContentGetValue
// Error: invalid field number given.
#define ft_nosuchfield -1
// File I/O error.
#define ft_fileerror -2
// Field is valid, but empty.
#define ft_fieldempty -3

// Field will be retrieved only when the user presses <SPACEBAR>.
#define ft_ondemand -4
// Field takes a long time to extract; try again in the background.
#define ft_delayed 0

// for FsContentSetValue
// Setting of the attribute succeeded.
#define ft_setsuccess 0

// for FsContentGetSupportedFieldFlags
#define contflags_edit 1
#define contflags_substsize 2
#define contflags_substdatetime 4
#define contflags_substdate 6
#define contflags_substtime 8
#define contflags_substattributes 10

#define contflags_substattributestr 12
#define contflags_substmask 14

// for FsContentSetValue
// First attribute of this file.
#define setflags_first_attribute 1
// Last attribute of this file.
#define setflags_last_attribute 2
// Only set the date of the datetime value.
#define setflags_only_date 4

// FsContentGetValue is called in the foreground.
#define CONTENT_DELAYIFSLOW 1

typedef struct
{
    int size;
    DWORD PluginInterfaceVersionLow;
    DWORD PluginInterfaceVersionHi;

    char DefaultIniName[MAX_PATH];
} ContentDefaultParamStruct;

typedef struct
{
    WORD wYear;
    WORD wMonth;
    WORD wDay;
} tdateformat, *pdateformat;

typedef struct
{
    WORD wHour;
    WORD wMinute;
    WORD wSecond;
} ttimeformat, *ptimeformat;

// Describes one supported field and its optional | separated units.
int __stdcall FsContentGetSupportedField(int FieldIndex, char* FieldName, char* Units, int maxlen);
// Retrieves one field value. FieldValue's type is selected by the field type
// returned from FsContentGetSupportedField.
int __stdcall FsContentGetValue(char* FileName, int FieldIndex, int UnitIndex, void* FieldValue,
                                int maxlen, int flags);
int __stdcall FsContentGetValueW(WCHAR* FileName, int FieldIndex, int UnitIndex, void* FieldValue,
                                 int maxlen, int flags);

// Requests cancellation of a slow FsContentGetValue operation.
void __stdcall FsContentStopGetValue(char* FileName);
void __stdcall FsContentStopGetValueW(WCHAR* FileName);
// Returns 1 for ascending or -1 for descending default sorting.
int __stdcall FsContentGetDefaultSortOrder(int FieldIndex);
// Called before unloading the content-plugin part; release buffers and abort
// outstanding work here.
void __stdcall FsContentPluginUnloading(void);
// Returns supported contflags_* values; FieldIndex=-1 requests the aggregate.
int __stdcall FsContentGetSupportedFieldFlags(int FieldIndex);
// Changes one field value. FileName=NULL/FieldIndex=-1 signal the end of a
// multi-file attribute update.
int __stdcall FsContentSetValue(char* FileName, int FieldIndex, int UnitIndex, int FieldType,
                                void* FieldValue, int flags);
int __stdcall FsContentSetValueW(WCHAR* FileName, int FieldIndex, int UnitIndex, int FieldType,
                                 void* FieldValue, int flags);

// Returns the default content view: fields, headers, widths, and options.
BOOL __stdcall FsContentGetDefaultView(char* ViewContents, char* ViewHeaders, char* ViewWidths,
                                       char* ViewOptions, int maxlen);
BOOL __stdcall FsContentGetDefaultViewW(WCHAR* ViewContents, WCHAR* ViewHeaders, WCHAR* ViewWidths,
                                        WCHAR* ViewOptions, int maxlen);

// Returns a combination of BG_DOWNLOAD, BG_UPLOAD, and BG_ASK_USER.
int __stdcall FsGetBackgroundFlags(void);

#ifdef __cplusplus
}
#endif
