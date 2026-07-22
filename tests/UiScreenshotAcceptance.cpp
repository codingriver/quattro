#include "../src/windows/BuiltinTools.h"
#include "../src/windows/LinkEditDialog.h"
#include "../src/windows/MainWindow.h"
#include "../src/domain/MenuCatalog.h"
#include "../src/domain/Models.h"
#include "../src/domain/TodoSchedule.h"
#include "../src/windows/QuickImportDialog.h"
#include "../src/windows/SimpleDialogs.h"
#include "../src/windows/ConfirmDialog.h"
#include "../src/windows/UpdateCheckDialog.h"
#include "../src/theme/Theme.h"
#include "../src/theme/ThemedD2D.h"
#include "../src/theme/ThemedUi.h"
#include "../src/theme/ThemedWindowUi.h"
#include "../src/theme/ThemedFileTransferQueueDialog.h"
#include "../src/common/Utilities.h"
#include "../src/windows/TodoEditDialog.h"
#include "../src/windows/UrlEditDialog.h"
#include "../src/services/WebDavClient.h"
#include "../src/services/WebDavFileIndexCache.h"
#include "../src/services/ShellItemService.h"
#include "../src/services/Storage.h"
#include "../src/domain/Config.h"
#include "../src/domain/PluginRegistry.h"
#include "Version.h"

#include <commctrl.h>
#include <gdiplus.h>
#include <shlobj.h>
#include <windows.h>

#ifndef HDS_NOSIZING
#define HDS_NOSIZING 0x0800
#endif

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

std::filesystem::path gAcceptanceOutputDir;

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
    std::vector<std::wstring> unexpectedVisibleChildTexts;
    std::wstring actionButtonText;
    std::vector<std::wstring> actionButtonTexts;
    DWORD closeDelayMs = 0;
    bool validateHotKeyTableLayout = false;
    bool validateContextMenuUninstalledRows = false;
    UINT forcedDpi = USER_DEFAULT_SCREEN_DPI;
    bool validateProcessToolsTableDpi = false;
    bool validateQuickImportIconLayout = false;
    bool waitForContextMenuIconLoad = true;
    bool validateContextMenuIconTransparency = false;
    std::wstring expectedContextMenuProvider;
    std::wstring expectedContextMenuStatus;
    int splitButtonMenuId = 0;
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

struct TableMutationProbe {
    int deleteAllCount = 0;
    int redrawSuspendCount = 0;
};

LRESULT CALLBACK TableMutationProbeProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR refData) {
    auto* probe = reinterpret_cast<TableMutationProbe*>(refData);
    if (probe) {
        if (message == LVM_DELETEALLITEMS) ++probe->deleteAllCount;
        if (message == WM_SETREDRAW && !wParam) ++probe->redrawSuspendCount;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, TableMutationProbeProc, id);
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

std::vector<ContextMenuProviderIconInfo> AcceptanceContextMenuProviderIcons(std::stop_token stopToken) {
    const auto providers = TrackedContextMenuProviders();
    std::vector<ContextMenuProviderIconInfo> result;
    result.reserve(providers.size());
    for (std::size_t index = 0; index < providers.size() && !stopToken.stop_requested(); ++index) {
        ContextMenuProviderIconInfo info;
        info.providerId = providers[index].providerId;
        info.installed = index + 2 < providers.size();
        info.installedViaProbe = info.installed;
        info.attempted = info.installed;
        if (info.installed) {
            info.icon.width = 32;
            info.icon.height = 32;
            info.icon.quality = 1;
            const std::uint32_t red = static_cast<std::uint32_t>(48 + (index * 37) % 176);
            const std::uint32_t green = static_cast<std::uint32_t>(64 + (index * 53) % 160);
            const std::uint32_t blue = static_cast<std::uint32_t>(80 + (index * 29) % 144);
            const std::uint32_t color = 0xFF000000u | (red << 16) | (green << 8) | blue;
            info.icon.pixels.assign(32 * 32, 0);
            const std::uint32_t translucentColor =
                0x80000000u | ((red / 2) << 16) | ((green / 2) << 8) | (blue / 2);
            for (int y = 4; y < 28; ++y) {
                for (int x = 4; x < 28; ++x) {
                    const bool opaque = x >= 8 && x < 24 && y >= 8 && y < 24;
                    info.icon.pixels[static_cast<std::size_t>(y) * 32 + x] =
                        opaque ? color : translucentColor;
                }
            }
        }
        result.push_back(std::move(info));
    }
    return result;
}

std::vector<ContextMenuProviderIconInfo> AcceptanceSystemContextMenuProviderIcons(
    std::stop_token stopToken) {
    std::vector<ContextMenuProviderIconInfo> result =
        AcceptanceContextMenuProviderIcons(stopToken);
    wchar_t windowsDirectory[MAX_PATH]{};
    if (GetWindowsDirectoryW(windowsDirectory, static_cast<UINT>(std::size(windowsDirectory))) == 0) {
        return result;
    }
    const std::filesystem::path windows = windowsDirectory;
    const std::vector<std::filesystem::path> executables{
        windows / L"System32" / L"cmd.exe",
        windows / L"System32" / L"notepad.exe",
        windows / L"regedit.exe",
        windows / L"System32" / L"taskmgr.exe",
        windows / L"System32" / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe",
    };
    for (std::size_t index = 0; index < result.size() && !stopToken.stop_requested(); ++index) {
        if (!result[index].installed) continue;
        ShellContextMenuItem item;
        TrackedProviderIconSource source = TrackedProviderIconSource::None;
        const auto providers = TrackedContextMenuProviders();
        bool loaded = index < providers.size() &&
            providers[index].providerId != ShellContextMenuProviderId::Terminal &&
            ShellItemService::LoadTrackedProviderIcon(providers[index], item, &source);
        if (!loaded) {
            const std::filesystem::path& executable = executables[index % executables.size()];
            loaded = ShellItemService::LoadExecutableMenuIcon(executable.wstring(), item);
        }
        if (!loaded) continue;
        result[index].icon.width = item.iconWidth;
        result[index].icon.height = item.iconHeight;
        result[index].icon.quality = item.iconQuality;
        result[index].icon.pixels = std::move(item.iconPixels);
    }
    return result;
}

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

bool ListViewContainsText(HWND listView, const std::wstring& expected) {
    const int count = static_cast<int>(SendMessageW(listView, LVM_GETITEMCOUNT, 0, 0));
    for (int row = 0; row < count && row < 20; ++row) {
        for (int column = 0; column < 8; ++column) {
            wchar_t buffer[512]{};
            LVITEMW item{};
            item.iSubItem = column;
            item.cchTextMax = static_cast<int>(std::size(buffer));
            item.pszText = buffer;
            SendMessageW(listView, LVM_GETITEMTEXTW, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&item));
            if (buffer[0] != L'\0' && Contains(buffer, expected)) {
                return true;
            }
        }
    }
    return false;
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
    const std::filesystem::path directory = gAcceptanceOutputDir.empty()
        ? std::filesystem::current_path() / L"screenshots" / L"acceptance"
        : gAcceptanceOutputDir;
    std::filesystem::create_directories(directory);
    std::ofstream log(directory / L"ui-acceptance-progress.txt", std::ios::binary | std::ios::app);
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

int CountTopWindowsForProcess(const std::wstring& title, DWORD processId) {
    struct CountData {
        const std::wstring* title = nullptr;
        DWORD processId = 0;
        int count = 0;
    } data{&title, processId, 0};
    EnumWindows([](HWND hwnd, LPARAM value) -> BOOL {
        auto* count = reinterpret_cast<CountData*>(value);
        if (!IsWindowVisible(hwnd)) {
            return TRUE;
        }
        DWORD windowProcessId = 0;
        GetWindowThreadProcessId(hwnd, &windowProcessId);
        if (windowProcessId == count->processId && WindowText(hwnd) == *count->title) {
            ++count->count;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&data));
    return data.count;
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

    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(
        hwnd,
        HWND_BOTTOM,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    Sleep(120);
    BOOL printed = PrintWindow(hwnd, dc, 0x00000002);
    if (!printed) {
        SelectObject(dc, old);
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        DeleteObject(bitmap);
        return BitmapCapture{};
    }

    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return BitmapCapture{bitmap, width, height};
}

BitmapCapture CaptureControlBitmap(HWND hwnd) {
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int height = std::max(1, static_cast<int>(rect.bottom - rect.top));
    HDC screen = GetDC(nullptr);
    HDC dc = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = screen ? CreateCompatibleBitmap(screen, width, height) : nullptr;
    HGDIOBJ old = dc && bitmap ? SelectObject(dc, bitmap) : nullptr;
    if (!screen || !dc || !bitmap || !old) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        if (screen) ReleaseDC(nullptr, screen);
        return {};
    }
    HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
    RECT fill{0, 0, width, height};
    FillRect(dc, &fill, magenta);
    DeleteObject(magenta);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    const BOOL printed = PrintWindow(hwnd, dc, 0x00000002);
    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    if (!printed) {
        DeleteObject(bitmap);
        return {};
    }
    return BitmapCapture{bitmap, width, height};
}

BitmapCapture CaptureClientBitmapWithChildren(HWND hwnd) {
    RECT client{};
    if (!GetClientRect(hwnd, &client)) return {};
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int height = std::max(1, static_cast<int>(client.bottom - client.top));
    HDC screen = GetDC(nullptr);
    HDC dc = screen ? CreateCompatibleDC(screen) : nullptr;
    HBITMAP bitmap = screen ? CreateCompatibleBitmap(screen, width, height) : nullptr;
    HGDIOBJ old = dc && bitmap ? SelectObject(dc, bitmap) : nullptr;
    if (!screen || !dc || !bitmap || !old) {
        if (dc) DeleteDC(dc);
        if (screen) ReleaseDC(nullptr, screen);
        if (bitmap) DeleteObject(bitmap);
        return {};
    }
    HBRUSH background = CreateSolidBrush(RGB(255, 0, 255));
    FillRect(dc, &client, background);
    DeleteObject(background);
    SendMessageW(hwnd, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);

    std::vector<HWND> children;
    for (HWND child = GetTopWindow(hwnd); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (IsWindowVisible(child)) children.push_back(child);
    }
    std::reverse(children.begin(), children.end());
    for (HWND child : children) {
        RECT childRect{};
        if (!GetWindowRect(child, &childRect)) continue;
        MapWindowPoints(HWND_DESKTOP, hwnd, reinterpret_cast<POINT*>(&childRect), 2);
        const int childWidth = childRect.right - childRect.left;
        const int childHeight = childRect.bottom - childRect.top;
        if (childWidth <= 0 || childHeight <= 0) continue;
        HDC childDc = CreateCompatibleDC(screen);
        HBITMAP childBitmap = CreateCompatibleBitmap(screen, childWidth, childHeight);
        HGDIOBJ childOld = childDc && childBitmap ? SelectObject(childDc, childBitmap) : nullptr;
        if (childDc && childBitmap && childOld) {
            COLORREF childBackground = GetPixel(
                dc,
                std::clamp(childRect.left, 0L, static_cast<LONG>(width - 1)),
                std::clamp(childRect.top, 0L, static_cast<LONG>(height - 1)));
            if (childBackground == CLR_INVALID) childBackground = RGB(255, 255, 255);
            HBRUSH childBrush = CreateSolidBrush(childBackground);
            RECT childFill{0, 0, childWidth, childHeight};
            FillRect(childDc, &childFill, childBrush);
            DeleteObject(childBrush);
        }
        BOOL childDrawn = FALSE;
        wchar_t childClass[64]{};
        GetClassNameW(child, childClass, static_cast<int>(std::size(childClass)));
        if (childDc && childBitmap && childOld && wcscmp(childClass, L"Button") == 0) {
            DRAWITEMSTRUCT draw{};
            draw.CtlType = ODT_BUTTON;
            draw.CtlID = static_cast<UINT>(GetDlgCtrlID(child));
            draw.itemAction = ODA_DRAWENTIRE;
            draw.itemState = IsWindowEnabled(child) ? 0 : ODS_DISABLED;
            if (GetFocus() == child) draw.itemState |= ODS_FOCUS;
            draw.hwndItem = child;
            draw.hDC = childDc;
            draw.rcItem = RECT{0, 0, childWidth, childHeight};
            childDrawn = SendMessageW(
                hwnd, WM_DRAWITEM, static_cast<WPARAM>(draw.CtlID),
                reinterpret_cast<LPARAM>(&draw)) != 0;
        } else if (childDc && childBitmap && childOld) {
            childDrawn = PrintWindow(child, childDc, 0x00000002);
        }
        if (childDrawn) {
            BitBlt(dc, childRect.left, childRect.top, childWidth, childHeight, childDc, 0, 0, SRCCOPY);
        }
        if (childDc && childOld) SelectObject(childDc, childOld);
        if (childBitmap) DeleteObject(childBitmap);
        if (childDc) DeleteDC(childDc);
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

int LargestLowDetailHorizontalRun(HBITMAP bitmap, int width, int height) {
    if (!bitmap || width < 8 || height < 8) return height;
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
    if (lines == 0) return height;

    int largest = 0;
    int current = 0;
    for (int y = 2; y < height - 2; ++y) {
        std::set<std::uint32_t> colors;
        const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
        for (int x = 2; x < width - 2; x += 2) {
            colors.insert(pixels[row + static_cast<std::size_t>(x)] & 0x00ffffffu);
            if (colors.size() > 2) break;
        }
        if (colors.size() <= 2) {
            largest = std::max(largest, ++current);
        } else {
            current = 0;
        }
    }
    return largest;
}

std::size_t DistinctToolMenuIconCount(HBITMAP bitmap, int width, int height, UINT dpi) {
    if (!bitmap || width < 40 || height < 40) return 0;
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
    if (lines == 0) return 0;

    const int rowHeight = std::max(1, MulDiv(28, static_cast<int>(dpi), 96));
    const int rowCount = std::clamp((height - 2) / rowHeight, 0, 16);
    const int topInset = std::max(0, (height - rowCount * rowHeight) / 2);
    const int iconLeft = std::max(2, MulDiv(5, static_cast<int>(dpi), 96));
    const int iconRight = std::min(width - 2, MulDiv(30, static_cast<int>(dpi), 96));
    std::set<std::uint64_t> fingerprints;
    for (int item = 0; item < rowCount; ++item) {
        const int top = std::clamp(topInset + item * rowHeight, 0, height);
        const int bottom = std::clamp(top + rowHeight, 0, height);
        std::uint64_t fingerprint = 1469598103934665603ull;
        for (int y = top; y < bottom; ++y) {
            const std::size_t row = static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
            for (int x = iconLeft; x < iconRight; ++x) {
                fingerprint ^= pixels[row + static_cast<std::size_t>(x)] & 0x00ffffffu;
                fingerprint *= 1099511628211ull;
            }
        }
        fingerprints.insert(fingerprint);
    }
    return fingerprints.size();
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

COLORREF BitmapPixel(HBITMAP bitmap, int x, int y) {
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, bitmap);
    const COLORREF color = GetPixel(dc, x, y);
    SelectObject(dc, old);
    DeleteDC(dc);
    return color;
}

COLORREF AverageBitmapRowColor(HBITMAP bitmap, int width, RECT row) {
    const int rowLeft = static_cast<int>(row.left);
    const int rowTop = static_cast<int>(row.top);
    const int rowRight = static_cast<int>(row.right);
    const int rowBottom = static_cast<int>(row.bottom);
    const int maxX = std::max(0, width - 1);
    const int maxY = std::max(0, rowBottom - 1);
    const int y = std::clamp((rowTop + rowBottom) / 2, 0, maxY);
    const int left = std::clamp(rowLeft + std::max(8, (rowRight - rowLeft) / 3), 0, maxX);
    const int right = std::clamp(rowRight - 12, left, maxX);
    int r = 0;
    int g = 0;
    int b = 0;
    int samples = 0;
    for (int i = 0; i < 9; ++i) {
        const int x = left + ((right - left) * i) / 8;
        const COLORREF color = BitmapPixel(bitmap, x, y);
        if (color == CLR_INVALID) {
            continue;
        }
        r += GetRValue(color);
        g += GetGValue(color);
        b += GetBValue(color);
        ++samples;
    }
    if (samples == 0) {
        return CLR_INVALID;
    }
    return RGB(r / samples, g / samples, b / samples);
}

int ColorDistance(COLORREF a, COLORREF b) {
    if (a == CLR_INVALID || b == CLR_INVALID) {
        return 0;
    }
    return std::abs(static_cast<int>(GetRValue(a)) - static_cast<int>(GetRValue(b))) +
        std::abs(static_cast<int>(GetGValue(a)) - static_cast<int>(GetGValue(b))) +
        std::abs(static_cast<int>(GetBValue(a)) - static_cast<int>(GetBValue(b)));
}

std::size_t CountChangedPixelSamples(
    const BitmapCapture& first,
    const BitmapCapture& second,
    int minimumColorDistance = 8,
    int sampleStep = 2) {
    if (!first.bitmap || !second.bitmap || first.width != second.width || first.height != second.height) {
        return 0;
    }
    HDC firstDc = CreateCompatibleDC(nullptr);
    HDC secondDc = CreateCompatibleDC(nullptr);
    HGDIOBJ oldFirst = SelectObject(firstDc, first.bitmap);
    HGDIOBJ oldSecond = SelectObject(secondDc, second.bitmap);
    std::size_t changed = 0;
    for (int y = 0; y < first.height; y += std::max(1, sampleStep)) {
        for (int x = 0; x < first.width; x += std::max(1, sampleStep)) {
            if (ColorDistance(GetPixel(firstDc, x, y), GetPixel(secondDc, x, y)) >= minimumColorDistance) {
                ++changed;
            }
        }
    }
    SelectObject(firstDc, oldFirst);
    SelectObject(secondDc, oldSecond);
    DeleteDC(firstDc);
    DeleteDC(secondDc);
    return changed;
}

std::size_t CountPixelsNearColor(
    HBITMAP bitmap,
    int width,
    int height,
    RECT area,
    COLORREF target,
    int maxDistance) {
    area.left = std::max(0L, area.left);
    area.top = std::max(0L, area.top);
    area.right = std::min(static_cast<LONG>(width), area.right);
    area.bottom = std::min(static_cast<LONG>(height), area.bottom);
    std::size_t count = 0;
    for (int y = area.top; y < area.bottom; ++y) {
        for (int x = area.left; x < area.right; ++x) {
            if (ColorDistance(BitmapPixel(bitmap, x, y), target) <= maxDistance) {
                ++count;
            }
        }
    }
    return count;
}

void ValidateSplitButtonChevron(
    HWND button,
    const Theme& theme,
    UINT dpi,
    TestState& state,
    const std::wstring& scenario,
    bool verifySemanticState) {
    state.Check(button != nullptr, scenario + L": split-button menu segment is missing");
    if (!button) {
        return;
    }
    if (verifySemanticState) {
        TablerIconManifest::Id icon{};
        state.Check(
            ThemedControls::ButtonTablerIcon(button, icon) && icon == TablerIconId::ChevronDown,
            scenario + L": split-button menu segment lost its typed ChevronDown state");
        state.Check(
            SendMessageW(button, BM_GETIMAGE, IMAGE_ICON, 0) == 0,
            scenario + L": split-button menu segment still uses an intermediate HICON");
    }

    BitmapCapture capture = CaptureControlBitmap(button);
    state.Check(capture.bitmap != nullptr, scenario + L": split-button icon capture failed");
    if (!capture.bitmap) {
        return;
    }
    const Color icon = theme.color(L"iconButton", L"normal", L"icon");
    const COLORREF iconColor = RGB(
        static_cast<BYTE>(std::clamp(icon.r, 0.0f, 1.0f) * 255.0f + 0.5f),
        static_cast<BYTE>(std::clamp(icon.g, 0.0f, 1.0f) * 255.0f + 0.5f),
        static_cast<BYTE>(std::clamp(icon.b, 0.0f, 1.0f) * 255.0f + 0.5f));
    const RECT probe{
        capture.width / 5,
        capture.height / 5,
        capture.width - capture.width / 5,
        capture.height - capture.height / 5};
    HDC dc = CreateCompatibleDC(nullptr);
    HGDIOBJ old = SelectObject(dc, capture.bitmap);
    int minX = capture.width;
    int minY = capture.height;
    int maxX = -1;
    int maxY = -1;
    std::size_t pixels = 0;
    for (int y = probe.top; y < probe.bottom; ++y) {
        for (int x = probe.left; x < probe.right; ++x) {
            if (ColorDistance(GetPixel(dc, x, y), iconColor) <= 96) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
                ++pixels;
            }
        }
    }
    SelectObject(dc, old);
    DeleteDC(dc);
    const int glyphWidth = maxX >= minX ? maxX - minX + 1 : 0;
    const int glyphHeight = maxY >= minY ? maxY - minY + 1 : 0;
    state.Check(
        pixels >= static_cast<std::size_t>(std::max(8, MulDiv(8, static_cast<int>(dpi), 96))),
        scenario + L": split-button chevron has too few visible pixels");
    state.Check(
        glyphWidth >= MulDiv(8, static_cast<int>(dpi), 96) &&
            glyphHeight >= MulDiv(3, static_cast<int>(dpi), 96) &&
            glyphWidth > glyphHeight,
        scenario + L": split-button chevron is too small or resembles a square fallback glyph");
    DeleteObject(capture.bitmap);
}

struct ChildInfo {
    HWND hwnd = nullptr;
    int id = 0;
    std::wstring className;
    std::wstring text;
};

BOOL CALLBACK EnumChildProc(HWND hwnd, LPARAM lParam) {
    auto* children = reinterpret_cast<std::vector<ChildInfo>*>(lParam);
    children->push_back(ChildInfo{hwnd, GetDlgCtrlID(hwnd), ClassName(hwnd), WindowText(hwnd)});
    return TRUE;
}

std::vector<ChildInfo> Children(HWND hwnd) {
    std::vector<ChildInfo> children;
    EnumChildWindows(hwnd, EnumChildProc, reinterpret_cast<LPARAM>(&children));
    return children;
}

HWND ChildById(HWND hwnd, int id) {
    const auto children = Children(hwnd);
    const auto found = std::find_if(children.begin(), children.end(), [id](const ChildInfo& child) {
        return child.id == id;
    });
    return found == children.end() ? nullptr : found->hwnd;
}

HWND VisibleButtonByText(HWND hwnd, const std::wstring& text) {
    const auto children = Children(hwnd);
    const auto found = std::find_if(children.begin(), children.end(), [&](const ChildInfo& child) {
        return IsWindowVisible(child.hwnd) && child.className == L"Button" && child.text == text;
    });
    return found == children.end() ? nullptr : found->hwnd;
}

bool WindowContainsText(HWND hwnd, const std::wstring& expected) {
    if (Contains(WindowText(hwnd), expected)) {
        return true;
    }
    for (const ChildInfo& child : Children(hwnd)) {
        if (IsWindowVisible(child.hwnd) && Contains(WindowText(child.hwnd), expected)) {
            return true;
        }
    }
    return false;
}

bool WaitForWindowText(HWND hwnd, const std::wstring& expected, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (IsWindow(hwnd) && GetTickCount64() < deadline) {
        if (WindowContainsText(hwnd, expected)) {
            return true;
        }
        Sleep(20);
    }
    return IsWindow(hwnd) && WindowContainsText(hwnd, expected);
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
    if (scenario.forcedDpi != USER_DEFAULT_SCREEN_DPI) {
        RECT windowRect{};
        GetWindowRect(hwnd, &windowRect);
        const int width = windowRect.right - windowRect.left;
        const int height = windowRect.bottom - windowRect.top;
        RECT suggested{
            windowRect.left,
            windowRect.top,
            windowRect.left + MulDiv(width, static_cast<int>(scenario.forcedDpi), USER_DEFAULT_SCREEN_DPI),
            windowRect.top + MulDiv(height, static_cast<int>(scenario.forcedDpi), USER_DEFAULT_SCREEN_DPI),
        };
        SendMessageW(
            hwnd,
            WM_DPICHANGED,
            MAKEWPARAM(scenario.forcedDpi, scenario.forcedDpi),
            reinterpret_cast<LPARAM>(&suggested));
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        Sleep(120);
    }

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
            if (scenario.activateButtonText == L"右键菜单" && scenario.waitForContextMenuIconLoad) {
                for (int elapsed = 0; elapsed < 5000; elapsed += 20) {
                    bool loading = false;
                    for (const auto& child : Children(hwnd)) {
                        if (!IsWindowVisible(child.hwnd) || child.className != L"SysListView32") continue;
                        const int rowCount = ListView_GetItemCount(child.hwnd);
                        for (int row = 0; row < rowCount; ++row) {
                            wchar_t status[64]{};
                            ListView_GetItemText(child.hwnd, row, 1, status, static_cast<int>(std::size(status)));
                            loading = loading || std::wstring(status) == L"检测中...";
                        }
                    }
                    if (!loading) break;
                    Sleep(20);
                }
            } else if (scenario.activateButtonText == L"右键菜单") {
                Sleep(80);
            }
            children = Children(hwnd);
        }
    }
    std::vector<std::wstring> actionButtonTexts = scenario.actionButtonTexts;
    if (!scenario.actionButtonText.empty()) {
        actionButtonTexts.push_back(scenario.actionButtonText);
    }
    for (const auto& actionButtonText : actionButtonTexts) {
        auto button = std::find_if(children.begin(), children.end(), [&](const ChildInfo& child) {
            return IsWindowVisible(child.hwnd) && child.className == L"Button" && child.text == actionButtonText;
        });
        state.Check(button != children.end(), scenario.name + L": action button not found: " + actionButtonText);
        if (button != children.end()) {
            SendMessageW(button->hwnd, BM_CLICK, 0, 0);
            if (actionButtonText == L"扫描") {
                WaitForWindowText(hwnd, L"扫描到", 7000);
            }
            Sleep(80);
            RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            children = Children(hwnd);
        }
    }
    ValidateChildBounds(hwnd, children, state, scenario.name);

    if (scenario.validateProcessToolsTableDpi) {
        HWND table = nullptr;
        for (const auto& child : children) {
            if (IsWindowVisible(child.hwnd) && child.className == L"SysListView32") {
                table = child.hwnd;
                break;
            }
        }
        state.Check(table != nullptr, scenario.name + L": visible process table not found");
        if (table) {
            RECT tableClient{};
            GetClientRect(table, &tableClient);
            const int columnCount = Header_GetItemCount(ListView_GetHeader(table));
            state.Check(columnCount == 4, scenario.name + L": process table column count mismatch");
            if (columnCount == 4) {
                int totalWidth = 0;
                for (int column = 0; column < columnCount; ++column) {
                    totalWidth += ListView_GetColumnWidth(table, column);
                }
                const int pidWidth = ListView_GetColumnWidth(table, 1);
                const int actionWidth = ListView_GetColumnWidth(table, 3);
                state.Check(
                    totalWidth <= tableClient.right - tableClient.left + 1,
                    scenario.name + L": process table columns overflow horizontally");
                state.Check(
                    pidWidth >= MulDiv(52, static_cast<int>(scenario.forcedDpi), USER_DEFAULT_SCREEN_DPI),
                    scenario.name + L": PID column did not scale with DPI");
                state.Check(
                    actionWidth >= MulDiv(72, static_cast<int>(scenario.forcedDpi), USER_DEFAULT_SCREEN_DPI),
                    scenario.name + L": action column did not scale with DPI");
            }
        }
    }

    if (scenario.validateQuickImportIconLayout) {
        HWND table = nullptr;
        for (const auto& child : children) {
            if (IsWindowVisible(child.hwnd) && child.className == L"SysListView32") {
                table = child.hwnd;
                break;
            }
        }
        state.Check(table != nullptr, scenario.name + L": quick import table not found");
        if (table) {
            const int view = static_cast<int>(SendMessageW(table, LVM_GETVIEW, 0, 0));
            const int rowCount = ListView_GetItemCount(table);
            const DWORD spacing = static_cast<DWORD>(SendMessageW(table, LVM_GETITEMSPACING, FALSE, 0));
            const int spacingX = LOWORD(spacing);
            const int spacingY = HIWORD(spacing);
            state.Check(view == LV_VIEW_ICON, scenario.name + L": quick import table did not switch to icon view");
            state.Check(rowCount > 0, scenario.name + L": quick import icon view has no scanned rows");
            state.Check(
                spacingX >= MulDiv(120, static_cast<int>(scenario.forcedDpi), USER_DEFAULT_SCREEN_DPI),
                scenario.name + L": quick import icon spacing X is too narrow");
            state.Check(
                spacingY >= MulDiv(104, static_cast<int>(scenario.forcedDpi), USER_DEFAULT_SCREEN_DPI),
                scenario.name + L": quick import icon spacing Y is too short");
        }
    }

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
        if (!found) {
            for (const auto& child : children) {
                if (IsWindowVisible(child.hwnd) &&
                    child.className == L"SysListView32" &&
                    ListViewContainsText(child.hwnd, expected)) {
                    found = true;
                    break;
                }
            }
        }
        state.Check(found, scenario.name + L": expected visible child text not found: " + expected);
    }

    for (const auto& unexpected : scenario.unexpectedVisibleChildTexts) {
        bool found = false;
        for (const auto& child : children) {
            if (IsWindowVisible(child.hwnd) && Contains(child.text, unexpected)) {
                found = true;
                break;
            }
        }
        state.Check(!found, scenario.name + L": unexpected visible child text found: " + unexpected);
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

    if (scenario.validateHotKeyTableLayout) {
        HWND table = nullptr;
        for (const auto& child : children) {
            if (IsWindowVisible(child.hwnd) && child.className == L"SysListView32") {
                table = child.hwnd;
                break;
            }
        }
        state.Check(table != nullptr, scenario.name + L": visible hotkey table not found");
        if (table) {
            RECT tableClient{};
            GetClientRect(table, &tableClient);
            const int columnCount = Header_GetItemCount(ListView_GetHeader(table));
            int columnsWidth = 0;
            for (int column = 0; column < columnCount; ++column) {
                columnsWidth += ListView_GetColumnWidth(table, column);
            }
            state.Check(columnCount == 3, scenario.name + L": hotkey table column count mismatch");
            state.Check(
                columnsWidth <= tableClient.right - tableClient.left,
                scenario.name + L": hotkey table columns overflow horizontally");
            if (columnCount == 3) {
                const int functionWidth = ListView_GetColumnWidth(table, 0);
                const int hotKeyWidth = ListView_GetColumnWidth(table, 1);
                const int actionWidth = ListView_GetColumnWidth(table, 2);
                state.Check(
                    functionWidth > hotKeyWidth && hotKeyWidth > actionWidth,
                    scenario.name + L": hotkey table column proportions are not compact");
            }
            const int rowCount = ListView_GetItemCount(table);
            state.Check(rowCount == 3, scenario.name + L": hotkey table row count mismatch");
            if (rowCount > 0) {
                RECT lastRow{};
                lastRow.left = LVIR_BOUNDS;
                const bool hasLastRowRect = SendMessageW(
                    table,
                    LVM_GETITEMRECT,
                    static_cast<WPARAM>(rowCount - 1),
                    reinterpret_cast<LPARAM>(&lastRow)) != FALSE;
                state.Check(hasLastRowRect, scenario.name + L": hotkey table last row bounds unavailable");
                state.Check(
                    hasLastRowRect && lastRow.bottom <= tableClient.bottom,
                    scenario.name + L": hotkey table rows require vertical scrolling");
            }
        }
    }

    if (scenario.validateContextMenuUninstalledRows) {
        HWND table = nullptr;
        for (const auto& child : children) {
            if (IsWindowVisible(child.hwnd) && child.className == L"SysListView32") {
                table = child.hwnd;
                break;
            }
        }
        state.Check(table != nullptr, scenario.name + L": visible context-menu table not found");
        if (table) {
            state.Check(
                ListView_GetTopIndex(table) == 0,
                scenario.name + L": context-menu table did not keep the initial viewport at the first row");
            int uninstalledRow = -1;
            const int rowCount = ListView_GetItemCount(table);
            HIMAGELIST images = ListView_GetImageList(table, LVSIL_SMALL);
            state.Check(images != nullptr && ImageList_GetImageCount(images) > 1,
                scenario.name + L": context-menu provider image list was not populated");
            bool foundRealProviderIcon = false;
            for (int row = 0; row < rowCount; ++row) {
                LVITEMW item{};
                item.mask = LVIF_IMAGE;
                item.iItem = row;
                if (ListView_GetItem(table, &item)) {
                    state.Check(item.iImage >= 0, scenario.name + L": context-menu row has no image index");
                    foundRealProviderIcon = foundRealProviderIcon || item.iImage > 0;
                }
                wchar_t status[64]{};
                ListView_GetItemText(table, row, 1, status, static_cast<int>(std::size(status)));
                if (std::wstring(status) == L"未安装") {
                    uninstalledRow = row;
                    break;
                }
            }
            state.Check(foundRealProviderIcon, scenario.name + L": installed providers did not display real icons");
            if (scenario.validateContextMenuIconTransparency && images && ImageList_GetImageCount(images) > 1) {
                int imageWidth = 0;
                int imageHeight = 0;
                const bool hasImageSize = ImageList_GetIconSize(images, &imageWidth, &imageHeight) != FALSE;
                state.Check(hasImageSize && imageWidth >= 8 && imageHeight >= 8,
                    scenario.name + L": context-menu provider image size is invalid");
                if (hasImageSize && imageWidth >= 8 && imageHeight >= 8) {
                    HDC screen = GetDC(nullptr);
                    HDC memory = CreateCompatibleDC(screen);
                    HBITMAP bitmap = CreateCompatibleBitmap(screen, imageWidth, imageHeight);
                    HGDIOBJ oldBitmap = bitmap ? SelectObject(memory, bitmap) : nullptr;
                    RECT imageRect{0, 0, imageWidth, imageHeight};
                    HBRUSH background = CreateSolidBrush(RGB(240, 244, 250));
                    if (bitmap && oldBitmap && background) {
                        FillRect(memory, &imageRect, background);
                        HICON icon = ImageList_GetIcon(images, 1, ILD_TRANSPARENT);
                        const BOOL drawn = icon
                            ? DrawIconEx(
                                memory, 0, 0, icon, imageWidth, imageHeight,
                                0, nullptr, DI_NORMAL)
                            : FALSE;
                        if (icon) DestroyIcon(icon);
                        state.Check(drawn != FALSE,
                            scenario.name + L": context-menu provider image could not be drawn");
                        const COLORREF corner = GetPixel(memory, 0, 0);
                        const COLORREF translucentEdge = GetPixel(
                            memory,
                            std::max(1, imageWidth / 8),
                            imageHeight / 2);
                        const COLORREF opaqueCore = GetPixel(
                            memory,
                            imageWidth / 4,
                            imageHeight / 2);
                        state.Check(
                            GetRValue(corner) > 220 && GetGValue(corner) > 220 && GetBValue(corner) > 220,
                            scenario.name + L": transparent provider icon pixels rendered as a dark border");
                        state.Check(
                            static_cast<int>(GetRValue(translucentEdge)) +
                                static_cast<int>(GetGValue(translucentEdge)) +
                            static_cast<int>(GetBValue(translucentEdge)) > 150,
                            scenario.name + L": translucent provider icon edge lost alpha blending");
                        const int opaqueCoreColorDistance =
                            std::abs(static_cast<int>(GetRValue(opaqueCore)) - 48) +
                            std::abs(static_cast<int>(GetGValue(opaqueCore)) - 64) +
                            std::abs(static_cast<int>(GetBValue(opaqueCore)) - 80);
                        state.Check(
                            opaqueCoreColorDistance < 48,
                            scenario.name + L": provider icon core was blurred by inset resampling");
                    } else {
                        state.Check(false, scenario.name + L": context-menu transparency probe bitmap failed");
                    }
                    if (background) DeleteObject(background);
                    if (oldBitmap) SelectObject(memory, oldBitmap);
                    if (bitmap) DeleteObject(bitmap);
                    if (memory) DeleteDC(memory);
                    if (screen) ReleaseDC(nullptr, screen);
                }
            }
            state.Check(uninstalledRow >= 0, scenario.name + L": no uninstalled provider row found");
            if (uninstalledRow >= 0) {
                state.Check(
                    ThemedUi::IsTableRowEnabled(table, uninstalledRow),
                    scenario.name + L": uninstalled provider row is disabled");
                ThemedUi::SetTableChecked(table, uninstalledRow, false);
                ThemedUi::SetTableSelectedIndex(table, uninstalledRow);
                SendMessageW(table, WM_KEYDOWN, VK_SPACE, 0);
                SendMessageW(table, WM_KEYUP, VK_SPACE, 0);
                state.Check(
                    ThemedUi::IsTableChecked(table, uninstalledRow),
                    scenario.name + L": uninstalled provider row cannot be checked");
                ThemedUi::SetTableSelectedIndex(table, -1);
            }
            if (!scenario.expectedContextMenuProvider.empty()) {
                int expectedRow = -1;
                std::wstring actualStatus;
                for (int row = 0; row < rowCount; ++row) {
                    wchar_t providerName[128]{};
                    ListView_GetItemText(
                        table, row, 0, providerName, static_cast<int>(std::size(providerName)));
                    if (providerName == scenario.expectedContextMenuProvider) {
                        wchar_t status[64]{};
                        ListView_GetItemText(
                            table, row, 1, status, static_cast<int>(std::size(status)));
                        expectedRow = row;
                        actualStatus = status;
                        break;
                    }
                }
                state.Check(
                    expectedRow >= 0,
                    scenario.name + L": expected context-menu provider row not found: " +
                        scenario.expectedContextMenuProvider);
                state.Check(
                    expectedRow >= 0 && actualStatus == scenario.expectedContextMenuStatus,
                    scenario.name + L": context-menu provider status mismatch: " +
                        scenario.expectedContextMenuProvider + L" = " + actualStatus);
                if (expectedRow >= 0) {
                    ListView_EnsureVisible(table, expectedRow, FALSE);
                }
            }
        }
    }

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

    if (scenario.splitButtonMenuId != 0) {
        const Theme validationTheme = Theme::Load(std::filesystem::current_path() / L"theme", L"default");
        ValidateSplitButtonChevron(
            ChildById(hwnd, scenario.splitButtonMenuId),
            validationTheme,
            scenario.forcedDpi,
            state,
            scenario.name,
            true);
    }

    BitmapCapture capture = CaptureWindowBitmap(hwnd);
    if (capture.bitmap && scenario.rejectDarkSurface &&
        NearBlackPixelRatio(capture.bitmap, capture.width, capture.height) >= 0.12) {
        DeleteObject(capture.bitmap);
        capture = CaptureClientBitmapWithChildren(hwnd);
        AcceptanceLog(scenario.name + L": used process-local client composite capture");
    }
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

void RunTooltipVisualScenario(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::filesystem::path& outputDir,
    TestState& state,
    UINT dpi,
    const std::wstring& tooltipText =
        L"恢复跟踪开关默认值，并清除全部菜单列表、状态与图标缓存。",
    const std::wstring& screenshotPrefix = L"public-tooltip") {
    const std::wstring suffix = std::to_wstring(dpi * 100 / USER_DEFAULT_SCREEN_DPI);
    const std::wstring scenarioName = screenshotPrefix + L"-" + suffix;
    AcceptanceLog(L"begin " + scenarioName);

    RECT ownerRect{};
    GetWindowRect(owner, &ownerRect);
    ThemedWindowUi tooltipHost(
        instance,
        owner,
        owner,
        theme,
        DialogLayoutKind::Compact,
        ownerRect.right - ownerRect.left,
        ownerRect.bottom - ownerRect.top);
    if (dpi != tooltipHost.dpi()) {
        LRESULT dpiResult = 0;
        tooltipHost.HandleMessage(
            WM_DPICHANGED,
            MAKEWPARAM(dpi, dpi),
            0,
            dpiResult);
    }

    ThemedTooltipOptions options{};
    options.placement = ThemedTooltipPlacement::Below;
    const POINT fixedScreenPoint{
        ownerRect.left + ThemedWindowUi::ScaleForDpi(48, dpi),
        ownerRect.top + ThemedWindowUi::ScaleForDpi(48, dpi)};
    tooltipHost.ui().ShowTooltip(tooltipText, fixedScreenPoint, options);

    HWND tooltip = WaitForTopWindow(
        FindWindowRequest{L"QuattroThemedTooltip", tooltipText, GetCurrentProcessId()},
        2000);
    state.Check(tooltip != nullptr, scenarioName + L": themed tooltip did not appear");
    if (tooltip) {
        Sleep(160);
        state.Check(
            IsWindowVisible(tooltip) != FALSE,
            scenarioName + L": themed tooltip did not remain visible");
        state.Check(
            GetWindow(tooltip, GW_OWNER) == owner,
            scenarioName + L": themed tooltip is not owned by its host window");
        const LONG_PTR tooltipStyle = GetWindowLongPtrW(tooltip, GWL_EXSTYLE);
        state.Check(
            (tooltipStyle & WS_EX_NOACTIVATE) != 0,
            scenarioName + L": themed tooltip can activate the acceptance desktop");
        state.Check(
            (tooltipStyle & WS_EX_TOPMOST) == 0,
            scenarioName + L": themed tooltip must remain behind unrelated programs during acceptance");
        state.Check(
            (tooltipStyle & WS_EX_TRANSPARENT) == 0,
            scenarioName + L": themed tooltip must not force transparent bottom-window repainting");
        state.Check(
            Contains(WindowText(tooltip), tooltipText),
            scenarioName + L": themed tooltip text mismatch");

        BitmapCapture tooltipCapture = CaptureWindowBitmap(tooltip);
        const std::filesystem::path tooltipScreenshot =
            outputDir / (screenshotPrefix + L"-" + suffix + L".png");
        state.Check(
            tooltipCapture.bitmap != nullptr,
            scenarioName + L": tooltip screenshot bitmap was not created");
        if (tooltipCapture.bitmap) {
            state.Check(
                BitmapHasVisualContent(tooltipCapture.bitmap, tooltipCapture.width, tooltipCapture.height),
                scenarioName + L": PrintWindow returned a blank or too-flat tooltip bitmap");
            state.Check(
                SavePng(tooltipCapture.bitmap, tooltipScreenshot),
                scenarioName + L": tooltip screenshot save failed: " + tooltipScreenshot.wstring());
            DeleteObject(tooltipCapture.bitmap);
        }
    }
    tooltipHost.ui().HideTooltip();
    AcceptanceLog(L"end " + scenarioName);
}

HWND CreateOwnerWindow(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"QuattroUiAcceptanceOwner";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"ui acceptance owner", WS_OVERLAPPEDWINDOW,
        80, 80, 760, 680, nullptr, nullptr, instance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(
        hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    UpdateWindow(hwnd);
    return hwnd;
}

void RunDialogScenario(
    const Scenario& scenario,
    const std::filesystem::path& outputDir,
    TestState& state,
    const std::function<void()>& runDialog,
    const std::function<void()>& beforeCloseRequest = {}) {
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
        if (scenario.closeDelayMs > 0) {
            Sleep(scenario.closeDelayMs);
        }
        if (beforeCloseRequest) {
            beforeCloseRequest();
        }
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

class ScopedAcceptanceChildEnvironment final {
public:
    ScopedAcceptanceChildEnvironment() {
        root_ = std::filesystem::temp_directory_path() /
            (L"QuattroUiAcceptance_" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(++sequence_));
        std::error_code ec;
        std::filesystem::create_directories(root_, ec);
        std::ofstream marker(root_ / L".quattro-test-root", std::ios::binary | std::ios::trunc);
        marker << "ui_acceptance=1\n";
        Set(L"QUATTRO_USER_CONFIG_DIR", root_.wstring());
        Set(L"QUATTRO_TEST_MODE", L"1");
        Set(L"QUATTRO_TEST_NO_FOCUS", L"1");
        Set(L"QUATTRO_ACCEPTANCE_MODE", L"background");
        Set(L"QUATTRO_TEST_RUN_ID", std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(sequence_));
        Set(L"QUATTRO_TEST_SUPPRESS_TRAY", L"1");
    }

    ~ScopedAcceptanceChildEnvironment() {
        for (auto it = saved_.rbegin(); it != saved_.rend(); ++it) {
            SetEnvironmentVariableW(it->name.c_str(), it->present ? it->value.c_str() : nullptr);
        }
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

    ScopedAcceptanceChildEnvironment(const ScopedAcceptanceChildEnvironment&) = delete;
    ScopedAcceptanceChildEnvironment& operator=(const ScopedAcceptanceChildEnvironment&) = delete;

    const std::filesystem::path& root() const { return root_; }

private:
    struct SavedValue {
        std::wstring name;
        std::wstring value;
        bool present = false;
    };

    static std::wstring Read(const wchar_t* name, bool& present) {
        std::wstring buffer(256, L'\0');
        for (;;) {
            const DWORD copied = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (copied == 0) {
                present = false;
                return {};
            }
            if (copied < buffer.size() - 1) {
                present = true;
                buffer.resize(copied);
                return buffer;
            }
            buffer.resize(buffer.size() * 2);
        }
    }

    void Set(const wchar_t* name, const std::wstring& value) {
        bool present = false;
        SavedValue saved;
        saved.name = name;
        saved.value = Read(name, present);
        saved.present = present;
        saved_.push_back(std::move(saved));
        SetEnvironmentVariableW(name, value.c_str());
    }

    std::filesystem::path root_;
    std::vector<SavedValue> saved_;
    static std::atomic<std::uint64_t> sequence_;
};

std::atomic<std::uint64_t> ScopedAcceptanceChildEnvironment::sequence_{0};

std::wstring DpiPercentSuffix(UINT dpi);

void RunMainWindowScenario(
    const std::filesystem::path& outputDir,
    const Theme& theme,
    TestState& state,
    UINT dpi) {
    const std::wstring dpiSuffix = DpiPercentSuffix(dpi);
    AcceptanceLog(L"begin main-window-" + dpiSuffix);
    const std::filesystem::path sourceExe = ModuleDirectory() / L"Quattro.exe";
    if (!std::filesystem::exists(sourceExe)) {
        state.Check(false, L"main-window: Quattro.exe not found beside acceptance executable");
        AcceptanceLog(L"missing main-window exe");
        return;
    }
    ScopedAcceptanceChildEnvironment childEnvironment;
    const std::filesystem::path exe = childEnvironment.root() / L"Quattro.exe";
    std::error_code copyError;
    std::filesystem::copy_file(sourceExe, exe, std::filesystem::copy_options::overwrite_existing, copyError);
    state.Check(!copyError, L"main-window: failed to copy isolated Quattro.exe");
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    StorageService storage(childEnvironment.root());
    storage.Load();
    Group linkGroup;
    linkGroup.name = L"启动项验收分组";
    linkGroup.pos = -1;
    state.Check(storage.InsertGroup(linkGroup), L"main-window-link-hover: seed group failed");
    Group linkTag;
    linkTag.name = L"普通标签";
    linkTag.parentGroup = linkGroup.id;
    linkTag.pos = -1;
    state.Check(storage.InsertGroup(linkTag), L"main-window-link-hover: seed tag failed");
    Link visualLink;
    visualLink.name = L"仅悬浮高亮";
    visualLink.parentGroup = linkTag.id;
    visualLink.path = childEnvironment.root().wstring();
    state.Check(storage.InsertLink(visualLink), L"main-window-link-hover: seed link failed");
    Group reminderGroup;
    reminderGroup.name = L"提醒验收分组";
    reminderGroup.pos = -1;
    state.Check(storage.InsertGroup(reminderGroup), L"todo-reminder-menu: seed group failed");
    Group reminderTag;
    reminderTag.name = L"待办提醒";
    reminderTag.parentGroup = reminderGroup.id;
    reminderTag.type = 4;
    reminderTag.content = L"todoItems";
    reminderTag.pos = -1;
    state.Check(storage.InsertGroup(reminderTag), L"todo-reminder-menu: seed tag failed");
    TodoItem reminderTodo;
    reminderTodo.tagId = reminderTag.id;
    reminderTodo.title = L"提交季度复盘";
    reminderTodo.content = L"整理指标、结论和后续行动项";
    reminderTodo.scheduleKind = TodoScheduleKind::Once;
    reminderTodo.anchorAt = L"2020-01-01 09:00:00";
    reminderTodo.nextDueAt = reminderTodo.anchorAt;
    reminderTodo.pos = -1;
    state.Check(storage.InsertTodoItem(reminderTodo), L"todo-reminder-menu: seed todo failed");
    Group futureTag;
    futureTag.name = L"未逾期待办";
    futureTag.parentGroup = reminderGroup.id;
    futureTag.type = 4;
    futureTag.content = L"todoItems";
    futureTag.pos = -1;
    state.Check(storage.InsertGroup(futureTag), L"todo-tag-overdue-menu: seed future tag failed");
    TodoItem futureTodo;
    futureTodo.tagId = futureTag.id;
    futureTodo.title = L"尚未到期的待办";
    futureTodo.scheduleKind = TodoScheduleKind::Once;
    futureTodo.anchorAt = L"2099-01-01 09:00:00";
    futureTodo.nextDueAt = futureTodo.anchorAt;
    futureTodo.pos = -1;
    state.Check(storage.InsertTodoItem(futureTodo), L"todo-tag-overdue-menu: seed future todo failed");
    ConfigService childConfig(childEnvironment.root() / L"conf.ini");
    AppConfig childSettings = childConfig.Load();
    childSettings.currentGroupId = linkGroup.id;
    childSettings.currentTagId = linkTag.id;
    childSettings.autoDock = false;
    childSettings.hideWhenInactive = false;
    childSettings.globalHotKeysEnabled = false;
    childSettings.width = 720;
    childSettings.height = 560;
    childConfig.Save(childSettings);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = command;
    if (!CreateProcessW(exe.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE, 0, nullptr, childEnvironment.root().c_str(), &startup, &process)) {
        state.Check(false, L"main-window: CreateProcess failed");
        AcceptanceLog(L"create main-window failed");
        return;
    }

    AcceptanceLog(L"wait main-window");
    HWND hwnd = WaitForTopWindow(FindWindowRequest{L"QuattroMainWindow", L"", process.dwProcessId}, 10000);
    if (hwnd) {
        AcceptanceLog(L"inspect main-window");
        const std::wstring marker = QuattroBuildMarkerText();
        const std::wstring markerDisplay = marker.empty() ? std::wstring{} : L"（" + marker + L"）";
        const std::wstring expectedTitle = L"Quattro快速启动器" + markerDisplay;
        state.Check(WindowText(hwnd) == expectedTitle, L"main-window: native title build marker mismatch");
        const std::wstring baseScreenshotName = marker.empty()
            ? L"main-window-official.png"
            : (marker == L"DEBUG-All" ? L"main-window-debug-all.png" : L"main-window-debug.png");
        const std::filesystem::path basePath(baseScreenshotName);
        const std::wstring screenshotName = basePath.stem().wstring() + L"-" + dpiSuffix + basePath.extension().wstring();
        Scenario scenario{L"main-window-" + dpiSuffix, L"QuattroMainWindow", L"", screenshotName, {expectedTitle}, {}, 0, 0, false};
        scenario.forcedDpi = dpi;
        ValidateAndCapture(hwnd, scenario, outputDir, state);

        constexpr UINT kTestLinkVisualStateMessage = WM_APP + 0x75;
        state.Check(
            SendMessageW(hwnd, kTestLinkVisualStateMessage, static_cast<WPARAM>(visualLink.id), 0) == TRUE,
            L"main-window-link-hover: failed to set clicked state");
        BitmapCapture clickedCapture = CaptureWindowBitmap(hwnd);
        state.Check(clickedCapture.bitmap != nullptr, L"main-window-link-hover: clicked capture failed");
        if (clickedCapture.bitmap) {
            state.Check(
                SavePng(clickedCapture.bitmap, outputDir / (L"main-window-link-clicked-" + dpiSuffix + L".png")),
                L"main-window-link-hover: clicked screenshot save failed");
        }

        state.Check(
            SendMessageW(hwnd, kTestLinkVisualStateMessage, static_cast<WPARAM>(visualLink.id), 1) == TRUE,
            L"main-window-link-hover: failed to set hover state");
        BitmapCapture hoveredCapture = CaptureWindowBitmap(hwnd);
        state.Check(hoveredCapture.bitmap != nullptr, L"main-window-link-hover: hover capture failed");
        if (hoveredCapture.bitmap) {
            state.Check(
                SavePng(hoveredCapture.bitmap, outputDir / (L"main-window-link-hovered-" + dpiSuffix + L".png")),
                L"main-window-link-hover: hover screenshot save failed");
        }

        state.Check(
            SendMessageW(hwnd, kTestLinkVisualStateMessage, static_cast<WPARAM>(visualLink.id), 0) == TRUE,
            L"main-window-link-hover: failed to clear hover state");
        BitmapCapture leftCapture = CaptureWindowBitmap(hwnd);
        state.Check(leftCapture.bitmap != nullptr, L"main-window-link-hover: mouse-leave capture failed");
        if (leftCapture.bitmap) {
            state.Check(
                SavePng(leftCapture.bitmap, outputDir / (L"main-window-link-mouse-left-" + dpiSuffix + L".png")),
                L"main-window-link-hover: mouse-leave screenshot save failed");
        }
        if (clickedCapture.bitmap && hoveredCapture.bitmap && leftCapture.bitmap) {
            const std::size_t hoverChanged = CountChangedPixelSamples(clickedCapture, hoveredCapture);
            const std::size_t leaveChanged = CountChangedPixelSamples(clickedCapture, leftCapture);
            AcceptanceLog(
                L"main-window-link-hover dpi=" + dpiSuffix +
                L" hoverChanged=" + std::to_wstring(hoverChanged) +
                L" leaveChanged=" + std::to_wstring(leaveChanged));
            state.Check(hoverChanged >= 100, L"main-window-link-hover: hover has no visible highlight");
            state.Check(leaveChanged <= 20, L"main-window-link-hover: clicked item remains visibly selected after mouse leave");
        }
        if (clickedCapture.bitmap) DeleteObject(clickedCapture.bitmap);
        if (hoveredCapture.bitmap) DeleteObject(hoveredCapture.bitmap);
        if (leftCapture.bitmap) DeleteObject(leftCapture.bitmap);

        BitmapCapture titleCapture = CaptureWindowBitmap(hwnd);
        state.Check(titleCapture.bitmap != nullptr, L"main-window: title color capture failed");
        if (titleCapture.bitmap) {
            const Color danger = theme.color(L"global", L"danger", L"text");
            const COLORREF dangerColor = RGB(
                static_cast<BYTE>(std::clamp(danger.r, 0.0f, 1.0f) * 255.0f + 0.5f),
                static_cast<BYTE>(std::clamp(danger.g, 0.0f, 1.0f) * 255.0f + 0.5f),
                static_cast<BYTE>(std::clamp(danger.b, 0.0f, 1.0f) * 255.0f + 0.5f));
            const RECT titleTextArea{32, 0, std::max(32, titleCapture.width - 120), std::min(52, titleCapture.height)};
            const std::size_t dangerPixels = CountPixelsNearColor(
                titleCapture.bitmap,
                titleCapture.width,
                titleCapture.height,
                titleTextArea,
                dangerColor,
                48);
            state.Check(
                marker.empty() ? dangerPixels <= 2 : dangerPixels >= 3,
                marker.empty()
                    ? L"main-window: official title unexpectedly contains danger-colored marker pixels"
                    : L"main-window: development marker is not rendered with the danger semantic color");
            DeleteObject(titleCapture.bitmap);
        }

        const auto captureTestMenu = [&](UINT message, const std::wstring& scenarioName, WPARAM parameter = 0) {
            BitmapCapture linkMenuIdleCapture;
            BitmapCapture linkMenuHoverReferenceCapture;
            if (scenarioName == L"link-popup-menu") {
                SendMessageW(hwnd, kTestLinkVisualStateMessage, parameter, 0);
                linkMenuIdleCapture = CaptureWindowBitmap(hwnd);
                SendMessageW(hwnd, kTestLinkVisualStateMessage, parameter, 1);
                linkMenuHoverReferenceCapture = CaptureWindowBitmap(hwnd);
                SendMessageW(hwnd, kTestLinkVisualStateMessage, parameter, 0);
                state.Check(
                    linkMenuIdleCapture.bitmap && linkMenuHoverReferenceCapture.bitmap,
                    L"main-window-link-menu-hover: reference captures failed");
            }
            std::thread popupThread([&]() {
                SendMessageW(hwnd, message, parameter, 0);
            });
            HWND testPopup = WaitForTopWindow(FindWindowRequest{L"#32768", L"", process.dwProcessId}, 5000);
            state.Check(testPopup != nullptr, scenarioName + L": popup did not appear");
            BitmapCapture linkMenuMainCapture;
            if (testPopup) {
                if (scenarioName == L"link-popup-menu") {
                    linkMenuMainCapture = CaptureWindowBitmap(hwnd);
                    state.Check(
                        linkMenuMainCapture.bitmap != nullptr,
                        L"main-window-link-menu-hover: main-window capture failed while menu was open");
                    if (linkMenuMainCapture.bitmap) {
                        state.Check(
                            SavePng(
                                linkMenuMainCapture.bitmap,
                                outputDir / (L"main-window-link-menu-hover-" + dpiSuffix + L".png")),
                            L"main-window-link-menu-hover: screenshot save failed");
                        if (linkMenuIdleCapture.bitmap && linkMenuHoverReferenceCapture.bitmap) {
                            const std::size_t changedFromIdle =
                                CountChangedPixelSamples(linkMenuIdleCapture, linkMenuMainCapture);
                            const std::size_t changedFromHover =
                                CountChangedPixelSamples(linkMenuHoverReferenceCapture, linkMenuMainCapture);
                            AcceptanceLog(
                                L"main-window-link-menu-hover dpi=" + dpiSuffix +
                                L" changedFromIdle=" + std::to_wstring(changedFromIdle) +
                                L" changedFromHover=" + std::to_wstring(changedFromHover));
                            state.Check(
                                changedFromIdle >= 100,
                                L"main-window-link-menu-hover: link lost its hover highlight while the menu was open");
                            state.Check(
                                changedFromHover <= 20,
                                L"main-window-link-menu-hover: menu-open highlight differs from normal hover");
                        }
                    }
                }
                BitmapCapture popupCapture = CaptureWindowBitmap(testPopup);
                state.Check(
                    popupCapture.bitmap && BitmapHasVisualContent(
                        popupCapture.bitmap, popupCapture.width, popupCapture.height),
                    scenarioName + L": popup screenshot is invalid");
                if (popupCapture.bitmap) {
                    const int lowDetailRun = LargestLowDetailHorizontalRun(
                        popupCapture.bitmap, popupCapture.width, popupCapture.height);
                    state.Check(
                        lowDetailRun < MulDiv(28, static_cast<int>(dpi), 96),
                        scenarioName + L": popup contains an owner-draw blank row band");
                    if (scenarioName == L"tool-popup-menu") {
                        state.Check(
                            DistinctToolMenuIconCount(
                                popupCapture.bitmap, popupCapture.width, popupCapture.height, dpi) >= 5,
                            scenarioName + L": builtin tools still reuse one menu icon");
                    }
                    state.Check(
                        SavePng(
                            popupCapture.bitmap,
                            outputDir / (scenarioName + L"-" + dpiSuffix + L".png")),
                        scenarioName + L": screenshot save failed");
                    DeleteObject(popupCapture.bitmap);
                }
                PostMessageW(testPopup, WM_CANCELMODE, 0, 0);
                PostMessageW(hwnd, WM_CANCELMODE, 0, 0);
            }
            popupThread.join();
            Sleep(100);
            if (scenarioName == L"link-popup-menu") {
                BitmapCapture closedCapture = CaptureWindowBitmap(hwnd);
                state.Check(
                    closedCapture.bitmap != nullptr,
                    L"main-window-link-menu-hover: post-menu capture failed");
                if (closedCapture.bitmap && linkMenuIdleCapture.bitmap) {
                    const std::size_t changedAfterClose =
                        CountChangedPixelSamples(linkMenuIdleCapture, closedCapture);
                    AcceptanceLog(
                        L"main-window-link-menu-hover-close dpi=" + dpiSuffix +
                        L" changedAfterClose=" + std::to_wstring(changedAfterClose));
                    state.Check(
                        changedAfterClose <= 20,
                        L"main-window-link-menu-hover: hover lock remained after the menu closed");
                }
                if (closedCapture.bitmap) DeleteObject(closedCapture.bitmap);
            }
            if (linkMenuMainCapture.bitmap) DeleteObject(linkMenuMainCapture.bitmap);
            if (linkMenuIdleCapture.bitmap) DeleteObject(linkMenuIdleCapture.bitmap);
            if (linkMenuHoverReferenceCapture.bitmap) DeleteObject(linkMenuHoverReferenceCapture.bitmap);
        };
        captureTestMenu(WM_QUATTRO_TEST_MAIN_MENU, L"main-popup-menu");
        captureTestMenu(WM_QUATTRO_TEST_TOOL_MENU, L"tool-popup-menu");
        captureTestMenu(WM_QUATTRO_TEST_LINK_MENU, L"link-popup-menu", static_cast<WPARAM>(visualLink.id));
        captureTestMenu(WM_QUATTRO_TEST_TAG_MENU, L"tag-popup-menu", static_cast<WPARAM>(linkTag.id));

        std::thread menuThread([&]() {
            constexpr UINT kTestTodoMenuMessage = WM_APP + 0x6F;
            SendMessageW(hwnd, kTestTodoMenuMessage, static_cast<WPARAM>(reminderTodo.id), 0);
        });
        HWND popup = WaitForTopWindow(FindWindowRequest{L"#32768", L"", process.dwProcessId}, 5000);
        state.Check(popup != nullptr, L"todo-reminder-menu: popup did not appear");
        if (popup) {
            BitmapCapture popupCapture = CaptureWindowBitmap(popup);
            state.Check(
                popupCapture.bitmap && BitmapHasVisualContent(popupCapture.bitmap, popupCapture.width, popupCapture.height),
                L"todo-reminder-menu: popup screenshot is invalid");
            if (popupCapture.bitmap) {
                state.Check(
                    SavePng(popupCapture.bitmap, outputDir / (L"todo-reminder-menu-" + dpiSuffix + L".png")),
                    L"todo-reminder-menu: screenshot save failed");
                DeleteObject(popupCapture.bitmap);
            }
            PostMessageW(popup, WM_CANCELMODE, 0, 0);
            PostMessageW(hwnd, WM_CANCELMODE, 0, 0);
        }
        menuThread.join();

        Sleep(100);
        std::thread tagMenuThread([&]() {
            constexpr UINT kTestTodoTagMenuMessage = WM_APP + 0x73;
            SendMessageW(hwnd, kTestTodoTagMenuMessage, static_cast<WPARAM>(reminderTag.id), 0);
        });
        HWND tagPopup = WaitForTopWindow(FindWindowRequest{L"#32768", L"", process.dwProcessId}, 5000);
        state.Check(tagPopup != nullptr, L"todo-tag-overdue-menu: popup did not appear");
        if (tagPopup) {
            BitmapCapture popupCapture = CaptureWindowBitmap(tagPopup);
            state.Check(
                popupCapture.bitmap && BitmapHasVisualContent(popupCapture.bitmap, popupCapture.width, popupCapture.height),
                L"todo-tag-overdue-menu: popup screenshot is invalid");
            if (popupCapture.bitmap) {
                state.Check(
                    SavePng(popupCapture.bitmap, outputDir / (L"todo-tag-overdue-menu-" + dpiSuffix + L".png")),
                    L"todo-tag-overdue-menu: screenshot save failed");
                DeleteObject(popupCapture.bitmap);
            }
            PostMessageW(tagPopup, WM_CANCELMODE, 0, 0);
            PostMessageW(hwnd, WM_CANCELMODE, 0, 0);
        }
        tagMenuThread.join();

        Sleep(100);
        std::thread noOverdueMenuThread([&]() {
            constexpr UINT kTestTodoTagMenuMessage = WM_APP + 0x73;
            SendMessageW(hwnd, kTestTodoTagMenuMessage, static_cast<WPARAM>(futureTag.id), 0);
        });
        HWND noOverduePopup = WaitForTopWindow(FindWindowRequest{L"#32768", L"", process.dwProcessId}, 5000);
        state.Check(noOverduePopup != nullptr, L"todo-tag-no-overdue-menu: popup did not appear");
        if (noOverduePopup) {
            BitmapCapture popupCapture = CaptureWindowBitmap(noOverduePopup);
            state.Check(
                popupCapture.bitmap && BitmapHasVisualContent(popupCapture.bitmap, popupCapture.width, popupCapture.height),
                L"todo-tag-no-overdue-menu: popup screenshot is invalid");
            if (popupCapture.bitmap) {
                state.Check(
                    SavePng(popupCapture.bitmap, outputDir / (L"todo-tag-no-overdue-menu-" + dpiSuffix + L".png")),
                    L"todo-tag-no-overdue-menu: screenshot save failed");
                DeleteObject(popupCapture.bitmap);
            }
            PostMessageW(noOverduePopup, WM_CANCELMODE, 0, 0);
            PostMessageW(hwnd, WM_CANCELMODE, 0, 0);
        }
        noOverdueMenuThread.join();
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
    AcceptanceLog(L"end main-window-" + dpiSuffix);
}

bool WaitForListViewRows(HWND listView, int minimumRows, DWORD timeoutMs = 6000) {
    const auto begin = GetTickCount64();
    while (GetTickCount64() - begin < timeoutMs) {
        const int count = static_cast<int>(SendMessageW(listView, LVM_GETITEMCOUNT, 0, 0));
        if (count >= minimumRows) {
            return true;
        }
        Sleep(50);
    }
    return false;
}

HWND WaitForChildClass(HWND parent, const std::wstring& className, DWORD timeoutMs = 6000) {
    const auto begin = GetTickCount64();
    while (GetTickCount64() - begin < timeoutMs) {
        const auto children = Children(parent);
        for (const auto& child : children) {
            if (child.className == className && IsWindowVisible(child.hwnd)) {
                return child.hwnd;
            }
        }
        Sleep(50);
    }
    return nullptr;
}

// In-process host window that builds a themed table through the public
// ThemedWindowUi / ThemedUi facade — the same infrastructure production windows
// use — so custom-draw (and therefore the alternating-row background) runs
// exactly as it does in the app. Running in-process is what makes the pixel
// assertions reliable: LVM_GETITEMRECT and selection state only work within the
// owning process.
struct TableHostWindow {
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND table_ = nullptr;
    bool gridLines_ = false;
    bool allowColumnResize_ = false;
    bool checkable_ = false;
    bool webDavColumns_ = false;
    bool showHeader_ = true;
    int visibleRows_ = 0;
    UINT forcedDpi_ = 0;
    HFONT forcedFont_ = nullptr;
    Theme theme_;
    std::unique_ptr<ThemedWindowUi> windowUi_;

    ~TableHostWindow() {
        if (forcedFont_) DeleteObject(forcedFont_);
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        TableHostWindow* self = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<TableHostWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        } else {
            self = reinterpret_cast<TableHostWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT result = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, result)) {
            return result;
        }
        if (message == WM_CREATE) {
            const int logicalWidth = webDavColumns_ ? 720 : 360;
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, nullptr, hwnd_, theme_, DialogLayoutKind::Compact, logicalWidth, 260);
            std::unique_ptr<ThemedUi> forcedUi;
            if (forcedDpi_) {
                forcedFont_ = ThemedControls::CreateDialogFont(forcedDpi_);
                forcedUi = std::make_unique<ThemedUi>(
                    instance_, hwnd_, theme_, forcedFont_, DialogLayoutKind::Compact,
                    ThemedWindowUi::ScaleForDpi(logicalWidth, forcedDpi_),
                    ThemedWindowUi::ScaleForDpi(260, forcedDpi_),
                    nullptr, windowUi_.get(), nullptr, nullptr, forcedDpi_);
            }
            const ThemedUi ui = forcedUi ? *forcedUi : windowUi_->ui();
            ThemedTableOptions tableOptions{};
            tableOptions.selection = ThemedTableSelection::Single;
            tableOptions.view = ThemedTableView::Details;
            tableOptions.checkable = checkable_;
            tableOptions.fullRowSelect = true;
            tableOptions.showHeader = showHeader_;
            tableOptions.showRowGridLines = gridLines_;
            tableOptions.showColumnGridLines = gridLines_;
            tableOptions.allowColumnResize = allowColumnResize_;
            tableOptions.reserveScrollBarGutter = checkable_;
            const int tableBottom = visibleRows_ > 0
                ? ui.scale(16) + ui.tableHeightForRows(visibleRows_, showHeader_)
                : ui.scale(240);
            const std::vector<ThemedTableColumn> columns = webDavColumns_
                ? std::vector<ThemedTableColumn>{
                    {L"name", L"文件名", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed,
                        ui.tableColumnWidth({L"文件名", L"quattro-file-name.ext"})},
                    {L"path", L"系统绝对路径", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
                    {L"size", L"大小", ThemedTableColumnAlign::End, ThemedTableColumnWidth::Fixed,
                        ui.tableColumnWidth({L"大小", L"999.99 GB"})},
                    {L"time", L"上传时间", ThemedTableColumnAlign::End, ThemedTableColumnWidth::Fixed,
                        ui.tableColumnWidth({L"上传时间", L"2000-00-00T00:00:00.000Z"})},
                    {L"action", L"操作", ThemedTableColumnAlign::Center, ThemedTableColumnWidth::Fixed,
                        ui.buttonWidth(L"…", ThemedButtonRole::Normal, ThemedButtonSize::Compact,
                            ThemedButtonWidthMode::Text) + ui.denseGap()},
                }
                : std::vector<ThemedTableColumn>{
                    {L"name", L"名称", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining},
                    {L"value", L"值", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, 96},
                };
            table_ = ui.Table(
                101,
                RECT{ui.scale(16), ui.scale(16), ui.scale(logicalWidth - 16), tableBottom},
                columns,
                tableOptions);
            if (webDavColumns_) {
                ThemedTableCell firstAction{L"…"};
                firstAction.role = ThemedTableCellRole::Action;
                firstAction.actionId = 434;
                ThemedTableCell secondAction = firstAction;
                ThemedUi::SetTableRows(table_, {
                    {1, {{L"quattro-webdav-rightclick-test-6d19acf4b6a74a4ab51a525522e4a38.ext"},
                         {L"C:\\Users\\tester\\Documents\\quattro-webdav-rightclick-test.ext"},
                         {L"51 B"}, {L"2026-07-21T06:50:00.000Z"}, firstAction}, false, true},
                    {2, {{L"AGENTS.md"}, {L"E:\\work\\quattro\\AGENTS.md"}, {L"4 KB"},
                         {L"2026-07-21T07:42:00.000Z"}, secondAction}, false, true},
                });
            } else if (checkable_) {
                ThemedUi::SetTableRows(table_, {
                    {1, {{L"已勾选"}, {L"已安装"}}, true, true},
                    {2, {{L"未勾选"}, {L"已安装"}}, false, true},
                    {3, {{L"禁用勾选"}, {L"未安装"}}, true, false},
                    {4, {{L"禁用未勾选"}, {L"未安装"}}, false, false},
                });
            } else {
                ThemedUi::SetTableRows(table_, {
                    {1, {{L"行 0"}, {L"A"}}, false, true},
                    {2, {{L"行 1"}, {L"B"}}, false, true},
                    {3, {{L"行 2"}, {L"C"}}, false, true},
                    {4, {{L"行 3"}, {L"D"}}, false, true},
                });
            }
            // Select row 0 only. This is the exact condition that exposed the
            // regression: under LVS_EX_FULLROWSELECT the CDIS_SELECTED draw flag
            // wrongly reported every row selected, so unselected rows were also
            // painted with the selected background and the stripe disappeared.
            ThemedUi::SetTableSelectedIndex(table_, checkable_ ? 1 : 0);
            return 0;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            windowUi_->FillBackground(dc);
            windowUi_->DrawRegisteredTableFrames(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }
};

constexpr UINT kShowSplitButtonMenuAcceptance = WM_APP + 0x5B;

struct SplitButtonMenuHostWindow {
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    UINT forcedDpi_ = USER_DEFAULT_SCREEN_DPI;
    HFONT forcedFont_ = nullptr;
    Theme theme_;
    ThemedSplitButton split_{};
    std::unique_ptr<ThemedWindowUi> windowUi_;
    std::unique_ptr<ThemedUi> menuUi_;

    ~SplitButtonMenuHostWindow() {
        if (forcedFont_) DeleteObject(forcedFont_);
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        SplitButtonMenuHostWindow* self = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = reinterpret_cast<SplitButtonMenuHostWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        } else {
            self = reinterpret_cast<SplitButtonMenuHostWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return self ? self->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        LRESULT result = 0;
        if (ThemedWindowUi::HandleCommonMessage(windowUi_, message, wParam, lParam, result)) {
            return result;
        }
        switch (message) {
        case WM_CREATE: {
            windowUi_ = std::make_unique<ThemedWindowUi>(
                instance_, nullptr, hwnd_, theme_, DialogLayoutKind::Compact, 260, 120);
            forcedFont_ = ThemedControls::CreateDialogFont(forcedDpi_);
            menuUi_ = std::make_unique<ThemedUi>(
                instance_, hwnd_, theme_, forcedFont_, DialogLayoutKind::Compact,
                ThemedWindowUi::ScaleForDpi(260, forcedDpi_),
                ThemedWindowUi::ScaleForDpi(120, forcedDpi_),
                nullptr, nullptr, nullptr, nullptr, forcedDpi_);
            split_ = menuUi_->SplitButton(
                501, 502, L"选择文件", menuUi_->scale(16), menuUi_->scale(20),
                ThemedButtonRole::Normal, ThemedButtonSize::Normal,
                ThemedButtonWidthMode::Fixed, menuUi_->scale(160));
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            windowUi_->FillBackground(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case kShowSplitButtonMenuAcceptance:
            return static_cast<LRESULT>(menuUi_->ShowSplitButtonMenu(
                hwnd_, split_.menu,
                {
                    {503, L"选择文件夹", true, TablerIconId::Folder},
                    {504, L"清空路径", false, TablerIconId::Clear},
                }));
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }
};

void RunSplitButtonMenuScenario(
    const std::filesystem::path& outputDir,
    TestState& state,
    UINT dpi) {
    const std::wstring suffix = DpiPercentSuffix(dpi);
    const std::wstring scenarioName = L"split-button-menu-" + suffix;
    AcceptanceLog(L"begin " + scenarioName);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    SplitButtonMenuHostWindow host;
    host.instance_ = instance;
    host.forcedDpi_ = dpi;
    host.theme_ = Theme::Load(std::filesystem::current_path() / L"theme", L"default");

    const std::wstring className = L"QuattroSplitButtonMenuHost" + std::to_wstring(dpi);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SplitButtonMenuHostWindow::Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = className.c_str();
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        scenarioName.c_str(),
        WS_OVERLAPPEDWINDOW,
        180,
        180,
        ThemedWindowUi::ScaleForDpi(300, dpi),
        ThemedWindowUi::ScaleForDpi(160, dpi),
        nullptr,
        nullptr,
        instance,
        &host);
    if (!hwnd || !host.menuUi_ || !host.split_.menu) {
        state.Check(false, scenarioName + L": host or split button creation failed");
        if (hwnd) DestroyWindow(hwnd);
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(
        hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    UpdateWindow(hwnd);
    ValidateSplitButtonChevron(
        host.split_.menu,
        host.theme_,
        dpi,
        state,
        scenarioName + L" button",
        true);

    const HWND foregroundBefore = GetForegroundWindow();
    const HWND activeBefore = GetActiveWindow();
    std::thread controller([&]() {
        HWND popup = WaitForTopWindow(
            FindWindowRequest{L"#32768", L"", GetCurrentProcessId()}, 5000);
        state.Check(popup != nullptr, scenarioName + L": popup did not appear");
        if (popup) {
            BitmapCapture capture = CaptureWindowBitmap(popup);
            state.Check(capture.bitmap != nullptr, scenarioName + L": popup capture failed");
            if (capture.bitmap) {
                state.Check(
                    BitmapHasVisualContent(capture.bitmap, capture.width, capture.height),
                    scenarioName + L": popup screenshot is blank");
                state.Check(
                    LargestLowDetailHorizontalRun(capture.bitmap, capture.width, capture.height) <
                        MulDiv(28, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
                    scenarioName + L": popup contains a blank owner-draw row");
                const Color accent = host.theme_.color(L"menuItem", L"accent", L"icon");
                const COLORREF accentColor = RGB(
                    static_cast<BYTE>(std::clamp(accent.r, 0.0f, 1.0f) * 255.0f + 0.5f),
                    static_cast<BYTE>(std::clamp(accent.g, 0.0f, 1.0f) * 255.0f + 0.5f),
                    static_cast<BYTE>(std::clamp(accent.b, 0.0f, 1.0f) * 255.0f + 0.5f));
                const RECT iconArea{
                    0, 0,
                    std::min(capture.width, MulDiv(34, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)),
                    capture.height};
                state.Check(
                    CountPixelsNearColor(
                        capture.bitmap, capture.width, capture.height, iconArea, accentColor, 56) >=
                        static_cast<std::size_t>(MulDiv(12, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI)),
                    scenarioName + L": semantic Tabler menu icon is not visible");
                state.Check(
                    SavePng(capture.bitmap, outputDir / (scenarioName + L".png")),
                    scenarioName + L": screenshot save failed");
                DeleteObject(capture.bitmap);
            }
            PostMessageW(popup, WM_CANCELMODE, 0, 0);
            PostMessageW(hwnd, WM_CANCELMODE, 0, 0);
        }
    });
    SendMessageW(hwnd, kShowSplitButtonMenuAcceptance, 0, 0);
    controller.join();
    state.Check(GetForegroundWindow() == foregroundBefore, scenarioName + L": changed the foreground window");
    state.Check(GetActiveWindow() == activeBefore, scenarioName + L": changed the active window");
    DestroyWindow(hwnd);
    AcceptanceLog(L"end " + scenarioName);
}

// Map a table item's rect (table client coordinates) into the coordinate space
// of a bitmap captured from the host top-level window.
RECT TableRowRectInHostBitmap(HWND host, HWND table, int row) {
    RECT itemRect{LVIR_BOUNDS, 0, 0, 0};
    if (!SendMessageW(table, LVM_GETITEMRECT, row, reinterpret_cast<LPARAM>(&itemRect))) {
        return RECT{0, 0, 0, 0};
    }
    POINT topLeft{itemRect.left, itemRect.top};
    POINT bottomRight{itemRect.right, itemRect.bottom};
    ClientToScreen(table, &topLeft);
    ClientToScreen(table, &bottomRight);
    RECT hostRect{};
    GetWindowRect(host, &hostRect);
    return RECT{topLeft.x - hostRect.left, topLeft.y - hostRect.top,
                bottomRight.x - hostRect.left, bottomRight.y - hostRect.top};
}

RECT TableSubItemRectInHostBitmap(HWND host, HWND table, int row, int column) {
    RECT itemRect{LVIR_BOUNDS, column, 0, 0};
    if (!SendMessageW(table, LVM_GETSUBITEMRECT, row, reinterpret_cast<LPARAM>(&itemRect))) {
        return RECT{0, 0, 0, 0};
    }
    POINT topLeft{itemRect.left, itemRect.top};
    POINT bottomRight{itemRect.right, itemRect.bottom};
    ClientToScreen(table, &topLeft);
    ClientToScreen(table, &bottomRight);
    RECT hostRect{};
    GetWindowRect(host, &hostRect);
    return RECT{topLeft.x - hostRect.left, topLeft.y - hostRect.top,
                bottomRight.x - hostRect.left, bottomRight.y - hostRect.top};
}

void RunTableGridLinesScenario(
    const std::filesystem::path& outputDir,
    TestState& state,
    bool gridLines,
    const std::wstring& scenarioName,
    const std::wstring& screenshotName) {
    AcceptanceLog(L"begin " + scenarioName);
    HINSTANCE instance = GetModuleHandleW(nullptr);
    TableHostWindow host;
    host.instance_ = instance;
    host.gridLines_ = gridLines;
    host.theme_ = Theme::Load(std::filesystem::current_path() / L"theme", L"default");

    const std::wstring className = L"QuattroTableGridLinesHost" + scenarioName;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TableHostWindow::Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = className.c_str();
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, scenarioName.c_str(), WS_OVERLAPPEDWINDOW,
        160, 160, 400, 320, nullptr, nullptr, instance, &host);
    if (!hwnd || !host.table_) {
        state.Check(false, scenarioName + L": host window/table creation failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    RedrawWindow(host.table_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    Sleep(200);

    BitmapCapture capture = CaptureWindowBitmap(hwnd);
    state.Check(capture.bitmap != nullptr, scenarioName + L": capture failed");
    if (capture.bitmap) {
        const int probeRow = 1;
        const RECT rowRect = TableRowRectInHostBitmap(hwnd, host.table_, probeRow);
        const RECT cellRect = TableSubItemRectInHostBitmap(hwnd, host.table_, probeRow, 0);
        const RECT secondCellRect = TableSubItemRectInHostBitmap(hwnd, host.table_, probeRow, 1);
        RECT headerFirstRect{};
        RECT headerSecondRect{};
        if (HWND header = ListView_GetHeader(host.table_)) {
            Header_GetItemRect(header, 0, &headerFirstRect);
            Header_GetItemRect(header, 1, &headerSecondRect);
            POINT headerTopLeft{headerFirstRect.left, headerFirstRect.top};
            POINT headerBottomRight{headerFirstRect.right, headerFirstRect.bottom};
            POINT headerSecondTopLeft{headerSecondRect.left, headerSecondRect.top};
            ClientToScreen(header, &headerTopLeft);
            ClientToScreen(header, &headerBottomRight);
            ClientToScreen(header, &headerSecondTopLeft);
            RECT hostRect{};
            GetWindowRect(hwnd, &hostRect);
            headerFirstRect = RECT{
                headerTopLeft.x - hostRect.left,
                headerTopLeft.y - hostRect.top,
                headerBottomRight.x - hostRect.left,
                headerBottomRight.y - hostRect.top};
            headerSecondRect.left = headerSecondTopLeft.x - hostRect.left;
        }
        const int columnX = std::clamp(static_cast<int>(secondCellRect.left), 0, std::max(0, capture.width - 1));
        const int rowY = std::clamp(static_cast<int>(rowRect.bottom - 1), 0, std::max(0, capture.height - 1));
        const int middleY = std::clamp(static_cast<int>((rowRect.top + rowRect.bottom) / 2), 0, std::max(0, capture.height - 1));
        const int middleX = std::clamp(static_cast<int>((cellRect.left + cellRect.right) / 2), 0, std::max(0, capture.width - 1));
        const int headerColumnX = std::clamp(static_cast<int>(headerSecondRect.left), 0, std::max(0, capture.width - 1));
        const int headerRowY = std::clamp(static_cast<int>(headerFirstRect.bottom - 1), 0, std::max(0, capture.height - 1));
        const int headerMiddleY = std::clamp(static_cast<int>((headerFirstRect.top + headerFirstRect.bottom) / 2), 0, std::max(0, capture.height - 1));
        const int headerMiddleX = std::clamp(static_cast<int>((headerFirstRect.left + headerFirstRect.right) / 2), 0, std::max(0, capture.width - 1));

        int columnDistance = 0;
        for (int x = std::max(0, columnX - 4); x <= std::min(capture.width - 1, columnX + 4); ++x) {
            columnDistance = std::max(columnDistance, ColorDistance(
                BitmapPixel(capture.bitmap, x, middleY),
                BitmapPixel(capture.bitmap, std::max(0, x - 3), middleY)));
        }
        const int rowDistance = ColorDistance(
            BitmapPixel(capture.bitmap, middleX, rowY),
            BitmapPixel(capture.bitmap, middleX, std::max(0, rowY - 3)));
        int headerColumnDistance = 0;
        for (int x = std::max(0, headerColumnX - 4); x <= std::min(capture.width - 1, headerColumnX + 4); ++x) {
            headerColumnDistance = std::max(headerColumnDistance, ColorDistance(
                BitmapPixel(capture.bitmap, x, headerMiddleY),
                BitmapPixel(capture.bitmap, std::max(0, x - 3), headerMiddleY)));
        }
        const int headerRowDistance = ColorDistance(
            BitmapPixel(capture.bitmap, headerMiddleX, headerRowY),
            BitmapPixel(capture.bitmap, headerMiddleX, std::max(0, headerRowY - 3)));

        AcceptanceLog(scenarioName + L" columnDistance=" + std::to_wstring(columnDistance) +
            L" rowDistance=" + std::to_wstring(rowDistance) +
            L" headerColumnDistance=" + std::to_wstring(headerColumnDistance) +
            L" headerRowDistance=" + std::to_wstring(headerRowDistance));
        state.Check(headerColumnDistance >= 24, scenarioName + L": header column grid line is not visible by default");
        state.Check(headerRowDistance >= 24, scenarioName + L": header bottom grid line is not visible by default");
        if (gridLines) {
            state.Check(columnDistance >= 24, scenarioName + L": column grid line is not visible");
            state.Check(rowDistance >= 24, scenarioName + L": row grid line is not visible");
        } else {
            state.Check(columnDistance < 24, scenarioName + L": data column grid line is visible by default");
            state.Check(rowDistance < 24, scenarioName + L": data row grid line is visible by default");
        }

        const std::filesystem::path screenshot = outputDir / screenshotName;
        SavePng(capture.bitmap, screenshot);
        DeleteObject(capture.bitmap);
    }

    DestroyWindow(hwnd);
    AcceptanceLog(L"end " + scenarioName);
}

void RunTableAlternatingRowsScenario(const std::filesystem::path& outputDir, TestState& state) {
    AcceptanceLog(L"begin table-alternating-rows");
    HINSTANCE instance = GetModuleHandleW(nullptr);
    TableHostWindow host;
    host.instance_ = instance;
    host.theme_ = Theme::Load(std::filesystem::current_path() / L"theme", L"default");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TableHostWindow::Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"QuattroTableAlternatingRowsHost";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"table alternating rows", WS_OVERLAPPEDWINDOW,
        120, 120, 400, 320, nullptr, nullptr, instance, &host);
    if (!hwnd || !host.table_) {
        state.Check(false, L"table-alternating-rows: host window/table creation failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    RedrawWindow(host.table_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    Sleep(200);

    // Row layout under TableRowState: row%2==0 -> "alternate", else "normal".
    // Row 0 is selected. Rows 1 (normal) and 2 (alternate) are unselected and
    // must differ from each other (the stripe) and neither may equal the
    // selected background (the regression guard).
    const RECT selectedRowRect = TableRowRectInHostBitmap(hwnd, host.table_, 0);
    const RECT normalRowRect = TableRowRectInHostBitmap(hwnd, host.table_, 1);
    const RECT alternateRowRect = TableRowRectInHostBitmap(hwnd, host.table_, 2);

    BitmapCapture capture = CaptureWindowBitmap(hwnd);
    state.Check(capture.bitmap != nullptr, L"table-alternating-rows: capture failed");
    if (capture.bitmap) {
        const COLORREF selectedColor = AverageBitmapRowColor(capture.bitmap, capture.width, selectedRowRect);
        const COLORREF normalColor = AverageBitmapRowColor(capture.bitmap, capture.width, normalRowRect);
        const COLORREF alternateColor = AverageBitmapRowColor(capture.bitmap, capture.width, alternateRowRect);

        const int stripeDistance = ColorDistance(normalColor, alternateColor);
        const int normalVsSelected = ColorDistance(normalColor, selectedColor);
        const int alternateVsSelected = ColorDistance(alternateColor, selectedColor);

        AcceptanceLog(L"table-alternating-rows stripe=" + std::to_wstring(stripeDistance) +
            L" normalVsSel=" + std::to_wstring(normalVsSelected) +
            L" altVsSel=" + std::to_wstring(alternateVsSelected));

        // Alternate and normal unselected rows must be visibly different.
        state.Check(stripeDistance >= 8,
            L"table-alternating-rows: adjacent unselected rows share the same background (zebra striping missing)");
        // Unselected rows must NOT be painted with the selected background — this
        // guards the CDIS_SELECTED custom-draw regression directly.
        state.Check(normalVsSelected >= 8,
            L"table-alternating-rows: an unselected normal row is painted with the selected background");

        const std::filesystem::path screenshot = outputDir / L"table-alternating-rows.png";
        SavePng(capture.bitmap, screenshot);
        DeleteObject(capture.bitmap);
    }

    DestroyWindow(hwnd);
    AcceptanceLog(L"end table-alternating-rows");
}

void RunWebDavFileColumnsScenario(const std::filesystem::path& outputDir, TestState& state, UINT dpi) {
    const std::wstring suffix = DpiPercentSuffix(dpi);
    const std::wstring scenario = L"webdav-file-columns-" + suffix;
    AcceptanceLog(L"begin " + scenario);
    const HWND foregroundBefore = GetForegroundWindow();
    const HWND activeBefore = GetActiveWindow();
    HINSTANCE instance = GetModuleHandleW(nullptr);
    TableHostWindow host;
    host.instance_ = instance;
    host.webDavColumns_ = true;
    host.forcedDpi_ = dpi;
    host.theme_ = Theme::Load(std::filesystem::current_path() / L"theme", L"default");

    const std::wstring className = L"QuattroWebDavFileColumnsHost" + suffix;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TableHostWindow::Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = className.c_str();
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, scenario.c_str(), WS_OVERLAPPEDWINDOW,
        120, 120, ThemedWindowUi::ScaleForDpi(760, dpi), ThemedWindowUi::ScaleForDpi(320, dpi),
        nullptr, nullptr, instance, &host);
    if (!hwnd || !host.table_) {
        state.Check(false, scenario + L": host window/table creation failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    Sleep(200);

    HWND header = ListView_GetHeader(host.table_);
    state.Check(header && Header_GetItemCount(header) == 5, scenario + L": expected five columns");
    const int pathWidth = ListView_GetColumnWidth(host.table_, 1);
    state.Check(pathWidth >= host.windowUi_->ui().tableColumnWidth(L"系统绝对路径"),
        scenario + L": remaining absolute-path column is not visibly wide");
    int actionId = 0;
    state.Check(ThemedControls::TableCellAction(host.table_, 0, 4, actionId) && actionId == 434,
        scenario + L": three-dot action cell is not invokable");
    RECT actionCell{};
    state.Check(ThemedUi::TableCellScreenRect(host.table_, 0, 4, actionCell),
        scenario + L": action cell screen rectangle is unavailable");
    const POINT actionCellCenter{
        (actionCell.left + actionCell.right) / 2,
        (actionCell.top + actionCell.bottom) / 2};
    state.Check(ThemedUi::TableScreenHitTest(host.table_, actionCellCenter) == 0,
        scenario + L": public screen-coordinate hit test did not resolve the action row");
    BitmapCapture capture = CaptureWindowBitmap(hwnd);
    state.Check(capture.bitmap != nullptr, scenario + L": capture failed");
    if (capture.bitmap) {
        state.Check(BitmapHasVisualContent(capture.bitmap, capture.width, capture.height),
            scenario + L": screenshot looks blank or too flat");
        SavePng(capture.bitmap, outputDir / (scenario + L".png"));
        DeleteObject(capture.bitmap);
    }
    HMENU actionMenu = CreatePopupMenu();
    AppendMenuW(actionMenu, MF_STRING, 431, L"下载");
    AppendMenuW(actionMenu, MF_STRING, 432, L"查看详情");
    AppendMenuW(actionMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(actionMenu, MF_STRING, 433, L"删除");
    wchar_t menuText[32]{};
    GetMenuStringW(actionMenu, 432, menuText, static_cast<int>(std::size(menuText)), MF_BYCOMMAND);
    state.Check(std::wstring(menuText) == L"查看详情", scenario + L": details menu item is missing");
    DestroyMenu(actionMenu);
    state.Check(GetForegroundWindow() == foregroundBefore, scenario + L": changed the foreground window");
    state.Check(GetActiveWindow() == activeBefore, scenario + L": changed the active window");
    DestroyWindow(hwnd);
    AcceptanceLog(L"end " + scenario);
}

void RunCheckableTableScenario(const std::filesystem::path& outputDir, TestState& state, UINT dpi) {
    AcceptanceLog(L"begin table-checkable-states dpi=" + std::to_wstring(dpi));
    HINSTANCE instance = GetModuleHandleW(nullptr);
    TableHostWindow host;
    host.instance_ = instance;
    host.checkable_ = true;
    host.showHeader_ = false;
    host.visibleRows_ = 4;
    host.forcedDpi_ = dpi;
    host.theme_ = Theme::Load(std::filesystem::current_path() / L"theme", L"default");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TableHostWindow::Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    const std::wstring className = L"QuattroCheckableTableHost" + std::to_wstring(dpi);
    wc.lpszClassName = className.c_str();
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"table checkable states", WS_OVERLAPPEDWINDOW,
        180, 180,
        ThemedWindowUi::ScaleForDpi(400, dpi),
        ThemedWindowUi::ScaleForDpi(240, dpi),
        nullptr, nullptr, instance, &host);
    if (!hwnd || !host.table_) {
        state.Check(false, L"table-checkable-states: host window/table creation failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    Sleep(150);

    RECT tableClient{};
    GetClientRect(host.table_, &tableClient);
    const int tableColumnWidth =
        ListView_GetColumnWidth(host.table_, 0) + ListView_GetColumnWidth(host.table_, 1);
    state.Check(tableColumnWidth <= tableClient.right - tableClient.left,
        L"table-checkable-states: public table columns exceed the client width");
    SCROLLINFO horizontalScroll{};
    horizontalScroll.cbSize = sizeof(horizontalScroll);
    horizontalScroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    if (GetScrollInfo(host.table_, SB_HORZ, &horizontalScroll)) {
        const int lastScrollablePosition = horizontalScroll.nMax
            - std::max<int>(static_cast<int>(horizontalScroll.nPage) - 1, 0);
        state.Check(lastScrollablePosition <= horizontalScroll.nMin,
            L"table-checkable-states: public table exposes a horizontal scroll range");
    }
    RECT row0{LVIR_BOUNDS, 0, 0, 0};
    RECT row2{LVIR_BOUNDS, 0, 0, 0};
    SendMessageW(host.table_, LVM_GETITEMRECT, 0, reinterpret_cast<LPARAM>(&row0));
    SendMessageW(host.table_, LVM_GETITEMRECT, 2, reinterpret_cast<LPARAM>(&row2));
    const int rowHeight = row0.bottom - row0.top;
    AcceptanceLog(L"table-checkable-states clientHeight=" + std::to_wstring(tableClient.bottom - tableClient.top)
        + L" rowHeight=" + std::to_wstring(rowHeight));
    state.Check(rowHeight > 0 && tableClient.bottom - tableClient.top == rowHeight * host.visibleRows_,
        L"table-checkable-states: visible-row helper does not end on an exact row boundary");

    HIMAGELIST stateImages = ListView_GetImageList(host.table_, LVSIL_STATE);
    int stateImageWidth = 0;
    int stateImageHeight = 0;
    if (stateImages) ImageList_GetIconSize(stateImages, &stateImageWidth, &stateImageHeight);
    AcceptanceLog(L"table-checkable-states stateImage=" + std::to_wstring(stateImageWidth)
        + L"x" + std::to_wstring(stateImageHeight));
    state.Check(stateImages && stateImageWidth > 16 && stateImageHeight + 1 == rowHeight,
        L"table-checkable-states: DPI-aware checkbox state hit area was not created");

    const bool disabledCheckedBefore = ThemedUi::IsTableChecked(host.table_, 2);
    const LPARAM disabledCheckPoint = MAKELPARAM(row2.left + 10, (row2.top + row2.bottom) / 2);
    SendMessageW(host.table_, WM_LBUTTONDOWN, MK_LBUTTON, disabledCheckPoint);
    SendMessageW(host.table_, WM_LBUTTONUP, 0, disabledCheckPoint);
    state.Check(disabledCheckedBefore && ThemedUi::IsTableChecked(host.table_, 2),
        L"table-checkable-states: disabled checkbox changed after mouse input");

    BitmapCapture beforeHover = CaptureWindowBitmap(hwnd);
    state.Check(beforeHover.bitmap != nullptr, L"table-checkable-states: initial capture failed");

    RECT selectedFirst = TableSubItemRectInHostBitmap(hwnd, host.table_, 1, 0);
    RECT selectedSecond = TableSubItemRectInHostBitmap(hwnd, host.table_, 1, 1);
    RECT uncheckedFirst = TableSubItemRectInHostBitmap(hwnd, host.table_, 1, 0);
    RECT hoverFirst = TableSubItemRectInHostBitmap(hwnd, host.table_, 0, 0);
    state.Check((GetWindowLongPtrW(host.table_, GWL_STYLE) & LVS_TYPEMASK) == LVS_REPORT,
        L"table-checkable-states: details table was created without LVS_REPORT style");
    if (beforeHover.bitmap) {
        const int selectedY = (selectedFirst.top + selectedFirst.bottom) / 2;
        const COLORREF firstSelectedBackground = BitmapPixel(
            beforeHover.bitmap, selectedFirst.right - 12, selectedY);
        const COLORREF secondSelectedBackground = BitmapPixel(
            beforeHover.bitmap, selectedSecond.right - 10, selectedY);
        state.Check(ColorDistance(firstSelectedBackground, secondSelectedBackground) <= 4,
            L"table-checkable-states: selected row uses different backgrounds across columns");

        const int boxCenterX = uncheckedFirst.left
            + ThemedWindowUi::ScaleForDpi(static_cast<int>(host.theme_.metric(L"listItem", L"paddingX", 8.0f)), dpi)
            + ThemedWindowUi::ScaleForDpi(static_cast<int>(host.theme_.metric(L"checkbox", L"boxSize", 16.0f)), dpi) / 2;
        const int boxCenterY = (uncheckedFirst.top + uncheckedFirst.bottom) / 2;
        state.Check(ColorDistance(BitmapPixel(beforeHover.bitmap, boxCenterX, boxCenterY), RGB(0, 0, 0)) >= 80,
            L"table-checkable-states: unchecked checkbox is rendered as a black block");

        const int checkedBoxCenterX = selectedFirst.left
            + ThemedWindowUi::ScaleForDpi(static_cast<int>(host.theme_.metric(L"listItem", L"paddingX", 8.0f)), dpi)
            + ThemedWindowUi::ScaleForDpi(static_cast<int>(host.theme_.metric(L"checkbox", L"boxSize", 16.0f)), dpi) / 2;
        const int checkedBoxCenterY = (selectedFirst.top + selectedFirst.bottom) / 2;
        const COLORREF checkedBoxColor = BitmapPixel(beforeHover.bitmap, checkedBoxCenterX, checkedBoxCenterY);
        state.Check(ColorDistance(checkedBoxColor, firstSelectedBackground) >= 8,
            L"table-checkable-states: first-column checkbox is not painted at the public column origin");
    }

    POINT hoverPoint{row0.right - 12, (row0.top + row0.bottom) / 2};
    SendMessageW(host.table_, WM_MOUSEMOVE, 0, MAKELPARAM(hoverPoint.x, hoverPoint.y));
    UpdateWindow(host.table_);
    Sleep(80);
    BitmapCapture hovered = CaptureWindowBitmap(hwnd);
    state.Check(hovered.bitmap != nullptr, L"table-checkable-states: hover capture failed");
    if (dpi == 96 && beforeHover.bitmap && hovered.bitmap) {
        const int hoverX = hoverFirst.right - 12;
        const int hoverY = (hoverFirst.top + hoverFirst.bottom) / 2;
        state.Check(ColorDistance(
                BitmapPixel(beforeHover.bitmap, hoverX, hoverY),
                BitmapPixel(hovered.bitmap, hoverX, hoverY)) >= 8,
            L"table-checkable-states: row hover has no visible themed state");
    }

    const std::wstring screenshotName = dpi == 96
        ? L"table-checkable-states.png"
        : (L"table-checkable-states-" + std::to_wstring(dpi * 100 / 96) + L".png");
    HBITMAP screenshotBitmap = dpi == 96 ? hovered.bitmap : beforeHover.bitmap;
    if (screenshotBitmap) SavePng(screenshotBitmap, outputDir / screenshotName);
    if (hovered.bitmap) DeleteObject(hovered.bitmap);
    if (beforeHover.bitmap) DeleteObject(beforeHover.bitmap);
    DestroyWindow(hwnd);
    AcceptanceLog(L"end table-checkable-states dpi=" + std::to_wstring(dpi));
}

// Count distinct vertical header dividers. A divider spans the header's full
// height while text glyphs stay inside the vertically centered text band, so
// only x positions that differ from the background near the top edge, at the
// center, and near the bottom edge count as dividers.
int CountHeaderDividers(HBITMAP bitmap, int captureWidth, HWND host, HWND table) {
    HWND header = ListView_GetHeader(table);
    RECT headerRect{};
    if (!header || !GetWindowRect(header, &headerRect)) {
        return -1;
    }
    RECT hostRect{};
    GetWindowRect(host, &hostRect);
    const int top = headerRect.top - hostRect.top;
    const int bottom = headerRect.bottom - hostRect.top;
    const int yTop = top + 3;
    const int yMiddle = (top + bottom) / 2;
    // Stay above the bottom border line the themed header draws across the
    // whole width.
    const int yBottom = bottom - 4;
    const int left = headerRect.left - hostRect.left;
    const int right = std::min(static_cast<int>(headerRect.right - hostRect.left), captureWidth);
    const COLORREF bg = BitmapPixel(bitmap, left + 2, yMiddle);
    int dividers = 0;
    bool inRun = false;
    for (int x = left + 2; x < right - 1; ++x) {
        const bool differs = ColorDistance(BitmapPixel(bitmap, x, yTop), bg) >= 8
            && ColorDistance(BitmapPixel(bitmap, x, yMiddle), bg) >= 8
            && ColorDistance(BitmapPixel(bitmap, x, yBottom), bg) >= 8;
        if (differs && !inRun) {
            ++dividers;
        }
        inRun = differs;
    }
    return dividers;
}

void RunTableColumnResizeScenario(const std::filesystem::path& outputDir, TestState& state) {
    AcceptanceLog(L"begin table-column-resize");
    HINSTANCE instance = GetModuleHandleW(nullptr);

    // Default options: dragging must be disabled via HDS_NOSIZING.
    {
        TableHostWindow defaultHost;
        defaultHost.instance_ = instance;
        defaultHost.theme_ = Theme::Load(std::filesystem::current_path() / L"theme", L"default");
        WNDCLASSEXW defaultWc{};
        defaultWc.cbSize = sizeof(defaultWc);
        defaultWc.lpfnWndProc = TableHostWindow::Proc;
        defaultWc.hInstance = instance;
        defaultWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        defaultWc.lpszClassName = L"QuattroTableColumnResizeDefaultHost";
        RegisterClassExW(&defaultWc);
        HWND defaultHwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, defaultWc.lpszClassName, L"table column resize default",
            WS_OVERLAPPEDWINDOW, 140, 140, 400, 320, nullptr, nullptr, instance, &defaultHost);
        state.Check(defaultHwnd && defaultHost.table_, L"table-column-resize: default host creation failed");
        if (defaultHwnd && defaultHost.table_) {
            HWND defaultHeader = ListView_GetHeader(defaultHost.table_);
            state.Check(defaultHeader
                    && (GetWindowLongPtrW(defaultHeader, GWL_STYLE) & HDS_NOSIZING) != 0,
                L"table-column-resize: default table header must disable divider dragging");
        }
        if (defaultHwnd) {
            DestroyWindow(defaultHwnd);
        }
    }

    TableHostWindow host;
    host.instance_ = instance;
    host.allowColumnResize_ = true;
    host.theme_ = Theme::Load(std::filesystem::current_path() / L"theme", L"default");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TableHostWindow::Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"QuattroTableColumnResizeHost";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"table column resize", WS_OVERLAPPEDWINDOW,
        140, 140, 400, 320, nullptr, nullptr, instance, &host);
    if (!hwnd || !host.table_) {
        state.Check(false, L"table-column-resize: host window/table creation failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    Sleep(200);

    HWND header = ListView_GetHeader(host.table_);
    state.Check(header != nullptr, L"table-column-resize: header not found");
    if (header) {
        // allowColumnResize=true must leave the header without HDS_NOSIZING so
        // the user can drag the divider.
        state.Check((GetWindowLongPtrW(header, GWL_STYLE) & HDS_NOSIZING) == 0,
            L"table-column-resize: header still carries HDS_NOSIZING with allowColumnResize enabled");
    }

    const int columnCount = header ? Header_GetItemCount(header) : 0;
    RECT tableClient{};
    GetClientRect(host.table_, &tableClient);

    // Simulate a completed header drag: apply the new width (what the header
    // does during the drag), then deliver HDN_ENDTRACK to the ListView the way
    // the header would. This exercises the shared subclass routing that refills
    // the Remaining column so the table keeps spanning its client width.
    const int firstWidthBefore = ListView_GetColumnWidth(host.table_, 0);
    SendMessageW(host.table_, LVM_SETCOLUMNWIDTH, 0, MAKELPARAM(firstWidthBefore - 40, 0));
    if (header) {
        NMHEADERW endTrack{};
        endTrack.hdr.hwndFrom = header;
        endTrack.hdr.idFrom = static_cast<UINT_PTR>(GetDlgCtrlID(header));
        endTrack.hdr.code = HDN_ENDTRACKW;
        endTrack.iItem = 0;
        HDITEMW item{};
        endTrack.pitem = &item;
        SendMessageW(host.table_, WM_NOTIFY, endTrack.hdr.idFrom, reinterpret_cast<LPARAM>(&endTrack));
    }
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    Sleep(100);

    BitmapCapture capture = CaptureWindowBitmap(hwnd);
    state.Check(capture.bitmap != nullptr, L"table-column-resize: capture failed");
    if (capture.bitmap) {
        const int dividers = CountHeaderDividers(capture.bitmap, capture.width, hwnd, host.table_);
        AcceptanceLog(L"table-column-resize columns=" + std::to_wstring(columnCount) +
            L" dividers=" + std::to_wstring(dividers));
        // The divider count must stay exactly column count - 1 after a width
        // change: one divider between each pair of adjacent columns and none at
        // the last column's right edge.
        state.Check(dividers == columnCount - 1,
            L"table-column-resize: header divider count is not column count - 1 after column width change");

        // The trailing area right of the last column must show no divider: the
        // relayout should have refilled the full client width, so the last
        // column's right edge coincides with the table client edge.
        int columnsWidth = 0;
        for (int i = 0; i < columnCount; ++i) {
            columnsWidth += ListView_GetColumnWidth(host.table_, i);
        }
        AcceptanceLog(L"table-column-resize clientWidth=" + std::to_wstring(tableClient.right) +
            L" columnsWidth=" + std::to_wstring(columnsWidth));
        state.Check(columnsWidth >= tableClient.right - 1 && columnsWidth <= tableClient.right + 1,
            L"table-column-resize: columns do not refill the table width after resize");

        const std::filesystem::path screenshot = outputDir / L"table-column-resize.png";
        SavePng(capture.bitmap, screenshot);
        DeleteObject(capture.bitmap);
    }

    DestroyWindow(hwnd);
    AcceptanceLog(L"end table-column-resize");
}

// The cursor entering or leaving the table used to invalidate the whole
// ListView with a background erase, which flickered every time the mouse
// crossed the list edge, the header, or the scrollbar. Assert that a simulated
// enter (WM_MOUSEMOVE) and leave (WM_MOUSELEAVE) only invalidate at most one
// row's rect and never request an erase of the full client area.
void RunTableHoverRepaintScenario(TestState& state) {
    AcceptanceLog(L"begin table-hover-repaint");
    HINSTANCE instance = GetModuleHandleW(nullptr);

    TableHostWindow host;
    host.instance_ = instance;
    host.theme_ = Theme::Load(std::filesystem::current_path() / L"theme", L"default");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TableHostWindow::Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"QuattroTableHoverRepaintHost";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"table hover repaint", WS_OVERLAPPEDWINDOW,
        140, 140, 400, 320, nullptr, nullptr, instance, &host);
    if (!hwnd || !host.table_) {
        state.Check(false, L"table-hover-repaint: host window/table creation failed");
        return;
    }
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(hwnd);
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    Sleep(100);

    RECT tableClient{};
    GetClientRect(host.table_, &tableClient);
    RECT rowRect{LVIR_BOUNDS, 0, 0, 0};
    SendMessageW(host.table_, LVM_GETITEMRECT, 1, reinterpret_cast<LPARAM>(&rowRect));
    const int rowHeight = rowRect.bottom - rowRect.top;
    state.Check(rowHeight > 0, L"table-hover-repaint: row rect unavailable");

    auto updateHeightAfter = [&](UINT message, LPARAM lParam) {
        ValidateRect(host.table_, nullptr);
        SendMessageW(host.table_, message, 0, lParam);
        RECT update{};
        // FALSE: do not send WM_ERASEBKGND, just measure the pending region.
        if (!GetUpdateRect(host.table_, &update, FALSE)) {
            return 0;
        }
        return static_cast<int>(update.bottom - update.top);
    };

    // Enter: cursor lands in the middle of row 1.
    const LPARAM rowPoint = MAKELPARAM((rowRect.left + rowRect.right) / 2,
                                       (rowRect.top + rowRect.bottom) / 2);
    const int enterHeight = updateHeightAfter(WM_MOUSEMOVE, rowPoint);
    // Leave: only the previously hot cell may repaint.
    const int leaveHeight = updateHeightAfter(WM_MOUSELEAVE, 0);

    AcceptanceLog(L"table-hover-repaint rowHeight=" + std::to_wstring(rowHeight) +
        L" enterHeight=" + std::to_wstring(enterHeight) +
        L" leaveHeight=" + std::to_wstring(leaveHeight));
    state.Check(enterHeight <= rowHeight + 2,
        L"table-hover-repaint: mouse enter invalidates more than the hovered row");
    state.Check(leaveHeight <= rowHeight + 2,
        L"table-hover-repaint: mouse leave invalidates more than the hovered row");
    state.Check(enterHeight < tableClient.bottom && leaveHeight < tableClient.bottom,
        L"table-hover-repaint: enter/leave invalidates the whole table client area");

    DestroyWindow(hwnd);
    AcceptanceLog(L"end table-hover-repaint");
}

void ValidateAppLaunchLockerTables(HWND hwnd, TestState& state) {
    const auto children = Children(hwnd);
    bool sawTable = false;
    bool sawNoSizingHeader = false;
    for (const auto& child : children) {
        if (child.className != L"SysListView32" || !IsWindowVisible(child.hwnd)) {
            continue;
        }
        sawTable = true;
        HWND header = reinterpret_cast<HWND>(SendMessageW(child.hwnd, LVM_GETHEADER, 0, 0));
        if (header && (GetWindowLongPtrW(header, GWL_STYLE) & HDS_NOSIZING) != 0) {
            sawNoSizingHeader = true;
        }
    }
    // NOTE: the alternating-row background is verified in-process by
    // RunTableAlternatingRowsScenario. It cannot be asserted reliably here
    // because LVM_GETITEMRECT / LVM_SETITEMSTATE carry pointer parameters that
    // Windows does not marshal across process boundaries, so querying this
    // separate AppLaunchLocker.exe process returns empty rects and stale state.
    state.Check(sawTable, L"app-launch-locker: table controls not found");
    // The launcher tables opt into allowColumnResize, so no header may carry
    // HDS_NOSIZING; the dividers must stay user-draggable.
    state.Check(!sawNoSizingHeader, L"app-launch-locker: table header unexpectedly disables column resizing");
}

void RunAppLaunchLockerScenario(const std::filesystem::path& outputDir, TestState& state, UINT dpi) {
    const std::wstring dpiSuffix = DpiPercentSuffix(dpi);
    AcceptanceLog(L"begin app-launch-locker-" + dpiSuffix);
    wchar_t executableOverride[32768]{};
    const DWORD overrideLength = GetEnvironmentVariableW(
        L"QUATTRO_UI_ACCEPTANCE_APP_LAUNCH_LOCKER",
        executableOverride,
        static_cast<DWORD>(std::size(executableOverride)));
    const std::filesystem::path exe = overrideLength > 0 && overrideLength < std::size(executableOverride)
        ? std::filesystem::path(executableOverride)
        : ModuleDirectory() / L"AppLaunchLocker.exe";
    if (!std::filesystem::exists(exe)) {
        state.Check(false, L"app-launch-locker: AppLaunchLocker.exe not found beside acceptance executable");
        AcceptanceLog(L"missing app-launch-locker exe");
        return;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + exe.wstring() + L"\"";
    ScopedAcceptanceChildEnvironment childEnvironment;
    const std::wstring dpiText = std::to_wstring(dpi);
    SetEnvironmentVariableW(L"QUATTRO_APP_LAUNCH_LOCKER_ACCEPTANCE_DPI", dpiText.c_str());
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, ModuleDirectory().c_str(), &startup, &process)) {
        SetEnvironmentVariableW(L"QUATTRO_APP_LAUNCH_LOCKER_ACCEPTANCE_DPI", nullptr);
        state.Check(false, L"app-launch-locker: CreateProcess failed");
        AcceptanceLog(L"create app-launch-locker failed");
        return;
    }
    SetEnvironmentVariableW(L"QUATTRO_APP_LAUNCH_LOCKER_ACCEPTANCE_DPI", nullptr);

    WaitForInputIdle(process.hProcess, 10000);
    AcceptanceLog(L"wait app-launch-locker");
    HWND hwnd = WaitForTopWindow(FindWindowRequest{L"AppLaunchLockerMainWindow", L"自启动管理", process.dwProcessId}, 10000);
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        RECT initialRect{};
        GetWindowRect(hwnd, &initialRect);
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        int captureX = initialRect.left;
        int captureY = initialRect.top;
        if (GetMonitorInfoW(monitor, &monitorInfo)) {
            const int width = initialRect.right - initialRect.left;
            const int height = initialRect.bottom - initialRect.top;
            captureX = std::clamp<int>(
                monitorInfo.rcWork.left + 16,
                monitorInfo.rcWork.left,
                std::max<int>(monitorInfo.rcWork.left, monitorInfo.rcWork.right - width));
            captureY = std::clamp<int>(
                monitorInfo.rcWork.top + 96,
                monitorInfo.rcWork.top,
                std::max<int>(monitorInfo.rcWork.top, monitorInfo.rcWork.bottom - height));
        }
        SetWindowPos(hwnd, HWND_BOTTOM, captureX, captureY, 0, 0,
            SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        UpdateWindow(hwnd);
        RECT windowRect{};
        RECT clientRect{};
        GetWindowRect(hwnd, &windowRect);
        GetClientRect(hwnd, &clientRect);
        AcceptanceLog(L"app-launch-locker hwnd=" + std::to_wstring(reinterpret_cast<std::uintptr_t>(hwnd)) +
            L" rect=" + std::to_wstring(windowRect.left) + L"," + std::to_wstring(windowRect.top) +
            L"," + std::to_wstring(windowRect.right) + L"," + std::to_wstring(windowRect.bottom) +
            L" client=" + std::to_wstring(clientRect.right - clientRect.left) +
            L"x" + std::to_wstring(clientRect.bottom - clientRect.top) +
            L" children=" + std::to_wstring(Children(hwnd).size()));
        AcceptanceLog(L"wait app-launch-locker table");
        HWND listView = WaitForChildClass(hwnd, L"SysListView32", 10000);
        if (!listView) {
            state.Check(false, L"app-launch-locker: table controls not found before capture");
        } else {
            WaitForListViewRows(listView, 2, 10000);
        }
        RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        Sleep(300);
        AcceptanceLog(L"inspect app-launch-locker");
        ValidateAppLaunchLockerTables(hwnd, state);
        state.Check(WindowText(hwnd) == L"自启动管理", L"app-launch-locker: title mismatch");
        int buttonCount = 0;
        for (const auto& child : Children(hwnd)) {
            if (child.className == L"Button" && IsWindowVisible(child.hwnd)) {
                ++buttonCount;
            }
        }
        state.Check(buttonCount >= 2, L"app-launch-locker: visible button count lower than expected");
        // Keep cross-process structural assertions separate from the visual
        // capture. The target HWND remains non-activating and behind unrelated
        // programs; PrintWindow must produce a valid bitmap without a desktop
        // pixel fallback or temporary topmost transition.
        UpdateWindow(hwnd);
        Sleep(80);
        BitmapCapture capture = CaptureWindowBitmap(hwnd);
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        state.Check(capture.bitmap != nullptr, L"app-launch-locker: screenshot bitmap was not created");
        if (capture.bitmap) {
            state.Check(BitmapHasVisualContent(capture.bitmap, capture.width, capture.height),
                L"app-launch-locker: screenshot looks blank or too flat");
            SavePng(capture.bitmap, outputDir / (L"app-launch-locker-" + dpiSuffix + L".png"));
            DeleteObject(capture.bitmap);
        }
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    } else {
        state.Check(false, L"app-launch-locker: window did not appear");
        AcceptanceLog(L"missing app-launch-locker window");
    }

    if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 2);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    AcceptanceLog(L"end app-launch-locker-" + dpiSuffix);
}

void RunAdBlockScenario(const std::filesystem::path& outputDir, TestState& state) {
    AcceptanceLog(L"begin ad-block");
    const Theme validationTheme = Theme::Load(
        std::filesystem::current_path() / L"theme", L"default");
    const std::filesystem::path progressRoot = outputDir / L"ad-block-progress-input";
    std::error_code progressError;
    std::filesystem::create_directories(progressRoot / L"nested", progressError);
    for (int index = 0; index < 256; ++index) {
        const std::filesystem::path parent = index % 2 == 0 ? progressRoot : progressRoot / L"nested";
        std::ofstream(parent / (L"candidate-" + std::to_wstring(index) + L".exe"), std::ios::binary)
            << "not-a-real-pe";
    }
    wchar_t executableOverride[32768]{};
    const DWORD overrideLength = GetEnvironmentVariableW(
        L"QUATTRO_UI_ACCEPTANCE_APP_LAUNCH_LOCKER",
        executableOverride,
        static_cast<DWORD>(std::size(executableOverride)));
    const std::filesystem::path exe = overrideLength > 0 && overrideLength < std::size(executableOverride)
        ? std::filesystem::path(executableOverride)
        : ModuleDirectory() / L"AppLaunchLocker.exe";
    if (!std::filesystem::exists(exe)) {
        state.Check(false, L"ad-block: AppLaunchLocker.exe not found beside acceptance executable");
        return;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = L"\"" + exe.wstring() + L"\" --ad-block";
    ScopedAcceptanceChildEnvironment childEnvironment;
    SetEnvironmentVariableW(L"QUATTRO_AD_BLOCK_ACCEPTANCE_DPI", L"96");
    SetEnvironmentVariableW(L"QUATTRO_TEST_AD_BLOCK_BATCH_DELAY_MS", L"200");
    SetEnvironmentVariableW(L"QUATTRO_TEST_AD_BLOCK_SHOW_CONFIRMATION", L"1");
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
            ModuleDirectory().c_str(), &startup, &process)) {
        SetEnvironmentVariableW(L"QUATTRO_AD_BLOCK_ACCEPTANCE_DPI", nullptr);
        SetEnvironmentVariableW(L"QUATTRO_TEST_AD_BLOCK_BATCH_DELAY_MS", nullptr);
        SetEnvironmentVariableW(L"QUATTRO_TEST_AD_BLOCK_SHOW_CONFIRMATION", nullptr);
        state.Check(false, L"ad-block: CreateProcess failed");
        return;
    }
    SetEnvironmentVariableW(L"QUATTRO_AD_BLOCK_ACCEPTANCE_DPI", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_AD_BLOCK_BATCH_DELAY_MS", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_AD_BLOCK_SHOW_CONFIRMATION", nullptr);

    WaitForInputIdle(process.hProcess, 10000);
    HWND hwnd = WaitForTopWindow(FindWindowRequest{L"AdBlockMainWindow", L"广告拦截", process.dwProcessId}, 10000);
    auto capture = [&](HWND target, const wchar_t* fileName, const std::wstring& scenario) {
        RedrawWindow(target, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        Sleep(200);
        BitmapCapture bitmap = CaptureWindowBitmap(target);
        state.Check(bitmap.bitmap != nullptr, scenario + L": screenshot bitmap was not created");
        if (bitmap.bitmap) {
            state.Check(BitmapHasVisualContent(bitmap.bitmap, bitmap.width, bitmap.height),
                scenario + L": screenshot looks blank or too flat");
            state.Check(SavePng(bitmap.bitmap, outputDir / fileName), scenario + L": screenshot save failed");
            DeleteObject(bitmap.bitmap);
        }
    };

    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        UpdateWindow(hwnd);

        HWND pathEdit = ChildById(hwnd, 1210);
        HWND pickPath = ChildById(hwnd, 1211);
        HWND pickPathMenu = ChildById(hwnd, 1218);
        HWND clearResults = ChildById(hwnd, 1217);
        HWND checkPath = ChildById(hwnd, 1222);
        HWND tabControl = ChildById(hwnd, 1200);
        HWND contentPanel = ChildById(hwnd, 1203);
        HWND exactMode = ChildById(hwnd, 1213);
        HWND nameMode = ChildById(hwnd, 1214);
        HWND scanTable = ChildById(hwnd, 1215);
        HWND blockButton = ChildById(hwnd, 1216);
        state.Check(pathEdit && pickPath && pickPathMenu && clearResults && checkPath && tabControl && contentPanel && exactMode && nameMode && scanTable && blockButton,
            L"ad-block: block page controls are incomplete");
        state.Check(!pathEdit || (IsWindowVisible(pathEdit) && IsWindowVisible(pickPath) && IsWindowVisible(pickPathMenu) && IsWindowVisible(clearResults)),
            L"ad-block: connected panel obscured the path action row");
        if (tabControl && contentPanel) {
            RECT tabRect{};
            RECT panelRect{};
            GetWindowRect(tabControl, &tabRect);
            GetWindowRect(contentPanel, &panelRect);
            state.Check(panelRect.top <= tabRect.bottom && tabRect.bottom - panelRect.top <= 3,
                L"ad-block: connected tabs are not attached to the content panel");
            state.Check(panelRect.left == tabRect.left && panelRect.right == tabRect.right,
                L"ad-block: connected tab and content panel widths differ");
            state.Check(GetWindow(contentPanel, GW_HWNDNEXT) == nullptr,
                L"ad-block: connected content panel is above page controls in z-order");
            SCROLLINFO panelScroll{sizeof(panelScroll), SIF_RANGE | SIF_PAGE | SIF_POS};
            GetScrollInfo(contentPanel, SB_VERT, &panelScroll);
            state.Check((GetWindowLongPtrW(contentPanel, GWL_STYLE) & WS_VSCROLL) == 0 &&
                    panelScroll.nMin == 0 && panelScroll.nMax == 0 && panelScroll.nPage == 0,
                L"ad-block: non-scrollable content panel exposes a vertical scrollbar range");
        }
        if (contentPanel && pathEdit && scanTable) {
            RECT panelRect{};
            RECT pathRect{};
            RECT tableRect{};
            GetWindowRect(contentPanel, &panelRect);
            GetWindowRect(pathEdit, &pathRect);
            GetWindowRect(scanTable, &tableRect);
            state.Check(pathRect.left > panelRect.left && pathRect.right < panelRect.right &&
                    tableRect.left > panelRect.left && tableRect.right < panelRect.right,
                L"ad-block: connected content controls do not respect panel insets");
        }
        state.Check(pickPath && WindowText(pickPath) == L"文件",
            L"ad-block: file picker split-button label is incorrect");
        state.Check(pickPathMenu && WindowText(pickPathMenu).empty(),
            L"ad-block: folder picker menu segment should use only the public icon");
        ValidateSplitButtonChevron(
            pickPathMenu, validationTheme, 96, state, L"ad-block 100% DPI", false);
        state.Check(clearResults && WindowText(clearResults) == L"清空",
            L"ad-block: clear-results action label is incorrect");
        state.Check(checkPath && WindowText(checkPath) == L"检查",
            L"ad-block: check action label is incorrect");
        state.Check(!clearResults || !IsWindowEnabled(clearResults),
            L"ad-block: clear-results action should start disabled");
        if (pathEdit && clearResults && pickPath && pickPathMenu && checkPath) {
            RECT pathRect{};
            RECT clearRect{};
            RECT pickRect{};
            RECT menuRect{};
            RECT checkRect{};
            GetWindowRect(pathEdit, &pathRect);
            GetWindowRect(clearResults, &clearRect);
            GetWindowRect(pickPath, &pickRect);
            GetWindowRect(pickPathMenu, &menuRect);
            GetWindowRect(checkPath, &checkRect);
            state.Check(pathRect.right < clearRect.left && clearRect.right < pickRect.left &&
                    pickRect.right <= menuRect.left && menuRect.right < checkRect.left,
                L"ad-block: path, clear, file picker, folder menu, and check controls overlap or are out of order");
            state.Check(std::max(pathRect.top, clearRect.top) < std::min(pathRect.bottom, clearRect.bottom) &&
                    std::max(clearRect.top, pickRect.top) < std::min(clearRect.bottom, pickRect.bottom) &&
                    std::max(pickRect.top, menuRect.top) < std::min(pickRect.bottom, menuRect.bottom) &&
                    std::max(menuRect.top, checkRect.top) < std::min(menuRect.bottom, checkRect.bottom),
                L"ad-block: path action controls are not aligned on one row");
        }

        auto validateActionRow = [&](const std::wstring& scenario) {
            if (!exactMode || !nameMode || !blockButton) return;
            RECT exactRect{};
            RECT nameRect{};
            RECT buttonRect{};
            GetWindowRect(exactMode, &exactRect);
            GetWindowRect(nameMode, &nameRect);
            GetWindowRect(blockButton, &buttonRect);
            state.Check(exactRect.left < nameRect.left && nameRect.right < buttonRect.left,
                scenario + L": blocking modes are not left of the primary action");
            state.Check(std::max(exactRect.top, buttonRect.top) < std::min(exactRect.bottom, buttonRect.bottom),
                scenario + L": blocking modes and primary action are not on the same row");
        };

        validateActionRow(L"ad-block 100% DPI");
        capture(hwnd, L"ad-block-block-page.png", L"ad-block block page");

        HWND statusText = nullptr;
        for (const auto& child : Children(hwnd)) {
            if (child.text == L"输入或选择文件、文件夹后点击“检查”。") {
                statusText = child.hwnd;
                break;
            }
        }
        state.Check(statusText != nullptr, L"ad-block: scan status text is missing");
        if (pathEdit && statusText && clearResults && checkPath) {
            for (const wchar_t character : progressRoot.wstring()) {
                DWORD_PTR typeResult = 0;
                SendMessageTimeoutW(pathEdit, WM_CHAR, character, 1,
                    SMTO_ABORTIFHUNG, 5000, &typeResult);
            }
            DWORD_PTR checkResult = 0;
            SendMessageTimeoutW(checkPath, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 5000, &checkResult);
            HWND progressWindow = WaitForTopWindow(
                FindWindowRequest{L"", L"广告拦截检查进度", process.dwProcessId}, 5000);
            state.Check(progressWindow != nullptr, L"ad-block: progress dialog did not appear");
            if (progressWindow) {
                ShowWindow(progressWindow, SW_SHOWNOACTIVATE);
                SetWindowPos(progressWindow, HWND_BOTTOM, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                state.Check(WaitForWindowText(progressWindow, L"个工作线程", 5000),
                    L"ad-block: progress dialog did not report parallel workers");
                state.Check(WindowContainsText(progressWindow, L"已注册开机/登录自启动"),
                    L"ad-block: progress dialog did not distinguish registered auto-start entries");
                state.Check(WindowText(checkPath) == L"查看进度",
                    L"ad-block: running check action did not switch to view-progress state");
                capture(progressWindow, L"ad-block-progress-running.png", L"ad-block running progress");
            }
            const auto scanBegin = GetTickCount64();
            while (!Contains(WindowText(statusText), L"检查完成") &&
                   GetTickCount64() - scanBegin < 10000) {
                Sleep(50);
            }
            state.Check(Contains(WindowText(statusText), L"检查完成") &&
                    Contains(WindowText(statusText), L"个可启动程序") &&
                    Contains(WindowText(statusText), L"已注册开机/登录自启动"),
                L"ad-block: check button did not complete recursive directory checking");
            const auto checkLabelBegin = GetTickCount64();
            while (WindowText(checkPath) != L"检查" && GetTickCount64() - checkLabelBegin < 2000) {
                Sleep(25);
            }
            state.Check(WindowText(checkPath) == L"检查",
                L"ad-block: check action did not return to its idle label");
            HWND confirmationWindow = WaitForTopWindow(
                FindWindowRequest{L"QuattroCommonThemedMessageBox", L"确认拦截", process.dwProcessId}, 5000);
            state.Check(confirmationWindow != nullptr, L"ad-block: confirmation dialog did not appear");
            if (confirmationWindow) {
                ShowWindow(confirmationWindow, SW_SHOWNOACTIVATE);
                SetWindowPos(confirmationWindow, HWND_BOTTOM, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                state.Check(WindowContainsText(confirmationWindow, L"确定拦截所选 1 个程序？") &&
                        WindowContainsText(confirmationWindow, L"模式：精确路径（仅拦截所选文件）") &&
                        WindowContainsText(confirmationWindow, L"需要管理员权限；可随时在“已拦截”中解除。"),
                    L"ad-block: confirmation dialog text is incomplete");
                capture(confirmationWindow, L"ad-block-confirmation.png", L"ad-block confirmation");
                HWND noButton = ChildById(confirmationWindow, IDNO);
                if (noButton) {
                    DWORD_PTR noResult = 0;
                    SendMessageTimeoutW(noButton, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 5000, &noResult);
                }
            }
            state.Check(IsWindowEnabled(clearResults) != FALSE,
                L"ad-block: clear-results action was not enabled after scanning");
            if (progressWindow) {
                state.Check(WaitForWindowText(progressWindow, L"检查完成", 3000),
                    L"ad-block: progress dialog did not enter completed state");
                state.Check(WindowContainsText(progressWindow, L"个可启动程序") &&
                        WindowContainsText(progressWindow, L"已注册开机/登录自启动"),
                    L"ad-block: completed progress summary is ambiguous");
                capture(progressWindow, L"ad-block-progress-completed.png", L"ad-block completed progress");
                HWND closeProgress = ChildById(progressWindow, 3);
                if (closeProgress) {
                    DWORD_PTR closeResult = 0;
                    SendMessageTimeoutW(closeProgress, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 5000, &closeResult);
                }
            }
            PostMessageW(hwnd, WM_APP + 0x164, 0, 0);
            Sleep(500);
            HWND unexpectedProgress = FindTopWindow(
                FindWindowRequest{L"", L"广告拦截检查进度", process.dwProcessId});
            state.Check(unexpectedProgress == nullptr,
                L"ad-block: operation completion unexpectedly started another check");
            state.Check(WindowText(checkPath) == L"检查",
                L"ad-block: operation completion changed the explicit check action state");
            capture(hwnd, L"ad-block-manual-path-result.png", L"ad-block manual path result");

            DWORD_PTR clearResult = 0;
            SendMessageTimeoutW(clearResults, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 5000, &clearResult);
            Sleep(200);
            state.Check(WindowText(statusText) == L"输入或选择文件、文件夹后点击“检查”。",
                L"ad-block: clearing displayed results did not restore the initial status");
            state.Check(WindowText(pathEdit).empty(),
                L"ad-block: clearing did not clear the entered path");
            state.Check(IsWindowEnabled(clearResults) == FALSE,
                L"ad-block: clear-results action remained enabled after clearing");
            capture(hwnd, L"ad-block-cleared-results.png", L"ad-block cleared results");

        }

        HWND blockedTab = ChildById(hwnd, 1202);
        state.Check(blockedTab != nullptr, L"ad-block: blocked tab is missing");
        if (blockedTab) {
            DWORD_PTR tabResult = 0;
            SendMessageTimeoutW(blockedTab, BM_CLICK, 0, 0, SMTO_ABORTIFHUNG, 5000, &tabResult);
            Sleep(500);
            HWND blockedTable = ChildById(hwnd, 1220);
            HWND unblockButton = ChildById(hwnd, 1221);
            state.Check(blockedTable && IsWindowVisible(blockedTable),
                L"ad-block: blocked table did not become visible");
            state.Check(unblockButton && IsWindowVisible(unblockButton),
                L"ad-block: unblock action did not become visible");
            state.Check(!exactMode || !IsWindowVisible(exactMode),
                L"ad-block: blocking mode remained visible on blocked page");
            capture(hwnd, L"ad-block-blocked-page.png", L"ad-block blocked page");
        }

        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    } else {
        state.Check(false, L"ad-block: window did not appear");
    }

    if (WaitForSingleObject(process.hProcess, 5000) == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 2);
    }
    const std::filesystem::path appLogPath = childEnvironment.root() / L"logs" / L"app.log";
    const std::wstring appLog = LoadUtf8File(appLogPath);
    state.Check(!appLog.empty(), L"ad-block: AppLaunchLocker did not create the isolated app.log");
    state.Check(Contains(appLog, L"AppLaunchLocker GUI 启动: mode=ad-block"),
        L"ad-block: AppLaunchLocker GUI startup log is missing");
    state.Check(Contains(appLog, L"AppLaunchLocker OLE 初始化: hr=0x0"),
        L"ad-block: AppLaunchLocker did not initialize the GUI thread as an OLE/STA apartment");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    for (const UINT dpi : {120u, 144u}) {
        STARTUPINFOW dpiStartup{};
        dpiStartup.cb = sizeof(dpiStartup);
        PROCESS_INFORMATION dpiProcess{};
        std::wstring dpiCommand = L"\"" + exe.wstring() + L"\" --ad-block";
        const std::wstring dpiText = std::to_wstring(dpi);
        SetEnvironmentVariableW(L"QUATTRO_AD_BLOCK_ACCEPTANCE_DPI", dpiText.c_str());
        const BOOL started = CreateProcessW(exe.c_str(), dpiCommand.data(), nullptr, nullptr, FALSE, 0, nullptr,
            ModuleDirectory().c_str(), &dpiStartup, &dpiProcess);
        SetEnvironmentVariableW(L"QUATTRO_AD_BLOCK_ACCEPTANCE_DPI", nullptr);
        state.Check(started != FALSE, L"ad-block: DPI acceptance process failed to start");
        if (!started) continue;
        WaitForInputIdle(dpiProcess.hProcess, 10000);
        HWND dpiWindow = WaitForTopWindow(
            FindWindowRequest{L"AdBlockMainWindow", L"广告拦截", dpiProcess.dwProcessId}, 10000);
        state.Check(dpiWindow != nullptr, L"ad-block: DPI acceptance window did not appear");
        if (dpiWindow) {
            ShowWindow(dpiWindow, SW_SHOWNOACTIVATE);
            SetWindowPos(dpiWindow, HWND_BOTTOM, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            UpdateWindow(dpiWindow);
            HWND exactMode = ChildById(dpiWindow, 1213);
            HWND nameMode = ChildById(dpiWindow, 1214);
            HWND tabControl = ChildById(dpiWindow, 1200);
            HWND contentPanel = ChildById(dpiWindow, 1203);
            HWND pathEdit = ChildById(dpiWindow, 1210);
            HWND pickPath = ChildById(dpiWindow, 1211);
            HWND pickPathMenu = ChildById(dpiWindow, 1218);
            HWND clearResults = ChildById(dpiWindow, 1217);
            HWND checkPath = ChildById(dpiWindow, 1222);
            HWND scanTable = ChildById(dpiWindow, 1215);
            HWND blockButton = ChildById(dpiWindow, 1216);
            state.Check(exactMode && nameMode && tabControl && contentPanel && pathEdit && pickPath && pickPathMenu && clearResults && checkPath && scanTable && blockButton,
                L"ad-block: DPI action row controls are incomplete");
            ValidateSplitButtonChevron(
                pickPathMenu,
                validationTheme,
                dpi,
                state,
                L"ad-block " + DpiPercentSuffix(dpi) + L"% DPI",
                false);
            if (tabControl && contentPanel) {
                RECT tabRect{};
                RECT panelRect{};
                GetWindowRect(tabControl, &tabRect);
                GetWindowRect(contentPanel, &panelRect);
                state.Check(panelRect.top <= tabRect.bottom && tabRect.bottom - panelRect.top <= 5,
                    L"ad-block: DPI connected tabs detached from the content panel");
                state.Check(panelRect.left == tabRect.left && panelRect.right == tabRect.right,
                    L"ad-block: DPI connected tab and panel widths differ");
            }
            if (pathEdit && clearResults && pickPath && pickPathMenu && checkPath) {
                RECT pathRect{};
                RECT clearRect{};
                RECT pickRect{};
                RECT menuRect{};
                RECT checkRect{};
                RECT tableRect{};
                GetWindowRect(pathEdit, &pathRect);
                GetWindowRect(clearResults, &clearRect);
                GetWindowRect(pickPath, &pickRect);
                GetWindowRect(pickPathMenu, &menuRect);
                GetWindowRect(checkPath, &checkRect);
                GetWindowRect(scanTable, &tableRect);
                state.Check(pathRect.right < clearRect.left && clearRect.right < pickRect.left &&
                        pickRect.right <= menuRect.left && menuRect.right < checkRect.left,
                    L"ad-block: DPI path row controls overlap or are out of order");
                state.Check(pathRect.bottom <= tableRect.top && clearRect.bottom <= tableRect.top &&
                        pickRect.bottom <= tableRect.top && menuRect.bottom <= tableRect.top && checkRect.bottom <= tableRect.top,
                    L"ad-block: DPI connected panel moved the path row behind the result table");
                bool visibleEditFrame = false;
                for (const auto& child : Children(dpiWindow)) {
                    if (child.className != L"QuattroThemedEditFrame" || !IsWindowVisible(child.hwnd)) continue;
                    RECT frameRect{};
                    GetWindowRect(child.hwnd, &frameRect);
                    if (frameRect.left <= pathRect.left && frameRect.right >= pathRect.right &&
                            frameRect.top <= pathRect.top && frameRect.bottom >= pathRect.bottom) {
                        visibleEditFrame = true;
                        break;
                    }
                }
                state.Check(visibleEditFrame,
                    L"ad-block: DPI path edit lost its public themed frame before the window became visible");
            }
            if (exactMode && nameMode && blockButton) {
                RECT exactRect{};
                RECT nameRect{};
                RECT buttonRect{};
                GetWindowRect(exactMode, &exactRect);
                GetWindowRect(nameMode, &nameRect);
                GetWindowRect(blockButton, &buttonRect);
                state.Check(exactRect.left < nameRect.left && nameRect.right < buttonRect.left,
                    L"ad-block: DPI action row overlaps horizontally");
                state.Check(std::max(exactRect.top, buttonRect.top) < std::min(exactRect.bottom, buttonRect.bottom),
                    L"ad-block: DPI action row is not vertically aligned");
            }
            RedrawWindow(dpiWindow, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            Sleep(200);
            BitmapCapture bitmap = CaptureWindowBitmap(dpiWindow);
            state.Check(bitmap.bitmap != nullptr, L"ad-block: DPI screenshot bitmap was not created");
            if (bitmap.bitmap) {
                const std::wstring fileName = dpi == 120 ? L"ad-block-block-page-125.png" : L"ad-block-block-page-150.png";
                state.Check(BitmapHasVisualContent(bitmap.bitmap, bitmap.width, bitmap.height),
                    L"ad-block: DPI screenshot looks blank or too flat");
                state.Check(SavePng(bitmap.bitmap, outputDir / fileName), L"ad-block: DPI screenshot save failed");
                DeleteObject(bitmap.bitmap);
            }
            SetWindowPos(dpiWindow, HWND_BOTTOM, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            PostMessageW(dpiWindow, WM_CLOSE, 0, 0);
        }
        if (WaitForSingleObject(dpiProcess.hProcess, 5000) == WAIT_TIMEOUT) {
            TerminateProcess(dpiProcess.hProcess, 2);
        }
        CloseHandle(dpiProcess.hThread);
        CloseHandle(dpiProcess.hProcess);
    }
    AcceptanceLog(L"end ad-block");
}

std::wstring DpiPercentSuffix(UINT dpi) {
    if (dpi == 120) return L"125";
    if (dpi == 144) return L"150";
    return L"100";
}

void PumpModelessBuiltinTool(HWND hwnd) {
    MSG message{};
    while (IsWindow(hwnd)) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) {
                PostQuitMessage(static_cast<int>(message.wParam));
            }
            break;
        }
        if (ThemedUi::PreTranslateMessage(message) || PreTranslateBuiltinToolMessage(message)) {
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void OpenAndPumpModelessBuiltinTool(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    PluginRegistry& registry,
    const AppConfig& config,
    const std::wstring& engine,
    const std::wstring& title,
    TestState& state,
    const std::wstring& scenarioName) {
    state.Check(
        ShowBuiltinTool(owner, instance, theme, registry, config, engine),
        scenarioName + L": open request failed");
    HWND tool = WaitForTopWindow(FindWindowRequest{L"", title, GetCurrentProcessId()}, 3000);
    state.Check(tool != nullptr, scenarioName + L": modeless window was not found");
    if (tool) {
        PumpModelessBuiltinTool(tool);
    }
}

void ValidateTimeDisplayScale(HWND hwnd, TestState& state, const std::wstring& name, bool expectDate) {
    const auto children = Children(hwnd);
    HWND display = nullptr;
    HWND baseText = nullptr;
    HWND date = nullptr;
    int largestStaticHeight = 0;
    for (const auto& child : children) {
        if (!IsWindowVisible(child.hwnd)) {
            continue;
        }
        if (child.className == L"Static" && child.text.find(L':') != std::wstring::npos) {
            RECT rect{};
            GetClientRect(child.hwnd, &rect);
            const int height = rect.bottom - rect.top;
            if (height > largestStaticHeight) {
                largestStaticHeight = height;
                display = child.hwnd;
            }
        }
        if (child.className == L"Static" && child.text.find(L'年') != std::wstring::npos) {
            date = child.hwnd;
        }
        if (!baseText && (child.className == L"Button" || child.className == L"Edit")) {
            baseText = child.hwnd;
        }
    }
    if (expectDate && date) {
        baseText = date;
    }
    state.Check(display != nullptr, name + L": time display control was not found");
    state.Check(baseText != nullptr, name + L": base font control was not found");
    if (!display || !baseText) {
        return;
    }

    LOGFONTW displayFont{};
    LOGFONTW baseFont{};
    GetObjectW(
        reinterpret_cast<HFONT>(SendMessageW(display, WM_GETFONT, 0, 0)),
        sizeof(displayFont),
        &displayFont);
    GetObjectW(
        reinterpret_cast<HFONT>(SendMessageW(baseText, WM_GETFONT, 0, 0)),
        sizeof(baseFont),
        &baseFont);
    state.Check(
        std::abs(displayFont.lfHeight) == std::abs(baseFont.lfHeight) * 5,
        name + L": time display font is not five times the base font");
    if (expectDate) {
        state.Check(date != nullptr, name + L": date label was not found");
    }

    wchar_t value[64]{};
    GetWindowTextW(display, value, static_cast<int>(std::size(value)));
    const SIZE measured = ThemedD2D::MeasureText(
        reinterpret_cast<HFONT>(SendMessageW(display, WM_GETFONT, 0, 0)),
        value,
        0,
        false);
    RECT client{};
    GetClientRect(display, &client);
    state.Check(
        measured.cx <= client.right - client.left && measured.cy <= client.bottom - client.top,
        name + L": time text exceeds its adaptive panel");
}

void RunClockScenarios(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::filesystem::path& outputDir,
    TestState& state) {
    PluginRegistry registry(std::filesystem::current_path());
    AppConfig config;
    for (const UINT dpi : {96u, 120u, 144u}) {
        for (const bool showMilliseconds : {false, true}) {
            if (!showMilliseconds) {
                registry.SetSetting(L"quattro.builtin.clock", L"showMilliseconds", L"0");
            }
            const std::wstring suffix = DpiPercentSuffix(dpi) +
                (showMilliseconds ? L"-milliseconds" : L"-seconds");
            Scenario scenario{
                L"builtin-clock-" + suffix,
                L"QuattroClockTool",
                L"时钟",
                L"builtin-clock-" + suffix + L".png",
                {L"时钟", L"显示毫秒"},
                {},
                0,
                1,
                false};
            scenario.forcedDpi = dpi;

            const HWND foregroundBefore = GetForegroundWindow();
            const HWND activeBefore = GetActiveWindow();
            RunDialogScenario(
                scenario,
                outputDir,
                state,
                [&]() {
                    state.Check(
                        ShowBuiltinTool(owner, instance, theme, registry, config, L"clock"),
                        scenario.name + L": open request failed");
                    HWND clock = WaitForTopWindow(
                        FindWindowRequest{L"QuattroClockTool", L"时钟", GetCurrentProcessId()},
                        3000);
                    state.Check(clock != nullptr, scenario.name + L": modeless window was not found");
                    if (!clock) {
                        return;
                    }
                    const LONG_PTR exStyle = GetWindowLongPtrW(clock, GWL_EXSTYLE);
                    state.Check((exStyle & WS_EX_TOPMOST) == 0,
                        scenario.name + L": background clock retained topmost style");
                    state.Check((exStyle & WS_EX_NOACTIVATE) != 0,
                        scenario.name + L": background clock is activatable");
                    HWND milliseconds = GetDlgItem(clock, 7801);
                    state.Check(milliseconds != nullptr,
                        scenario.name + L": milliseconds checkbox was not found");
                    state.Check(
                        milliseconds && ThemedUi::IsChecked(milliseconds) == showMilliseconds,
                        scenario.name + L": milliseconds state was not restored");
                    state.Check(
                        ShowBuiltinTool(owner, instance, theme, registry, config, L"clock") &&
                            CountTopWindowsForProcess(L"时钟", GetCurrentProcessId()) == 1,
                        scenario.name + L": repeated open created another window");
                    PumpModelessBuiltinTool(clock);
                },
                [&]() {
                    HWND clock = FindTopWindow(
                        FindWindowRequest{L"QuattroClockTool", L"时钟", GetCurrentProcessId()});
                    ValidateTimeDisplayScale(clock, state, scenario.name, true);
                    if (showMilliseconds) {
                        return;
                    }
                    HWND milliseconds = clock ? GetDlgItem(clock, 7801) : nullptr;
                    state.Check(milliseconds != nullptr,
                        scenario.name + L": milliseconds checkbox disappeared before toggle");
                    if (milliseconds) {
                        SendMessageW(milliseconds, BM_CLICK, 0, 0);
                        state.Check(ThemedUi::IsChecked(milliseconds),
                            scenario.name + L": milliseconds checkbox did not toggle");
                        state.Check(
                            registry.GetSetting(
                                L"quattro.builtin.clock", L"showMilliseconds", L"0") == L"1",
                            scenario.name + L": milliseconds setting was not retained for reopen");
                    }
                });
            state.Check(GetForegroundWindow() == foregroundBefore,
                scenario.name + L": changed the foreground window");
            state.Check(GetActiveWindow() == activeBefore,
                scenario.name + L": changed the active window");
        }
    }


    for (const UINT dpi : {96u, 120u, 144u}) {
        const std::wstring dpiSuffix = DpiPercentSuffix(dpi);
        auto run = [&](Scenario scenario, const wchar_t* engine) {
            scenario.forcedDpi = dpi;
            RunDialogScenario(
                scenario,
                outputDir,
                state,
                [&]() {
                    OpenAndPumpModelessBuiltinTool(
                        owner, instance, theme, registry, config, engine, scenario.title, state, scenario.name);
                },
                [&]() {
                    HWND tool = FindTopWindow(
                        FindWindowRequest{L"", scenario.title, GetCurrentProcessId()});
                    ValidateTimeDisplayScale(tool, state, scenario.name, false);
                });
        };
        run(
            Scenario{
                L"builtin-timer-time-display-" + dpiSuffix,
                L"",
                L"计时器",
                L"builtin-timer-time-display-" + dpiSuffix + L".png",
                {L"计时器", L"开始(&S)", L"重置(&R)"},
                {},
                3,
                3,
                false},
            L"timer");
        run(
            Scenario{
                L"builtin-stopwatch-time-display-" + dpiSuffix,
                L"",
                L"秒表",
                L"builtin-stopwatch-time-display-" + dpiSuffix + L".png",
                {L"秒表", L"开始(&S)", L"计次(&L)", L"导出(&E)"},
                {},
                0,
                5,
                false},
            L"stopwatch");
    }
}

void RunProcessToolsSingletonScenario(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    TestState& state) {
    AcceptanceLog(L"begin process-tools-singleton");
    PluginRegistry registry(std::filesystem::current_path());
    AppConfig config;
    std::atomic<bool> inspected = false;
    std::thread controller([&]() {
        HWND firstWindow = WaitForTopWindow(
            FindWindowRequest{L"QuattroProcessTools", L"进程工具", GetCurrentProcessId()},
            7000);
        state.Check(firstWindow != nullptr, L"process-tools-singleton: initial window did not appear");
        if (!firstWindow) {
            inspected = true;
            CloseTopWindowsForProcess(GetCurrentProcessId(), nullptr);
            return;
        }

        state.Check(
            CountTopWindowsForProcess(L"进程工具", GetCurrentProcessId()) == 1,
            L"process-tools-singleton: initial window count is not one");
        for (int attempt = 0; attempt < 3; ++attempt) {
            const bool reopened = ShowBuiltinTool(
                owner,
                instance,
                theme,
                registry,
                config,
                L"process-locator",
                attempt == 2);
            state.Check(reopened, L"process-tools-singleton: repeated open request failed");
            Sleep(120);
            const HWND currentWindow = FindTopWindow(
                FindWindowRequest{L"QuattroProcessTools", L"进程工具", GetCurrentProcessId()});
            state.Check(
                currentWindow == firstWindow,
                L"process-tools-singleton: repeated request replaced the existing HWND");
            state.Check(
                CountTopWindowsForProcess(L"进程工具", GetCurrentProcessId()) == 1,
                L"process-tools-singleton: repeated request created another window");
        }
        inspected = true;
        PostMessageW(firstWindow, WM_CLOSE, 0, 0);
    });

    state.Check(
        ShowBuiltinTool(owner, instance, theme, registry, config, L"process-locator"),
        L"process-tools-singleton: initial open request failed");
    HWND firstWindow = WaitForTopWindow(
        FindWindowRequest{L"QuattroProcessTools", L"进程工具", GetCurrentProcessId()},
        3000);
    if (firstWindow) {
        PumpModelessBuiltinTool(firstWindow);
    }
    controller.join();
    state.Check(inspected.load(), L"process-tools-singleton: controller did not complete");
    AcceptanceLog(L"end process-tools-singleton");
}

void RunProcessToolsScenarios(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::filesystem::path& outputDir,
    TestState& state,
    UINT dpi) {
    PluginRegistry registry(std::filesystem::current_path());
    AppConfig config;
    const std::wstring dpiSuffix = DpiPercentSuffix(dpi);

    auto run = [&](Scenario scenario, const wchar_t* engine) {
        scenario.forcedDpi = dpi;
        scenario.validateProcessToolsTableDpi = std::wstring(engine) != L"process-locator";
        RunDialogScenario(
            scenario,
            outputDir,
            state,
            [&]() {
                OpenAndPumpModelessBuiltinTool(
                    owner, instance, theme, registry, config, engine, L"进程工具", state, scenario.name);
            });
    };

    run(
        Scenario{
            L"builtin-process-tools-locator-" + dpiSuffix,
            L"",
            L"进程工具",
            L"builtin-process-tools-locator-" + dpiSuffix + L".png",
            {L"进程工具", L"进程定位", L"PID 查询", L"端口占用", L"文件占用", L"结束进程", L"打开所在目录", L"立即获取", L"进程 ID", L"绝对路径", L"全局快捷键"},
            {L"等待获取"},
            2,
            3,
            false,
            false,
            false,
            {L"将鼠标移到目标窗口或托盘图标上，然后获取进程信息。", L"全局快捷键", L"立即获取"}},
        L"process-locator");

    const std::wstring processId = std::to_wstring(GetCurrentProcessId());
    registry.SetSetting(L"quattro.builtin.process-tools", L"pid", processId);
    Scenario processIdScenario{
        L"builtin-process-tools-pid-" + dpiSuffix,
        L"",
        L"进程工具",
        L"builtin-process-tools-pid-" + dpiSuffix + L".png",
        {L"进程工具", L"进程定位", L"PID 查询", L"端口占用", L"文件占用"},
        {processId},
        1,
        1,
        false,
        false,
        false,
        {L"进程 ID", L"查询(&Q)", processId}};
    processIdScenario.actionButtonText = L"查询(&Q)";
    run(std::move(processIdScenario), L"process-inspector");

    registry.SetSetting(L"quattro.builtin.process-tools", L"port", L"0");
    Scenario portScenario{
        L"builtin-process-tools-port-" + dpiSuffix,
        L"",
        L"进程工具",
        L"builtin-process-tools-port-" + dpiSuffix + L".png",
        {L"进程工具", L"进程定位", L"PID 查询", L"端口占用", L"文件占用"},
        {L"0"},
        1,
        1,
        false,
        false,
        false,
        {L"端口号", L"检查(&C)", L"请输入 1–65535 之间的端口号。"}};
    portScenario.actionButtonText = L"检查(&C)";
    run(std::move(portScenario), L"port-inspector");

    const std::wstring filePath = (std::filesystem::current_path() / L"CMakeLists.txt").wstring();
    registry.SetSetting(L"quattro.builtin.process-tools", L"path", filePath);
    Scenario fileScenario{
        L"builtin-process-tools-file-lock-" + dpiSuffix,
        L"",
        L"进程工具",
        L"builtin-process-tools-file-lock-" + dpiSuffix + L".png",
        {L"进程工具", L"进程定位", L"PID 查询", L"端口占用", L"文件占用"},
        {L"CMakeLists.txt"},
        1,
        3,
        false,
        false,
        false,
        {L"路径", L"检查(&C)", L"已检查 1 个路径"}};
    fileScenario.actionButtonText = L"检查(&C)";
    run(std::move(fileScenario), L"file-lock-inspector");
}

void RunProcessToolsFileLockProgressScenario(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const std::filesystem::path& outputDir,
    const std::filesystem::path& scanRoot,
    TestState& state,
    UINT dpi) {
    const std::wstring dpiSuffix = DpiPercentSuffix(dpi);
    const std::wstring scenarioName = L"builtin-process-tools-file-lock-progress-" + dpiSuffix;
    AcceptanceLog(L"begin " + scenarioName);
    PluginRegistry registry(std::filesystem::current_path());
    AppConfig config;
    registry.SetSetting(L"quattro.builtin.process-tools", L"path", scanRoot.wstring());

    SetEnvironmentVariableW(L"QUATTRO_TEST_MODE", L"1");
    SetEnvironmentVariableW(L"QUATTRO_TEST_FILE_LOCK_BATCH_DELAY_MS", L"300");
    std::atomic_bool inspected{false};
    std::thread controller([&]() {
        const auto forceDpi = [dpi](HWND window) {
            if (!window || dpi == USER_DEFAULT_SCREEN_DPI) {
                return;
            }
            RECT windowRect{};
            GetWindowRect(window, &windowRect);
            const int width = windowRect.right - windowRect.left;
            const int height = windowRect.bottom - windowRect.top;
            RECT suggested{
                windowRect.left,
                windowRect.top,
                windowRect.left + MulDiv(width, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
                windowRect.top + MulDiv(height, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI),
            };
            SendMessageW(
                window,
                WM_DPICHANGED,
                MAKEWPARAM(dpi, dpi),
                reinterpret_cast<LPARAM>(&suggested));
            RedrawWindow(
                window,
                nullptr,
                nullptr,
                RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            Sleep(120);
        };

        HWND mainWindow = WaitForTopWindow(
            FindWindowRequest{L"QuattroProcessTools", L"进程工具", GetCurrentProcessId()},
            7000);
        state.Check(mainWindow != nullptr, scenarioName + L": process tools window did not appear");
        if (!mainWindow) {
            inspected = true;
            CloseTopWindowsForProcess(GetCurrentProcessId(), nullptr);
            return;
        }

        forceDpi(mainWindow);

        HWND checkButton = VisibleButtonByText(mainWindow, L"检查(&C)");
        state.Check(checkButton != nullptr, scenarioName + L": directory check button was not found");
        if (!checkButton) {
            inspected = true;
            PostMessageW(mainWindow, WM_CLOSE, 0, 0);
            return;
        }
        SendMessageW(checkButton, BM_CLICK, 0, 0);

        HWND progressWindow = WaitForTopWindow(
            FindWindowRequest{L"", L"文件占用检查进度", GetCurrentProcessId()},
            5000);
        state.Check(progressWindow != nullptr, scenarioName + L": progress window did not appear");
        if (!progressWindow) {
            inspected = true;
            PostMessageW(mainWindow, WM_CLOSE, 0, 0);
            return;
        }
        forceDpi(progressWindow);
        state.Check(
            WaitForWindowText(progressWindow, L"个工作线程", 5000),
            scenarioName + L": parallel worker progress was not displayed");
        state.Check(
            ChildById(progressWindow, 7730) != nullptr,
            scenarioName + L": shared progress-bar control was not created");
        Scenario runningScenario{
            scenarioName + L"-running",
            L"",
            L"文件占用检查进度",
            L"builtin-process-tools-file-lock-progress-running-" + dpiSuffix + L".png",
            {L"文件占用检查进度", L"正在并行检查目录占用", L"个工作线程", L"停止", L"关闭"},
            {},
            0,
            2};
        ValidateAndCapture(progressWindow, runningScenario, outputDir, state);

        HWND closeButton = VisibleButtonByText(progressWindow, L"关闭");
        state.Check(closeButton != nullptr, scenarioName + L": progress close button was not found");
        if (closeButton) {
            SendMessageW(closeButton, BM_CLICK, 0, 0);
        }
        for (int elapsed = 0; IsWindow(progressWindow) && elapsed < 2000; elapsed += 20) {
            Sleep(20);
        }
        state.Check(!IsWindow(progressWindow), scenarioName + L": close button did not close the progress window");
        state.Check(
            WindowContainsText(mainWindow, L"正在后台检查目录占用"),
            scenarioName + L": closing progress unexpectedly ended the background task");

        checkButton = VisibleButtonByText(mainWindow, L"检查(&C)");
        if (checkButton) {
            SendMessageW(checkButton, BM_CLICK, 0, 0);
        }
        progressWindow = WaitForTopWindow(
            FindWindowRequest{L"", L"文件占用检查进度", GetCurrentProcessId()},
            3000);
        state.Check(progressWindow != nullptr, scenarioName + L": active progress could not be reopened");
        if (progressWindow) {
            forceDpi(progressWindow);
            HWND stopButton = VisibleButtonByText(progressWindow, L"停止");
            state.Check(stopButton != nullptr, scenarioName + L": progress stop button was not found");
            if (stopButton) {
                SendMessageW(stopButton, BM_CLICK, 0, 0);
            }
            state.Check(
                WaitForWindowText(progressWindow, L"检查已停止", 10000),
                scenarioName + L": stop request did not terminate the directory check");
            stopButton = VisibleButtonByText(progressWindow, L"停止");
            state.Check(
                stopButton != nullptr && IsWindowEnabled(stopButton) == FALSE,
                scenarioName + L": stop button remained enabled after cancellation");
            Scenario stoppedScenario{
                scenarioName + L"-stopped",
                L"",
                L"文件占用检查进度",
                L"builtin-process-tools-file-lock-progress-stopped-" + dpiSuffix + L".png",
                {L"文件占用检查进度", L"检查已停止", L"已检查", L"停止", L"关闭"},
                {},
                0,
                2};
            ValidateAndCapture(progressWindow, stoppedScenario, outputDir, state);
            if (HWND stoppedClose = VisibleButtonByText(progressWindow, L"关闭")) {
                SendMessageW(stoppedClose, BM_CLICK, 0, 0);
            }
        }

        inspected = true;
        PostMessageW(mainWindow, WM_CLOSE, 0, 0);
    });

    state.Check(
        ShowBuiltinTool(owner, instance, theme, registry, config, L"file-lock-inspector"),
        scenarioName + L": open request failed");
    HWND mainWindow = WaitForTopWindow(
        FindWindowRequest{L"QuattroProcessTools", L"进程工具", GetCurrentProcessId()},
        3000);
    if (mainWindow) {
        PumpModelessBuiltinTool(mainWindow);
    }
    controller.join();
    SetEnvironmentVariableW(L"QUATTRO_TEST_FILE_LOCK_BATCH_DELAY_MS", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_MODE", nullptr);
    state.Check(inspected.load(), scenarioName + L": controller did not finish");
    AcceptanceLog(L"end " + scenarioName);
}

std::filesystem::path CreateFileLockProgressAcceptanceRoot() {
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
        (L"quattro_file_lock_progress_acceptance_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / L"nested", ec);
    for (int index = 0; index < 2048; ++index) {
        const std::filesystem::path parent = index % 2 == 0 ? root : root / L"nested";
        std::ofstream(parent / (L"resource-" + std::to_wstring(index) + L".txt"), std::ios::binary) << "acceptance";
    }
    return root;
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

std::wstring AcceptanceKnownFolderPath(REFKNOWNFOLDERID folderId) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(folderId, KF_FLAG_DEFAULT, nullptr, &raw)) || !raw) {
        return {};
    }
    std::wstring path(raw);
    CoTaskMemFree(raw);
    return path;
}

void ValidateQuickImportService(
    const std::filesystem::path& outputDir,
    TestState& state) {
    const std::filesystem::path root = outputDir / L"quick-import-service";
    std::error_code ec;
    std::filesystem::create_directories(root / L"nested", ec);
    std::ofstream(root / L"root.exe", std::ios::binary) << "test";
    std::ofstream(root / L"nested" / L"nested.exe", std::ios::binary) << "test";

    QuickImportService service;
    std::wstring error;
    const auto relativeItems = service.Scan(L"relative", error);
    state.Check(relativeItems.empty() && error == L"扫描目录必须是绝对路径。", L"quick-import-service: relative path validation failed");

    const auto missingItems = service.Scan(root / L"missing", error);
    state.Check(missingItems.empty() && error == L"扫描目录不存在或无法访问。", L"quick-import-service: missing path validation failed");

    const auto items = service.Scan(root, error);
    state.Check(error.empty(), L"quick-import-service: valid directory returned an error");
    state.Check(items.size() == 2, L"quick-import-service: explicit directory scan returned unexpected items");
    state.Check(std::all_of(items.begin(), items.end(), [](const QuickImportService::Item& item) {
            return item.selected && item.status == L"可导入";
        }),
        L"quick-import-service: scanned items should stay importable without automatic duplicate filtering");

    std::filesystem::remove_all(root, ec);
}

void RunQuickImportScenarios(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const AppModel& model,
    const std::filesystem::path& outputDir,
    TestState& state) {
    ValidateQuickImportService(outputDir, state);
    for (const UINT dpi : {96u, 120u, 144u}) {
        const std::wstring dpiSuffix = DpiPercentSuffix(dpi);
        const bool switchToStartMenu = dpi != USER_DEFAULT_SCREEN_DPI;
        std::vector<Link> selected;
        Scenario scenario{
            L"quick-import-dialog-" + dpiSuffix,
            L"QuattroQuickImportDialog",
            L"快速导入",
            L"quick-import-dialog-" + dpiSuffix + L".png",
            {L"快速导入", L"桌面", L"开始菜单", L"选择目录", L"扫描", L"列表", L"全选", L"清空", L"导入选中", L"取消"},
            {},
            1,
            7,
            false,
            true,
            true};
        scenario.expectedEditTexts = {
            AcceptanceKnownFolderPath(switchToStartMenu ? FOLDERID_StartMenu : FOLDERID_Desktop)};
        if (switchToStartMenu) {
            scenario.actionButtonText = L"开始菜单";
        }
        scenario.forcedDpi = dpi;
        scenario.requireThemedEditFrames = true;
        RunDialogScenario(
            scenario,
            outputDir,
            state,
            [&]() {
                QuickImportDialog::Show(owner, instance, theme, model.links, selected);
            });
    }
}

void RunLinkEditSplitButtonScenarios(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const AppModel& model,
    const std::filesystem::path& outputDir,
    TestState& state) {
    Link link;
    link.name = L"验收启动项";
    link.path = L"C:\\Windows\\notepad.exe";
    link.parameter = L"acceptance.txt";
    link.workDir = L"C:\\Windows";
    link.remark = L"启动项备注";
    for (const UINT dpi : {96u, 120u, 144u}) {
        const std::wstring dpiSuffix = DpiPercentSuffix(dpi);
        Scenario scenario{
            L"link-edit-split-button-" + dpiSuffix,
            L"QuattroLinkEditDialog",
            L"添加启动项",
            L"link-edit-split-button-" + dpiSuffix + L".png",
            {L"添加启动项", L"确定", L"取消"},
            {L"验收启动项", L"C:\\Windows\\notepad.exe", L"启动项备注"},
            5,
            2,
            false};
        scenario.forcedDpi = dpi;
        scenario.splitButtonMenuId = 1006;
        RunDialogScenario(
            scenario,
            outputDir,
            state,
            [&]() {
                LinkEditDialog::Show(owner, instance, theme, link, model.groups, true);
            });
    }
}

void RunWebDavTransferQueueScenario(
    HINSTANCE instance,
    const Theme& theme,
    const std::filesystem::path& outputDir,
    TestState& state,
    UINT dpi) {
    ThemedFileTransferQueueSnapshot snapshot;
    snapshot.title = L"WebDAV 文件传输";
    snapshot.status = L"总进度 48 MB / 216 MB（22%） · 成功 1 · 失败 0 · 处理中 1 · 等待 1";
    snapshot.detail = L"当前 2 / 3 · report.zip · 正在上传（3 / 4） · 46 MB / 136 MB（33%）";
    snapshot.progress = 0.63;
    snapshot.currentProgress = 0.58;
    snapshot.currentProgressVisible = true;
    snapshot.running = true;
    snapshot.rows = {
        {1, L"config.json", L"C:\\Users\\acceptance\\config.json", 2048, ThemedFileTransferStatus::UploadCompleted},
        {2, L"report.zip", L"D:\\项目资料\\report.zip", 142606336, ThemedFileTransferStatus::Uploading},
        {3, L"数据文件.bin", L"\\\\server\\share\\数据文件.bin", 83886080, ThemedFileTransferStatus::Waiting},
    };
    snapshot.rows[1].phaseIndex = 3;
    snapshot.rows[1].phaseCount = 4;
    snapshot.rows[1].phaseTransferred = 48234496;
    snapshot.rows[1].phaseTotal = 142606336;
    snapshot.rows[1].contentTransferred = snapshot.rows[1].phaseTransferred;
    snapshot.rows[1].contentTotal = snapshot.rows[1].phaseTotal;
    snapshot.rows[1].active = true;
    ThemedFileTransferQueueDialogOptions options;
    options.instance = instance;
    options.theme = theme;
    options.className = L"QuattroWebDavTransferQueueAcceptance_" + std::to_wstring(dpi);
    options.title = snapshot.title;
    options.readSnapshot = [&snapshot] { return snapshot; };
    ThemedFileTransferQueueDialog dialog(std::move(options));
    state.Check(dialog.Show(), L"webdav transfer queue: show failed");
    if (!dialog.hwnd()) return;
    HWND table = GetDlgItem(dialog.hwnd(), 6104);
    TableMutationProbe mutationProbe;
    if (table) SetWindowSubclass(table, TableMutationProbeProc, 41, reinterpret_cast<DWORD_PTR>(&mutationProbe));
    SetWindowPos(dialog.hwnd(), HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    Scenario scenario{
        L"webdav-transfer-queue-" + DpiPercentSuffix(dpi),
        L"QuattroWebDavTransferQueueAcceptance_" + std::to_wstring(dpi),
        snapshot.title,
        L"webdav-transfer-queue-" + DpiPercentSuffix(dpi) + L".png",
        {L"停止", L"关闭"}, {}, 0, 2, false};
    scenario.forcedDpi = dpi;
    scenario.expectedVisibleChildTexts = {snapshot.status, snapshot.detail};
    ValidateAndCapture(dialog.hwnd(), scenario, outputDir, state);
    const BitmapCapture first = CaptureWindowBitmap(dialog.hwnd());
    dialog.NotifyChanged();
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    const BitmapCapture second = CaptureWindowBitmap(dialog.hwnd());
    const std::size_t changed = CountChangedPixelSamples(first, second, 12, 4);
    state.Check(changed <= 8,
        scenario.name + L": unchanged snapshot changed visible pixels=" + std::to_wstring(changed));

    snapshot.rows[1].status = ThemedFileTransferStatus::UploadCompleted;
    snapshot.rows[1].phaseIndex = 0;
    snapshot.rows[1].phaseCount = 0;
    snapshot.rows[1].phaseTransferred = snapshot.rows[1].size;
    snapshot.rows[1].phaseTotal = snapshot.rows[1].size;
    snapshot.rows[1].contentTransferred = snapshot.rows[1].size;
    snapshot.rows[1].contentTotal = snapshot.rows[1].size;
    snapshot.rows[1].active = false;
    snapshot.rows[2].status = ThemedFileTransferStatus::Uploading;
    snapshot.rows[2].phaseIndex = 3;
    snapshot.rows[2].phaseCount = 4;
    snapshot.rows[2].phaseTransferred = 20971520;
    snapshot.rows[2].phaseTotal = snapshot.rows[2].size;
    snapshot.rows[2].contentTransferred = snapshot.rows[2].phaseTransferred;
    snapshot.rows[2].contentTotal = snapshot.rows[2].size;
    snapshot.rows[2].active = true;
    snapshot.status = L"总进度 158 MB / 216 MB（73%） · 成功 2 · 失败 0 · 处理中 1 · 等待 0";
    snapshot.detail = L"当前 3 / 3 · 数据文件.bin · 正在上传（3 / 4） · 20 MB / 80 MB（25%）";
    snapshot.progress = 0.73;
    snapshot.currentProgress = 0.56;
    dialog.NotifyChanged();
    const ULONGLONG transitionDeadline = GetTickCount64() + 2000;
    while (GetTickCount64() < transitionDeadline) {
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        if (table && ThemedUi::TableRowCount(table) == 3 &&
            !ThemedUi::IsTableRowActive(table, 1) && ThemedUi::IsTableRowActive(table, 2)) break;
        Sleep(10);
    }
    Scenario transitioned = scenario;
    transitioned.name = L"webdav-transfer-queue-transition-" + DpiPercentSuffix(dpi);
    transitioned.screenshotName = transitioned.name + L".png";
    transitioned.expectedVisibleChildTexts = {snapshot.status, snapshot.detail};
    ValidateAndCapture(dialog.hwnd(), transitioned, outputDir, state);
    state.Check(table && ThemedUi::TableRowCount(table) == 3 &&
            !ThemedUi::IsTableRowActive(table, 1) && ThemedUi::IsTableRowActive(table, 2),
        L"webdav transfer queue: active frame did not move exclusively to the processing row");
    state.Check(mutationProbe.deleteAllCount == 0 && mutationProbe.redrawSuspendCount == 0,
        L"webdav transfer queue: single-task progress update rebuilt or suspended the whole table");
    if (table) RemoveWindowSubclass(table, TableMutationProbeProc, 41);
    dialog.Close();
}

void RunWebDavFileManagerQueueEntryScenario(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const AppConfig& config,
    const std::filesystem::path& outputDir,
    TestState& state,
    UINT dpi) {
    SetEnvironmentVariableW(L"QUATTRO_TEST_MODE", L"1");
    SetEnvironmentVariableW(L"QUATTRO_TEST_WEBDAV_FILE_MANAGER_SKIP_REFRESH", L"1");
    SetEnvironmentVariableW(L"QUATTRO_TEST_WEBDAV_FILE_MANAGER_INCREMENTAL", L"1");
    const std::filesystem::path cacheRoot = outputDir /
        (L"webdav-file-manager-cache-" + DpiPercentSuffix(dpi));
    std::filesystem::create_directories(cacheRoot);
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", cacheRoot.c_str());
    WebDavFileRecord healthy;
    healthy.id = std::wstring(64, L'a');
    healthy.displayName = L"report.zip";
    healthy.absolutePath = L"C:\\Users\\demo\\Documents\\report.zip";
    healthy.size = 8 * 1024 * 1024;
    healthy.sha256 = std::wstring(64, L'1');
    healthy.uploadedAtUtc = L"2026-07-21T14:32:00.000Z";
    WebDavFileRecord abnormal;
    abnormal.id = std::wstring(64, L'b');
    abnormal.displayName = L"异常记录 · bbbbbbbbbbbb";
    abnormal.health = WebDavFileRecordHealth::MissingMetadata;
    abnormal.recordError = L"metadata.json 缺失";
    abnormal.uploadState = L"invalid";
    abnormal.contentReady = false;
    WebDavFileIndexCache(config).Replace({healthy, abnormal}, L"2026-07-21T14:35:00.000Z");
    std::thread inspector([&]() {
        HWND window = WaitForTopWindow(FindWindowRequest{L"", L"WebDAV 文件管理", GetCurrentProcessId()}, 5000);
        state.Check(window != nullptr, L"webdav file manager queue entry: window not found");
        if (!window) return;
        const ULONGLONG incrementalDeadline = GetTickCount64() + 3000;
        while (!GetPropW(window, L"QuattroWebDavIncrementalApplied") &&
               GetTickCount64() < incrementalDeadline) {
            Sleep(20);
        }
        state.Check(reinterpret_cast<INT_PTR>(GetPropW(
                window, L"QuattroWebDavIncrementalApplied")) == 1,
            L"webdav file manager incremental refresh did not preserve stable rows");
        HWND table = GetDlgItem(window, 430);
        wchar_t firstName[128]{};
        wchar_t secondName[128]{};
        if (table) {
            ListView_GetItemText(table, 0, 0, firstName, static_cast<int>(std::size(firstName)));
            ListView_GetItemText(table, 1, 0, secondName, static_cast<int>(std::size(secondName)));
        }
        state.Check(table && ListView_GetItemCount(table) == 2 &&
                std::wstring(firstName) == L"report-updated.zip" &&
                std::wstring(secondName) == L"new-tail.txt",
            L"webdav file manager incremental rows were not updated in place and appended at tail");
        Scenario scenario{
            L"webdav-file-manager-queue-entry-" + DpiPercentSuffix(dpi),
            L"", L"WebDAV 文件管理",
            L"webdav-file-manager-queue-entry-" + DpiPercentSuffix(dpi) + L".png",
            {L"刷新", L"队列", L"全选", L"清除选择", L"下载所选", L"删除所选"},
            {}, 0, 6, false};
        scenario.forcedDpi = dpi;
        scenario.expectedVisibleChildTexts = {
            L"远端目录：/Quattro/files/", L"已选择 0 项 · 共 2 项"};
        scenario.unexpectedVisibleChildTexts = {L"关闭"};
        ValidateAndCapture(window, scenario, outputDir, state);
        PostMessageW(window, WM_CLOSE, 0, 0);
    });
    ShowWebDavFileManagerDialog(owner, instance, theme, config);
    inspector.join();
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_WEBDAV_FILE_MANAGER_INCREMENTAL", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_WEBDAV_FILE_MANAGER_SKIP_REFRESH", nullptr);
}

void RunWebDavFileDetailsScenario(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const AppConfig& config,
    const std::filesystem::path& outputDir,
    TestState& state,
    UINT dpi) {
    SetEnvironmentVariableW(L"QUATTRO_TEST_MODE", L"1");
    SetEnvironmentVariableW(L"QUATTRO_TEST_WEBDAV_FILE_MANAGER_SKIP_REFRESH", L"1");
    SetEnvironmentVariableW(L"QUATTRO_TEST_WEBDAV_FILE_DETAILS", L"1");
    std::thread inspector([&]() {
        HWND details = WaitForTopWindow(
            FindWindowRequest{L"", L"WebDAV 文件详情", GetCurrentProcessId()}, 5000);
        state.Check(details != nullptr, L"webdav file details: window not found");
        if (!details) return;
        Scenario scenario{
            L"webdav-file-details-" + DpiPercentSuffix(dpi),
            L"", L"WebDAV 文件详情",
            L"webdav-file-details-" + DpiPercentSuffix(dpi) + L".png",
            {L"确定"}, {}, 0, 1, false};
        scenario.forcedDpi = dpi;
        scenario.expectedVisibleChildTexts = {
            L"文件名：", L"EntryFlow.md", L"文件大小：", L"5 KB",
            L"上传时间：", WebDavFileService::FormatUploadedAtLocal(L"2026-07-21T08:31:35.872Z"), L"上传状态：",
            L"complete · 内容可用", L"系统绝对路径", L"远端记录路径", L"SHA-256"};
        ValidateAndCapture(details, scenario, outputDir, state);
        PostMessageW(details, WM_CLOSE, 0, 0);
        HWND manager = WaitForTopWindow(
            FindWindowRequest{L"", L"WebDAV 文件管理", GetCurrentProcessId()}, 2000);
        if (manager) PostMessageW(manager, WM_CLOSE, 0, 0);
    });
    ShowWebDavFileManagerDialog(owner, instance, theme, config);
    inspector.join();
    SetEnvironmentVariableW(L"QUATTRO_TEST_WEBDAV_FILE_DETAILS", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_WEBDAV_FILE_MANAGER_SKIP_REFRESH", nullptr);
}

void RunWebDavDeleteProgressScenario(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    const AppConfig& config,
    const std::filesystem::path& outputDir,
    TestState& state,
    UINT dpi) {
    SetEnvironmentVariableW(L"QUATTRO_TEST_MODE", L"1");
    SetEnvironmentVariableW(L"QUATTRO_TEST_WEBDAV_FILE_MANAGER_SKIP_REFRESH", L"1");
    SetEnvironmentVariableW(L"QUATTRO_TEST_WEBDAV_DELETE_PROGRESS", L"1");
    const std::filesystem::path cacheRoot = outputDir /
        (L"webdav-delete-progress-cache-" + DpiPercentSuffix(dpi));
    std::filesystem::create_directories(cacheRoot);
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", cacheRoot.c_str());
    std::thread inspector([&]() {
        HWND progress = WaitForTopWindow(
            FindWindowRequest{L"", L"删除 WebDAV 文件", GetCurrentProcessId()}, 5000);
        state.Check(progress != nullptr, L"webdav delete progress: window not found");
        if (!progress) return;
        Scenario scenario{
            L"webdav-delete-progress-" + DpiPercentSuffix(dpi),
            L"", L"删除 WebDAV 文件",
            L"webdav-delete-progress-" + DpiPercentSuffix(dpi) + L".png",
            {L"停止", L"关闭"}, {}, 0, 2, false};
        scenario.forcedDpi = dpi;
        scenario.expectedVisibleChildTexts = {
            L"正在删除 2 / 5", L"正在删除元数据：archive-report.zip"};
        ValidateAndCapture(progress, scenario, outputDir, state);
        PostMessageW(progress, WM_CLOSE, 0, 0);
        HWND manager = WaitForTopWindow(
            FindWindowRequest{L"", L"WebDAV 文件管理", GetCurrentProcessId()}, 2000);
        if (manager) PostMessageW(manager, WM_CLOSE, 0, 0);
    });
    ShowWebDavFileManagerDialog(owner, instance, theme, config);
    inspector.join();
    SetEnvironmentVariableW(L"QUATTRO_USER_CONFIG_DIR", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_WEBDAV_DELETE_PROGRESS", nullptr);
    SetEnvironmentVariableW(L"QUATTRO_TEST_WEBDAV_FILE_MANAGER_SKIP_REFRESH", nullptr);
}

} // namespace

int wmain() {
    SetEnvironmentVariableW(L"QUATTRO_ACCEPTANCE_MODE", L"background");
    SetEnvironmentVariableW(L"QUATTRO_TEST_NO_FOCUS", L"1");
    wchar_t outputOverride[32768]{};
    const DWORD outputLength = GetEnvironmentVariableW(
        L"QUATTRO_UI_ACCEPTANCE_OUTPUT_DIR",
        outputOverride,
        static_cast<DWORD>(std::size(outputOverride)));
    gAcceptanceOutputDir = outputLength > 0 && outputLength < std::size(outputOverride)
        ? std::filesystem::path(std::wstring(outputOverride, outputLength))
        : std::filesystem::current_path() / L"screenshots" / L"acceptance";
    std::filesystem::create_directories(gAcceptanceOutputDir);
    std::ofstream(gAcceptanceOutputDir / L"ui-acceptance-progress.txt", std::ios::binary | std::ios::trunc);
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

    const std::filesystem::path outputDir = gAcceptanceOutputDir;
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
    config.webDavRemotePath = L"/Quattro/";
    config.webDavBackupPath = L"/Quattro/backups/";
    config.webDavFilesPath = L"/Quattro/files/";
    config.webDavUserName = L"acceptance-user";

    wchar_t webDavRowTooltipOnly[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_WEBDAV_ROW_TOOLTIP_ONLY",
            webDavRowTooltipOnly,
            static_cast<DWORD>(std::size(webDavRowTooltipOnly))) > 0) {
        WebDavFileRecord record;
        record.displayName = L"report-updated.zip";
        record.absolutePath = L"C:\\Users\\demo\\Documents\\report.zip";
        record.size = 8 * 1024 * 1024;
        record.uploadedAtUtc = L"2026-07-21T14:32:00.000Z";
        const std::wstring tooltipText = WebDavFileService::FormatRecordTooltip(record);
        for (const UINT dpi : {96u, 120u, 144u}) {
            RunTooltipVisualScenario(
                owner, instance, theme, outputDir, state, dpi,
                tooltipText, L"webdav-file-row-tooltip");
        }
        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) std::wcerr << failure << L"\n";
            return 1;
        }
        std::wcout << L"ui_webdav_row_tooltip_acceptance=passed screenshots="
                   << outputDir.wstring() << L"\n";
        return 0;
    }

    wchar_t webDavTransferQueueOnly[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_WEBDAV_TRANSFER_QUEUE_ONLY",
            webDavTransferQueueOnly,
            static_cast<DWORD>(std::size(webDavTransferQueueOnly))) > 0) {
        UINT requestedDpi = 0;
        wchar_t dpiValue[16]{};
        const DWORD dpiLength = GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_WEBDAV_DPI", dpiValue,
            static_cast<DWORD>(std::size(dpiValue)));
        if (dpiLength > 0 && dpiLength < std::size(dpiValue)) {
            requestedDpi = static_cast<UINT>(_wtoi(dpiValue));
        }
        const std::vector<UINT> dpis = requestedDpi == 0
            ? std::vector<UINT>{96u}
            : std::vector<UINT>{requestedDpi};
        wchar_t queueDialogOnly[8]{};
        const bool queueDialogOnlyRequested = GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_WEBDAV_QUEUE_DIALOG_ONLY", queueDialogOnly,
            static_cast<DWORD>(std::size(queueDialogOnly))) > 0;
        for (const UINT dpi : dpis) {
            RunWebDavTransferQueueScenario(instance, theme, outputDir, state, dpi);
            RunTooltipVisualScenario(
                owner, instance, theme, outputDir, state, dpi,
                L"report.zip  ·  136 MB\nD:\\项目资料\\report.zip\n上传中（3 / 4） · 46 MB / 136 MB（33%）",
                L"webdav-transfer-row-tooltip");
            if (!queueDialogOnlyRequested) {
                RunWebDavFileManagerQueueEntryScenario(owner, instance, theme, config, outputDir, state, dpi);
                RunWebDavFileDetailsScenario(owner, instance, theme, config, outputDir, state, dpi);
                RunWebDavDeleteProgressScenario(owner, instance, theme, config, outputDir, state, dpi);
            }
        }
        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) std::wcerr << failure << L"\n";
            return 1;
        }
        std::wcout << L"ui_webdav_transfer_queue_acceptance=passed screenshots=" << outputDir.wstring() << L"\n";
        return 0;
    }

    wchar_t quickImportOnly[8]{};
    wchar_t splitButtonMenuOnly[8]{};
    wchar_t linkEditSplitButtonOnly[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_SPLIT_BUTTON_MENU_ONLY",
            splitButtonMenuOnly,
            static_cast<DWORD>(std::size(splitButtonMenuOnly))) > 0) {
        for (const UINT dpi : {96u, 120u, 144u}) {
            RunSplitButtonMenuScenario(outputDir, state, dpi);
        }
        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) {
                AcceptanceLog(L"split-button-menu target failure " + failure);
                std::wcerr << failure << L"\n";
            }
            return 1;
        }
        std::wcout << L"ui_split_button_menu_acceptance=passed screenshots=" << outputDir.wstring() << L"\n";
        return 0;
    }

    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_LINK_EDIT_SPLIT_BUTTON_ONLY",
            linkEditSplitButtonOnly,
            static_cast<DWORD>(std::size(linkEditSplitButtonOnly))) > 0) {
        RunLinkEditSplitButtonScenarios(owner, instance, theme, model, outputDir, state);
        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) {
                AcceptanceLog(L"link-edit split-button target failure " + failure);
                std::wcerr << failure << L"\n";
            }
            return 1;
        }
        std::wcout << L"ui_link_edit_split_button_acceptance=passed screenshots="
                   << outputDir.wstring() << L"\n";
        return 0;
    }

    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_QUICK_IMPORT_ONLY",
            quickImportOnly,
            static_cast<DWORD>(std::size(quickImportOnly))) > 0) {
        RunQuickImportScenarios(owner, instance, theme, model, outputDir, state);
        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) {
                AcceptanceLog(L"quick-import target failure " + failure);
                std::wcerr << failure << L"\n";
            }
            return 1;
        }
        std::wcout << L"ui_quick_import_acceptance=passed screenshots=" << outputDir.wstring() << L"\n";
        return 0;
    }

    wchar_t tooltipOnly[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_TOOLTIP_ONLY",
            tooltipOnly,
            static_cast<DWORD>(std::size(tooltipOnly))) > 0) {
        for (const UINT dpi : {96u, 120u, 144u}) {
            RunTooltipVisualScenario(owner, instance, theme, outputDir, state, dpi);
        }
        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) {
                AcceptanceLog(L"tooltip target failure " + failure);
                std::wcerr << failure << L"\n";
            }
            return 1;
        }
        std::wcout << L"ui_tooltip_acceptance=passed screenshots=" << outputDir.wstring() << L"\n";
        return 0;
    }

    wchar_t adBlockOnly[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_AD_BLOCK_ONLY",
            adBlockOnly,
            static_cast<DWORD>(std::size(adBlockOnly))) > 0) {
        RunAdBlockScenario(outputDir, state);
        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) {
                AcceptanceLog(L"ad-block target failure " + failure);
                std::wcerr << failure << L"\n";
            }
            return 1;
        }
        std::wcout << L"ui_ad_block_acceptance=passed screenshots=" << outputDir.wstring() << L"\n";
        return 0;
    }

    wchar_t processToolsOnly[8]{};
    wchar_t clockOnly[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_CLOCK_ONLY",
            clockOnly,
            static_cast<DWORD>(std::size(clockOnly))) > 0) {
        RunClockScenarios(owner, instance, theme, outputDir, state);
        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) {
                AcceptanceLog(L"clock target failure " + failure);
                std::wcerr << failure << L"\n";
            }
            return 1;
        }
        std::wcout << L"ui_clock_acceptance=passed screenshots=" << outputDir.wstring() << L"\n";
        return 0;
    }

    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_PROCESS_TOOLS_ONLY",
            processToolsOnly,
            static_cast<DWORD>(std::size(processToolsOnly))) > 0) {
        RunProcessToolsSingletonScenario(owner, instance, theme, state);
        RunProcessToolsScenarios(owner, instance, theme, outputDir, state, 96);
        RunProcessToolsScenarios(owner, instance, theme, outputDir, state, 120);
        RunProcessToolsScenarios(owner, instance, theme, outputDir, state, 144);
        const std::filesystem::path progressRoot = CreateFileLockProgressAcceptanceRoot();
        RunProcessToolsFileLockProgressScenario(owner, instance, theme, outputDir, progressRoot, state, 96);
        RunProcessToolsFileLockProgressScenario(owner, instance, theme, outputDir, progressRoot, state, 120);
        RunProcessToolsFileLockProgressScenario(owner, instance, theme, outputDir, progressRoot, state, 144);
        std::error_code progressCleanupError;
        std::filesystem::remove_all(progressRoot, progressCleanupError);
        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) {
                AcceptanceLog(L"process-tools target failure " + failure);
                std::wcerr << failure << L"\n";
            }
            return 1;
        }
        std::wcout << L"ui_process_tools_acceptance=passed screenshots=" << outputDir.wstring() << L"\n";
        return 0;
    }

    wchar_t appLaunchLockerOnly[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_APP_LAUNCH_LOCKER_ONLY",
            appLaunchLockerOnly,
            static_cast<DWORD>(std::size(appLaunchLockerOnly))) > 0) {
        for (const UINT dpi : {96u, 120u, 144u}) {
            RunAppLaunchLockerScenario(outputDir, state, dpi);
        }
        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) {
                AcceptanceLog(L"app-launch-locker target failure " + failure);
                std::wcerr << failure << L"\n";
            }
            return 1;
        }
        std::wcout << L"ui_app_launch_locker_acceptance=passed screenshots=" << outputDir.wstring() << L"\n";
        return 0;
    }

    wchar_t mainWindowOnly[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_MAIN_WINDOW_ONLY",
            mainWindowOnly,
            static_cast<DWORD>(std::size(mainWindowOnly))) > 0) {
        for (const UINT dpi : {96u, 120u, 144u}) {
            RunMainWindowScenario(outputDir, theme, state, dpi);
        }
        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) {
                AcceptanceLog(L"main-window target failure " + failure);
                std::wcerr << failure << L"\n";
            }
            return 1;
        }
        std::wcout << L"ui_main_window_acceptance=passed screenshots=" << outputDir.wstring() << L"\n";
        return 0;
    }

    wchar_t contextMenuOnly[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_CONTEXT_MENU_ONLY",
            contextMenuOnly,
            static_cast<DWORD>(std::size(contextMenuOnly))) > 0) {
        wchar_t contextMenuIconOnly[8]{};
        const bool contextMenuIconOnlyRequested = GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_CONTEXT_MENU_ICON_ONLY",
            contextMenuIconOnly,
            static_cast<DWORD>(std::size(contextMenuIconOnly))) > 0;
        Scenario settingsScenario{
            L"settings-dialog-右键菜单",
            L"QuattroSettingsDialog",
            L"设置",
            L"settings-dialog-右键菜单.png",
            {L"设置", L"确定", L"取消"},
            {},
            0,
            2,
            false,
            false,
            false,
            {L"系统集成", L"注册“复制绝对路径”右键菜单", L"未注册。", L"自动跟踪", L"缓存维护", L"重置右键菜单"},
            L"右键菜单"};
        settingsScenario.unexpectedVisibleChildTexts = {
            L"恢复跟踪开关默认值，并清除全部菜单列表、状态与图标缓存。",
        };
        settingsScenario.validateContextMenuUninstalledRows = true;
        settingsScenario.validateContextMenuIconTransparency = true;
        std::atomic_int automaticIconLoads{0};
        RunDialogScenario(
            settingsScenario,
            outputDir,
            state,
            [&]() {
                bool imported = false;
                const std::filesystem::path baseDirectory = std::filesystem::current_path();
                ShowSettingsDialog(
                    owner, instance, config, theme, baseDirectory, baseDirectory, &imported,
                    nullptr, false, false, false, {}, {}, {}, {}, {},
                    [&](std::stop_token stopToken) {
                        ++automaticIconLoads;
                        return AcceptanceContextMenuProviderIcons(stopToken);
                    });
            });
        state.Check(automaticIconLoads.load() == 1,
            L"settings context-menu page should request provider icons exactly once per dialog");

        for (UINT dpi : {120u, 144u}) {
            Scenario dpiScenario = settingsScenario;
            const std::wstring suffix = DpiPercentSuffix(dpi);
            dpiScenario.name = L"settings-dialog-右键菜单-" + suffix;
            dpiScenario.screenshotName = L"settings-dialog-右键菜单-" + suffix + L".png";
            dpiScenario.forcedDpi = dpi;
            RunDialogScenario(
                dpiScenario,
                outputDir,
                state,
                [&]() {
                    bool imported = false;
                    const std::filesystem::path baseDirectory = std::filesystem::current_path();
                    ShowSettingsDialog(
                        owner, instance, config, theme, baseDirectory, baseDirectory, &imported,
                        nullptr, false, false, false, {}, {}, {}, {}, {},
                        AcceptanceContextMenuProviderIcons);
                });
        }

        if (contextMenuIconOnlyRequested) {
            for (UINT dpi : {96u, 120u, 144u}) {
                Scenario realIconScenario = settingsScenario;
                const std::wstring suffix = dpi == USER_DEFAULT_SCREEN_DPI
                    ? L""
                    : L"-" + DpiPercentSuffix(dpi);
                realIconScenario.name = L"settings-dialog-右键菜单-real-icons" + suffix;
                realIconScenario.screenshotName =
                    L"settings-dialog-右键菜单-real-icons" + suffix + L".png";
                realIconScenario.forcedDpi = dpi;
                realIconScenario.validateContextMenuIconTransparency = false;
                realIconScenario.expectedContextMenuProvider = L"Everything";
                realIconScenario.expectedContextMenuStatus = L"已安装(注册表)";
                RunDialogScenario(
                    realIconScenario,
                    outputDir,
                    state,
                    [&]() {
                        bool imported = false;
                        const std::filesystem::path baseDirectory = std::filesystem::current_path();
                        ShowSettingsDialog(
                            owner, instance, config, theme, baseDirectory, baseDirectory, &imported,
                            nullptr, false, false, false, {}, {}, {}, {}, {},
                            AcceptanceSystemContextMenuProviderIcons);
                    });
            }

            const std::filesystem::path resetReopenRoot = outputDir / L"reset-reopen-cache";
            std::filesystem::create_directories(resetReopenRoot);
            ShellContextMenuCacheService resetReopenCache(resetReopenRoot);
            Link cachedLink;
            cachedLink.id = 91001;
            cachedLink.path = std::filesystem::current_path().wstring();
            ShellContextMenuSnapshot cachedSnapshot;
            cachedSnapshot.complete = true;
            for (std::size_t index = 0; index < 2; ++index) {
                ShellContextMenuItem item;
                item.providerId = index == 0
                    ? ShellContextMenuProviderId::Git
                    : ShellContextMenuProviderId::Svn;
                item.text = index == 0 ? L"Git cached command" : L"SVN cached command";
                item.iconWidth = 16;
                item.iconHeight = 16;
                item.iconQuality = 1;
                item.iconPixels.assign(
                    16 * 16,
                    index == 0 ? 0xFF2A70D6u : 0xFFD69B2Au);
                cachedSnapshot.items.push_back(std::move(item));
            }
            ShellContextMenuTrackingOptions cachedTracking;
            cachedTracking.git = true;
            cachedTracking.svn = true;
            resetReopenCache.Update(cachedLink, cachedSnapshot, cachedTracking);
            state.Check(
                resetReopenCache.BestIconForProvider(ShellContextMenuProviderId::Git).has_value() &&
                resetReopenCache.BestIconForProvider(ShellContextMenuProviderId::Svn).has_value(),
                L"context-menu reset/reopen fixture did not persist cached provider icons");

            std::atomic_int resetCallbackCount{0};
            std::atomic_bool resetDialogInspected{false};
            std::thread resetController([&]() {
                HWND settings = WaitForTopWindow(
                    FindWindowRequest{L"QuattroSettingsDialog", L"设置", GetCurrentProcessId()}, 7000);
                state.Check(settings != nullptr, L"context-menu reset/reopen settings dialog did not appear");
                if (!settings) return;
                auto children = Children(settings);
                auto contextMenuTab = std::find_if(children.begin(), children.end(), [](const ChildInfo& child) {
                    return child.className == L"Button" && child.text == L"右键菜单";
                });
                state.Check(
                    contextMenuTab != children.end(),
                    L"context-menu reset/reopen tab button was not found");
                if (contextMenuTab == children.end()) {
                    PostMessageW(settings, WM_CLOSE, 0, 0);
                    return;
                }
                PostMessageW(contextMenuTab->hwnd, BM_CLICK, 0, 0);
                HWND resetButton = nullptr;
                for (int elapsed = 0; elapsed < 5000 && !resetButton; elapsed += 20) {
                    for (const auto& child : Children(settings)) {
                        if (IsWindowVisible(child.hwnd) && child.className == L"Button" &&
                            child.text == L"重置右键菜单") {
                            resetButton = child.hwnd;
                            break;
                        }
                    }
                    if (!resetButton) Sleep(20);
                }
                state.Check(resetButton != nullptr, L"context-menu reset/reopen reset button was not found");
                if (!resetButton) {
                    PostMessageW(settings, WM_CLOSE, 0, 0);
                    return;
                }
                // 等待自动图标加载结束，否则产品会按设计拒绝重置。
                for (int elapsed = 0; elapsed < 5000; elapsed += 20) {
                    bool loading = false;
                    for (const auto& child : Children(settings)) {
                        if (!IsWindowVisible(child.hwnd) || child.className != L"SysListView32") continue;
                        const int rows = ListView_GetItemCount(child.hwnd);
                        for (int row = 0; row < rows; ++row) {
                            wchar_t status[64]{};
                            ListView_GetItemText(child.hwnd, row, 1, status, static_cast<int>(std::size(status)));
                            loading = loading || std::wstring(status) == L"检测中...";
                        }
                    }
                    if (!loading) break;
                    Sleep(20);
                }
                PostMessageW(resetButton, BM_CLICK, 0, 0);
                HWND confirmation = WaitForTopWindow(
                    FindWindowRequest{
                        L"QuattroThemedMessageDialog", L"重置右键菜单", GetCurrentProcessId()},
                    5000);
                state.Check(
                    confirmation != nullptr,
                    L"context-menu reset/reopen confirmation dialog did not appear");
                if (confirmation) {
                    resetDialogInspected = true;
                    PostMessageW(confirmation, WM_COMMAND, MAKEWPARAM(IDYES, BN_CLICKED), 0);
                }
                for (int elapsed = 0; elapsed < 3000 && resetCallbackCount.load() == 0; elapsed += 20) {
                    Sleep(20);
                }
                PostMessageW(settings, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                PostMessageW(settings, WM_CLOSE, 0, 0);
            });
            AppConfig resetConfig = config;
            resetConfig.trackGitContextMenu = true;
            resetConfig.trackSvnContextMenu = true;
            bool resetImported = false;
            ShowSettingsDialog(
                owner, instance, resetConfig, theme,
                std::filesystem::current_path(), std::filesystem::current_path(), &resetImported,
                nullptr, false, false, false, {},
                [&]() {
                    ++resetCallbackCount;
                    return resetReopenCache.Reset();
                },
                {}, {}, {},
                [&](std::stop_token stopToken) {
                    return ContextMenuProviderIconService(resetReopenRoot).Load(stopToken);
                });
            resetController.join();
            state.Check(resetDialogInspected.load(), L"context-menu reset/reopen confirmation was not inspected");
            state.Check(resetCallbackCount.load() == 1, L"context-menu reset callback count mismatch");
            state.Check(
                !resetReopenCache.BestIconForProvider(ShellContextMenuProviderId::Git).has_value() &&
                !resetReopenCache.BestIconForProvider(ShellContextMenuProviderId::Svn).has_value(),
                L"context-menu reset retained native provider icon cache");

            const std::vector<ContextMenuProviderIconInfo> reopenedIcons =
                ContextMenuProviderIconService(resetReopenRoot).Load();
            const auto reopenedGit = std::find_if(reopenedIcons.begin(), reopenedIcons.end(), [](const auto& info) {
                return info.providerId == ShellContextMenuProviderId::Git;
            });
            const auto reopenedSvn = std::find_if(reopenedIcons.begin(), reopenedIcons.end(), [](const auto& info) {
                return info.providerId == ShellContextMenuProviderId::Svn;
            });
            if (reopenedGit != reopenedIcons.end() && reopenedSvn != reopenedIcons.end() &&
                reopenedGit->installedViaProbe && reopenedSvn->installedViaProbe) {
                state.Check(
                    !reopenedGit->icon.pixels.empty() && !reopenedSvn->icon.pixels.empty(),
                    L"context-menu reset/reopen did not resolve installed Git/SVN fallback icons");
                state.Check(
                    reopenedGit->icon.pixels != reopenedSvn->icon.pixels,
                    L"context-menu reset/reopen resolved Git/SVN to the same generic DLL icon");
            }

            ShellContextMenuSnapshot refreshedCommandSnapshot;
            refreshedCommandSnapshot.complete = true;
            for (std::size_t index = 0; index < 2; ++index) {
                ShellContextMenuItem item;
                item.providerId = index == 0
                    ? ShellContextMenuProviderId::Git
                    : ShellContextMenuProviderId::Svn;
                item.text = index == 0 ? L"Git refreshed command" : L"SVN refreshed command";
                item.iconWidth = 16;
                item.iconHeight = 16;
                item.iconQuality = 9;
                item.iconPixels.assign(
                    16 * 16,
                    index == 0 ? 0xFF38B249u : 0xFF3975D1u);
                refreshedCommandSnapshot.items.push_back(std::move(item));
            }
            resetReopenCache.Update(cachedLink, refreshedCommandSnapshot, cachedTracking);
            const std::vector<ContextMenuProviderIconInfo> refreshedIcons =
                ContextMenuProviderIconService(resetReopenRoot).Load();
            const auto refreshedGit = std::find_if(refreshedIcons.begin(), refreshedIcons.end(), [](const auto& info) {
                return info.providerId == ShellContextMenuProviderId::Git;
            });
            const auto refreshedSvn = std::find_if(refreshedIcons.begin(), refreshedIcons.end(), [](const auto& info) {
                return info.providerId == ShellContextMenuProviderId::Svn;
            });
            if (reopenedGit != reopenedIcons.end() && reopenedSvn != reopenedIcons.end() &&
                refreshedGit != refreshedIcons.end() && refreshedSvn != refreshedIcons.end() &&
                reopenedGit->installedViaProbe && reopenedSvn->installedViaProbe) {
                state.Check(
                    refreshedGit->icon.pixels == reopenedGit->icon.pixels &&
                    refreshedSvn->icon.pixels == reopenedSvn->icon.pixels,
                    L"context-menu refresh replaced stable Git/SVN brand icons with command icons");
            }

            Scenario resetReopenScenario = settingsScenario;
            resetReopenScenario.name = L"settings-dialog-右键菜单-reset-reopen";
            resetReopenScenario.screenshotName = L"settings-dialog-右键菜单-reset-reopen.png";
            resetReopenScenario.validateContextMenuIconTransparency = false;
            if (reopenedGit != reopenedIcons.end() && reopenedGit->installedViaProbe) {
                resetReopenScenario.expectedContextMenuProvider = L"Git (TortoiseGit)";
                resetReopenScenario.expectedContextMenuStatus = L"已安装(注册表)";
            }
            RunDialogScenario(
                resetReopenScenario,
                outputDir,
                state,
                [&]() {
                    bool imported = false;
                    ShowSettingsDialog(
                        owner, instance, resetConfig, theme,
                        std::filesystem::current_path(), std::filesystem::current_path(), &imported,
                        nullptr, false, false, false, {}, {}, {}, {}, {},
                        [reopenedIcons](std::stop_token stopToken) {
                            return stopToken.stop_requested()
                                ? std::vector<ContextMenuProviderIconInfo>{}
                                : reopenedIcons;
                        });
                });

            Scenario refreshStableScenario = resetReopenScenario;
            refreshStableScenario.name = L"settings-dialog-右键菜单-refresh-stable-icons";
            refreshStableScenario.screenshotName = L"settings-dialog-右键菜单-refresh-stable-icons.png";
            RunDialogScenario(
                refreshStableScenario,
                outputDir,
                state,
                [&]() {
                    bool imported = false;
                    ShowSettingsDialog(
                        owner, instance, resetConfig, theme,
                        std::filesystem::current_path(), std::filesystem::current_path(), &imported,
                        nullptr, false, false, false, {}, {}, {}, {}, {},
                        [refreshedIcons](std::stop_token stopToken) {
                            return stopToken.stop_requested()
                                ? std::vector<ContextMenuProviderIconInfo>{}
                                : refreshedIcons;
                        });
                });
            DestroyWindow(owner);
            OleUninitialize();
            Gdiplus::GdiplusShutdown(gdiplusToken);
            if (!state.ok) {
                for (const auto& failure : state.failures) {
                    AcceptanceLog(L"context-menu icon target failure " + failure);
                    std::wcerr << failure << L"\n";
                }
                return 1;
            }
            std::wcout << L"ui_context_menu_icon_acceptance=passed screenshots="
                       << outputDir.wstring() << L"\n";
            return 0;
        }

        for (const UINT dpi : {96u, 120u, 144u}) {
            RunTooltipVisualScenario(owner, instance, theme, outputDir, state, dpi);
        }

        Scenario nonBlockingScenario{
            L"settings-context-menu-icon-loading-close",
            L"QuattroSettingsDialog",
            L"设置",
            L"settings-context-menu-icon-loading.png",
            {L"设置", L"确定", L"取消"},
            {},
            0,
            2,
            false,
            false,
            true,
            {L"系统集成", L"注册“复制绝对路径”右键菜单", L"未注册。", L"自动跟踪", L"缓存维护", L"获取图标中..."},
            L"右键菜单"};
        nonBlockingScenario.waitForContextMenuIconLoad = false;
        std::atomic_bool slowIconRunnerStarted{false};
        std::atomic<std::int64_t> closeRequestedAt{};
        std::atomic<std::int64_t> dialogReturnedAt{};
        RunDialogScenario(
            nonBlockingScenario,
            outputDir,
            state,
            [&]() {
                bool imported = false;
                const std::filesystem::path baseDirectory = std::filesystem::current_path();
                ShowSettingsDialog(
                    owner, instance, config, theme, baseDirectory, baseDirectory, &imported,
                    nullptr, false, false, false, {}, {}, {}, {}, {},
                    [&](std::stop_token) {
                        slowIconRunnerStarted = true;
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        return AcceptanceContextMenuProviderIcons({});
                    });
                dialogReturnedAt = std::chrono::steady_clock::now().time_since_epoch().count();
            },
            [&]() {
                closeRequestedAt = std::chrono::steady_clock::now().time_since_epoch().count();
            });
        state.Check(slowIconRunnerStarted.load(),
            L"settings context-menu slow icon runner did not start");
        const auto closeRequestedTicks = closeRequestedAt.load();
        const auto dialogReturnedTicks = dialogReturnedAt.load();
        state.Check(closeRequestedTicks > 0,
            L"settings context-menu close request was not observed");
        state.Check(dialogReturnedTicks >= closeRequestedTicks,
            L"settings context-menu dialog returned before the close request");
        const auto closeDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::duration(dialogReturnedTicks - closeRequestedTicks));
        state.Check(closeRequestedTicks > 0 &&
                dialogReturnedTicks >= closeRequestedTicks &&
                closeDuration < std::chrono::milliseconds(1500),
            L"settings context-menu close waited for the background icon runner");

        AppConfig refreshConfig = config;
        refreshConfig.trackGitContextMenu = true;
        Link refreshLink;
        refreshLink.id = 9001;
        refreshLink.name = L"异步刷新验收";
        refreshLink.path = std::filesystem::current_path().wstring();
        SettingsContextMenuRefreshRunner delayedRefresh = [](
            const ShellContextMenuRefreshRequest& request,
            std::stop_token stopToken) {
            for (int elapsed = 0; elapsed < 600 && !stopToken.stop_requested(); elapsed += 20) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            ShellContextMenuRefreshResult result;
            result.tracking = request.tracking;
            result.totalLinks = static_cast<int>(request.links.size());
            result.cancelled = stopToken.stop_requested();
            if (!result.cancelled) {
                result.succeededLinks = static_cast<int>(request.links.size());
            }
            return result;
        };
        Scenario busyScenario{
            L"settings-context-menu-refresh-busy",
            L"QuattroSettingsDialog",
            L"设置",
            L"settings-context-menu-refresh-busy.png",
            {L"设置", L"确定", L"取消"},
            {},
            0,
            2,
            false,
            false,
            false,
            {L"系统集成", L"注册“复制绝对路径”右键菜单", L"未注册。", L"自动跟踪", L"缓存维护", L"扫描中..."},
            L"右键菜单"};
        busyScenario.actionButtonText = L"从Windows菜单刷新";
        busyScenario.closeDelayMs = 700;
        std::atomic_bool refreshApplied{false};
        RunDialogScenario(
            busyScenario,
            outputDir,
            state,
            [&]() {
                bool imported = false;
                const std::filesystem::path baseDirectory = std::filesystem::current_path();
                ShowSettingsDialog(
                    owner,
                    instance,
                    refreshConfig,
                    theme,
                    baseDirectory,
                    baseDirectory,
                    &imported,
                    nullptr,
                    false,
                    false,
                    false,
                    {},
                    {},
                    {refreshLink},
                    delayedRefresh,
                    [&](const ShellContextMenuRefreshResult&) {
                        refreshApplied = true;
                    },
                    AcceptanceContextMenuProviderIcons);
            });
        state.Check(refreshApplied.load(), L"settings context menu refresh result was not applied on completion");

        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) {
                AcceptanceLog(L"target failure " + failure);
                std::wcerr << failure << L"\n";
            }
            return 1;
        }
        std::wcout << L"ui_screenshot_acceptance=passed screenshots=" << outputDir.wstring() << L"\n";
        return 0;
    }

    wchar_t tableOnly[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_TABLE_ONLY",
            tableOnly,
            static_cast<DWORD>(std::size(tableOnly))) > 0) {
        RunTableAlternatingRowsScenario(outputDir, state);
        RunCheckableTableScenario(outputDir, state, 96);
        RunCheckableTableScenario(outputDir, state, 120);
        RunCheckableTableScenario(outputDir, state, 144);
        RunWebDavFileColumnsScenario(outputDir, state, 96);
        RunWebDavFileColumnsScenario(outputDir, state, 120);
        RunWebDavFileColumnsScenario(outputDir, state, 144);
        RunTableHoverRepaintScenario(state);
        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) {
                AcceptanceLog(L"table target failure " + failure);
                std::wcerr << failure << L"\n";
            }
            return 1;
        }
        std::wcout << L"ui_table_acceptance=passed screenshots=" << outputDir.wstring() << L"\n";
        return 0;
    }

    wchar_t d2dDialogsOnly[8]{};
    if (GetEnvironmentVariableW(
            L"QUATTRO_UI_ACCEPTANCE_D2D_DIALOGS_ONLY",
            d2dDialogsOnly,
            static_cast<DWORD>(std::size(d2dDialogsOnly))) > 0) {
        const std::vector<WebDavRemoteFile> backups{
            {L"quattro-backup-20260630-215427-with-a-very-long-file-name.q4cfg", L"/remote/quattro-backup.q4cfg", 139264, L"Tue, 30 Jun 2026 17:54:27 GMT", false},
            {L"quattro-backup-20260626-173428.q4cfg", L"/remote/quattro-backup-old.q4cfg", 77824, L"Fri, 26 Jun 2026 09:34:11 GMT", false}};
        for (const UINT dpi : {96u, 120u, 144u}) {
            const std::wstring suffix = DpiPercentSuffix(dpi);
            Scenario messageScenario{
                L"d2d-message-dialog-" + suffix,
                L"QuattroThemedMessageDialog",
                L"D2D 消息框",
                L"d2d-message-dialog-" + suffix + L".png",
                {L"D2D 消息框", L"确定", L"取消"}, {}, 0, 2, false};
            messageScenario.forcedDpi = dpi;
            RunDialogScenario(messageScenario, outputDir, state, [&]() {
                ShowThemedMessageBox(
                    owner, instance, theme,
                    L"DWrite 多行消息测量与公共 D2D 背景绘制验收。",
                    L"D2D 消息框", MB_OKCANCEL | MB_ICONINFORMATION);
            });

            Scenario restoreDeletedScenario{
                L"webdav-restore-deleted-" + suffix,
                L"QuattroThemedMessageDialog",
                L"确认恢复已删除待办",
                L"webdav-restore-deleted-" + suffix + L".png",
                {L"确认恢复已删除待办", L"是", L"否", L"取消", L"本地删除"}, {}, 0, 3, false};
            restoreDeletedScenario.forcedDpi = dpi;
            RunDialogScenario(restoreDeletedScenario, outputDir, state, [&]() {
                ShowThemedMessageBox(
                    owner,
                    instance,
                    theme,
                    L"云端备份中有 3 条待办已在本地删除。是否恢复这些条目？\n\n"
                    L"- 准备季度汇报材料\n"
                    L"- 更新服务器证书\n"
                    L"- 整理项目复盘记录\n\n"
                    L"“是”恢复这些待办；“否”保持删除并继续合并；“取消”终止本次合并。",
                    L"确认恢复已删除待办",
                    MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
            });

            Scenario downloadConfirmScenario{
                L"webdav-download-confirm-" + suffix,
                L"QuattroThemedMessageDialog",
                L"从云端下载",
                L"webdav-download-confirm-" + suffix + L".png",
                {L"从云端下载", L"确定", L"取消", L"最后更新时间", L"再次询问"}, {}, 0, 2, false};
            downloadConfirmScenario.forcedDpi = dpi;
            RunDialogScenario(downloadConfirmScenario, outputDir, state, [&]() {
                ShowThemedMessageBox(
                    owner,
                    instance,
                    theme,
                    L"请确认要下载并合并以下 WebDAV 备份：\n\n"
                    L"文件名: quattro-backup-20260630-215427.q4cfg\n"
                    L"文件大小: 136 KB\n"
                    L"备份时间: 2026年7月1日 01:54\n\n"
                    L"将把该备份中的分组、标签、启动项、便签和待办合并到当前数据。\n"
                    L"同一待办按最后更新时间保留较新版本；本地已删除的条目会再次询问是否恢复。\n"
                    L"导入前会自动备份。",
                    L"从云端下载",
                    MB_OKCANCEL | MB_ICONINFORMATION);
            });

            std::wstring selectedBackup = backups.front().name;
            Scenario webDavScenario{
                L"d2d-webdav-table-" + suffix,
                L"",
                L"选择 WebDAV 备份",
                L"d2d-webdav-table-" + suffix + L".png",
                {L"选择 WebDAV 备份", L"下载"}, {}, 0, 2, false};
            webDavScenario.forcedDpi = dpi;
            RunDialogScenario(webDavScenario, outputDir, state, [&]() {
                ShowWebDavBackupSelectionDialog(owner, instance, theme, backups, selectedBackup);
            });

            TodoItem todo;
            todo.title = L"D2D 日历验收";
            todo.content = L"验证日期选中、圆角、文字与 Footer。";
            SYSTEMTIME selectedDate{};
            GetLocalTime(&selectedDate);
            selectedDate.wDay = selectedDate.wDay > 1 ? selectedDate.wDay - 1 : 2;
            selectedDate.wHour = 9;
            selectedDate.wMinute = 30;
            selectedDate.wSecond = 0;
            selectedDate.wMilliseconds = 0;
            todo.anchorAt = FormatTodoTimestamp(selectedDate);
            Scenario todoScenario{
                L"d2d-todo-calendar-" + suffix,
                L"QuattroTodoEditDialog",
                L"新建待办",
                L"d2d-todo-calendar-" + suffix + L".png",
                {L"新建待办", L"保存待办", L"取消"},
                {todo.title, todo.content}, 2, 2, false};
            todoScenario.forcedDpi = dpi;
            todoScenario.rejectDarkSurface = true;
            RunDialogScenario(todoScenario, outputDir, state, [&]() {
                TodoEditDialog::Show(owner, instance, theme, todo, true);
            });
        }
        DestroyWindow(owner);
        OleUninitialize();
        Gdiplus::GdiplusShutdown(gdiplusToken);
        if (!state.ok) {
            for (const auto& failure : state.failures) {
                AcceptanceLog(L"d2d dialog target failure " + failure);
                std::wcerr << failure << L"\n";
            }
            return 1;
        }
        std::wcout << L"ui_d2d_dialog_acceptance=passed screenshots=" << outputDir.wstring() << L"\n";
        return 0;
    }

    for (const UINT dpi : {96u, 120u, 144u}) {
        RunMainWindowScenario(outputDir, theme, state, dpi);
    }
    RunTableAlternatingRowsScenario(outputDir, state);
    RunCheckableTableScenario(outputDir, state, 96);
    RunCheckableTableScenario(outputDir, state, 120);
    RunCheckableTableScenario(outputDir, state, 144);
    RunTableColumnResizeScenario(outputDir, state);
    RunTableHoverRepaintScenario(state);
    RunTableGridLinesScenario(outputDir, state, false, L"table-grid-lines-default", L"table-grid-lines-default.png");
    RunTableGridLinesScenario(outputDir, state, true, L"table-grid-lines-enabled", L"table-grid-lines-enabled.png");
    for (const UINT dpi : {96u, 120u, 144u}) {
        RunAppLaunchLockerScenario(outputDir, state, dpi);
    }
    RunAdBlockScenario(outputDir, state);

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
        L"同一待办按最后更新时间保留较新版本；本地已删除的条目会再次询问是否恢复。\n"
        L"导入前会自动备份。";
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
        {L"行为", {L"窗口行为", L"未贴边时，打开工具后隐藏主窗口", L"运行与数据", L"系统集成", L"启动后隐藏", L"开机启动", L"启用日志"}},
        {L"右键菜单", {L"系统集成", L"注册“复制绝对路径”右键菜单", L"未注册。", L"自动跟踪", L"缓存维护", L"重置右键菜单"}},
        {L"交互", {L"启动操作", L"悬停激活", L"双击运行", L"分组激活延迟", L"标签激活延迟"}},
        {L"热键", {L"全局快捷键", L"启用全局快捷键", L"主窗口显隐", L"进程定位器", L"复制选中项绝对路径"}},
        {L"链接", {L"目录命令", L"公共链接", L"打开目录命令", L"帮助链接", L"更新链接", L"FAQ 链接"}},
        {L"WebDAV", {L"WebDAV 备份", L"启用 WebDAV 备份", L"服务器地址", L"用户名", L"备份目录", L"保留数量", L"文件目录", L"/Quattro/backups/", L"/Quattro/files/", L"打开文件管理", L"注册“上传到 WebDAV”右键菜单", L"测试连接", L"上传到云端"}},
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
        if (page == L"行为") {
            settingsScenario.expectedEditTexts = {L"1000"};
        }
        settingsScenario.validateHotKeyTableLayout = page == L"热键";
        settingsScenario.validateContextMenuUninstalledRows = page == L"右键菜单";
        if (page == L"右键菜单") {
            settingsScenario.unexpectedVisibleChildTexts = {
                L"恢复跟踪开关默认值，并清除全部菜单列表、状态与图标缓存。",
            };
        }
        RunDialogScenario(
            settingsScenario,
            outputDir,
            state,
            [&]() {
                bool imported = false;
                const std::filesystem::path baseDirectory = std::filesystem::current_path();
                ShowSettingsDialog(
                    owner, instance, config, theme, baseDirectory, baseDirectory, &imported,
                    nullptr, false, false, false, {}, {}, {}, {}, {},
                    AcceptanceContextMenuProviderIcons);
            });
    }

    for (const UINT dpi : {96u, 120u, 144u}) {
        RunTooltipVisualScenario(owner, instance, theme, outputDir, state, dpi);
    }

    RunQuickImportScenarios(owner, instance, theme, model, outputDir, state);

    RunLinkEditSplitButtonScenarios(owner, instance, theme, model, outputDir, state);

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
    RunClockScenarios(owner, instance, theme, outputDir, state);
    RunDialogScenario(
        Scenario{L"builtin-clicker", L"", L"连点器", L"builtin-clicker.png", {L"连点器", L"启动(&S)"}, {L"0, 0", L"10", L"1000"}, 4, 2, false},
        outputDir,
        state,
        [&]() {
            OpenAndPumpModelessBuiltinTool(
                owner, instance, theme, registry, builtinToolConfig, L"clicker", L"连点器", state, L"builtin-clicker");
        });
    RunDialogScenario(
        Scenario{L"builtin-timer", L"", L"计时器", L"builtin-timer.png", {L"计时器", L"开始(&S)", L"暂停(&P)", L"重置(&R)"}, {}, 3, 3, false},
        outputDir,
        state,
        [&]() {
            OpenAndPumpModelessBuiltinTool(
                owner, instance, theme, registry, builtinToolConfig, L"timer", L"计时器", state, L"builtin-timer");
        });
    RunDialogScenario(
        Scenario{L"builtin-stopwatch", L"", L"秒表", L"builtin-stopwatch.png", {L"秒表", L"开始(&S)", L"暂停(&P)", L"导出(&E)"}, {}, 0, 5, false},
        outputDir,
        state,
        [&]() {
            OpenAndPumpModelessBuiltinTool(
                owner, instance, theme, registry, builtinToolConfig, L"stopwatch", L"秒表", state, L"builtin-stopwatch");
        });
    RunProcessToolsSingletonScenario(owner, instance, theme, state);
    RunProcessToolsScenarios(owner, instance, theme, outputDir, state, 96);
    const std::filesystem::path progressRoot = CreateFileLockProgressAcceptanceRoot();
    RunProcessToolsFileLockProgressScenario(owner, instance, theme, outputDir, progressRoot, state, 96);
    std::error_code progressCleanupError;
    std::filesystem::remove_all(progressRoot, progressCleanupError);

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
