#pragma once

#include "../common/Utilities.h"
#include "../domain/DoubleAltGesture.h"
#include <string>

constexpr int kMainHotKeyDoubleAlt = -1;

// The hook only recognizes input. Dispatch runs later, after the physical Alt
// release has had a chance to leave the foreground application's menu loop.
constexpr UINT kDoubleAltDispatchDelayMs = 40;
constexpr UINT kMainWindowWakeRetryDelayMs = 80;

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
