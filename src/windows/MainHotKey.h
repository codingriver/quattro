#pragma once

#include "../common/Utilities.h"
#include <array>
#include <string>

constexpr int kMainHotKeyDoubleAlt = -1;

// The hook only recognizes input. Dispatch runs later, after the physical Alt
// release has had a chance to leave the foreground application's menu loop.
constexpr UINT kDoubleAltDispatchDelayMs = 40;
constexpr UINT kMainWindowWakeRetryDelayMs = 80;

class DoubleAltGesture {
public:
    UINT_PTR OnKey(DWORD key, bool down, DWORD tick, bool injected = false, ULONG_PTR extraInfo = 0) {
        // Ignore only our tagged injected recovery pair. It must neither
        // re-trigger the hotkey nor cancel the input serial of its own wake.
        if (injected && extraInfo == kForegroundRecoveryInputTag) return 0;
        ++serial_;
        pending_ = 0; // Any newer input invalidates a deferred gesture.
        if (key >= keys_.size()) {
            firstTap_ = false;
            interrupted_ = true;
            return 0;
        }
        const bool wasDown = keys_[key];
        keys_[key] = down;
        const bool alt = IsAlt(key);
        if (injected || !alt) {
            if (injected || down) {
                firstTap_ = false;
                interrupted_ = true;
            }
            return 0;
        }
        if (down) {
            if (!wasDown) {
                interrupted_ = false;
                for (DWORD other = 0; other < keys_.size(); ++other) {
                    if (other != key && keys_[other]) {
                        interrupted_ = true;
                        firstTap_ = false;
                    }
                }
            }
            return 0; // Auto-repeat must not reset an interrupted Alt chord.
        }
        if (!wasDown || interrupted_) {
            firstTap_ = false;
            return 0;
        }
        if (firstTap_ && tick - lastAltUp_ <= 450) {
            firstTap_ = false;
            pending_ = serial_;
            return pending_;
        }
        firstTap_ = true;
        lastAltUp_ = tick;
        return 0;
    }

    bool IsPending(UINT_PTR token) const { return token && token == pending_; }
    UINT_PTR InputSerial() const { return serial_; }
    void CancelPending() { pending_ = 0; }
    bool Consume(UINT_PTR token) {
        if (!IsPending(token)) return false;
        pending_ = 0;
        return true;
    }
    void Reset() {
        ++serial_; // Never reuse tokens from messages queued before re-registration.
        keys_.fill(false);
        firstTap_ = false;
        interrupted_ = false;
        pending_ = 0;
    }
private:
    static bool IsAlt(DWORD key) {
        return key == VK_MENU || key == VK_LMENU || key == VK_RMENU;
    }
    std::array<bool, 256> keys_{};
    DWORD lastAltUp_ = 0;
    UINT_PTR serial_ = 0;
    UINT_PTR pending_ = 0;
    bool firstTap_ = false;
    bool interrupted_ = false;
};

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
