#pragma once

#include "Theme.h"

#include <string>
#include <windows.h>

constexpr wchar_t kFileHelperWindowClass[] = L"QuattroFileHelperDialog";
constexpr UINT WM_QUATTRO_TEST_FILE_HELPER = WM_APP + 0x8B;

constexpr int ID_FILE_HELPER_PATH = 7901;
constexpr int ID_FILE_HELPER_PICK = 7902;
constexpr int ID_FILE_HELPER_PICK_MENU = 7903;
constexpr int ID_FILE_HELPER_OPEN_FILE = 7904;
constexpr int ID_FILE_HELPER_OPEN_FOLDER = 7905;
constexpr int ID_FILE_HELPER_CREATE_FILE = 7906;
constexpr int ID_FILE_HELPER_CREATE_FOLDER = 7907;
constexpr int ID_FILE_HELPER_OPEN_LOCATION = 7908;
enum class FileHelperTestCommand {
    SetPath,
    OpenFile,
    OpenFolder,
    CreateFileAction,
    CreateFileOverwriteAction,
    CreateFolder,
    OpenContainingLocation,
    QueryFocusRequested,
    ResetFocusRequested,
    QueryContainingLocationEnabled,
    QueryLastAction,
};

struct FileHelperTestRequest {
    std::wstring path;
};

bool ShowFileHelperDialog(HWND owner, HINSTANCE instance, const Theme& theme);
bool ToggleFileHelperDialog(HWND owner, HINSTANCE instance, const Theme& theme);
bool PreTranslateFileHelperMessage(const MSG& message);
