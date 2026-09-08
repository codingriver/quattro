#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

std::filesystem::path GetModuleDirectory();
std::filesystem::path UserHomeDirectory();
std::filesystem::path QuattroUserConfigDirectory();
std::wstring Trim(const std::wstring& value);
std::wstring ToLower(std::wstring value);
std::wstring FormatVersionForDisplay(const std::wstring& version);
std::wstring FormatByteSizeForDisplay(std::uint64_t bytes);
std::wstring FormatLastError(DWORD error);
std::wstring LoadUtf8File(const std::filesystem::path& path);
bool FileExists(const std::filesystem::path& path);
bool DirectoryExists(const std::filesystem::path& path);
std::wstring QuoteForCommandLine(const std::wstring& value);
std::wstring ReplaceAll(std::wstring value, const std::wstring& from, const std::wstring& to);
std::uint32_t StablePathHash(const std::wstring& value);
std::wstring Hex8(std::uint32_t value);
bool HasUrlScheme(const std::wstring& value);
std::wstring NormalizeUrl(std::wstring value);
std::wstring ExpandEnvironmentStringsSafe(const std::wstring& value);
std::optional<int> ParseInt(const std::wstring& value);
bool SuppressForegroundActivation();
bool QuattroTestMode();
bool BackgroundAcceptanceMode();
void ApplyWindowBackgroundPolicy(HWND hwnd);
bool ActivateWindow(HWND hwnd);
enum class WindowFrontness { Unknown, Front, Behind };

struct WindowZOrderEntry {
    HWND window = nullptr;
    HWND rootOwner = nullptr;
    bool valid = false;
    bool visible = false;
    bool minimized = false;
    bool topMost = false;
    bool cloaked = false;
    bool auxiliary = false;
    bool operator==(const WindowZOrderEntry&) const = default;
};

// Entries are ordered from top to bottom and must include the target.
WindowFrontness EvaluateWindowFrontness(HWND target, const std::vector<WindowZOrderEntry>& entries);

struct WindowPresentation {
    bool visible = false;
    bool minimized = false;
    bool topMost = false;
    bool foreground = false;
    bool focused = false;
    HWND foregroundWindow = nullptr;
    WindowFrontness frontness = WindowFrontness::Unknown;
};

WindowPresentation QueryWindowPresentation(HWND hwnd);

enum class ForegroundInputRecoveryStatus {
    NotRequested, Suppressed, Cancelled, KeysHeld, InputRejected, PartialInput, Injected,
};

struct ForegroundInputSendResult {
    UINT inserted = 0;
    DWORD error = ERROR_SUCCESS;
};

struct ForegroundInputRecoveryResult {
    ForegroundInputRecoveryStatus status = ForegroundInputRecoveryStatus::NotRequested;
    ForegroundInputSendResult pair;
    ForegroundInputSendResult release;
    bool MayActivate() const {
        return status == ForegroundInputRecoveryStatus::NotRequested ||
            status == ForegroundInputRecoveryStatus::Injected;
    }
};

// Injectable operations keep all input recovery tests free of desktop input.
struct ForegroundInputRecoveryOperations {
    std::function<bool()> contextCurrent;
    std::function<bool()> keysReleased;
    std::function<ForegroundInputSendResult()> sendAltPair;
    std::function<ForegroundInputSendResult()> releaseAlt;
};
constexpr ULONG_PTR kForegroundRecoveryInputTag = 0x51575241; // QWRA
ForegroundInputRecoveryResult PerformForegroundInputRecovery(
    const ForegroundInputRecoveryOperations& operations, bool suppressed);
std::array<INPUT, 2> MakeForegroundRecoveryAltInputs();
const wchar_t* ForegroundInputRecoveryStatusText(ForegroundInputRecoveryStatus status);

struct WindowActivationResult {
    WindowPresentation presentation;
    ForegroundInputRecoveryResult inputRecovery;
    HWND foregroundBeforeRequest = nullptr;
    DWORD foregroundGuiFlags = 0;
    bool foregroundGuiKnown = false;
    bool suppressed = false;
    bool cancelled = false;
    bool foregroundAttempted = false;
    // Raw SetForegroundWindow return, separate from actual presentation.
    bool foregroundRequested = false;
    DWORD positionError = ERROR_SUCCESS;
    bool Succeeded() const {
        return !suppressed && !cancelled && inputRecovery.MayActivate() && positionError == ERROR_SUCCESS &&
            presentation.visible && !presentation.minimized &&
            presentation.foreground && presentation.focused &&
            presentation.frontness == WindowFrontness::Front;
    }
};

// Explicit operations keep the production sequence testable without desktop input.
struct WindowActivationOperations {
    std::function<void()> restore;
    std::function<bool()> requestForeground;
    std::function<DWORD()> raise;
    std::function<WindowPresentation()> query;
    std::function<bool()> continueRequest;
    std::function<ForegroundInputRecoveryResult()> recoverForeground;
};
WindowActivationResult PerformWindowActivationAttempt(
    const WindowActivationOperations& operations, bool suppressed);
WindowActivationResult RequestWindowForeground(
    HWND hwnd, bool topMost, std::function<bool()> continueRequest = {});
// Explicit opt-in for the sole retry of a physical user gesture. Ordinary
// ActivateWindow/RequestWindowForeground callers never synthesize input.
WindowActivationResult RequestWindowForegroundWithInputRecovery(
    HWND hwnd, bool topMost, HWND expectedForeground, std::function<bool()> continueRequest);

class WindowActivationRetry {
public:
    UINT_PTR Begin() { Cancel(); return generation_; }
    void Cancel() { ++generation_; pending_ = false; scheduled_ = false; inputRecovery_ = false; }
    bool IsCurrent(UINT_PTR generation) const { return generation == generation_; }
    bool Schedule(UINT_PTR generation, const WindowActivationResult& result, HWND previousForeground,
        bool allowInputRecovery = false);
    bool Consume(UINT_PTR generation, HWND currentForeground, bool targetForeground, bool visible,
        bool* recoverInput = nullptr);
private:
    UINT_PTR generation_ = 0;
    bool pending_ = false;
    bool scheduled_ = false;
    HWND previousForeground_ = nullptr;
    bool inputRecovery_ = false;
};

void ShowWindowRespectFocusPolicy(HWND hwnd, int showCommand);
POINT ClampWindowToOwnerMonitor(HWND owner, int x, int y, int width, int height);
POINT CenterWindowOnOwnerMonitor(HWND owner, int width, int height);
POINT OffsetWindowFromOwnerOnMonitor(HWND owner, int width, int height, int offsetX, int offsetY);
bool ShowModalWindow(HWND owner, HWND hwnd);
void RestoreModalOwner(HWND owner, bool ownerWasEnabled, bool& ownerRestored);
