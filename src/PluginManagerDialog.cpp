#include "PluginManagerDialog.h"

#include "AppLog.h"
#include "PluginStoreService.h"
#include "ThemedControls.h"
#include "Utilities.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace {
constexpr int ID_PLUGIN_LIST = 6101;
constexpr int ID_PLUGIN_ENABLE = 6102;
constexpr int ID_PLUGIN_DISABLE = 6103;
constexpr int ID_PLUGIN_INSTALL = 6104;
constexpr int ID_PLUGIN_REMOVE = 6105;
constexpr int ID_PLUGIN_REFRESH = 6106;

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
}

std::wstring CategoryLabel(const std::wstring& category) {
    if (category == L"builtin-tools") {
        return L"内置工具";
    }
    if (category == L"theme") {
        return L"主题";
    }
    if (category == L"icons") {
        return L"图标";
    }
    return category.empty() ? L"其他" : category;
}

class PluginManagerDialog {
public:
    PluginManagerDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        PluginRegistry& registry,
        StorageService& storage,
        std::filesystem::path appDirectory,
        std::wstring storeUrl)
        : owner_(owner),
          instance_(instance),
          theme_(theme),
          registry_(registry),
          storage_(storage),
          appDirectory_(std::move(appDirectory)),
          storeUrl_(std::move(storeUrl)) {}

    bool Run() {
        registry_.Initialize();
        RefreshStore(false);
        plugins_ = registry_.LoadPlugins();

        const std::wstring className = L"QuattroPluginManagerDialog_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PluginManagerDialog::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = className.c_str();
        if (!RegisterClassExW(&wc)) {
            const DWORD error = GetLastError();
            if (error != ERROR_CLASS_ALREADY_EXISTS) {
                WriteAppLog(L"插件商店窗口类注册失败: " + FormatLastError(error));
                return false;
            }
        }

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            className.c_str(),
            L"插件商店",
            WS_CAPTION | WS_SYSMENU | WS_POPUP,
            ownerRect.left + 48,
            ownerRect.top + 48,
            620,
            430,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            WriteAppLog(L"插件商店窗口创建失败: " + FormatLastError(GetLastError()));
            return false;
        }

        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
        return changed_;
    }

private:
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        PluginManagerDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<PluginManagerDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<PluginManagerDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            CreateControls();
            RefreshList();
            SelectPlugin(0);
            return 0;
        case WM_COMMAND: {
            const int id = LOWORD(wParam);
            if (id == ID_PLUGIN_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
                SelectPlugin(static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0)));
                return 0;
            }
            if (id == ID_PLUGIN_ENABLE || id == ID_PLUGIN_DISABLE) {
                const int index = static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
                if (index >= 0 && index < static_cast<int>(plugins_.size()) && plugins_[index].installed) {
                    const bool enable = id == ID_PLUGIN_ENABLE;
                    if (!registry_.SetEnabled(plugins_[index].id, enable)) {
                        MessageBoxW(hwnd_, registry_.lastError().c_str(), L"插件商店", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    plugins_[index].enabled = enable;
                    changed_ = true;
                    RefreshList();
                    SelectPlugin(index);
                }
                return 0;
            }
            if (id == ID_PLUGIN_INSTALL) {
                InstallSelected();
                return 0;
            }
            if (id == ID_PLUGIN_REMOVE) {
                RemoveSelected();
                return 0;
            }
            if (id == ID_PLUGIN_REFRESH) {
                RefreshStore(true);
                plugins_ = registry_.LoadPlugins();
                RefreshList();
                SelectPlugin(0);
                return 0;
            }
            if (id == IDCANCEL || id == IDOK) {
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rect{};
            GetClientRect(hwnd_, &rect);
            FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            ThemedControls::DrawListFrame(theme_, dc, listFrame_, list_);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
            if (message == WM_CTLCOLORLISTBOX) {
                SetBkColor(dc, ToColorRef(theme_.color(L"list", L"normal", L"bg")));
                return reinterpret_cast<LRESULT>(listBrush_ ? listBrush_ : GetStockObject(WHITE_BRUSH));
            }
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_CLOSE:
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            done_ = true;
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            if (font_) {
                DeleteObject(font_);
                font_ = nullptr;
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
                backgroundBrush_ = nullptr;
            }
            if (listBrush_) {
                DeleteObject(listBrush_);
                listBrush_ = nullptr;
            }
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void CreateControls() {
        font_ = ThemedControls::CreateDialogFont();
        HFONT font = font_ ? font_ : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
        listBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"list", L"normal", L"bg")));

        ThemedControls::CreateStaticText(instance_, hwnd_, L"分类", 26, 18, 180, 22, font);
        ThemedControls::CreateStaticText(instance_, hwnd_, L"插件详情", 252, 18, 260, 22, font);

        listFrame_ = RECT{24, 46, 220, 330};
        list_ = CreateWindowExW(
            0,
            L"LISTBOX",
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
            32,
            54,
            180,
            268,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PLUGIN_LIST)),
            instance_,
            nullptr);
        SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);

        name_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"", 252, 52, 320, 24, font);
        status_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"", 252, 84, 320, 24, font);
        desc_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"", 252, 122, 320, 72, font, SS_LEFT | SS_WORDELLIPSIS);
        permissions_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"", 252, 214, 320, 56, font, SS_LEFT | SS_WORDELLIPSIS);

        const int buttonHeight = ThemedControls::ButtonHeight(theme_);
        enableButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_ENABLE, L"启用", 252, 292, 74, buttonHeight, font);
        disableButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_DISABLE, L"禁用", 336, 292, 74, buttonHeight, font);
        installButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_INSTALL, L"安装", 420, 292, 74, buttonHeight, font);
        removeButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_REMOVE, L"删除", 504, 292, 66, buttonHeight, font);
        ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_REFRESH, L"刷新商店", 252, 348, 96, buttonHeight, font);
        ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"关闭", 486, 348, 84, buttonHeight, font, true);
    }

    void RefreshList() {
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        for (const auto& plugin : plugins_) {
            const std::wstring state = !plugin.installed ? L"未安装" : (plugin.enabled ? L"已启用" : L"已禁用");
            const std::wstring text = L"[" + CategoryLabel(plugin.category) + L"] " + plugin.name + L" - " + state;
            SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
        }
    }

    void SelectPlugin(int index) {
        if (plugins_.empty()) {
            SetWindowTextW(name_, L"暂无插件");
            SetWindowTextW(status_, L"");
            SetWindowTextW(desc_, L"");
            SetWindowTextW(permissions_, L"");
        EnableWindow(enableButton_, FALSE);
        EnableWindow(disableButton_, FALSE);
        EnableWindow(installButton_, FALSE);
        EnableWindow(removeButton_, FALSE);
            return;
        }
        index = std::max(0, std::min(index, static_cast<int>(plugins_.size()) - 1));
        SendMessageW(list_, LB_SETCURSEL, index, 0);
        const PluginRecord& plugin = plugins_[index];
        SetWindowTextW(name_, (plugin.name + L"  " + plugin.version).c_str());
        const std::wstring status = L"分类：" + CategoryLabel(plugin.category) +
            L"    类型：" + plugin.kind +
            L"    状态：" + (!plugin.installed ? L"未安装" : (plugin.enabled ? L"已启用" : L"已禁用"));
        SetWindowTextW(status_, status.c_str());
        SetWindowTextW(desc_, plugin.description.c_str());
        const std::wstring permissions = plugin.permissions.empty() ? L"权限：无" : L"权限：" + plugin.permissions;
        SetWindowTextW(permissions_, permissions.c_str());
        EnableWindow(enableButton_, plugin.installed && !plugin.enabled);
        EnableWindow(disableButton_, plugin.installed && plugin.enabled);
        EnableWindow(installButton_, !plugin.installed && !plugin.builtin);
        EnableWindow(removeButton_, plugin.installed && plugin.deletable && !plugin.builtin);
    }

    void RefreshStore(bool showResult) {
        PluginStoreService store(appDirectory_, storeUrl_);
        if (!store.Refresh(registry_)) {
            if (showResult) {
                MessageBoxW(hwnd_, store.lastError().c_str(), L"刷新插件商店", MB_OK | MB_ICONWARNING);
            }
            return;
        }
        storePlugins_ = store.plugins();
        if (showResult) {
            MessageBoxW(hwnd_, L"插件商店已刷新。", L"刷新插件商店", MB_OK | MB_ICONINFORMATION);
        }
    }

    void InstallSelected() {
        const int index = static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
        if (index < 0 || index >= static_cast<int>(plugins_.size())) {
            return;
        }
        PluginStoreService store(appDirectory_, storeUrl_);
        store.Refresh(registry_);
        if (!store.Install(plugins_[index].id, registry_, storage_)) {
            MessageBoxW(hwnd_, store.lastError().c_str(), L"安装插件", MB_OK | MB_ICONWARNING);
            return;
        }
        changed_ = true;
        plugins_ = registry_.LoadPlugins();
        RefreshList();
        SelectPlugin(index);
    }

    void RemoveSelected() {
        const int index = static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
        if (index < 0 || index >= static_cast<int>(plugins_.size())) {
            return;
        }
        const PluginRecord& plugin = plugins_[index];
        if (plugin.builtin || !plugin.deletable) {
            return;
        }
        std::wstring message = L"确定删除插件“" + plugin.name + L"”及其创建的内容？";
        if (MessageBoxW(hwnd_, message.c_str(), L"删除插件", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) {
            return;
        }
        PluginStoreService store(appDirectory_, storeUrl_);
        if (!store.Remove(plugin.id, registry_, storage_)) {
            MessageBoxW(hwnd_, store.lastError().c_str(), L"删除插件", MB_OK | MB_ICONWARNING);
            return;
        }
        changed_ = true;
        plugins_ = registry_.LoadPlugins();
        RefreshList();
        SelectPlugin(std::min(index, static_cast<int>(plugins_.size()) - 1));
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    PluginRegistry& registry_;
    StorageService& storage_;
    std::filesystem::path appDirectory_;
    std::wstring storeUrl_;
    HWND hwnd_ = nullptr;
    HWND list_ = nullptr;
    HWND name_ = nullptr;
    HWND status_ = nullptr;
    HWND desc_ = nullptr;
    HWND permissions_ = nullptr;
    HWND enableButton_ = nullptr;
    HWND disableButton_ = nullptr;
    HWND installButton_ = nullptr;
    HWND removeButton_ = nullptr;
    RECT listFrame_{};
    HFONT font_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH listBrush_ = nullptr;
    std::vector<PluginRecord> plugins_;
    std::vector<StorePluginDefinition> storePlugins_;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool changed_ = false;
    bool done_ = false;
};
}

bool ShowPluginManagerDialog(
    HWND owner,
    HINSTANCE instance,
    const Theme& theme,
    PluginRegistry& registry,
    StorageService& storage,
    const std::filesystem::path& appDirectory,
    const std::wstring& storeUrl) {
    PluginManagerDialog dialog(owner, instance, theme, registry, storage, appDirectory, storeUrl);
    return dialog.Run();
}
