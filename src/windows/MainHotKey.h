#pragma once

#include "../common/Utilities.h"
#include "../domain/DoubleModifierGesture.h"
#include "../domain/Models.h"
#include <string>

// The hook only recognizes input. Dispatch runs later, after the physical Alt
// release has had a chance to leave the foreground application's menu loop.
constexpr UINT kModifierGestureDispatchDelayMs = 40;
constexpr UINT kMainWindowWakeRetryDelayMs = 80;

enum class MainHotKeyAction {
    Wake,
    Hide,
};

enum class ModifierGestureAction {
    None,
    ToggleMainWindow,
    ToggleFileHelper,
};

constexpr ModifierGestureAction DecideModifierGestureAction(
    bool globalHotKeysEnabled,
    int mainHotKey,
    int fileHelperHotKey,
    DoubleModifierGestureKind kind) noexcept {
    if (!globalHotKeysEnabled) return ModifierGestureAction::None;
    if (kind == DoubleModifierGestureKind::DoubleAlt && mainHotKey == kMainHotKeyDoubleAlt) {
        return ModifierGestureAction::ToggleMainWindow;
    }
    if (kind == DoubleModifierGestureKind::DoubleCtrl &&
        fileHelperHotKey == kFileHelperHotKeyDoubleCtrl) {
        return ModifierGestureAction::ToggleFileHelper;
    }
    return ModifierGestureAction::None;
}

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
bool IsDoubleCtrlFileHelperHotKey(int key);
std::wstring FormatMainHotKeyText(int key);
std::wstring FormatFileHelperHotKeyText(int key);
std::wstring FormatGlobalHotKeyText(int key);
