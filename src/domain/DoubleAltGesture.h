#pragma once

#include "../common/Utilities.h"
#include <algorithm>
#include <array>

using HotKeyKeyboardSnapshot = std::array<bool, 256>;

class DoubleAltGesture {
public:
    static DWORD CanonicalKey(DWORD key) {
        if (key == VK_CONTROL) return VK_LCONTROL;
        if (key == VK_SHIFT) return VK_LSHIFT;
        if (key == VK_MENU) return VK_LMENU;
        return key;
    }

    UINT_PTR OnKey(DWORD key, bool down, DWORD tick, bool injected = false, ULONG_PTR extraInfo = 0) {
        if (injected && extraInfo == kForegroundRecoveryInputTag) return 0;
        ++serial_;
        pending_ = 0;
        key = CanonicalKey(key);
        if (key >= keys_.size()) {
            firstTap_ = false;
            interrupted_ = true;
            return 0;
        }
        const bool wasDown = keys_[key];
        keys_[key] = down;
        const bool alt = key == VK_LMENU || key == VK_RMENU;
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
            return 0;
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

    // Only call outside the hook, after the OS has settled the input state.
    // A mismatch discards the entire partial gesture, never invents a release.
    bool Reconcile(HotKeyKeyboardSnapshot snapshot) {
        for (const DWORD generic : {DWORD(VK_CONTROL), DWORD(VK_SHIFT), DWORD(VK_MENU)}) {
            if (snapshot[generic]) snapshot[CanonicalKey(generic)] = true;
            snapshot[generic] = false;
        }
        if (snapshot == keys_) return false;
        Reset();
        keys_ = snapshot;
        interrupted_ = std::any_of(keys_.begin(), keys_.end(), [](bool down) { return down; });
        return true;
    }

    bool Idle(DWORD tick) const {
        return !(firstTap_ && tick - lastAltUp_ <= 450) &&
            std::none_of(keys_.begin(), keys_.end(), [](bool down) { return down; });
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
        ++serial_;
        keys_.fill(false);
        firstTap_ = false;
        interrupted_ = false;
        pending_ = 0;
    }
private:
    HotKeyKeyboardSnapshot keys_{};
    DWORD lastAltUp_ = 0;
    UINT_PTR serial_ = 0;
    UINT_PTR pending_ = 0;
    bool firstTap_ = false;
    bool interrupted_ = false;
};
