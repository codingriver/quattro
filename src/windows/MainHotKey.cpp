#include "MainHotKey.h"

#include "HotKeyEditor.h"

bool IsDoubleAltMainHotKey(int key) {
    return key == kMainHotKeyDoubleAlt;
}

std::wstring FormatMainHotKeyText(int key) {
    if (IsDoubleAltMainHotKey(key)) {
        return L"双击 Alt";
    }
    return FormatHotKeyText(key);
}

