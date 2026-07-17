#include "../src/theme/Theme.h"
#include "../src/theme/ThemedUi.h"
#include "../src/theme/ThemedWindowUi.h"

#include <windows.h>
#include <commctrl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

std::unique_ptr<ThemedWindowUi> g_windowUi;

LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    LRESULT result = 0;
    if (ThemedWindowUi::HandleCommonMessage(g_windowUi, message, wParam, lParam, result)) {
        return result;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool Require(bool condition, const wchar_t* message) {
    if (!condition) {
        std::wcerr << L"FAIL: " << message << L"\n";
    }
    return condition;
}

void PumpMessages(DWORD durationMs) {
    const ULONGLONG begin = GetTickCount64();
    while (GetTickCount64() - begin < durationMs) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
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

HWND FindToastWindow(HWND host) {
    struct Data {
        DWORD processId;
        HWND host;
        HWND match;
    } data{GetCurrentProcessId(), host, nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM lParam) -> BOOL {
            auto* data = reinterpret_cast<Data*>(lParam);
            DWORD processId = 0;
            GetWindowThreadProcessId(hwnd, &processId);
            if (processId != data->processId) {
                return TRUE;
            }
            wchar_t className[64]{};
            GetClassNameW(hwnd, className, static_cast<int>(std::size(className)));
            if (std::wstring(className) != L"QuattroThemedToast") {
                return TRUE;
            }
            data->match = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&data));
    return data.match;
}

bool RequireToastWindowPolicy(HWND toast) {
    const LONG_PTR exStyle = GetWindowLongPtrW(toast, GWL_EXSTYLE);
    bool ok = true;
    ok &= Require(GetWindow(toast, GW_OWNER) == nullptr, L"toast should not be owned by the host window");
    ok &= Require((exStyle & WS_EX_NOACTIVATE) != 0, L"toast should not activate the desktop");
    ok &= Require((exStyle & WS_EX_TOOLWINDOW) != 0, L"toast should stay out of the taskbar");
    ok &= Require((exStyle & WS_EX_APPWINDOW) == 0, L"toast should not request a taskbar button");
    ok &= Require((exStyle & WS_EX_TOPMOST) == 0, L"toast should not be topmost during background acceptance");
    return ok;
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

bool CaptureWindowPng(HWND hwnd, const std::filesystem::path& path) {
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    const int width = std::max(1, static_cast<int>(rect.right - rect.left));
    const int height = std::max(1, static_cast<int>(rect.bottom - rect.top));

    HDC screen = GetDC(nullptr);
    if (!screen) {
        return false;
    }
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    if (!dc || !bitmap) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return false;
    }
    HGDIOBJ old = SelectObject(dc, bitmap);
    HBRUSH magenta = CreateSolidBrush(RGB(255, 0, 255));
    RECT fill{0, 0, width, height};
    FillRect(dc, &fill, magenta);
    DeleteObject(magenta);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    Sleep(60);
    const bool printed = PrintWindow(hwnd, dc, 0x00000002) != FALSE;
    SelectObject(dc, old);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);

    bool valid = false;
    if (printed) {
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width;
        info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        std::vector<std::uint32_t> pixels(
            static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
        HDC readDc = GetDC(nullptr);
        const int lines = readDc
            ? GetDIBits(readDc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info, DIB_RGB_COLORS)
            : 0;
        if (readDc) ReleaseDC(nullptr, readDc);
        if (lines != 0) {
            std::vector<std::uint32_t> colors;
            const std::size_t step = std::max<std::size_t>(1, pixels.size() / 5000);
            for (std::size_t index = 0; index < pixels.size() && colors.size() < 12; index += step) {
                const std::uint32_t color = pixels[index] & 0x00ffffffu;
                if (std::find(colors.begin(), colors.end(), color) == colors.end()) {
                    colors.push_back(color);
                }
            }
            valid = colors.size() >= 12;
        }
    }

    CLSID pngClsid{};
    bool saved = false;
    if (valid && GetEncoderClsid(L"image/png", &pngClsid) >= 0) {
        Gdiplus::Bitmap image(bitmap, nullptr);
        saved = image.Save(path.c_str(), &pngClsid, nullptr) == Gdiplus::Ok;
    }
    DeleteObject(bitmap);
    return saved;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::filesystem::path outputDir = std::filesystem::current_path();
    if (argc > 1 && argv[1] && argv[1][0] != L'\0') {
        outputDir = argv[1];
        std::filesystem::create_directories(outputDir);
    }

    HINSTANCE instance = GetModuleHandleW(nullptr);
    // Acceptance rule: tests must not steal focus or bring windows to front.
    // Background mode also makes the toast window use HWND_BOTTOM instead of HWND_TOPMOST.
    SetEnvironmentVariableW(L"QUATTRO_TEST_NO_FOCUS", L"1");
    SetEnvironmentVariableW(L"QUATTRO_ACCEPTANCE_MODE", L"background");
    const HWND initialForeground = GetForegroundWindow();
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gdiplusInput{};
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        return 2;
    }

    Theme theme = Theme::Load(std::filesystem::current_path() / L"theme", L"default");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HostProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"QuattroToastAcceptanceHost";
    RegisterClassExW(&wc);
    HWND host = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Toast 验收", WS_OVERLAPPEDWINDOW,
        120, 120, 640, 420, nullptr, nullptr, instance, nullptr);
    if (!Require(host != nullptr, L"host window created")) {
        return 1;
    }
    ShowWindow(host, SW_SHOWNOACTIVATE);
    SetWindowPos(host, HWND_BOTTOM, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    g_windowUi = std::make_unique<ThemedWindowUi>(
        instance, nullptr, host, theme, DialogLayoutKind::Compact, 640, 420);

    bool ok = true;

    const struct RoleCase {
        ThemedToastRole role;
        const wchar_t* name;
        const wchar_t* text;
    } cases[] = {
        {ThemedToastRole::Normal, L"normal", L"普通提示：操作已记录。"},
        {ThemedToastRole::Info, L"info", L"信息提示：已置顶主窗口。"},
        {ThemedToastRole::Success, L"success", L"成功提示：网址已复制到剪贴板。"},
        {ThemedToastRole::Warning, L"warning", L"警告提示：请先选择一个标签。"},
        {ThemedToastRole::Danger, L"danger", L"危险提示：便签保存失败，请复制备份内容。"},
        {ThemedToastRole::Info, L"delete-tag", L"已删除标签“工作”（3 项内容）。"},
        {ThemedToastRole::Success, L"settings-saved", L"设置已保存。"},
        {ThemedToastRole::Warning, L"import-partial", L"已添加 2 项，1 项失败。"},
        {ThemedToastRole::Success, L"recycle-bin", L"回收站已清空。"},
    };

    for (const auto& roleCase : cases) {
        ThemedToastOptions options{};
        options.role = roleCase.role;
        options.durationMs = 0; // persistent for capture
        g_windowUi->ui().ShowToast(roleCase.text, options);
        PumpMessages(120);

        HWND toast = FindToastWindow(host);
        ok &= Require(toast != nullptr, L"toast window exists");
        if (!toast) {
            continue;
        }
        ok &= Require(IsWindowVisible(toast) != FALSE, L"toast window visible");
        ok &= Require(WindowText(toast) == roleCase.text, L"toast text matches");
        ok &= RequireToastWindowPolicy(toast);
        const std::filesystem::path shot = outputDir / (std::wstring(L"toast-") + roleCase.name + L".png");
        ok &= Require(CaptureWindowPng(toast, shot), L"toast screenshot saved");
        std::wcout << L"captured " << shot.wstring() << L"\n";
    }

    // Auto-hide: a short duration toast must disappear via the WM_TIMER path.
    {
        ThemedToastOptions options{};
        options.role = ThemedToastRole::Success;
        options.durationMs = 300;
        g_windowUi->ui().ShowToast(L"自动隐藏验证", options);
        PumpMessages(120);
        HWND toast = FindToastWindow(host);
        ok &= Require(toast && IsWindowVisible(toast), L"auto-hide toast visible before timeout");
        PumpMessages(700);
        toast = FindToastWindow(host);
        ok &= Require(!toast || !IsWindowVisible(toast), L"toast auto-hidden after durationMs");
    }

    // Replacement: a second ShowToast reuses the single toast window with new text.
    {
        ThemedToastOptions options{};
        options.role = ThemedToastRole::Info;
        options.durationMs = 0;
        g_windowUi->ui().ShowToast(L"第一条", options);
        PumpMessages(60);
        g_windowUi->ui().ShowToast(L"第二条", options);
        PumpMessages(60);
        HWND toast = FindToastWindow(host);
        ok &= Require(toast && WindowText(toast) == L"第二条", L"later toast replaces earlier one");
        g_windowUi->ui().HideToast();
        PumpMessages(60);
        toast = FindToastWindow(host);
        ok &= Require(!toast || !IsWindowVisible(toast), L"HideToast hides the window");
    }

    DestroyWindow(host);
    PumpMessages(60);
    g_windowUi.reset();
    Gdiplus::GdiplusShutdown(gdiplusToken);

    // Acceptance rule: the test must not have changed foreground ownership.
    ok &= Require(GetForegroundWindow() == initialForeground,
        L"foreground window unchanged during acceptance run");

    if (ok) {
        std::wcout << L"toast_acceptance=passed\n";
        return 0;
    }
    return 1;
}
