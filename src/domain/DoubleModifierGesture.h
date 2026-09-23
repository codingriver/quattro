#pragma once

#include "../common/Utilities.h"

#include <algorithm>
#include <array>

using HotKeyKeyboardSnapshot = std::array<bool, 256>;

enum class DoubleModifierGestureKind : UINT_PTR {
    None = 0,
    DoubleAlt = 1,
    DoubleCtrl = 2,
};

struct DoubleModifierGestureResult {
    DoubleModifierGestureKind kind = DoubleModifierGestureKind::None;
    UINT_PTR token = 0;

    explicit operator bool() const noexcept {
        return kind != DoubleModifierGestureKind::None && token != 0;
    }
};

class DoubleModifierGesture {
public:
    static DWORD CanonicalKey(DWORD key) {
        if (key == VK_CONTROL) return VK_LCONTROL;
        if (key == VK_SHIFT) return VK_LSHIFT;
        if (key == VK_MENU) return VK_LMENU;
        return key;
    }

    static DoubleModifierGestureKind KindForKey(DWORD key) {
        key = CanonicalKey(key);
        if (key == VK_LMENU || key == VK_RMENU) {
            return DoubleModifierGestureKind::DoubleAlt;
        }
        if (key == VK_LCONTROL || key == VK_RCONTROL) {
            return DoubleModifierGestureKind::DoubleCtrl;
        }
        return DoubleModifierGestureKind::None;
    }

    DoubleModifierGestureResult OnKey(
        DWORD key,
        bool down,
        DWORD tick,
        bool injected = false,
        ULONG_PTR extraInfo = 0) {
        if (injected && extraInfo == kForegroundRecoveryInputTag) return {};
        ++serial_;
        pending_ = 0;
        pendingKind_ = DoubleModifierGestureKind::None;
        key = CanonicalKey(key);
        if (key >= keys_.size()) {
            ClearFirstTap();
            interrupted_ = true;
            return {};
        }

        const bool wasDown = keys_[key];
        keys_[key] = down;
        const DoubleModifierGestureKind kind = KindForKey(key);
        if (injected || kind == DoubleModifierGestureKind::None) {
            if (injected || down) {
                ClearFirstTap();
                interrupted_ = true;
            }
            return {};
        }

        if (down) {
            if (!wasDown) {
                interrupted_ = false;
                for (DWORD other = 0; other < keys_.size(); ++other) {
                    if (other != key && keys_[other]) {
                        interrupted_ = true;
                        ClearFirstTap();
                    }
                }
            }
            return {};
        }

        if (!wasDown || interrupted_) {
            ClearFirstTap();
            return {};
        }
        if (firstTapKind_ == kind && tick - lastModifierUp_ <= 450) {
            ClearFirstTap();
            pending_ = serial_;
            pendingKind_ = kind;
            return {kind, pending_};
        }
        firstTapKind_ = kind;
        lastModifierUp_ = tick;
        return {};
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
        return !(firstTapKind_ != DoubleModifierGestureKind::None &&
                 tick - lastModifierUp_ <= 450) &&
            std::none_of(keys_.begin(), keys_.end(), [](bool down) { return down; });
    }
    bool IsPending(DoubleModifierGestureKind kind, UINT_PTR token) const {
        return token && token == pending_ && kind == pendingKind_;
    }
    UINT_PTR InputSerial() const { return serial_; }
    void CancelPending() {
        pending_ = 0;
        pendingKind_ = DoubleModifierGestureKind::None;
    }
    bool Consume(DoubleModifierGestureKind kind, UINT_PTR token) {
        if (!IsPending(kind, token)) return false;
        CancelPending();
        return true;
    }
    void Reset() {
        ++serial_;
        keys_.fill(false);
        ClearFirstTap();
        interrupted_ = false;
        CancelPending();
    }

private:
    void ClearFirstTap() {
        firstTapKind_ = DoubleModifierGestureKind::None;
    }

    HotKeyKeyboardSnapshot keys_{};
    DWORD lastModifierUp_ = 0;
    UINT_PTR serial_ = 0;
    UINT_PTR pending_ = 0;
    DoubleModifierGestureKind firstTapKind_ = DoubleModifierGestureKind::None;
    DoubleModifierGestureKind pendingKind_ = DoubleModifierGestureKind::None;
    bool interrupted_ = false;
};
