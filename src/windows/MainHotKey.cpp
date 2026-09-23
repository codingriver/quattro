#include "MainHotKey.h"

#include "HotKeyEditor.h"

bool IsDoubleAltMainHotKey(int key) {
    return key == kMainHotKeyDoubleAlt;
}

bool IsDoubleCtrlFileHelperHotKey(int key) {
    return key == kFileHelperHotKeyDoubleCtrl;
}

std::wstring FormatMainHotKeyText(int key) {
    if (IsDoubleAltMainHotKey(key)) {
        return L"双击 Alt";
    }
    return FormatHotKeyText(key);
}

std::wstring FormatFileHelperHotKeyText(int key) {
    if (IsDoubleCtrlFileHelperHotKey(key)) {
        return L"双击 Ctrl";
    }
    return FormatHotKeyText(key);
}

std::wstring FormatGlobalHotKeyText(int key) {
    if (IsDoubleAltMainHotKey(key)) {
        return L"双击 Alt";
    }
    if (IsDoubleCtrlFileHelperHotKey(key)) {
        return L"双击 Ctrl";
    }
    return FormatHotKeyText(key);
}
