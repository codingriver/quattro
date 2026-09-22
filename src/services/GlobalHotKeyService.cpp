#include "GlobalHotKeyService.h"

#include "../common/AppLog.h"
#include <atomic>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr DWORD kPollMs = 100;
constexpr ULONGLONG kQuietMs = 60;
constexpr ULONGLONG kRenewMs = 60000;
constexpr ULONGLONG kRetryMs = 1000;
constexpr ULONGLONG kMaxRetryMs = 60000;
constexpr ULONGLONG kWakeGraceMs = 2000;
constexpr ULONGLONG kCandidateLifetimeMs = 1000;
std::atomic<UINT_PTR> nextSequence{0};
UINT_PTR NextSequence() {
    auto value = ++nextSequence;
    if (!value) value = ++nextSequence;
    return value;
}

bool BackgroundTest() {
    return QuattroTestMode() || BackgroundAcceptanceMode() || SuppressForegroundActivation();
}

std::optional<DWORD> IntegrityLevel(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return std::nullopt;
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    const bool read = size && GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size);
    CloseHandle(token);
    if (!read) return std::nullopt;
    const auto label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
    const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
    if (!count) return std::nullopt;
    return *GetSidSubAuthority(label->Label.Sid, count - 1);
}

std::optional<HotKeyKeyboardSnapshot> SampleKeyboard() {
    if (BackgroundTest()) return std::nullopt;
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_HOOKCONTROL);
    if (!input) return std::nullopt;
    wchar_t inputName[256]{}, threadName[256]{};
    DWORD needed = 0;
    const bool sameDesktop =
        GetUserObjectInformationW(input, UOI_NAME, inputName, sizeof(inputName), &needed) &&
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME,
            threadName, sizeof(threadName), &needed) &&
        wcscmp(inputName, threadName) == 0;
    CloseDesktop(input);
    if (!sameDesktop) return std::nullopt;

    // GetAsyncKeyState also returns zero when UIPI denies access. Do not mistake
    // an inaccessible foreground for "all keys released".
    const HWND foreground = GetForegroundWindow();
    DWORD processId = 0;
    if (!foreground || !GetWindowThreadProcessId(foreground, &processId)) return std::nullopt;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) return std::nullopt;
    const auto foregroundIntegrity = IntegrityLevel(process);
    CloseHandle(process);
    const auto ownIntegrity = IntegrityLevel(GetCurrentProcess());
    if (!foregroundIntegrity || !ownIntegrity || *foregroundIntegrity > *ownIntegrity) return std::nullopt;

    HotKeyKeyboardSnapshot result{};
    for (DWORD key = VK_BACK; key < result.size(); ++key) {
        // Generic modifier aliases must not look like a second held key.
        if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU) continue;
        result[key] = (GetAsyncKeyState(static_cast<int>(key)) & 0x8000) != 0;
    }
    if (GetForegroundWindow() != foreground) return std::nullopt;
    return result;
}

GlobalHotKeyOperations NativeOperations() {
    return {
        [](HOOKPROC callback) {
            if (BackgroundTest()) return GlobalHotKeyRegistration{nullptr, ERROR_ACCESS_DISABLED_BY_POLICY};
            const auto hook = SetWindowsHookExW(WH_KEYBOARD_LL, callback, GetModuleHandleW(nullptr), 0);
            return GlobalHotKeyRegistration{hook, hook ? ERROR_SUCCESS : GetLastError()};
        },
        [](HHOOK hook) { UnhookWindowsHookEx(hook); },
        SampleKeyboard,
        [] { return GetForegroundWindow(); },
        [] { return GetTickCount64(); },
    };
}
}

struct GlobalHotKeyService::State {
    struct Key {
        DWORD key;
        bool down;
        DWORD tick;
        bool injected;
        ULONG_PTR extraInfo;
        bool downstreamConsumed;
    };

    GlobalHotKeyOperations operations;
    GlobalHotKeyNotifications notifications;
    const UINT_PTR id = NextSequence();
    HANDLE signal = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    std::atomic<bool> active{true}, finished{false}, registered{false}, refresh{false};
    std::atomic<DWORD> error{ERROR_SUCCESS};
    std::atomic<UINT_PTR> serial{NextSequence()}, pending{0}, cancelledToken{0};
    std::atomic<ULONGLONG> pendingExpires{0};
    std::atomic<ULONGLONG> wakeGraceUntil{0};
    std::mutex testMutex;
    std::vector<Key> testKeys;
    HHOOK hook = nullptr;
    DoubleAltGesture recognizer;
    bool samplingAvailable = false;
    bool statusReported = false;
    ULONGLONG lastInput = 0, nextRenew = 0, retryDelay = kRetryMs;
    UINT_PTR candidate = 0;
    HWND candidateForeground = nullptr;
    static thread_local State* current;

    ~State() { if (signal) CloseHandle(signal); }

    UINT_PTR Invalidate() {
        pending = 0;
        const auto token = NextSequence();
        serial = token;
        return token;
    }

    UINT_PTR OnKey(const Key& event) {
        if (!active || (event.injected && event.extraInfo == kForegroundRecoveryInputTag)) return 0;
        const auto token = Invalidate();
        candidate = 0;
        lastInput = operations.clock();
        if (!samplingAvailable) return 0;
        const auto recognized = recognizer.OnKey(
            event.key, event.down, event.tick, event.injected, event.extraInfo);
        return recognized ? token : 0;
    }

    void QueueCandidate(UINT_PTR token, HWND foreground, bool consumed) {
        if (token && !consumed && active && !refresh && serial == token && cancelledToken != token) {
            pendingExpires = lastInput + kCandidateLifetimeMs;
            pending = token;
            candidate = token;
            candidateForeground = foreground;
            SetEvent(signal);
        }
    }

    static LRESULT CALLBACK HookProc(int code, WPARAM message, LPARAM data) {
        State* state = current;
        UINT_PTR token = 0;
        HWND foreground = nullptr;
        try {
            if (code == HC_ACTION && state) {
                const auto* event = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data);
                const bool down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
                const bool up = message == WM_KEYUP || message == WM_SYSKEYUP;
                if (down || up) {
                    token = state->OnKey({event->vkCode, down, event->time,
                        (event->flags & LLKHF_INJECTED) != 0, event->dwExtraInfo, false});
                    if (token) foreground = state->operations.foreground();
                }
            }
        } catch (...) {
            if (state) state->Invalidate();
        }
        const LRESULT next = CallNextHookEx(nullptr, code, message, data);
        if (state) state->QueueCandidate(token, foreground, next != 0);
        return next;
    }

    void ReportStatus(DWORD value) {
        error = value;
        registered = hook != nullptr;
        if (active && notifications.registrationChanged) notifications.registrationChanged(id);
        statusReported = true;
    }

    void Maintain() {
        const auto now = operations.clock();
        auto outstanding = pending.load();
        if (outstanding && (now >= pendingExpires || outstanding == cancelledToken || outstanding != serial)) {
            pending.compare_exchange_strong(outstanding, 0);
        }
        if (refresh.exchange(false)) {
            recognizer.Reset();
            Invalidate();
            candidate = 0;
            samplingAvailable = false;
            nextRenew = 0;
            retryDelay = kRetryMs;
        }
        if (now < wakeGraceUntil || now - lastInput < kQuietMs) return;
        const auto before = serial.load();
        const auto snapshot = operations.keyboard();
        // Native sampling can reenter the hook. Never reconcile an obsolete snapshot.
        if (!active || before != serial || now < wakeGraceUntil) return;
        if (!snapshot) {
            if (samplingAvailable) {
                recognizer.Reset();
                Invalidate();
                candidate = 0;
            }
            samplingAvailable = false;
            return;
        }
        if (!samplingAvailable) {
            recognizer.Reset();
            Invalidate();
            if (hook) nextRenew = 0;
        }
        samplingAvailable = true;
        if (recognizer.Reconcile(*snapshot)) {
            Invalidate();
            candidate = 0;
            WriteAppLog(L"Double Alt: reconciled keyboard state; cancelled incomplete gesture.");
        }
        // Consume publishes the grace deadline before clearing pending. Read in
        // this order so a concurrent UI consumption cannot race an idle renewal.
        if (now < nextRenew || pending || now < wakeGraceUntil ||
            !recognizer.Idle(static_cast<DWORD>(now))) return;
        // Replace only in an idle interval. Failure retains the previous hook.
        const auto installSerial = serial.load();
        const auto replacement = operations.install(HookProc);
        if (!active) {
            if (replacement.hook) operations.uninstall(replacement.hook);
            return;
        }
        if (replacement.hook) {
            const auto previous = std::exchange(hook, replacement.hook);
            if (previous) operations.uninstall(previous);
            const bool changedDuringInstall = serial != installSerial || refresh;
            recognizer.Reset();
            Invalidate();
            if (changedDuringInstall) samplingAvailable = false;
            retryDelay = kRetryMs;
            nextRenew = now + kRenewMs;
            if (!statusReported || !registered || error != ERROR_SUCCESS) ReportStatus(ERROR_SUCCESS);
            WriteAppLog(L"Double Alt: hook registration renewed on input service thread.");
        } else {
            nextRenew = now + retryDelay;
            retryDelay = (std::min)(retryDelay * 2, kMaxRetryMs);
            if (!statusReported || error != replacement.error) ReportStatus(replacement.error);
            WriteAppLog(L"Double Alt: hook registration failed; error=" + std::to_wstring(replacement.error));
        }
    }

    void Run() noexcept {
        current = this;
        try {
            while (active) {
                MSG message{};
                // This thread owns no HWND. PeekMessage dispatches sent hook callbacks;
                // no application window or UI message loop is hosted here.
                while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                    if (message.message == WM_QUIT) active = false;
                }
                std::vector<Key> events;
                {
                    std::lock_guard lock(testMutex);
                    events.swap(testKeys);
                }
                for (const auto& event : events) {
                    const auto token = OnKey(event);
                    QueueCandidate(token, token ? operations.foreground() : nullptr, event.downstreamConsumed);
                }
                if (!active) break;
                Maintain();
                if (candidate) {
                    const auto token = std::exchange(candidate, 0);
                    if (active && !refresh && pending == token && serial == token &&
                        cancelledToken != token && notifications.gesture) {
                        notifications.gesture(token, candidateForeground);
                    }
                }
                const DWORD wait = MsgWaitForMultipleObjectsEx(
                    1, &signal, kPollMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
                if (wait == WAIT_FAILED) {
                    ReportStatus(GetLastError());
                    break;
                }
            }
        } catch (...) {
            error = ERROR_UNHANDLED_EXCEPTION;
            registered = false;
            try {
                if (active && notifications.registrationChanged) notifications.registrationChanged(id);
            } catch (...) {}
        }
        active = false;
        Invalidate();
        if (hook) {
            try { operations.uninstall(std::exchange(hook, nullptr)); } catch (...) {}
        }
        registered = false;
        current = nullptr;
        finished = true;
    }
};

thread_local GlobalHotKeyService::State* GlobalHotKeyService::State::current = nullptr;

GlobalHotKeyService::GlobalHotKeyService() : operations_(NativeOperations()) {}
GlobalHotKeyService::GlobalHotKeyService(GlobalHotKeyOperations operations)
    : operations_(std::move(operations)), testOperations_(true) {}
GlobalHotKeyService::~GlobalHotKeyService() { Stop(); }

bool GlobalHotKeyService::Start(GlobalHotKeyNotifications notifications) {
    Stop();
    startError_ = ERROR_SUCCESS;
    if ((!testOperations_ && BackgroundTest()) || (testOperations_ && !QuattroTestMode())) {
        startError_ = ERROR_ACCESS_DISABLED_BY_POLICY;
        return false;
    }
    if (!operations_.install || !operations_.uninstall || !operations_.keyboard ||
        !operations_.foreground || !operations_.clock) {
        startError_ = ERROR_INVALID_PARAMETER;
        return false;
    }
    auto state = std::make_shared<State>();
    if (!state->signal) {
        startError_ = GetLastError();
        return false;
    }
    state->operations = operations_;
    state->notifications = std::move(notifications);
    state_ = state;
    try {
        // A persistent system-input listener, not a batch TaskExecutionService job.
        // Shared state owns the worker lifetime; UI teardown only signals, never joins.
        std::thread([state] { state->Run(); }).detach();
    } catch (...) {
        state->active = false;
        state->finished = true;
        startError_ = ERROR_NOT_ENOUGH_MEMORY;
        return false;
    }
    return true;
}

void GlobalHotKeyService::Stop() {
    if (!state_) return;
    state_->active = false;
    state_->Invalidate();
    SetEvent(state_->signal);
}
void GlobalHotKeyService::Refresh() {
    if (!state_ || !state_->active) return;
    state_->refresh = true;
    state_->Invalidate();
    SetEvent(state_->signal);
}
bool GlobalHotKeyService::Registered() const {
    return state_ && state_->active && state_->registered;
}
DWORD GlobalHotKeyService::LastError() const {
    return startError_ != ERROR_SUCCESS ? startError_ : state_ ? state_->error.load() : ERROR_SUCCESS;
}
UINT_PTR GlobalHotKeyService::RegistrationId() const { return state_ ? state_->id : 0; }
UINT_PTR GlobalHotKeyService::InputSerial() const { return state_ ? state_->serial.load() : 0; }
bool GlobalHotKeyService::Stopped() const { return !state_ || state_->finished; }
bool GlobalHotKeyService::IsPending(UINT_PTR token) const {
    return token && state_ && state_->active && !state_->refresh &&
        state_->cancelledToken != token && state_->pending == token && state_->serial == token &&
        state_->operations.clock() < state_->pendingExpires;
}
bool GlobalHotKeyService::Consume(UINT_PTR token) {
    if (!IsPending(token)) return false;
    state_->wakeGraceUntil = state_->operations.clock() + kWakeGraceMs;
    return state_->pending.compare_exchange_strong(token, 0) && state_->serial == token && state_->active;
}
void GlobalHotKeyService::CancelPending() {
    if (!state_) return;
    state_->cancelledToken = state_->serial.load();
    state_->pending = 0;
}
bool GlobalHotKeyService::PostTestKey(
    DWORD key, bool down, DWORD tick, bool injected, ULONG_PTR extraInfo, bool consumed) {
    if (!testOperations_ || !QuattroTestMode() || !state_ || !state_->active) return false;
    {
        std::lock_guard lock(state_->testMutex);
        state_->testKeys.push_back({key, down, tick, injected, extraInfo, consumed});
    }
    SetEvent(state_->signal);
    return true;
}
void GlobalHotKeyService::PollForTesting() {
    if (testOperations_ && QuattroTestMode() && state_) SetEvent(state_->signal);
}
