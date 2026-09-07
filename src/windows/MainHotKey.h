#pragma once

#include "../common/Utilities.h"
#include <string>

constexpr int kMainHotKeyDoubleAlt = -1;

enum class MainHotKeyAction {
    Wake,
    Hide,
};

struct MainHotKeyWindowState {
    bool effectivelyVisible = false;
    bool minimized = false;
    WindowFrontness frontness = WindowFrontness::Unknown;
};

constexpr MainHotKeyAction DecideMainHotKeyAction(const MainHotKeyWindowState& state) noexcept {
    if (!state.effectivelyVisible || state.minimized) {
        return MainHotKeyAction::Wake;
    }
    if (state.frontness == WindowFrontness::Front) {
        return MainHotKeyAction::Hide;
    }
    return MainHotKeyAction::Wake;
}

bool IsDoubleAltMainHotKey(int key);
std::wstring FormatMainHotKeyText(int key);
std::wstring FormatGlobalHotKeyText(int key);
