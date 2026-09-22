#include "../src/windows/MainHotKey.h"

#include <iostream>

// Diagnostic only: fixed in-memory events, no HWND or desktop input APIs.
int main() {
    int failures = 0;
    const auto check = [&](bool passed, const char* name) {
        std::cout << name << "=" << (passed ? "passed" : "failed") << '\n';
        if (!passed) ++failures;
    };
    DoubleAltGesture gesture;
    const auto tap = [&](DWORD tick) {
        gesture.OnKey(VK_LMENU, true, tick);
        return gesture.OnKey(VK_LMENU, false, tick + 10);
    };
    const auto pair = [&](DWORD tick) {
        const auto first = tap(tick);
        const auto second = tap(tick + 100);
        return first != 0 || second != 0;
    };

    check(pair(100), "clean_double_alt_recognized");
    gesture.Reset();
    gesture.OnKey(VK_LCONTROL, true, 1000);
    check(!pair(1100), "ctrl_without_release_blocks_double_alt");
    check(!pair(61000), "one_minute_does_not_clear_missing_release");
    check(!pair(3601000), "one_hour_does_not_clear_missing_release");
    gesture.OnKey('X', true, 3602000);
    gesture.OnKey('X', false, 3602010);
    check(!pair(3602100), "unrelated_events_do_not_clear_missing_release");
    gesture.OnKey(VK_RCONTROL, true, 3603000);
    gesture.OnKey(VK_RCONTROL, false, 3603010);
    check(!pair(3603100), "other_ctrl_release_does_not_clear_left_ctrl");
    gesture.OnKey(VK_LCONTROL, false, 3604000);
    check(pair(3604100), "matching_ctrl_release_restores_recognition");
    gesture.OnKey(VK_LCONTROL, true, 3605000);
    gesture.Reset();
    check(pair(3605100), "reregistration_reset_restores_recognition");

    std::cout << "scope=state_machine_only_not_live_hook_delivery\n";
    return failures ? 1 : 0;
}
