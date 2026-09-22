#include "../src/services/GlobalHotKeyService.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
int failures = 0;
int checks = 0;
void Check(bool value, const char* text) {
    ++checks;
    if (!value) {
        ++failures;
        std::cerr << "GlobalHotKey: " << text << '\n';
    }
}
template<class Predicate>
bool Wait(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (std::chrono::steady_clock::now() < end);
    return predicate();
}

struct FakeInput {
    std::atomic<ULONGLONG> now{10000};
    std::atomic<int> installs{0}, uninstalls{0}, samples{0}, gestures{0}, statuses{0};
    std::atomic<DWORD> installerThread{0}, uninstallerThread{0}, sampleThread{0};
    std::atomic<UINT_PTR> lastToken{0}, statusId{0};
    std::atomic<bool> failInstall{false}, sampleAvailable{true}, throwSample{false};
    std::atomic<bool> blockInstall{false}, installEntered{false}, releaseInstall{false};
    std::mutex mutex;
    HotKeyKeyboardSnapshot keyboard{};

    GlobalHotKeyOperations Operations() {
        return {
            [this](HOOKPROC) {
                installerThread = GetCurrentThreadId();
                const int count = ++installs;
                installEntered = true;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                while (blockInstall && !releaseInstall && std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return failInstall ? GlobalHotKeyRegistration{nullptr, ERROR_ACCESS_DENIED}
                    : GlobalHotKeyRegistration{reinterpret_cast<HHOOK>(UINT_PTR(count)), ERROR_SUCCESS};
            },
            [this](HHOOK) { uninstallerThread = GetCurrentThreadId(); ++uninstalls; },
            [this]() -> std::optional<HotKeyKeyboardSnapshot> {
                sampleThread = GetCurrentThreadId();
                ++samples;
                if (throwSample) throw std::runtime_error("test sampler failure");
                if (!sampleAvailable) return std::nullopt;
                std::lock_guard lock(mutex);
                return keyboard;
            },
            [] { return reinterpret_cast<HWND>(UINT_PTR(42)); },
            [this] { return now.load(); },
        };
    }
    GlobalHotKeyNotifications Notifications() {
        return {
            [this](UINT_PTR token, HWND foreground) {
                if (foreground != reinterpret_cast<HWND>(UINT_PTR(42))) return;
                lastToken = token;
                ++gestures;
            },
            [this](UINT_PTR id) { statusId = id; ++statuses; },
        };
    }
    void Key(DWORD key, bool down) {
        std::lock_guard lock(mutex);
        keyboard[DoubleAltGesture::CanonicalKey(key)] = down;
    }
};

struct Fixture {
    // Keep fake operation captures alive even if a failed assertion times out.
    std::shared_ptr<FakeInput> input = std::make_shared<FakeInput>();
    GlobalHotKeyService service{KeepAliveOperations(input)};
    static GlobalHotKeyOperations KeepAliveOperations(std::shared_ptr<FakeInput> input) {
        auto ops = input->Operations();
        return {
            [input, call = ops.install](HOOKPROC callback) { return call(callback); },
            [input, call = ops.uninstall](HHOOK hook) { call(hook); },
            [input, call = ops.keyboard] { return call(); },
            [input, call = ops.foreground] { return call(); },
            [input, call = ops.clock] { return call(); },
        };
    }
    bool Start() {
        return service.Start(input->Notifications());
    }
    void Advance(ULONGLONG delta) {
        input->now += delta;
        service.PollForTesting();
    }
    bool ProcessKey(DWORD key, bool down, bool updateSnapshot = true,
        bool injected = false, ULONG_PTR extra = 0, bool consumed = false) {
        if (updateSnapshot) input->Key(key, down);
        const auto before = service.InputSerial();
        input->now += 10;
        service.PostTestKey(key, down, static_cast<DWORD>(input->now.load()), injected, extra, consumed);
        return Wait([&] { return service.InputSerial() != before; });
    }
    bool Pair(bool consumed = false) {
        bool processed = ProcessKey(VK_LMENU, true);
        processed &= ProcessKey(VK_LMENU, false);
        processed &= ProcessKey(VK_LMENU, true);
        processed &= ProcessKey(VK_LMENU, false, true, false, 0, consumed);
        return processed;
    }
    ~Fixture() {
        input->releaseInstall = true;
        service.Stop();
        Check(Wait([&] { return service.Stopped(); }), "worker finishes after nonblocking stop");
    }
};

void TestReconciliation() {
    DoubleAltGesture gesture;
    HotKeyKeyboardSnapshot keys{};
    const auto pair = [&](DWORD tick) {
        gesture.OnKey(VK_LMENU, true, tick);
        gesture.OnKey(VK_LMENU, false, tick + 10);
        gesture.OnKey(VK_LMENU, true, tick + 100);
        return gesture.OnKey(VK_LMENU, false, tick + 110);
    };
    for (const DWORD key : {DWORD(VK_LCONTROL), DWORD(VK_RCONTROL), DWORD(VK_LSHIFT),
            DWORD(VK_RSHIFT), DWORD(VK_LWIN), DWORD(VK_RWIN), DWORD('X')}) {
        gesture.Reset();
        gesture.OnKey(key, true, 100);
        Check(!pair(200), "remembered held key suppresses Alt");
        Check(gesture.Reconcile(keys), "missed release is reconciled");
        Check(gesture.Consume(pair(1000)), "complete pair works after reconciliation");
        gesture.Reset();
        keys[key] = true;
        gesture.Reconcile(keys);
        Check(!pair(2000), "real held key still suppresses Alt");
        Check(!gesture.Reconcile(keys) && !pair(3600000), "elapsed time never clears a real held key");
        keys[key] = false;
    }
    gesture.Reset();
    const auto token = pair(100);
    keys[VK_RSHIFT] = true;
    Check(gesture.Reconcile(keys) && !gesture.Consume(token), "reconciliation invalidates pending token");
    keys.fill(false);
    gesture.Reconcile(keys);
    gesture.OnKey(VK_LMENU, true, 300);
    gesture.Reconcile(keys);
    Check(!gesture.OnKey(VK_LMENU, false, 310), "reconciliation cannot invent an Alt release");
    Check(gesture.Consume(pair(400)), "new complete pair recovers after missed Alt release");
    gesture.Reset();
    gesture.OnKey(VK_MENU, true, 100);
    keys[VK_LMENU] = true;
    Check(!gesture.Reconcile(keys), "generic Alt aliases match the physical snapshot");
    gesture.OnKey(VK_MENU, false, 110);
    Check(!gesture.Idle(120) && gesture.Idle(561), "maintenance preserves double-tap interval");
    keys.fill(false);
    gesture.Reset();
    gesture.OnKey(VK_LCONTROL, true, 1000, true);
    Check(!pair(1100), "injected key-down cannot leave an unguarded modifier");
    gesture.Reconcile(keys);
    Check(gesture.Consume(pair(2000)), "snapshot clears missed injected modifier release");
}

void TestWorkerAndDelivery() {
    Fixture f;
    Check(f.Start() && Wait([&] { return f.service.Registered(); }), "asynchronous registration succeeds");
    Check(f.input->installerThread != GetCurrentThreadId() &&
        f.input->installerThread == f.input->sampleThread, "hook and sampler share a non-UI thread");
    Check(Wait([&] { return f.input->statusId == f.service.RegistrationId(); }), "status carries registration identity");
    Check(f.Pair() && Wait([&] { return f.input->gestures == 1; }), "worker publishes a clean pair");
    const auto token = f.input->lastToken.load();
    Check(f.service.IsPending(token) && f.service.Consume(token) && !f.service.Consume(token),
        "UI consumes each candidate once");
    const auto serial = f.service.InputSerial();
    f.service.PostTestKey(VK_MENU, true, 11000, true, kForegroundRecoveryInputTag);
    f.service.PostTestKey(VK_MENU, false, 11001, true, kForegroundRecoveryInputTag);
    // A following ordinary event is a processing barrier for both ignored events.
    Check(f.ProcessKey('X', true), "worker processes input after recovery pair");
    Check(f.service.InputSerial() == serial + 1, "own recovery input does not change input serial");
    f.ProcessKey('X', false);
    f.Advance(3000);
    Check(Wait([&] { return f.input->samples >= 2; }), "sampling resumes after wake grace");
    Check(f.Pair() && Wait([&] { return f.input->gestures == 2; }), "next clean pair works");
    const auto cancelled = f.input->lastToken.load();
    f.service.CancelPending();
    Check(!f.service.IsPending(cancelled) && !f.service.Consume(cancelled), "UI cancellation rejects queued candidate");
    Check(f.Pair(true), "downstream-consumed pair processed");
    Check(f.input->gestures == 2, "downstream hook consumption does not wake");
    Check(f.Pair() && Wait([&] { return f.input->gestures == 3; }), "new token after cancellation");
    const auto expired = f.input->lastToken.load();
    f.Advance(1100);
    Check(!f.service.IsPending(expired), "stalled UI cannot dispatch an expired candidate");
    f.service.Stop();
    Check(!f.service.Registered() && !f.service.Consume(expired), "stop immediately disables dispatch");
    Check(Wait([&] { return f.service.Stopped(); }), "listener exits");
    Check(f.input->uninstallerThread == f.input->installerThread, "hook uninstalls on its owning thread");
}

void TestMissedReleaseRecovery() {
    Fixture f;
    Check(f.Start() && Wait([&] { return f.service.Registered(); }), "recovery fixture registered");
    Check(f.ProcessKey(VK_LCONTROL, true), "Ctrl down received");
    f.input->Key(VK_LCONTROL, false); // The OS released it, but no hook key-up arrived.
    Check(f.Pair() && f.input->gestures == 0, "lost release reproduces suppression before sampling");
    const auto before = f.service.InputSerial();
    f.Advance(200);
    Check(Wait([&] { return f.service.InputSerial() != before; }), "worker reconciles missing release");
    Check(f.Pair() && Wait([&] { return f.input->gestures == 1; }), "worker recovers without restart");
    f.service.CancelPending();
    f.ProcessKey(VK_LCONTROL, true);
    f.Advance(3600000);
    const auto samples = f.input->samples.load();
    f.service.PollForTesting();
    Check(Wait([&] { return f.input->samples > samples; }), "held-key snapshot sampled");
    Check(f.Pair() && f.input->gestures == 1, "real held Ctrl never triggers after long idle");
    Check(f.input->installs == 1, "maintenance does not renew while a key is held");
}

void TestRegistrationRecovery() {
    Fixture f;
    f.input->failInstall = true;
    Check(f.Start() && Wait([&] { return f.input->statuses > 0; }), "initial registration failure reported");
    Check(!f.service.Registered() && f.service.LastError() == ERROR_ACCESS_DENIED,
        "failed registration is not reported as registered");
    f.Advance(500);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Check(f.input->installs == 1, "failed install does not spin");
    f.input->failInstall = false;
    f.Advance(500);
    Check(Wait([&] { return f.service.Registered(); }), "backoff retry recovers");
    const auto registration = f.service.RegistrationId();
    Check(f.Pair() && Wait([&] { return f.input->gestures == 1; }), "pre-refresh gesture");
    const auto oldToken = f.input->lastToken.load();
    f.service.Refresh();
    Check(!f.service.IsPending(oldToken), "refresh immediately invalidates queued token");
    f.Advance(500);
    Check(Wait([&] { return f.input->installs == 3; }), "refresh renews in idle interval");
    Check(f.service.RegistrationId() == registration, "renewal keeps the service instance identity");
    f.input->failInstall = true;
    f.Advance(60000);
    Check(Wait([&] { return f.service.LastError() == ERROR_ACCESS_DENIED; }), "renewal failure reported");
    Check(f.service.Registered() && f.input->uninstalls == 1, "failed replacement retains old hook");
    f.input->failInstall = false;
    f.Advance(1000);
    Check(Wait([&] { return f.input->uninstalls == 2; }), "old hook removed only after replacement succeeds");
    Check(f.Pair() && Wait([&] { return f.input->gestures == 2; }), "post-renewal gesture");
    const auto previousToken = f.input->lastToken.load();
    f.service.Stop();
    Check(Wait([&] { return f.service.Stopped(); }), "old service stopped before restart test");
    f.Advance(1000);
    Check(f.Start() && Wait([&] { return f.service.Registered(); }), "service restarts");
    Check(f.service.RegistrationId() != registration && !f.service.Consume(previousToken),
        "new service rejects all old registration tokens");
}

void TestUnavailableAndStop() {
    {
        Fixture f;
        Check(f.Start() && Wait([&] { return f.service.Registered(); }), "desktop fixture registered");
        f.input->sampleAvailable = false;
        const auto before = f.service.InputSerial();
        f.Advance(500);
        Check(Wait([&] { return f.service.InputSerial() != before; }), "unavailable desktop cancels recognition");
        Check(f.Pair() && f.input->gestures == 0, "unavailable snapshot cannot invent released keys");
        f.input->sampleAvailable = true;
        f.Advance(500);
        Check(Wait([&] { return f.input->installs == 2; }), "desktop return renews listener");
        Check(f.Pair() && Wait([&] { return f.input->gestures == 1; }), "recognition resumes after desktop return");
        f.service.CancelPending();
        f.input->throwSample = true;
        f.Advance(500);
        Check(Wait([&] { return f.service.Stopped(); }), "exception ends listener safely");
        Check(!f.service.Registered() && f.service.LastError() == ERROR_UNHANDLED_EXCEPTION,
            "exception reported as failure, not success");
    }
    {
        Fixture f;
        f.input->blockInstall = true;
        Check(f.Start() && Wait([&] { return f.input->installEntered.load(); }), "slow native call entered");
        const auto start = std::chrono::steady_clock::now();
        f.service.Stop();
        Check(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(100),
            "UI stop does not wait for a blocked native operation");
        f.input->releaseInstall = true;
        Check(Wait([&] { return f.service.Stopped(); }), "late registration completes cleanup");
        Check(f.input->statuses == 0 && f.input->uninstalls == 1, "late registration never publishes after stop");
    }
}

void TestMaintenanceBoundaries() {
    Fixture f;
    Check(f.Start() && Wait([&] { return f.service.Registered(); }), "maintenance fixture registered");
    f.Advance(59950);
    f.ProcessKey(VK_LMENU, true);
    f.ProcessKey(VK_LMENU, false);
    const auto samples = f.input->samples.load();
    f.Advance(100);
    Check(Wait([&] { return f.input->samples > samples; }), "maintenance sampled after first Alt tap");
    Check(f.input->installs == 1, "renewal cannot interrupt a half-completed double tap");
    f.ProcessKey(VK_LMENU, true);
    f.ProcessKey(VK_LMENU, false);
    Check(Wait([&] { return f.input->gestures == 1; }), "second tap completes during renewal interval");
    const auto token = f.input->lastToken.load();
    const auto nextSamples = f.input->samples.load();
    f.Advance(100);
    Check(Wait([&] { return f.input->samples > nextSamples; }), "pending candidate maintenance ran");
    Check(f.input->installs == 1 && f.service.IsPending(token),
        "renewal cannot discard a valid UI candidate");
    Check(f.service.Consume(token), "pending maintenance candidate consumed");
    f.Advance(1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    Check(f.input->installs == 1, "foreground recovery grace prevents maintenance cancellation");
    f.Advance(1100);
    Check(Wait([&] { return f.input->installs == 2; }), "idle renewal resumes after recovery grace");
    Check(f.Pair() && Wait([&] { return f.input->gestures == 2; }), "new candidate after renewal");
    const auto oldToken = f.input->lastToken.load();
    f.ProcessKey('X', true);
    Check(!f.service.IsPending(oldToken) && !f.service.Consume(oldToken),
        "new keyboard input cancels a queued UI candidate");
}
}

int RunGlobalHotKeyServiceTests() {
    failures = 0;
    checks = 0;
    if (!QuattroTestMode() || !BackgroundAcceptanceMode() || !SuppressForegroundActivation()) {
        std::cerr << "GlobalHotKey tests require the isolated background harness\n";
        return 1;
    }
    GlobalHotKeyService native;
    Check(!native.Start({}) && native.Stopped(), "background mode never installs native input hooks");
    TestReconciliation();
    TestWorkerAndDelivery();
    TestMissedReleaseRecovery();
    TestRegistrationRecovery();
    TestUnavailableAndStop();
    TestMaintenanceBoundaries();
    if (!failures) std::cout << "global_hotkey_service_tests=passed checks=" << checks << '\n';
    return failures;
}
