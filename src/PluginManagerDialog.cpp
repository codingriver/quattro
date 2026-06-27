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
constexpr int ID_PLUGIN_FILTER = 6107;
constexpr int ID_PLUGIN_PREV_PAGE = 6108;
constexpr int ID_PLUGIN_NEXT_PAGE = 6109;

constexpr int kPageSize = 20;
constexpr int kDialogWidth = 760;
constexpr int kDialogHeight = 560;
constexpr int kListX = 24;
constexpr int kListY = 142;
constexpr int kListWidth = 712;
constexpr int kListHeight = 306;
constexpr int kHeaderY = 114;
constexpr int kRowHeight = 48;

enum class PluginFilter {
    All = 0,
    NotInstalled,
    Installed,
    Enabled,
    Disabled,
};

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
}

COLORREF Blend(COLORREF a, COLORREF b, float t) {
    t = ClampFloat(t, 0.0f, 1.0f);
    const int ar = GetRValue(a);
    const int ag = GetGValue(a);
    const int ab = GetBValue(a);
    const int br = GetRValue(b);
    const int bg = GetGValue(b);
    const int bb = GetBValue(b);
    return RGB(
        static_cast<int>(ar + (br - ar) * t),
        static_cast<int>(ag + (bg - ag) * t),
        static_cast<int>(ab + (bb - ab) * t));
}

void FillRectColor(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void DrawLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawCircle(HDC dc, int cx, int cy, int radius, COLORREF fill, COLORREF border) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

std::wstring CategoryLabel(const std::wstring& category) {
    if (category == L"builtin-tools") {
        return L"内置工具";
    }
    if (category == L"theme" || category == L"theme-pack") {
        return L"主题";
    }
    if (category == L"icons" || category == L"icon-pack") {
        return L"图标";
    }
    if (category == L"link-pack") {
        return L"启动项包";
    }
    return category.empty() ? L"其他" : category;
}

std::wstring StateLabel(const PluginRecord& plugin) {
    if (!plugin.installed) {
        return L"未安装";
    }
    return plugin.enabled ? L"已启用" : L"已禁用";
}

std::wstring FilterLabel(PluginFilter filter) {
    switch (filter) {
    case PluginFilter::NotInstalled:
        return L"未安装";
    case PluginFilter::Installed:
        return L"已安装";
    case PluginFilter::Enabled:
        return L"已启用";
    case PluginFilter::Disabled:
        return L"已禁用";
    case PluginFilter::All:
    default:
        return L"全部";
    }
}

bool MatchesFilter(const PluginRecord& plugin, PluginFilter filter) {
    switch (filter) {
    case PluginFilter::NotInstalled:
        return !plugin.installed;
    case PluginFilter::Installed:
        return plugin.installed;
    case PluginFilter::Enabled:
        return plugin.installed && plugin.enabled;
    case PluginFilter::Disabled:
        return plugin.installed && !plugin.enabled;
    case PluginFilter::All:
    default:
        return true;
    }
}

std::wstring DisplayDate(const std::wstring& value) {
    if (value.empty()) {
        return L"-";
    }
    std::wstring text = value;
    std::replace(text.begin(), text.end(), L'T', L' ');
    if (text.size() > 19) {
        text.resize(19);
    }
    return text;
}

std::wstring FirstNonEmpty(const std::vector<std::wstring>& values) {
    for (const auto& value : values) {
        if (!value.empty()) {
            return value;
        }
    }
    return {};
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
            kDialogWidth,
            kDialogHeight,
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
            ApplyFilterAndPagination();
            SelectVisiblePlugin(0);
            return 0;
        case WM_COMMAND:
            return HandleCommand(wParam);
        case WM_MEASUREITEM:
            if (reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->CtlID == ID_PLUGIN_LIST) {
                reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = kRowHeight;
                return TRUE;
            }
            return 0;
        case WM_DRAWITEM: {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw && draw->CtlID == ID_PLUGIN_LIST) {
                DrawPluginRow(draw);
                return TRUE;
            }
            if (ThemedControls::Draw(theme_, draw)) {
                return TRUE;
            }
            return 0;
        }
        case WM_NOTIFY:
            return HandleNotify(reinterpret_cast<NMHDR*>(lParam));
        case WM_PAINT:
            Paint();
            return 0;
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
        case WM_CLOSE:
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            done_ = true;
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            Cleanup();
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    LRESULT HandleCommand(WPARAM wParam) {
        const int id = LOWORD(wParam);
        if (id == ID_PLUGIN_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
            SelectVisiblePlugin(static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0)));
            return 0;
        }
        if (id == ID_PLUGIN_FILTER && HIWORD(wParam) == CBN_SELCHANGE) {
            const LRESULT selected = SendMessageW(filterCombo_, CB_GETCURSEL, 0, 0);
            filter_ = selected >= 0 ? static_cast<PluginFilter>(selected) : PluginFilter::All;
            page_ = 0;
            ApplyFilterAndPagination();
            SelectVisiblePlugin(0);
            return 0;
        }
        if (id == ID_PLUGIN_PREV_PAGE) {
            if (page_ > 0) {
                --page_;
                ApplyFilterAndPagination();
                SelectVisiblePlugin(0);
            }
            return 0;
        }
        if (id == ID_PLUGIN_NEXT_PAGE) {
            if (page_ + 1 < PageCount()) {
                ++page_;
                ApplyFilterAndPagination();
                SelectVisiblePlugin(0);
            }
            return 0;
        }
        if (id == ID_PLUGIN_ENABLE || id == ID_PLUGIN_DISABLE) {
            const int pluginIndex = SelectedPluginIndex();
            if (pluginIndex >= 0 && pluginIndex < static_cast<int>(plugins_.size()) && plugins_[pluginIndex].installed) {
                const bool enable = id == ID_PLUGIN_ENABLE;
                if (!registry_.SetEnabled(plugins_[pluginIndex].id, enable)) {
                    MessageBoxW(hwnd_, registry_.lastError().c_str(), L"插件商店", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                plugins_[pluginIndex].enabled = enable;
                changed_ = true;
                ReloadPluginsKeepingSelection(plugins_[pluginIndex].id);
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
            ReloadPluginsKeepingSelection(selectedPluginId_);
            return 0;
        }
        if (id == IDCANCEL || id == IDOK) {
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        }
        return 0;
    }

    LRESULT HandleNotify(NMHDR* header) {
        if (!header || header->hwndFrom != tooltip_ || header->code != TTN_GETDISPINFOW) {
            return 0;
        }
        auto* info = reinterpret_cast<NMTTDISPINFOW*>(header);
        tooltipText_ = TooltipTextForSelection();
        info->lpszText = const_cast<LPWSTR>(tooltipText_.c_str());
        return 0;
    }

    void CreateControls() {
        font_ = ThemedControls::CreateDialogFont();
        HFONT font = font_ ? font_ : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        titleFont_ = CreateFontW(
            -20, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
        listBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"list", L"normal", L"bg")));

        title_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"插件商店", 24, 18, 180, 28, titleFont_ ? titleFont_ : font);
        subtitle_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"从远程仓库同步 manifest.json，安装、启用或禁用插件。", 24, 50, 520, 22, font);
        ThemedControls::CreateStaticText(instance_, hwnd_, L"状态", 24, 84, 42, 22, font);
        filterCombo_ = ThemedControls::CreateComboBox(instance_, hwnd_, ID_PLUGIN_FILTER, 68, 78, 132, 180, font, theme_);
        for (PluginFilter filter : {PluginFilter::All, PluginFilter::NotInstalled, PluginFilter::Installed, PluginFilter::Enabled, PluginFilter::Disabled}) {
            const std::wstring label = FilterLabel(filter);
            SendMessageW(filterCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SendMessageW(filterCombo_, CB_SETCURSEL, 0, 0);

        summary_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"", 218, 84, 300, 22, font);
        source_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"", 420, 84, 316, 22, font, SS_RIGHT);

        listFrame_ = RECT{kListX - 1, kListY - 1, kListX + kListWidth + 1, kListY + kListHeight + 1};
        list_ = CreateWindowExW(
            0,
            L"LISTBOX",
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
            kListX,
            kListY,
            kListWidth,
            kListHeight,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PLUGIN_LIST)),
            instance_,
            nullptr);
        SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(list_, LB_SETITEMHEIGHT, 0, kRowHeight);

        const int buttonHeight = ThemedControls::ButtonHeight(theme_);
        enableButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_ENABLE, L"启用", 24, 466, 74, buttonHeight, font);
        disableButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_DISABLE, L"禁用", 108, 466, 74, buttonHeight, font);
        installButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_INSTALL, L"安装", 192, 466, 74, buttonHeight, font);
        removeButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_REMOVE, L"删除", 276, 466, 74, buttonHeight, font);
        prevButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_PREV_PAGE, L"上一页", 432, 466, 76, buttonHeight, font);
        pageText_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"", 516, 471, 92, 22, font, SS_CENTER);
        nextButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_NEXT_PAGE, L"下一页", 616, 466, 76, buttonHeight, font);
        ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_REFRESH, L"刷新", 572, 18, 76, buttonHeight, font);
        ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"关闭", 660, 18, 76, buttonHeight, font, true);

        tooltip_ = CreateWindowExW(
            WS_EX_TOPMOST,
            TOOLTIPS_CLASSW,
            nullptr,
            WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            hwnd_,
            nullptr,
            instance_,
            nullptr);
        if (tooltip_) {
            SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 520);
            TOOLINFOW tool{};
            tool.cbSize = sizeof(tool);
            tool.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
            tool.hwnd = hwnd_;
            tool.uId = reinterpret_cast<UINT_PTR>(list_);
            tool.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
    }

    void Paint() {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

        DrawHeader(dc);
        ThemedControls::DrawListFrame(theme_, dc, listFrame_, list_);
        EndPaint(hwnd_, &ps);
    }

    void DrawHeader(HDC dc) {
        const COLORREF text = ToColorRef(theme_.color(L"label", L"normal", L"text"));
        const COLORREF muted = ToColorRef(theme_.color(L"label", L"muted", L"text"));
        const COLORREF line = ToColorRef(theme_.color(L"separator", L"normal", L"line"));
        const COLORREF panel = ToColorRef(theme_.color(L"panel", L"raised", L"bg"));

        RECT header{kListX, kHeaderY, kListX + kListWidth, kHeaderY + 28};
        FillRectColor(dc, header, panel);
        DrawLine(dc, header.left, header.bottom - 1, header.right, header.bottom - 1, line);

        HFONT font = font_ ? font_ : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ oldFont = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, muted);
        DrawHeaderText(dc, L"名称", ColumnName());
        DrawHeaderText(dc, L"版本", ColumnVersion());
        DrawHeaderText(dc, L"状态", ColumnState());
        DrawHeaderText(dc, L"分类", ColumnCategory());
        SetTextColor(dc, text);
        if (oldFont) {
            SelectObject(dc, oldFont);
        }
    }

    void DrawHeaderText(HDC dc, const wchar_t* text, RECT rect) {
        rect.top = kHeaderY + 4;
        rect.bottom = kHeaderY + 26;
        DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void DrawPluginRow(const DRAWITEMSTRUCT* draw) {
        if (!draw || draw->itemID == static_cast<UINT>(-1)) {
            return;
        }
        const int visibleIndex = static_cast<int>(draw->itemID);
        if (visibleIndex < 0 || visibleIndex >= static_cast<int>(pagePluginIndexes_.size())) {
            return;
        }
        const PluginRecord& plugin = plugins_[pagePluginIndexes_[visibleIndex]];
        const bool selected = (draw->itemState & ODS_SELECTED) != 0;
        const COLORREF listBg = ToColorRef(theme_.color(L"list", L"normal", L"bg"));
        const COLORREF selectedBg = ToColorRef(theme_.color(L"listItem", L"selected", L"bg"));
        const COLORREF rowBg = selected ? selectedBg : (visibleIndex % 2 == 0 ? listBg : Blend(listBg, RGB(244, 247, 250), 0.45f));
        const COLORREF text = ToColorRef(theme_.color(L"listItem", selected ? L"selected" : L"normal", L"text"));
        const COLORREF muted = ToColorRef(theme_.color(L"label", L"muted", L"text"));
        const COLORREF line = ToColorRef(theme_.color(L"separator", L"normal", L"line"));

        RECT row = draw->rcItem;
        FillRectColor(draw->hDC, row, rowBg);
        DrawLine(draw->hDC, row.left + 10, row.bottom - 1, row.right - 10, row.bottom - 1, line);

        HFONT font = font_ ? font_ : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ oldFont = SelectObject(draw->hDC, font);
        SetBkMode(draw->hDC, TRANSPARENT);

        RECT nameRect = ColumnName(row);
        RECT titleRect = nameRect;
        titleRect.top += 6;
        titleRect.bottom = titleRect.top + 19;
        SetTextColor(draw->hDC, text);
        DrawTextW(draw->hDC, plugin.name.c_str(), static_cast<int>(plugin.name.size()), &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT descRect = nameRect;
        descRect.top += 27;
        descRect.bottom = descRect.top + 17;
        SetTextColor(draw->hDC, muted);
        const std::wstring desc = plugin.description.empty() ? plugin.id : plugin.description;
        DrawTextW(draw->hDC, desc.c_str(), static_cast<int>(desc.size()), &descRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        DrawCellText(draw->hDC, plugin.version, ColumnVersion(row), text);
        DrawStatus(draw->hDC, plugin, ColumnState(row), text);
        DrawCellText(draw->hDC, CategoryLabel(plugin.category), ColumnCategory(row), muted);

        if (oldFont) {
            SelectObject(draw->hDC, oldFont);
        }
    }

    void DrawCellText(HDC dc, const std::wstring& value, RECT rect, COLORREF color) {
        rect.top += 13;
        rect.bottom = rect.top + 20;
        SetTextColor(dc, color);
        DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void DrawStatus(HDC dc, const PluginRecord& plugin, RECT rect, COLORREF textColor) {
        const COLORREF dot = StatusColor(plugin);
        DrawCircle(dc, rect.left + 6, rect.top + 24, 5, dot, dot);
        rect.left += 20;
        DrawCellText(dc, StateLabel(plugin), rect, textColor);
    }

    COLORREF StatusColor(const PluginRecord& plugin) const {
        if (plugin.installed && plugin.enabled) {
            return ToColorRef(theme_.color(L"global", L"success", L"text"));
        }
        if (plugin.installed) {
            return ToColorRef(theme_.color(L"text", L"disabled", L"text"));
        }
        return Blend(ToColorRef(theme_.color(L"text", L"disabled", L"text")), ToColorRef(theme_.color(L"dialog", L"normal", L"bg")), 0.35f);
    }

    RECT ColumnName() const {
        return RECT{kListX + 16, kListX, kListX + 356, kListX};
    }

    RECT ColumnVersion() const {
        return RECT{kListX + 372, kListX, kListX + 466, kListX};
    }

    RECT ColumnState() const {
        return RECT{kListX + 488, kListX, kListX + 594, kListX};
    }

    RECT ColumnCategory() const {
        return RECT{kListX + 616, kListX, kListX + kListWidth - 16, kListX};
    }

    RECT ColumnName(RECT row) const {
        return RECT{row.left + 16, row.top, row.left + 356, row.bottom};
    }

    RECT ColumnVersion(RECT row) const {
        return RECT{row.left + 372, row.top, row.left + 466, row.bottom};
    }

    RECT ColumnState(RECT row) const {
        return RECT{row.left + 488, row.top, row.left + 594, row.bottom};
    }

    RECT ColumnCategory(RECT row) const {
        return RECT{row.left + 616, row.top, row.right - 16, row.bottom};
    }

    void ApplyFilterAndPagination() {
        filteredPluginIndexes_.clear();
        for (int i = 0; i < static_cast<int>(plugins_.size()); ++i) {
            if (MatchesFilter(plugins_[i], filter_)) {
                filteredPluginIndexes_.push_back(i);
            }
        }
        const int pages = PageCount();
        if (page_ >= pages) {
            page_ = std::max(0, pages - 1);
        }

        pagePluginIndexes_.clear();
        const int start = page_ * kPageSize;
        const int end = std::min(start + kPageSize, static_cast<int>(filteredPluginIndexes_.size()));
        for (int i = start; i < end; ++i) {
            pagePluginIndexes_.push_back(filteredPluginIndexes_[i]);
        }

        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        for (int pluginIndex : pagePluginIndexes_) {
            const PluginRecord& plugin = plugins_[pluginIndex];
            const std::wstring text = plugin.name + L"\t" + plugin.version + L"\t" + StateLabel(plugin) + L"\t" + CategoryLabel(plugin.category);
            const LRESULT item = SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            if (item >= 0) {
                SendMessageW(list_, LB_SETITEMDATA, static_cast<WPARAM>(item), static_cast<LPARAM>(pluginIndex));
            }
        }
        UpdateSummary();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    int PageCount() const {
        if (filteredPluginIndexes_.empty()) {
            return 1;
        }
        return static_cast<int>((filteredPluginIndexes_.size() + kPageSize - 1) / kPageSize);
    }

    void UpdateSummary() {
        int enabled = 0;
        int disabled = 0;
        int notInstalled = 0;
        for (const auto& plugin : plugins_) {
            if (!plugin.installed) {
                ++notInstalled;
            } else if (plugin.enabled) {
                ++enabled;
            } else {
                ++disabled;
            }
        }
        const std::wstring summary = L"共 " + std::to_wstring(plugins_.size()) +
            L" 个，当前 " + std::to_wstring(filteredPluginIndexes_.size()) +
            L" 个    启用 " + std::to_wstring(enabled) +
            L" / 禁用 " + std::to_wstring(disabled) +
            L" / 未安装 " + std::to_wstring(notInstalled);
        SetWindowTextW(summary_, summary.c_str());

        const std::wstring sourceText = storeUrl_.empty() ? L"来源：plugins/store/index.json" : L"来源：" + storeUrl_;
        SetWindowTextW(source_, sourceText.c_str());

        const std::wstring pageText = L"第 " + std::to_wstring(page_ + 1) + L" / " + std::to_wstring(PageCount()) + L" 页";
        SetWindowTextW(pageText_, pageText.c_str());
        EnableWindow(prevButton_, page_ > 0);
        EnableWindow(nextButton_, page_ + 1 < PageCount());
    }

    void SelectVisiblePlugin(int visibleIndex) {
        if (pagePluginIndexes_.empty()) {
            selectedPluginId_.clear();
            EnableWindow(enableButton_, FALSE);
            EnableWindow(disableButton_, FALSE);
            EnableWindow(installButton_, FALSE);
            EnableWindow(removeButton_, FALSE);
            return;
        }
        visibleIndex = std::max(0, std::min(visibleIndex, static_cast<int>(pagePluginIndexes_.size()) - 1));
        SendMessageW(list_, LB_SETCURSEL, visibleIndex, 0);
        const PluginRecord& plugin = plugins_[pagePluginIndexes_[visibleIndex]];
        selectedPluginId_ = plugin.id;
        EnableWindow(enableButton_, plugin.installed && !plugin.enabled);
        EnableWindow(disableButton_, plugin.installed && plugin.enabled);
        EnableWindow(installButton_, !plugin.installed && !plugin.builtin);
        EnableWindow(removeButton_, plugin.installed && plugin.deletable && !plugin.builtin);
    }

    int SelectedVisibleIndex() const {
        const int visibleIndex = static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
        if (visibleIndex < 0 || visibleIndex >= static_cast<int>(pagePluginIndexes_.size())) {
            return -1;
        }
        return visibleIndex;
    }

    int SelectedPluginIndex() const {
        const int visibleIndex = SelectedVisibleIndex();
        return visibleIndex < 0 ? -1 : pagePluginIndexes_[visibleIndex];
    }

    void ReloadPluginsKeepingSelection(const std::wstring& pluginId) {
        plugins_ = registry_.LoadPlugins();
        ApplyFilterAndPagination();
        int visible = FindVisibleIndex(pluginId);
        if (visible < 0 && page_ > 0) {
            page_ = 0;
            ApplyFilterAndPagination();
            visible = FindVisibleIndex(pluginId);
        }
        SelectVisiblePlugin(visible >= 0 ? visible : 0);
    }

    int FindVisibleIndex(const std::wstring& pluginId) const {
        if (pluginId.empty()) {
            return -1;
        }
        for (int i = 0; i < static_cast<int>(pagePluginIndexes_.size()); ++i) {
            if (plugins_[pagePluginIndexes_[i]].id == pluginId) {
                return i;
            }
        }
        return -1;
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
        const int pluginIndex = SelectedPluginIndex();
        if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
            return;
        }
        const std::wstring pluginId = plugins_[pluginIndex].id;
        PluginStoreService store(appDirectory_, storeUrl_);
        store.Refresh(registry_);
        if (!store.Install(pluginId, registry_, storage_)) {
            MessageBoxW(hwnd_, store.lastError().c_str(), L"安装插件", MB_OK | MB_ICONWARNING);
            return;
        }
        changed_ = true;
        ReloadPluginsKeepingSelection(pluginId);
    }

    void RemoveSelected() {
        const int pluginIndex = SelectedPluginIndex();
        if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
            return;
        }
        const PluginRecord& plugin = plugins_[pluginIndex];
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
        ReloadPluginsKeepingSelection(plugin.id);
    }

    std::wstring TooltipTextForSelection() const {
        int pluginIndex = HoveredPluginIndex();
        if (pluginIndex < 0) {
            pluginIndex = SelectedPluginIndex();
        }
        if (pluginIndex < 0) {
            return L"暂无插件";
        }
        const PluginRecord& plugin = plugins_[pluginIndex];
        const std::wstring remote = FirstNonEmpty({plugin.sourceUrl, plugin.packageUrl, plugin.homepageUrl});
        std::wstring text = plugin.name + L"\r\n";
        text += L"版本：" + plugin.version + L"\r\n";
        text += L"状态：" + StateLabel(plugin) + L"\r\n";
        text += L"分类：" + CategoryLabel(plugin.category) + L"\r\n";
        text += L"添加时间：" + DisplayDate(FirstNonEmpty({plugin.addedAt, plugin.createdAt})) + L"\r\n";
        if (plugin.installed) {
            text += L"更新时间：" + DisplayDate(plugin.updatedAt) + L"\r\n";
        }
        if (!remote.empty()) {
            text += L"远程链接：" + remote + L"\r\n";
        }
        if (!plugin.homepageUrl.empty() && plugin.homepageUrl != remote) {
            text += L"主页：" + plugin.homepageUrl + L"\r\n";
        }
        if (!plugin.author.empty()) {
            text += L"作者：" + plugin.author + L"\r\n";
        }
        if (!plugin.permissions.empty()) {
            text += L"权限：" + plugin.permissions;
        }
        return text;
    }

    void Cleanup() {
        if (tooltip_) {
            DestroyWindow(tooltip_);
            tooltip_ = nullptr;
        }
        if (font_) {
            DeleteObject(font_);
            font_ = nullptr;
        }
        if (titleFont_) {
            DeleteObject(titleFont_);
            titleFont_ = nullptr;
        }
        if (backgroundBrush_) {
            DeleteObject(backgroundBrush_);
            backgroundBrush_ = nullptr;
        }
        if (listBrush_) {
            DeleteObject(listBrush_);
            listBrush_ = nullptr;
        }
    }

    int HoveredPluginIndex() const {
        if (!list_) {
            return -1;
        }
        POINT point{};
        GetCursorPos(&point);
        ScreenToClient(list_, &point);
        RECT listRect{};
        GetClientRect(list_, &listRect);
        if (!PtInRect(&listRect, point)) {
            return -1;
        }
        const LRESULT hit = SendMessageW(list_, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
        if (HIWORD(hit) != 0) {
            return -1;
        }
        const int visibleIndex = LOWORD(hit);
        if (visibleIndex < 0 || visibleIndex >= static_cast<int>(pagePluginIndexes_.size())) {
            return -1;
        }
        return pagePluginIndexes_[visibleIndex];
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    PluginRegistry& registry_;
    StorageService& storage_;
    std::filesystem::path appDirectory_;
    std::wstring storeUrl_;
    HWND hwnd_ = nullptr;
    HWND title_ = nullptr;
    HWND subtitle_ = nullptr;
    HWND summary_ = nullptr;
    HWND source_ = nullptr;
    HWND filterCombo_ = nullptr;
    HWND list_ = nullptr;
    HWND enableButton_ = nullptr;
    HWND disableButton_ = nullptr;
    HWND installButton_ = nullptr;
    HWND removeButton_ = nullptr;
    HWND prevButton_ = nullptr;
    HWND nextButton_ = nullptr;
    HWND pageText_ = nullptr;
    HWND tooltip_ = nullptr;
    RECT listFrame_{};
    HFONT font_ = nullptr;
    HFONT titleFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH listBrush_ = nullptr;
    std::vector<PluginRecord> plugins_;
    std::vector<int> filteredPluginIndexes_;
    std::vector<int> pagePluginIndexes_;
    std::vector<StorePluginDefinition> storePlugins_;
    PluginFilter filter_ = PluginFilter::All;
    int page_ = 0;
    std::wstring selectedPluginId_;
    std::wstring tooltipText_;
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
