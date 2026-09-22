#pragma once

#include "../domain/DoubleAltGesture.h"
#include <functional>
#include <memory>
#include <optional>

struct GlobalHotKeyRegistration {
    HHOOK hook = nullptr;
    DWORD error = ERROR_SUCCESS;
};

// Native operations are replaceable so background tests never sample or inject
// desktop input and never install a real global hook.
struct GlobalHotKeyOperations {
    std::function<GlobalHotKeyRegistration(HOOKPROC)> install;
    std::function<void(HHOOK)> uninstall;
    std::function<std::optional<HotKeyKeyboardSnapshot>()> keyboard;
    std::function<HWND()> foreground;
    std::function<ULONGLONG()> clock; // Monotonic and safe to read on either thread.
};

struct GlobalHotKeyNotifications {
    // Worker-thread notifications: callers may only post an owning-thread message.
    std::function<void(UINT_PTR token, HWND foreground)> gesture;
    std::function<void(UINT_PTR registration)> registrationChanged;
};

class GlobalHotKeyService {
public:
    GlobalHotKeyService();
    explicit GlobalHotKeyService(GlobalHotKeyOperations testOperations);
    ~GlobalHotKeyService();
    GlobalHotKeyService(const GlobalHotKeyService&) = delete;
    GlobalHotKeyService& operator=(const GlobalHotKeyService&) = delete;

    // Start reports acceptance, not completed registration. Never waits for the worker.
    bool Start(GlobalHotKeyNotifications notifications);
    void Stop();
    void Refresh();
    bool Registered() const;
    DWORD LastError() const;
    UINT_PTR RegistrationId() const;
    bool IsPending(UINT_PTR token) const;
    bool Consume(UINT_PTR token);
    void CancelPending();
    UINT_PTR InputSerial() const;
    bool Stopped() const;

    // Only available with explicitly supplied operations AND isolated test mode.
    bool PostTestKey(DWORD key, bool down, DWORD tick, bool injected = false,
        ULONG_PTR extraInfo = 0, bool downstreamConsumed = false);
    void PollForTesting();

private:
    struct State;
    std::shared_ptr<State> state_;
    GlobalHotKeyOperations operations_;
    bool testOperations_ = false;
    DWORD startError_ = ERROR_SUCCESS;
};
