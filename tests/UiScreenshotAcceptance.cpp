#include "../src/windows/BuiltinTools.h"
#include "../src/windows/LinkEditDialog.h"
#include "../src/domain/MenuCatalog.h"
#include "../src/domain/Models.h"
#include "../src/windows/QuickImportDialog.h"
#include "../src/windows/SimpleDialogs.h"
#include "../src/windows/ConfirmDialog.h"
#include "../src/windows/UpdateCheckDialog.h"
#include "../src/theme/Theme.h"
#include "../src/windows/TodoEditDialog.h"
#include "../src/windows/UrlEditDialog.h"
#include "../src/services/WebDavClient.h"
#include "../src/domain/PluginRegistry.h"

#include <commctrl.h>
#include <gdiplus.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Scenario {
    std::wstring name;
    std::wstring windowClass;
    std::wstring title;
    std::wstring screenshotName;
    std::vector<std::wstring> expectedTexts;
    std::vector<std::wstring> expectedEditTexts;
    int minEditCount = 0;
    int minButtonCount = 0;
    bool probeFirstEdit = false;
    bool rejectDarkSurface = false;
    bool cancelOnly = false;
    std::vector<std::wstring> expectedVisibleChildTexts;
    std::wstring activateButtonText;
    bool requireThemedEditFrames = false;
};

struct TestState {
    bool ok = true;
    std::vector<std::wstring> failures;

    void Check(bool condition, const std::wstring& message) {
        if (!condition) {
            ok = false;
            failures.push_back(message);
        }
    }
};

std::wstring WindowText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

std::wstring ClassName(HWND hwnd) {
    wchar_t buffer[128]{};
    GetClassNameW(hwnd, buffer, static_cast<int>(std::size(buffer)));
    return buffer;
}

bool Contains(const std::wstring& text, const std::wstring& needle) {
    return text.find(needle) != std::wstring::npos;
}

std::string NarrowDiagnostic(const std::wstring& text) {
    std::string out;
    out.reserve(text.size());
    for (wchar_t ch : text) {
        out.push_back(ch >= 0x20 && ch <= 0x7e ? static_cast<char>(ch) : '?');
    }
    return out;
}

void AcceptanceLog(const std::wstring& text) {
    std::filesystem::create_directories("screenshots/acceptance");
    std::ofstream log("screenshots/acceptance/ui-acceptance-progress.txt", std::ios::binary | std::ios::app);
    log << NarrowDiagnostic(text) << "\n";
}

struct FindWindowRequest {
    std::wstring windowClass;
    std::wstring title;
    DWORD processId = GetCurrentProcessId();
};

struct EnumWindowData {
    FindWindowRequest request;
    HWND match = nullptr;
};

BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<EnumWindowData*>(lParam);
    if (!IsWindowVisible(hwnd)) {
        return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (data->request.processId != 0 && processId != data->request.processId) {
        return TRUE;
    }
    if (!data->request.windowClass.empty() && ClassName(hwnd) != data->request.windowClass) {
        return TRUE;
    }
    if (!data->request.title.empty() && WindowText(hwnd) != data->request.title) {
        return TRUE;
    }
    data->match = hwnd;
    return FALSE;
}

HWND FindTopWindow(const FindWindowRequest& request) {
    EnumWindowData data{request, nullptr};
    EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(&data));
    return data.match;
}

HWND WaitForTopWindow(const FindWindowRequest& request, DWORD timeoutMs = 5000) {
    const auto begin = GetTickCount64();
    while (GetTickCount64() - begin < timeoutMs) {
        HWND hwnd = FindTopWindow(request);
        if (hwnd) {
            return hwnd;
        }
        Sleep(50);
    }
    return nullptr;
}

struct CloseWindowData {
    DWORD processId = GetCurrentProcessId();
    HWND except = nullptr;
};

BOOL CALLBACK CloseTopWindowsProc(HWND hwnd, LPARAM lParam) {
    auto* data = reinterpret_cast<CloseWindowData*>(lParam);
    if (!IsWindowVisible(hwnd) || hwnd == data->except) {
        return TRUE;
    }
    if (ClassName(hwnd) == L"QuattroUiAcceptanceOwner") {
        return TRUE;
    }
    DWORD processId = 0;
    GetWindowThreadProcessId(hwnd, &processId);
    if (processId == data->processId) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

void CloseTopWindowsForProcess(DWORD processId, HWND except) {
    CloseWindowData data{processId, except};
    EnumWindows(CloseTopWindowsProc, reinterpret_cast<LPARAM>(&data));
}

int GetEncoderClsid(const WCHAR* format, CLSID* clsid) {
    UINT count = 0;
    UINT size = 0;
    Gdiplus::GetImageEncodersSize(&count, &size);
    if (size == 0) {
        return -1;
    }
    std::vector<BYTE> storage(size);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(storage.data());
    Gdiplus::GetImageEncoders(count, size, encoders);
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, format) == 0) {
            *clsid = encoders[i].Clsid;
            return static_cast<int>(i);
        }
    }
    return -1;
}

struct BitmapCapture {
    HBITMAP bitmap = nullptr;
    int width = 0;
    int height = 0;
};

BitmapCapture CaptureWindowBitmap(HWND hwnd) {
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int height = std::max(1, static_cast<int>(rect.bottom - rect.top));

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ old = SelectObject(dc, bitmap);

    HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
    RECT fill{0, 0, width, height};
    FillRect(dc, &fill, magenta);
    DeleteObject(magenta);

    SetForegroundWindow(hwnd);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    Sleep(120);
    BOOL printed = PrintWindow(hwnd, dc, 0x00000002);
    if (!printed) {
        BitBlt(dc, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY);
    }

    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return BitmapCapture{bitmap, width, height};
}

bool SavePng(HBITMAP bitmap, const std::filesystem::path& path) {
    CLSID pngClsid{};
    if (GetEncoderClsid(L"image/png", &pngClsid) < 0) {
        return false;
    }
    Gdiplus::Bitmap image(bitmap, nullptr);
    return image.Save(path.c_str(), &pngClsid, nullptr) == Gdiplus::Ok;
}

bool BitmapHasVisualContent(HBITMAP bitmap, int width, int height) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    HDC dc = GetDC(nullptr);
    const int lines = GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines == 0 || pixels.empty()) {
        return false;
    }
    std::set<std::uint32_t> colors;
    const std::size_t step = std::max<std::size_t>(1, pixels.size() / 5000);
    for (std::size_t i = 0; i < pixels.size(); i += step) {
        colors.insert(pixels[i] & 0x00ffffffu);
        if (colors.size() >= 24) {
            return true;
        }
    }
    return colors.size() >= 12;
}

bool BitmapHasAtLeastColors(HBITMAP bitmap, int width, int height, std::size_t requiredColors) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    HDC dc = GetDC(nullptr);
    const int lines = GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines == 0 || pixels.empty()) {
        return false;
    }
    std::set<std::uint32_t> colors;
    for (std::uint32_t pixel : pixels) {
        colors.insert(pixel & 0x00ffffffu);
        if (colors.size() >= requiredColors) {
            return true;
        }
    }
    return false;
}

double NearBlackPixelRatio(HBITMAP bitmap, int width, int height) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    HDC dc = GetDC(nullptr);
    const int lines = GetDIBits(dc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines == 0 || pixels.empty()) {
        return 1.0;
    }

    std::size_t sampled = 0;
    std::size_t nearBlack = 0;
    const std::size_t step = std::max<std::size_t>(1, pixels.size() / 20000);
    for (std::size_t i = 0; i < pixels.size(); i += step) {
        const std::uint32_t pixel = pixels[i] & 0x00ffffffu;
        const int r = static_cast<int>((pixel >> 16) & 0xff);
        const int g = static_cast<int>((pixel >> 8) & 0xff);
        const int b = static_cast<int>(pixel & 0xff);
        ++sampled;
        if (r < 24 && g < 24 && b < 24) {
            ++nearBlack;
        }
    }
    return sampled == 0 ? 1.0 : static_cast<double>(nearBlack) / static_cast<double>(sampled);
}

struct ChildInfo {
    HWND hwnd = nullptr;
    std::wstring className;
    std::wstring text;
};

BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM lParam) {
    auto* children = reinterpret_cast<std::vector<ChildInfo>*>(lParam);
    children->push_back(ChildInfo{hwnd, ClassName(hwnd), WindowText(hwnd)});
    return TRUE;
}

std::vector<ChildInfo> Children(HWND hwnd) {
    std::vector<ChildInfo> children;
    EnumChildWindows(hwnd, EnumChildProc, reinterpret_cast<LPARAM>(&children));
    return children;
}

std::vector<std::wstring> CollectTexts(HWND hwnd, const std::vector<ChildInfo>& children) {
    std::vector<std::wstring> texts;
    texts.push_back(WindowText(hwnd));
    for (const auto& child : children) {
        if (!child.text.empty()) {
            texts.push_back(child.text);
        }
        if (child.className == L"ListBox") {
            const int count = static_cast<int>(SendMessageW(child.hwnd, LB_GETCOUNT, 0, 0));
            for (int i = 0; i < count && i < 20; ++i) {
                const int length = static_cast<int>(SendMessageW(child.hwnd, LB_GETTEXTLEN, i, 0));
                if (length > 0) {
                    std::wstring item(static_cast<std::size_t>(length) + 1, L'\0');
                    SendMessageW(child.hwnd, LB_GETTEXT, i, reinterpret_cast<LPARAM>(item.data()));
                    item.resize(static_cast<std::size_t>(length));
                    texts.push_back(item);
                }
            }
        } else if (child.className == L"ComboBox") {
            const int selected = static_cast<int>(SendMessageW(child.hwnd, CB_GETCURSEL, 0, 0));
            if (selected >= 0) {
                const int length = static_cast<int>(SendMessageW(child.hwnd, CB_GETLBTEXTLEN, selected, 0));
                if (length > 0) {
                    std::wstring item(static_cast<std::size_t>(length) + 1, L'\0');
                    SendMessageW(child.hwnd, CB_GETLBTEXT, static_cast<WPARAM>(selected), reinterpret_cast<LPARAM>(item.data()));
                    item.resize(static_cast<std::size_t>(length));
                    texts.push_back(item);
                }
            }
        } else if (child.className == L"SysListView32") {
            const int count = static_cast<int>(SendMessageW(child.hwnd, LVM_GETITEMCOUNT, 0, 0));
            for (int row = 0; row < count && row < 20; ++row) {
                for (int column = 0; column < 4; ++column) {
                    wchar_t buffer[512]{};
                    LVITEMW item{};
                    item.iSubItem = column;
                    item.cchTextMax = static_cast<int>(std::size(buffer));
                    item.pszText = buffer;
                    SendMessageW(child.hwnd, LVM_GETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
                    if (buffer[0] != L'\0') {
                        texts.push_back(buffer);
                    }
                }
            }
        }
    }
    return texts;
}

void ValidateChildBounds(HWND parent, const std::vector<ChildInfo>& children, TestState& state, const std::wstring& name) {
    RECT client{};
    GetClientRect(parent, &client);
    for (const auto& child : children) {
        if (!IsWindowVisible(child.hwnd)) {
            continue;
        }
        if (child.className == L"ComboBox") {
            continue;
        }
        if (child.className != L"Button" &&
            child.className != L"Edit" &&
            child.className != L"ListBox" &&
            child.className != L"Static" &&
            child.className != L"SysListView32") {
            continue;
        }
        RECT rect{};
        GetWindowRect(child.hwnd, &rect);
        MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&rect), 2);
        const bool inside =
            rect.left >= client.left - 2 &&
            rect.top >= client.top - 2 &&
            rect.right <= client.right + 2 &&
            rect.bottom <= client.bottom + 2;
        state.Check(inside, name + L": child control outside client area, class=" + child.className + L", text=" + child.text);
    }
}

void ValidateAndCapture(HWND hwnd, const Scenario& scenario, const std::filesystem::path& outputDir, TestState& state) {
    RECT client{};
    GetClientRect(hwnd, &client);
    state.Check(client.right - client.left >= 120 && client.bottom - client.top >= 80, scenario.name + L": client area too small");

    auto children = Children(hwnd);
    if (!scenario.activateButtonText.empty()) {
        auto button = std::find_if(children.begin(), children.end(), [&](const ChildInfo& child) {
            return child.className == L"Button" && child.text == scenario.activateButtonText;
        });
        state.Check(button != children.end(), scenario.name + L": activation button not found: " + scenario.activateButtonText);
        if (button != children.end()) {
            SendMessageW(button->hwnd, BM_CLICK, 0, 0);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            children = Children(hwnd);
        }
    }
    ValidateChildBounds(hwnd, children, state, scenario.name);

    const auto texts = CollectTexts(hwnd, children);
    for (const auto& expected : scenario.expectedTexts) {
        bool found = false;
        for (const auto& text : texts) {
            if (Contains(text, expected)) {
                found = true;
                break;
            }
        }
        state.Check(found, scenario.name + L": expected text not found: " + expected);
    }

    for (const auto& expected : scenario.expectedVisibleChildTexts) {
        bool found = false;
        for (const auto& child : children) {
            if (IsWindowVisible(child.hwnd) && Contains(child.text, expected)) {
                found = true;
                RECT childRect{};
                GetWindowRect(child.hwnd, &childRect);
                std::vector<HWND> zOrder;
                for (HWND current = GetTopWindow(hwnd); current; current = GetWindow(current, GW_HWNDNEXT)) {
                    zOrder.push_back(current);
                }
                const auto childZ = std::find(zOrder.begin(), zOrder.end(), child.hwnd);
                for (const auto& candidate : children) {
                    if (!IsWindowVisible(candidate.hwnd) || candidate.className != L"Button" || !candidate.text.empty()) continue;
                    RECT containerRect{};
                    GetWindowRect(candidate.hwnd, &containerRect);
                    const bool containsChild = containerRect.left <= childRect.left && containerRect.top <= childRect.top
                        && containerRect.right >= childRect.right && containerRect.bottom >= childRect.bottom;
                    if (!containsChild) continue;
                    const auto containerZ = std::find(zOrder.begin(), zOrder.end(), candidate.hwnd);
                    state.Check(
                        childZ != zOrder.end() && containerZ != zOrder.end() && childZ < containerZ,
                        scenario.name + L": visible child is behind its group container: " + expected);
                }
                break;
            }
        }
        state.Check(found, scenario.name + L": expected visible child text not found: " + expected);
    }

    int editCount = 0;
    int buttonCount = 0;
    std::vector<HWND> edits;
    for (const auto& child : children) {
        if (child.className == L"Edit") {
            ++editCount;
            edits.push_back(child.hwnd);
        } else if (child.className == L"Button") {
            ++buttonCount;
        }
    }
    state.Check(editCount >= scenario.minEditCount, scenario.name + L": edit count lower than expected");
    state.Check(buttonCount >= scenario.minButtonCount, scenario.name + L": button count lower than expected");

    for (const auto& expected : scenario.expectedEditTexts) {
        bool found = false;
        for (HWND edit : edits) {
            if (Contains(WindowText(edit), expected)) {
                found = true;
                break;
            }
        }
        state.Check(found, scenario.name + L": expected edit text not found: " + expected);
    }
    if (scenario.probeFirstEdit && !edits.empty()) {
        const std::wstring probe = L"UI_ACCEPTANCE_INPUT_123";
        SetWindowTextW(edits.front(), probe.c_str());
        state.Check(WindowText(edits.front()) == probe, scenario.name + L": edit set/get probe failed");
    }

    if (scenario.requireThemedEditFrames) {
        std::vector<HWND> visibleEdits;
        std::vector<HWND> visibleFrames;
        for (const auto& child : children) {
            if (!IsWindowVisible(child.hwnd)) continue;
            if (child.className == L"Edit") visibleEdits.push_back(child.hwnd);
            else if (child.className == L"QuattroThemedEditFrame") visibleFrames.push_back(child.hwnd);
        }
        state.Check(!visibleEdits.empty(), scenario.name + L": expected at least one visible Edit");
        state.Check(
            visibleFrames.size() == visibleEdits.size(),
            scenario.name + L": visible themed Edit frame count does not match visible Edit count");

        std::vector<HWND> zOrder;
        for (HWND current = GetTopWindow(hwnd); current; current = GetWindow(current, GW_HWNDNEXT)) {
            zOrder.push_back(current);
        }
        for (HWND edit : visibleEdits) {
            RECT editRect{};
            GetWindowRect(edit, &editRect);
            HWND matchingFrame = nullptr;
            RECT matchingFrameRect{};
            for (HWND frame : visibleFrames) {
                RECT frameRect{};
                GetWindowRect(frame, &frameRect);
                if (frameRect.left <= editRect.left && frameRect.top <= editRect.top &&
                    frameRect.right >= editRect.right && frameRect.bottom >= editRect.bottom) {
                    matchingFrame = frame;
                    matchingFrameRect = frameRect;
                    break;
                }
            }
            state.Check(matchingFrame != nullptr, scenario.name + L": visible Edit is not enclosed by a themed frame");
            if (!matchingFrame) continue;

            const auto editZ = std::find(zOrder.begin(), zOrder.end(), edit);
            const auto frameZ = std::find(zOrder.begin(), zOrder.end(), matchingFrame);
            state.Check(
                editZ != zOrder.end() && frameZ != zOrder.end() && editZ < frameZ,
                scenario.name + L": Edit must stay above its themed frame");
            for (const auto& candidate : children) {
                if (!IsWindowVisible(candidate.hwnd) || candidate.className != L"Button" || !candidate.text.empty()) continue;
                RECT containerRect{};
                GetWindowRect(candidate.hwnd, &containerRect);
                if (containerRect.left <= matchingFrameRect.left && containerRect.top <= matchingFrameRect.top &&
                    containerRect.right >= matchingFrameRect.right && containerRect.bottom >= matchingFrameRect.bottom) {
                    const auto containerZ = std::find(zOrder.begin(), zOrder.end(), candidate.hwnd);
                    state.Check(
                        frameZ != zOrder.end() && containerZ != zOrder.end() && frameZ < containerZ,
                        scenario.name + L": themed Edit frame must stay above its GroupBox");
                }
            }

            BitmapCapture frameCapture = CaptureWindowBitmap(matchingFrame);
            state.Check(frameCapture.bitmap != nullptr, scenario.name + L": themed Edit frame capture failed");
            if (frameCapture.bitmap) {
                state.Check(
                    BitmapHasAtLeastColors(frameCapture.bitmap, frameCapture.width, frameCapture.height, 2),
                    scenario.name + L": themed Edit frame did not paint a distinct border and background");
                DeleteObject(frameCapture.bitmap);
            }
        }
    }

    BitmapCapture capture = CaptureWindowBitmap(hwnd);
    const std::filesystem::path screenshot = outputDir / scenario.screenshotName;
    state.Check(capture.bitmap != nullptr, scenario.name + L": screenshot bitmap was not created");
    if (capture.bitmap) {
        state.Check(BitmapHasVisualContent(capture.bitmap, capture.width, capture.height), scenario.name + L": screenshot looks blank or too flat");
        if (scenario.rejectDarkSurface) {
            state.Check(NearBlackPixelRatio(capture.bitmap, capture.width, capture.height) < 0.12, scenario.name + L": screenshot contains too much near-black background");
        }
        state.Check(SavePng(capture.bitmap, screenshot), scenario.name + L": screenshot save failed: " + screenshot.wstring());
        DeleteObject(capture.bitmap);
    }
}

HWND CreateOwnerWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"QuattroUiAcceptanceOwner";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"ui acceptance owner", WS_OVERLAPPEDWINDOW,
        80, 80, 760, 680, nullptr, nullptr, instance, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return hwnd;
}

void RunDialogScenario(const Scenario& scenario, const std::filesystem::path& outputDir, TestState& state, const std::function<void()>& runDialog) {
    std::atomic<bool> inspected = false;
    AcceptanceLog(L"begin " + scenario.name);
    std::wcout << L"ui_scenario_begin=" << scenario.name << L"\n";
    std::thread controller([&]() {
        AcceptanceLog(L"wait " + scenario.name);
        HWND hwnd = WaitForTopWindow(FindWindowRequest{scenario.windowClass, scenario.title, GetCurrentProcessId()}, 7000);
        if (!hwnd) {
            state.Check(false, scenario.name + L": window did not appear");
            inspected = true;
            AcceptanceLog(L"missing " + scenario.name);
            CloseTopWindowsForProcess(GetCurrentProcessId(), nullptr);
            return;
        }
        AcceptanceLog(L"inspect " + scenario.name);
        ValidateAndCapture(hwnd, scenario, outputDir, state);
        inspected = true;
        if (!scenario.cancelOnly) {
            PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
        }
        PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    });

    AcceptanceLog(L"run " + scenario.name);
    runDialog();
    AcceptanceLog(L"returned " + scenario.name);
    controller.join();
    state.Check(inspected.load(), scenario.name + L": controller did not inspect window");
    std::wcout << L"ui_scenario_end=" << scenario.name << L"\n";
    AcceptanceLog(L"end " + scenario.name);
}

std::filesystem::path ModuleDirectory() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

void RunMainWindowScenario(const std::filesystem::path& outputDir, TestState& state) {
    AcceptanceLog(L"begin main-window");
    const std::filesystem::path exe = ModuleDirectory() / L"Quattro.exe";
    if (!std::filesystem::exists(exe)) {
        state.Check(false, L"main-window: Quattro.exe not found beside acceptance executable");
        AcceptanceLog(L"missing main-window exe");
        return;
    }
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    if (!CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, ModuleDirectory().c_str(), &startup, &process)) {
        state.Check(false, L"main-window: CreateProcess failed");
        AcceptanceLog(L"create main-window failed");
        return;
    }

    AcceptanceLog(L"wait main-window");
    HWND hwnd = WaitForTopWindow(FindWindowRequest{L"QuattroMainWindow", L"", process.dwProcessId}, 10000);
    if (hwnd) {
        AcceptanceLog(L"inspect main-window");
        Scenario scenario{L"main-window", L"QuattroMainWindow", L"", L"main-window.png", {L"Quattro"}, {}, 0, 0, false};
        ValidateAndCapture(hwnd, scenario, outputDir, state);
        PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(ID_MENU_EXIT, 0), 0);
    } else {
        state.Check(false, L"main-window: window did not appear");
        AcceptanceLog(L"missing main-window");
    }

    if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 2);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    AcceptanceLog(L"end main-window");
}

AppModel SampleModel() {
    AppModel model;
    Group group;
    group.id = 1;
    group.name = L"验收分组";
    model.groups.push_back(group);

    Link link;
    link.id = 1;
    link.name = L"验收链接";
    link.parentGroup = 1;
    link.path = L"https://example.com";
    link.remark = L"验收备注";
    model.links.push_back(link);

    TodoItem todo;
    todo.id = 1;
    todo.title = L"验收待办";
    todo.content = L"验收待办内容";
    model.todos.push_back(todo);
    return model;
}

} // namespace

int wmain() {
    std::ofstream("ui-acceptance-progress.txt", std::ios::binary | std::ios::trunc);
    AcceptanceLog(L"start");
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_DATE_CLASSES;
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gdiplusInput{};
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        std::wcerr << L"GDI+ startup failed\n";
        return 2;
    }
    OleInitialize(nullptr);

    const std::filesystem::path outputDir = std::filesystem::current_path() / L"screenshots" / L"acceptance";
    std::filesystem::create_directories(outputDir);

    TestState state;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    HWND owner = CreateOwnerWindow(instance);
    AcceptanceLog(L"owner-created");
    Theme theme = Theme::Load(std::filesystem::current_path() / L"theme", L"default");
    AppModel model = SampleModel();
    AppConfig config;
    config.webDavEnabled = true;
    config.webDavUrl = L"https://dav.example.test/remote.php/dav/files/demo";
    config.webDavRemotePath = L"/Quattro/backups/";
    config.webDavUserName = L"acceptance-user";

    RunMainWindowScenario(outputDir, state);

    RunDialogScenario(
        Scenario{L"message-dialog", L"QuattroThemedMessageDialog", L"验收消息", L"message-dialog.png", {L"验收消息"}, {}, 0, 1, false},
        outputDir,
        state,
        [&]() {
            ShowThemedMessageBox(owner, instance, theme, L"确认删除验收项目？", L"验收消息", MB_OKCANCEL | MB_ICONINFORMATION);
        });

    const std::wstring webDavConfirmText =
        L"请确认要下载并合并以下 WebDAV 备份：\n\n"
        L"文件名:\n"
        L"quattro-backup-20260630-215427-with-a-ver\n"
        L"y-long-file-name.q4cfg\n"
        L"文件大小: 136 KB\n"
        L"备份时间: 2026年7月1日 01:54\n\n"
        L"将把该备份中的分组、标签、启动项、便签和待办合并到当前数据。\n"
        L"当前数据不会被覆盖，导入前会自动备份。";
    RunDialogScenario(
        Scenario{L"webdav-download-confirm", L"QuattroThemedMessageDialog", L"从云端下载", L"webdav-download-confirm.png", {L"从云端下载"}, {}, 0, 2, false},
        outputDir,
        state,
        [&]() {
            ShowThemedMessageBox(owner, instance, theme, webDavConfirmText, L"从云端下载", MB_OKCANCEL | MB_ICONINFORMATION);
        });

    RunDialogScenario(
        Scenario{L"update-confirm", L"QuattroUpdateCheckDialog", L"检查更新", L"update-confirm.png", {L"发现新版本", L"当前版本", L"v0.0.1", L"最新版本", L"v0.3.0", L"Quattro-x64.exe（12.0 MB）", L"下载更新", L"发布页", L"取消"}, {}, 0, 3, false},
        outputDir,
        state,
        [&]() {
            UpdateReleaseInfo info;
            info.updateAvailable = true;
            info.currentVersion = L"0.0.1";
            info.latestVersion = L"0.3.0";
            info.assetName = L"Quattro-x64.exe";
            info.assetSizeBytes = 12ull * 1024ull * 1024ull;
            info.releaseNotes = L"修复更新下载进度；统一检查更新窗口布局；优化公共主题组件。";
            ShowUpdateCheckDialog(owner, instance, theme, info);
        });

    RunDialogScenario(
        Scenario{L"confirm-dialog", L"QuattroConfirmDialog", L"取消下载", L"confirm-dialog.png", {L"更新包正在下载", L"取消下载", L"继续下载"}, {}, 0, 2, false},
        outputDir,
        state,
        [&]() {
            ShowConfirmDialog(
                owner,
                instance,
                theme,
                L"取消下载",
                L"更新包正在下载，确定要取消下载吗？",
                L"取消下载",
                L"继续下载");
        });

    std::wstring inputValue = L"初始输入值";
    RunDialogScenario(
        Scenario{L"text-input-dialog", L"", L"验收输入", L"text-input-dialog.png", {L"验收输入", L"确定", L"取消"}, {L"初始输入值"}, 1, 2, true},
        outputDir,
        state,
        [&]() {
            ShowTextInputDialog(owner, instance, theme, L"验收输入", L"名称", inputValue);
        });

    std::wstring newTagName = L"新的标签 2";
    RunDialogScenario(
        Scenario{L"new-tag-dialog", L"", L"新建标签", L"new-tag-dialog.png", {L"新建标签", L"标签名称", L"确定", L"取消"}, {L"新的标签 2"}, 1, 2, false},
        outputDir,
        state,
        [&]() {
            ShowTextInputDialog(owner, instance, theme, L"新建标签", L"标签名称", newTagName);
        });

    std::vector<WebDavRemoteFile> backups{
        {L"quattro-backup-20260630-215427-with-a-very-long-file-name.q4cfg", L"/remote/quattro-backup-20260630-215427-with-a-very-long-file-name.q4cfg", 139264, L"Tue, 30 Jun 2026 17:54:27 GMT", false},
        {L"quattro-backup-20260626-173428.q4cfg", L"/remote/quattro-backup-20260626-173428.q4cfg", 77824, L"Fri, 26 Jun 2026 09:34:11 GMT", false},
        {L"quattro-backup-20260626-171610.q4cfg", L"/remote/quattro-backup-20260626-171610.q4cfg", 77824, L"Fri, 26 Jun 2026 09:16:10 GMT", false}};
    std::wstring selectedBackup = backups.front().name;
    RunDialogScenario(
        Scenario{L"webdav-backup-selection", L"", L"选择 WebDAV 备份", L"webdav-backup-selection.png", {L"选择 WebDAV 备份", L"quattro-backup-20260630", L"136 KB", L"2026年7月1日 01:54", L"下载"}, {}, 0, 2, false},
        outputDir,
        state,
        [&]() {
            ShowWebDavBackupSelectionDialog(owner, instance, theme, backups, selectedBackup);
        });

    const std::vector<std::pair<std::wstring, std::vector<std::wstring>>> settingsPages{
        {L"显示", {L"界面元素", L"布局与外观", L"显示标题栏", L"透明度", L"标签文字", L"分组宽度"}},
        {L"行为", {L"窗口行为", L"运行与数据", L"系统集成", L"启动后隐藏", L"开机启动", L"启用日志"}},
        {L"右键菜单", {L"自动跟踪", L"自动跟踪 Git 右键菜单", L"自动跟踪 SVN 右键菜单", L"自动跟踪 VS Code 右键菜单", L"显示 CMD/PowerShell/WSL", L"自动跟踪压缩工具右键菜单", L"缓存维护", L"重置右键菜单"}},
        {L"交互", {L"启动操作", L"悬停激活", L"双击运行", L"分组激活延迟", L"标签激活延迟"}},
        {L"热键", {L"全局快捷键", L"启用全局快捷键", L"主窗口显隐", L"进程定位器"}},
        {L"链接", {L"目录命令", L"公共链接", L"打开目录命令", L"帮助链接", L"更新链接", L"FAQ 链接"}},
        {L"WebDAV", {L"WebDAV 备份", L"启用 WebDAV 备份", L"服务器地址", L"用户名", L"远端目录", L"测试连接", L"上传到云端"}},
        {L"HTTP", {L"服务配置", L"运行控制", L"高级配置", L"配置目录"}},
        {L"备份", {L"配置包", L"导出配置包", L"待办事项", L"含已完成"}},
    };
    for (const auto& [page, expected] : settingsPages) {
        Scenario settingsScenario{
            L"settings-dialog-" + page,
            L"QuattroSettingsDialog",
            L"设置",
            L"settings-dialog-" + page + L".png",
            {L"设置", L"确定", L"取消"},
            {},
            0,
            2,
            false,
            false,
            false,
            expected,
            page};
        settingsScenario.requireThemedEditFrames = page == L"HTTP";
        RunDialogScenario(
            settingsScenario,
            outputDir,
            state,
            [&]() {
                bool imported = false;
                const std::filesystem::path baseDirectory = std::filesystem::current_path();
                ShowSettingsDialog(owner, instance, config, theme, baseDirectory, baseDirectory, &imported);
            });
    }

    std::vector<Link> quickImportSelected;
    RunDialogScenario(
        Scenario{
            L"quick-import-dialog",
            L"QuattroQuickImportDialog",
            L"快速导入",
            L"quick-import-dialog.png",
            {L"快速导入", L"来源", L"选择目录", L"扫描", L"全选", L"清空", L"导入选中", L"取消"},
            {},
            0,
            5,
            false,
            true,
            true},
        outputDir,
        state,
        [&]() {
            QuickImportDialog::Show(owner, instance, theme, model.links, quickImportSelected);
        });

    Link link;
    link.name = L"验收启动项";
    link.path = L"C:\\Windows\\notepad.exe";
    link.parameter = L"acceptance.txt";
    link.workDir = L"C:\\Windows";
    link.remark = L"启动项备注";
    RunDialogScenario(
        Scenario{L"link-edit-dialog", L"QuattroLinkEditDialog", L"添加启动项", L"link-edit-dialog.png", {L"添加启动项", L"确定", L"取消"}, {L"验收启动项", L"C:\\Windows\\notepad.exe", L"启动项备注"}, 5, 2, false},
        outputDir,
        state,
        [&]() {
            LinkEditDialog::Show(owner, instance, theme, link, model.groups, true);
        });

    Link url;
    url.name = L"验收网址";
    url.path = L"https://example.com/acceptance";
    url.remark = L"网址备注";
    RunDialogScenario(
        Scenario{L"url-edit-dialog", L"QuattroUrlEditDialog", L"添加网址", L"url-edit-dialog.png", {L"添加网址", L"确定", L"取消"}, {L"验收网址", L"https://example.com/acceptance", L"网址备注"}, 3, 2, false},
        outputDir,
        state,
        [&]() {
            UrlEditDialog::Show(owner, instance, theme, url, true);
        });

    TodoItem todo;
    todo.title = L"验收待办标题";
    todo.content = L"验收待办正文";
    RunDialogScenario(
        Scenario{L"todo-edit-dialog", L"QuattroTodoEditDialog", L"新建待办", L"todo-edit-dialog.png", {L"新建待办", L"保存待办", L"取消"}, {L"验收待办标题", L"验收待办正文"}, 2, 2, false},
        outputDir,
        state,
        [&]() {
            TodoEditDialog::Show(owner, instance, theme, todo, true);
        });

    PluginRegistry registry(std::filesystem::current_path());
    AppConfig builtinToolConfig;
    RunDialogScenario(
        Scenario{L"builtin-clicker", L"", L"连点器", L"builtin-clicker.png", {L"连点器", L"启动(&S)"}, {L"0, 0", L"10", L"1000"}, 4, 2, false},
        outputDir,
        state,
        [&]() {
            ShowBuiltinTool(owner, instance, theme, registry, builtinToolConfig, L"clicker");
        });
    RunDialogScenario(
        Scenario{L"builtin-timer", L"", L"计时器", L"builtin-timer.png", {L"计时器", L"开始(&S)", L"暂停(&P)", L"重置(&R)"}, {}, 3, 3, false},
        outputDir,
        state,
        [&]() {
            ShowBuiltinTool(owner, instance, theme, registry, builtinToolConfig, L"timer");
        });
    RunDialogScenario(
        Scenario{L"builtin-stopwatch", L"", L"秒表", L"builtin-stopwatch.png", {L"秒表", L"开始(&S)", L"暂停(&P)", L"导出(&E)"}, {}, 0, 5, false},
        outputDir,
        state,
        [&]() {
            ShowBuiltinTool(owner, instance, theme, registry, builtinToolConfig, L"stopwatch");
        });
    RunDialogScenario(
        Scenario{L"builtin-port-inspector", L"", L"端口占用检查", L"builtin-port-inspector.png", {L"端口占用检查", L"扫描(&S)", L"输入端口号后点击扫描。"}, {}, 1, 1, false},
        outputDir,
        state,
        [&]() {
            ShowBuiltinTool(owner, instance, theme, registry, builtinToolConfig, L"port-inspector");
        });
    RunDialogScenario(
        Scenario{L"builtin-process-inspector", L"", L"进程ID查询", L"builtin-process-inspector.png", {L"进程ID查询", L"查询(&Q)", L"输入进程ID后点击查询。"}, {}, 1, 1, false},
        outputDir,
        state,
        [&]() {
            ShowBuiltinTool(owner, instance, theme, registry, builtinToolConfig, L"process-inspector");
        });
    RunDialogScenario(
        Scenario{
            L"builtin-process-locator",
            L"",
            L"进程定位器",
            L"builtin-process-locator.png",
            {L"进程定位器", L"结束进程", L"打开所在目录", L"立即获取", L"进程 ID", L"绝对路径", L"全局快捷键"},
            {L"等待获取"},
            2,
            3,
            false},
        outputDir,
        state,
        [&]() {
            ShowBuiltinTool(owner, instance, theme, registry, builtinToolConfig, L"process-locator");
        });

    DestroyWindow(owner);
    OleUninitialize();
    Gdiplus::GdiplusShutdown(gdiplusToken);

    if (!state.ok) {
        AcceptanceLog(L"failed");
        std::ofstream failureLog("ui-acceptance-failures.txt", std::ios::binary);
        for (const auto& failure : state.failures) {
            std::wcerr << failure << L"\n";
            std::cerr << NarrowDiagnostic(failure) << "\n";
            failureLog << NarrowDiagnostic(failure) << "\n";
        }
        return 1;
    }
    AcceptanceLog(L"passed");
    std::wcout << L"ui_screenshot_acceptance=passed screenshots=" << outputDir.wstring() << L"\n";
    return 0;
}
