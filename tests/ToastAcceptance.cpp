#include "../src/theme/Theme.h"
#include "../src/theme/ThemedUi.h"
#include "../src/theme/ThemedWindowUi.h"
#include "../src/theme/ThemedTaskProgressDialog.h"
#include "../src/theme/ThemedD2D.h"
#include "../src/theme/ThemedGdiFallback.h"

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
bool g_forwardCommonMessages = true;

LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    LRESULT result = 0;
    if (g_forwardCommonMessages &&
            ThemedWindowUi::HandleCommonMessage(g_windowUi, message, wParam, lParam, result)) {
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
            if (ThemedUi::PreTranslateMessage(msg)) continue;
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
            if (std::wstring(className) != L"QuattroThemedToast" || GetWindow(hwnd, GW_OWNER) != data->host) {
                return TRUE;
            }
            data->match = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&data));
    return data.match;
}

bool RequireToastWindowPolicy(HWND host, HWND toast) {
    const LONG_PTR exStyle = GetWindowLongPtrW(toast, GWL_EXSTYLE);
    bool ok = true;
    ok &= Require(GetWindow(toast, GW_OWNER) == host, L"toast should be owned by the host window");
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
            const auto sentinelPixels = std::count_if(pixels.begin(), pixels.end(), [](std::uint32_t value) {
                return (value & 0x00ffffffu) == 0x00ff00ffu;
            });
            valid = colors.size() >= 12 && static_cast<std::size_t>(sentinelPixels) < pixels.size() / 20;
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

bool RequireSurfacePixels(const std::filesystem::path& file, Color foreground, Color base) {
    // Independent opaque-surface oracle; do not call the production compositor.
    const int r = static_cast<int>(255 * (foreground.r * foreground.a + base.r * (1 - foreground.a)));
    const int g = static_cast<int>(255 * (foreground.g * foreground.a + base.g * (1 - foreground.a)));
    const int b = static_cast<int>(255 * (foreground.b * foreground.a + base.b * (1 - foreground.a)));
    Gdiplus::Bitmap image(file.c_str());
    if (image.GetLastStatus() != Gdiplus::Ok) return false;
    std::size_t matching = 0;
    for (UINT y = 0; y < image.GetHeight(); ++y) {
        for (UINT x = 0; x < image.GetWidth(); ++x) {
            Gdiplus::Color pixel;
            image.GetPixel(x, y, &pixel);
            matching += std::abs(int(pixel.GetR()) - r) + std::abs(int(pixel.GetG()) - g) +
                std::abs(int(pixel.GetB()) - b) <= 6;
        }
    }
    std::wcout << L"surface_pixels file=" << file.filename().wstring() << L" matching=" << matching << L"\n";
    return Require(matching > image.GetWidth() * image.GetHeight() / 4,
        L"translucent role background is composed over its public surface");
}

bool RunToastRegressionAcceptance(HWND host, const Theme& theme, const std::filesystem::path& outputDir) {
    bool ok = true;
    for (bool fallback : {false, true}) {
        g_windowUi.reset();
        SetEnvironmentVariableW(L"QUATTRO_FORCE_GDI_FALLBACK", fallback ? L"1" : nullptr);
        g_windowUi = std::make_unique<ThemedWindowUi>(
            GetModuleHandleW(nullptr), nullptr, host, theme, DialogLayoutKind::Compact, 640, 420);
        for (UINT dpi : {96u, 120u, 144u}) {
            RECT hostRect{120, 120, 760, 540};
            SendMessageW(host, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&hostRect));
            SetWindowPos(host, HWND_BOTTOM, 120, 120, 640, 420, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
            const std::wstring suffix = std::wstring(fallback ? L"-gdi-" : L"-d2d-") + std::to_wstring(dpi);
            const struct { const wchar_t* name; const wchar_t* text; bool multiline; } texts[] = {
                {L"medium", L"成功提示：网址已复制到剪贴板。", true},
                {L"short", L"设置已保存。", true},
                {L"mixed", L"保存 Quattro 设置失败，请重试；路径 C:/test/conf.ini。", true},
                {L"newline", L"第一行\r\n第二行的内容仍应完整显示。", true},
                {L"long", L"批量处理已经结束，其中部分项目未能完成。请保留当前结果，检查失败项目后重试；之前已完成的内容不会丢失。", true},
                {L"single", L"成功提示：网址已复制到剪贴板。", false},
            };
            for (const auto& sample : texts) {
                ThemedToastOptions options;
                options.durationMs = 0;
                options.role = ThemedToastRole::Danger;
                options.multiline = sample.multiline;
                const auto layout = g_windowUi->ui().MeasureToast(sample.text, options);
                g_windowUi->ui().ShowToast(sample.text, options);
                PumpMessages(30);
                HWND toast = FindToastWindow(host);
                ok &= Require(toast != nullptr, L"regression toast exists");
                if (!toast) continue;
                RECT client{};
                GetClientRect(toast, &client);
                const int width = layout.text.right - layout.text.left;
                const SIZE required = fallback
                    ? ThemedGdiFallback::MeasureTextLayout(g_windowUi->font(), sample.text, -1, width, sample.multiline)
                    : ThemedD2D::MeasureText(g_windowUi->font(), sample.text, width, sample.multiline);
                ok &= Require(required.cy > 0 && required.cy <= layout.text.bottom - layout.text.top &&
                    required.cx <= width && client.right == layout.size.cx && client.bottom == layout.size.cy,
                    L"all toast text fits inside the actual window");
                ok &= Require(layout.text.right < layout.closeButton.left &&
                    layout.closeButton.bottom <= client.bottom, L"toast text and close button do not overlap");
                POINT close{(layout.closeButton.left + layout.closeButton.right) / 2,
                    (layout.closeButton.top + layout.closeButton.bottom) / 2};
                ClientToScreen(toast, &close);
                ok &= Require(SendMessageW(toast, WM_NCHITTEST, 0, MAKELPARAM(close.x, close.y)) == HTCLIENT,
                    L"close hit test consumes the measured geometry");
                const auto file = outputDir / (L"toast-fit-" + std::wstring(sample.name) + suffix + L".png");
                ok &= Require(CaptureWindowPng(toast, file), L"toast fit capture contains valid pixels");
                ok &= RequireSurfacePixels(file, theme.color(L"toast", L"danger", L"bg"),
                    theme.color(L"toast", L"normal", L"bg"));
                ok &= RequireToastWindowPolicy(host, toast);
                std::wcout << L"toast_fit sample=" << sample.name << L" dpi=" << dpi << L" gdi=" << fallback
                    << L" available=" << width << L"x" << layout.text.bottom - layout.text.top
                    << L" required=" << required.cx << L"x" << required.cy << L"\n";
            }
            HWND badge = g_windowUi->ui().StatusBadge(
                L"保存失败", 20, 20, g_windowUi->ui().scale(180), ThemedStatusRole::Danger);
            const auto badgeFile = outputDir / (L"badge-danger" + suffix + L".png");
            ok &= Require(CaptureWindowPng(badge, badgeFile), L"danger badge capture contains valid pixels");
            ok &= RequireSurfacePixels(badgeFile, theme.color(L"global", L"danger", L"bg"),
                theme.color(L"dialog", L"normal", L"bg"));
            DestroyWindow(badge);

            // An embedded facade must work even when its host does not forward common messages.
            g_forwardCommonMessages = false;
            for (auto anchor : {ThemedToastAnchor::OwnerBottomRight, ThemedToastAnchor::OwnerTopRight,
                    ThemedToastAnchor::ScreenBottomRight}) {
                SetWindowPos(host, HWND_BOTTOM, 120, 120, 640, 420, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                ThemedToastOptions options;
                options.durationMs = 0;
                options.anchor = anchor;
                g_windowUi->ui().ShowToast(L"跟随位置验证", options);
                HWND toast = FindToastWindow(host);
                RECT before{}, after{};
                GetWindowRect(toast, &before);
                SetWindowPos(host, HWND_BOTTOM, 200, 160, 640, 420, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                GetWindowRect(toast, &after);
                const bool screen = anchor == ThemedToastAnchor::ScreenBottomRight;
                ok &= Require(after.left - before.left == (screen ? 0 : 80) &&
                    after.top - before.top == (screen ? 0 : 40), L"toast follows only owner anchor movement");
                before = after;
                SetWindowPos(host, HWND_BOTTOM, 200, 160, 700, 450, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                GetWindowRect(toast, &after);
                ok &= Require(after.left - before.left == (screen ? 0 : 60) &&
                    after.top - before.top == (anchor == ThemedToastAnchor::OwnerBottomRight ? 30 : 0),
                    L"toast follows its anchor when owner is resized");
                std::wcout << L"toast_anchor dpi=" << dpi << L" gdi=" << fallback
                    << L" anchor=" << static_cast<int>(anchor) << L" moved_and_resized=checked\n";
                if (!screen) {
                    const auto file = outputDir / (L"toast-moved-" + std::to_wstring(static_cast<int>(anchor)) + suffix + L".png");
                    ok &= Require(CaptureWindowPng(toast, file), L"moved toast capture is valid");
                    ShowWindow(host, SW_HIDE);
                    ok &= Require(!IsWindowVisible(toast), L"hiding owner dismisses its toast");
                    ShowWindow(host, SW_SHOWNOACTIVATE);
                    SetWindowPos(host, HWND_BOTTOM, 0, 0, 0, 0,
                        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                    ok &= Require(!IsWindowVisible(toast), L"restoring owner does not revive a stale toast");
                    g_windowUi->ui().ShowToast(L"最小化状态验证", options);
                    SendMessageW(host, WM_SIZE, SIZE_MINIMIZED, 0);
                    ok &= Require(!IsWindowVisible(toast), L"owner minimized state dismisses its toast");
                }
            }
            ThemedToastOptions timeout;
            timeout.durationMs = 80;
            g_windowUi->ui().ShowToast(L"不转发消息时仍自动关闭", timeout);
            PumpMessages(160);
            ok &= Require(!IsWindowVisible(FindToastWindow(host)), L"toast lifetime does not depend on host message forwarding");
            g_forwardCommonMessages = true;
        }
    }
    g_windowUi.reset();
    SetEnvironmentVariableW(L"QUATTRO_FORCE_GDI_FALLBACK", nullptr);
    g_windowUi = std::make_unique<ThemedWindowUi>(
        GetModuleHandleW(nullptr), nullptr, host, theme, DialogLayoutKind::Compact, 640, 420);
    HWND detachedHost = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"STATIC", L"",
        WS_POPUP, 120, 120, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ok &= Require(detachedHost != nullptr, L"independent toast lifecycle host created");
    if (detachedHost) {
        ShowWindow(detachedHost, SW_SHOWNOACTIVATE);
        SetWindowPos(detachedHost, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        ThemedWindowUi detached(GetModuleHandleW(nullptr), nullptr, detachedHost, theme,
            DialogLayoutKind::Compact, 320, 240);
        detached.ui().ShowToast(L"宿主销毁验证");
        const HWND toast = FindToastWindow(detachedHost);
        ok &= Require(toast != nullptr, L"independent toast created before owner teardown");
        DestroyWindow(detachedHost);
        ok &= Require(!IsWindow(toast) && detached.hwnd() == nullptr,
            L"owner teardown detaches a surviving facade and destroys its toast");
        detached.ui().ShowToast(L"宿主已销毁");
        ok &= Require(FindToastWindow(detachedHost) == nullptr, L"destroyed owner cannot create another toast");
    }
    return ok;
}

bool RunTaskProgressAcceptance(HWND owner, const Theme& theme, const std::filesystem::path& outputDir) {
    bool ok = true;
    for (UINT dpi : {96u, 120u, 144u}) {
        TaskProgressSnapshot snapshot;
        snapshot.status = L"正在扫描";
        snapshot.detail = L"正在读取文件";
        snapshot.error = L"读取失败，请重试。";
        snapshot.current = 3;
        snapshot.total = 10;
        snapshot.indeterminate = false;
        snapshot.workerCount = 2;
        snapshot.taskStatus = TaskStatus::Running;
        ThemedTaskProgressDialogOptions options;
        options.owner = owner;
        options.instance = GetModuleHandleW(nullptr);
        options.theme = theme;
        options.className = L"QuattroTaskProgressAcceptance";
        options.title = L"任务进度验收";
        options.closeOnCompleted = false;
        options.readSnapshot = [&] { return ToThemedTaskProgressSnapshot(snapshot); };
        ThemedTaskProgressDialog dialog(options);
        ok &= Require(dialog.Show(), L"task progress window created");
        if (!dialog.hwnd()) continue;
        RECT rect{};
        GetWindowRect(dialog.hwnd(), &rect);
        rect.right = rect.left + MulDiv(rect.right - rect.left, dpi, 96);
        rect.bottom = rect.top + MulDiv(rect.bottom - rect.top, dpi, 96);
        SendMessageW(dialog.hwnd(), WM_DPICHANGED, MAKEWPARAM(dpi, dpi),
            reinterpret_cast<LPARAM>(&rect));
        const struct {
            TaskStatus status;
            const wchar_t* name;
            const wchar_t* text;
            const wchar_t* role;
        } cases[] = {
            {TaskStatus::Running, L"running", L"正在扫描", L"info"},
            {TaskStatus::Stopped, L"stopped", L"任务已停止", L"warning"},
            {TaskStatus::Failed, L"failed", L"任务失败", L"danger"},
            {TaskStatus::Completed, L"completed", L"任务完成", L"success"},
        };
        for (const auto& state : cases) {
            snapshot.taskStatus = state.status;
            PumpMessages(120);
            struct TextQuery { const wchar_t* text; HWND hwnd = nullptr; } query{state.text};
            EnumChildWindows(dialog.hwnd(), [](HWND child, LPARAM value) -> BOOL {
                auto& query = *reinterpret_cast<TextQuery*>(value);
                if (WindowText(child) == query.text) query.hwnd = child;
                return TRUE;
            }, reinterpret_cast<LPARAM>(&query));
            ok &= Require(query.hwnd != nullptr, L"task progress displays the current terminal status");
            const auto file = outputDir /
                (L"task-progress-" + std::wstring(state.name) + L"-" + std::to_wstring(dpi) + L".png");
            ok &= Require(CaptureWindowPng(dialog.hwnd(), file), L"task progress screenshot is valid");
            if (query.hwnd) {
                RECT statusRect{}, windowRect{};
                GetWindowRect(query.hwnd, &statusRect);
                GetWindowRect(dialog.hwnd(), &windowRect);
                OffsetRect(&statusRect, -windowRect.left, -windowRect.top);
                const Color expected = theme.color(L"global", state.role, L"text");
                Gdiplus::Bitmap image(file.c_str());
                int coloredPixels = 0;
                for (int y = std::max(0L, statusRect.top);
                        y < std::min<LONG>(statusRect.bottom, image.GetHeight()); ++y) {
                    for (int x = std::max(0L, statusRect.left);
                            x < std::min<LONG>(statusRect.right, image.GetWidth()); ++x) {
                        Gdiplus::Color pixel;
                        image.GetPixel(x, y, &pixel);
                        const int distance = std::abs(int(pixel.GetR()) - int(expected.r * 255)) +
                            std::abs(int(pixel.GetG()) - int(expected.g * 255)) +
                            std::abs(int(pixel.GetB()) - int(expected.b * 255));
                        coloredPixels += distance < 45;
                    }
                }
                ok &= Require(coloredPixels >= 5, L"task progress status applies the semantic role color");
                std::wcout << L"task_progress dpi=" << dpi << L" state=" << state.name
                           << L" role_pixels=" << coloredPixels << L"\n";
            }
        }
        dialog.Close();
    }
    return ok;
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
        ok &= RequireToastWindowPolicy(host, toast);
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

    ok &= RunToastRegressionAcceptance(host, theme, outputDir);
    ok &= RunTaskProgressAcceptance(host, theme, outputDir);
    DestroyWindow(host);
    PumpMessages(60);
    ok &= Require(FindToastWindow(host) == nullptr, L"destroying the host destroys its owned toast");
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
