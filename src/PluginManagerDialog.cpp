#include "PluginManagerDialog.h"

#include "AppLog.h"
#include "CatalogDialogLayout.h"
#include "PluginStoreService.h"
#include "ThemedControls.h"
#include "Utilities.h"

#include <commctrl.h>
#include <shellapi.h>
#include <wininet.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr int ID_PLUGIN_LIST = 6101;
constexpr int ID_PLUGIN_ENABLE = 6102;
constexpr int ID_PLUGIN_DISABLE = 6103;
constexpr int ID_PLUGIN_INSTALL = 6104;
constexpr int ID_PLUGIN_REMOVE = 6105;
constexpr int ID_PLUGIN_REFRESH = 6106;
constexpr int ID_PLUGIN_FILTER_BASE = 6107;
constexpr int ID_PLUGIN_PREV_PAGE = 6108;
constexpr int ID_PLUGIN_NEXT_PAGE = 6109;
constexpr int ID_PLUGIN_TOGGLE_FAVORITE = 6111;
constexpr int ID_PLUGIN_SOURCE_LINK = 6112;
constexpr int ID_PLUGIN_OPEN_SOURCE = 6113;
constexpr int ID_PLUGIN_COPY_SOURCE = 6114;
constexpr int ID_PLUGIN_SEARCH = 6115;
constexpr int ID_PLUGIN_SORT = 6116;
constexpr int ID_PLUGIN_VIEW = 6117;
constexpr int ID_PLUGIN_SORT_BASE = 6120;
constexpr int ID_PLUGIN_VIEW_BASE = 6130;
constexpr UINT WM_PLUGIN_ICONS_READY = WM_APP + 41;

constexpr int kPageSize = 20;
constexpr int kDialogWidth = 760;
constexpr int kDialogHeight = 840;
constexpr int kMinDialogWidth = 640;
constexpr int kMinDialogHeight = 560;
constexpr int kListRowHeight = 64;
constexpr int kCompactRowHeight = 48;
constexpr int kGridRowHeight = 96;
constexpr int kGridColumns = 3;
constexpr int kRefreshWidth = 76;
constexpr int kButtonWidth = 76;
constexpr int kSortButtonWidth = 116;
constexpr int kViewButtonWidth = 116;
constexpr int kPageTextWidth = 86;
constexpr int kGap = 8;
constexpr int kSourceDialogWidth = 520;
constexpr int kSourceDialogHeight = 260;
constexpr int kPluginIconSize = 32;
constexpr int kPluginIconBox = 38;

CatalogDialogMetrics PluginCatalogMetrics() {
    CatalogDialogMetrics metrics{};
    metrics.contentPaddingX = 24;
    metrics.toolbarHeight = 24;
    metrics.verticalGap = 4;
    metrics.toolbarY = metrics.verticalGap;
    metrics.statusBarHeight = 22;
    metrics.pagerHeight = 24;
    metrics.bottomPadding = 0;
    metrics.horizontalGap = kGap;
    metrics.minListHeight = 80;
    metrics.searchWidth = 180;
    metrics.searchLeftReserve = 410;
    return metrics;
}

enum class PluginFilter {
    All = 0,
    Installed,
    NotInstalled,
    Enabled,
    Favorite,
    Count,
};

enum class PluginSort {
    Name = 0,
    Category,
    State,
    Version,
    Favorite,
    UpdatedAt,
    Count,
};

enum class PluginView {
    List = 0,
    Compact,
    Grid3,
    Count,
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

void DrawRectOutline(HDC dc, const RECT& rect, COLORREF color) {
    DrawLine(dc, rect.left, rect.top, rect.right, rect.top, color);
    DrawLine(dc, rect.right - 1, rect.top, rect.right - 1, rect.bottom, color);
    DrawLine(dc, rect.left, rect.bottom - 1, rect.right, rect.bottom - 1, color);
    DrawLine(dc, rect.left, rect.top, rect.left, rect.bottom, color);
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
    case PluginFilter::Favorite:
        return L"已收藏";
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
    case PluginFilter::Favorite:
        return plugin.favorite;
    case PluginFilter::All:
    default:
        return true;
    }
}

std::wstring SortLabel(PluginSort sort) {
    switch (sort) {
    case PluginSort::Category:
        return L"分类";
    case PluginSort::State:
        return L"状态";
    case PluginSort::Version:
        return L"版本";
    case PluginSort::Favorite:
        return L"收藏";
    case PluginSort::UpdatedAt:
        return L"更新";
    case PluginSort::Name:
    default:
        return L"名称";
    }
}

std::wstring ViewLabel(PluginView view) {
    switch (view) {
    case PluginView::Compact:
        return L"紧凑列表";
    case PluginView::Grid3:
        return L"三列卡片";
    case PluginView::List:
    default:
        return L"列表";
    }
}

int StateRank(const PluginRecord& plugin) {
    if (plugin.installed && plugin.enabled) {
        return 0;
    }
    if (plugin.installed) {
        return 1;
    }
    return 2;
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

std::wstring StoreSourceText(const std::wstring& storeUrl) {
    return storeUrl.empty() ? L"plugins/store/index.json" : storeUrl;
}

void ShowPluginSourceDialog(HWND owner, HINSTANCE instance, const Theme& theme, const PluginRecord* plugin, const std::wstring& storeUrl);

std::wstring WindowText(HWND hwnd) {
    if (!hwnd) {
        return {};
    }
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return false;
    }
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* target = GlobalLock(memory);
        if (target) {
            memcpy(target, text.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
            memory = nullptr;
        }
    }
    if (memory) {
        GlobalFree(memory);
    }
    CloseClipboard();
    return true;
}

bool IsHttpUrl(const std::wstring& value) {
    const std::wstring lower = ToLower(Trim(value));
    return lower.starts_with(L"http://") || lower.starts_with(L"https://");
}

std::wstring UrlOrigin(const std::wstring& url) {
    if (!IsHttpUrl(url)) {
        return {};
    }
    URL_COMPONENTSW parts{};
    wchar_t scheme[16]{};
    wchar_t host[260]{};
    parts.dwStructSize = sizeof(parts);
    parts.lpszScheme = scheme;
    parts.dwSchemeLength = static_cast<DWORD>(std::size(scheme));
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    if (!InternetCrackUrlW(url.c_str(), 0, 0, &parts) || parts.dwHostNameLength == 0 || parts.dwSchemeLength == 0) {
        return {};
    }
    return std::wstring(scheme, parts.dwSchemeLength) + L"://" + std::wstring(host, parts.dwHostNameLength);
}

std::wstring PluginIconCachePath(const std::filesystem::path& appDirectory, const PluginRecord& plugin) {
    const std::wstring file = Hex8(StablePathHash(plugin.id)) + L".ico";
    return (appDirectory / L"icons" / L"plugin" / file).wstring();
}

bool ReadUrlBytes(const std::wstring& url, std::vector<std::uint8_t>& bytes) {
    HINTERNET internet = InternetOpenW(L"Quattro Plugin Icon", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!internet) {
        return false;
    }
    HINTERNET request = InternetOpenUrlW(
        internet,
        url.c_str(),
        nullptr,
        0,
        INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI,
        0);
    if (!request) {
        InternetCloseHandle(internet);
        return false;
    }
    bytes.clear();
    char buffer[4096]{};
    DWORD read = 0;
    while (InternetReadFile(request, buffer, sizeof(buffer), &read) && read > 0) {
        bytes.insert(bytes.end(), reinterpret_cast<std::uint8_t*>(buffer), reinterpret_cast<std::uint8_t*>(buffer) + read);
        read = 0;
        if (bytes.size() > 256 * 1024) {
            break;
        }
    }
    InternetCloseHandle(request);
    InternetCloseHandle(internet);
    return !bytes.empty() && bytes.size() <= 256 * 1024;
}

std::vector<std::wstring> IconCandidateUrls(const PluginRecord& plugin, const std::wstring& storeUrl) {
    std::vector<std::wstring> urls;
    for (const auto& value : {plugin.homepageUrl, plugin.sourceUrl, plugin.packageUrl, storeUrl}) {
        const std::wstring origin = UrlOrigin(value);
        if (!origin.empty()) {
            urls.push_back(origin + L"/favicon.ico");
        }
    }
    std::sort(urls.begin(), urls.end());
    urls.erase(std::unique(urls.begin(), urls.end()), urls.end());
    return urls;
}

bool MatchesSearch(const PluginRecord& plugin, const std::wstring& query) {
    const std::wstring needle = ToLower(Trim(query));
    if (needle.empty()) {
        return true;
    }
    const std::wstring haystack = ToLower(
        plugin.name + L" " +
        plugin.description + L" " +
        plugin.id + L" " +
        plugin.version + L" " +
        CategoryLabel(plugin.category) + L" " +
        StateLabel(plugin) + L" " +
        plugin.author);
    return haystack.find(needle) != std::wstring::npos;
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
            WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX | WS_POPUP,
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

    static LRESULT CALLBACK ListProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData) {
        auto* dialog = reinterpret_cast<PluginManagerDialog*>(refData);
        if (dialog && message == WM_LBUTTONDOWN) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            dialog->SelectPluginAtPoint(point);
        }
        if (dialog && message == WM_LBUTTONUP) {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (dialog->HandleListClick(point)) {
                return 0;
            }
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
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
                reinterpret_cast<MEASUREITEMSTRUCT*>(lParam)->itemHeight = CurrentRowHeight();
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
        case WM_SIZE:
            LayoutControls();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = kMinDialogWidth;
            info->ptMinTrackSize.y = kMinDialogHeight;
            return 0;
        }
        case WM_PLUGIN_ICONS_READY:
            LoadCachedPluginIcons();
            if (list_) {
                InvalidateRect(list_, nullptr, TRUE);
            }
            return 0;
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wParam) == list_) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ShowPluginMenu(point);
                return 0;
            }
            return 0;
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            if (reinterpret_cast<HWND>(lParam) == source_) {
                SetTextColor(dc, ToColorRef(theme_.color(L"linkItem", L"normal", L"text")));
            } else {
                SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
            }
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
        if (id >= ID_PLUGIN_FILTER_BASE && id < ID_PLUGIN_FILTER_BASE + static_cast<int>(PluginFilter::Count)) {
            filter_ = static_cast<PluginFilter>(id - ID_PLUGIN_FILTER_BASE);
            page_ = 0;
            UpdateFilterButtons();
            ApplyFilterAndPagination();
            SelectVisiblePlugin(0);
            return 0;
        }
        if (id == ID_PLUGIN_SEARCH && HIWORD(wParam) == EN_CHANGE) {
            searchText_ = WindowText(searchEdit_);
            page_ = 0;
            ApplyFilterAndPagination();
            SelectVisiblePlugin(0);
            return 0;
        }
        if (id == ID_PLUGIN_SORT) {
            ShowSortMenu();
            return 0;
        }
        if (id == ID_PLUGIN_VIEW) {
            ShowViewMenu();
            return 0;
        }
        if (id >= ID_PLUGIN_SORT_BASE && id < ID_PLUGIN_SORT_BASE + static_cast<int>(PluginSort::Count)) {
            sort_ = static_cast<PluginSort>(id - ID_PLUGIN_SORT_BASE);
            page_ = 0;
            ApplyFilterAndPagination();
            SelectVisiblePlugin(0);
            return 0;
        }
        if (id >= ID_PLUGIN_VIEW_BASE && id < ID_PLUGIN_VIEW_BASE + static_cast<int>(PluginView::Count)) {
            view_ = static_cast<PluginView>(id - ID_PLUGIN_VIEW_BASE);
            ApplyFilterAndPagination();
            SelectVisiblePlugin(0);
            LayoutControls();
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
        if (id == ID_PLUGIN_TOGGLE_FAVORITE) {
            ToggleFavoriteSelected();
            return 0;
        }
        if (id == ID_PLUGIN_SOURCE_LINK) {
            ShowSourceInfoDialog();
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
        backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
        listBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"list", L"normal", L"bg")));

        const CatalogDialogMetrics& metrics = layout_.metrics();
        const int toolbarControlHeight = std::max(1, metrics.toolbarHeight - 2);
        const int pagerControlHeight = std::max(1, metrics.pagerHeight - 2);
        const int statusTextHeight = std::max(1, metrics.statusBarHeight - 2);
        int filterX = metrics.contentPaddingX;
        for (PluginFilter filter : {PluginFilter::All, PluginFilter::Installed, PluginFilter::NotInstalled, PluginFilter::Enabled, PluginFilter::Favorite}) {
            const int index = static_cast<int>(filter);
            const int width = filter == PluginFilter::All ? 58 : 76;
            filterButtons_[index] = ThemedControls::CreateTabButton(
                instance_,
                hwnd_,
                ID_PLUGIN_FILTER_BASE + index,
                FilterLabel(filter).c_str(),
                filterX,
                metrics.toolbarY + 1,
                width,
                toolbarControlHeight,
                font,
                filter == filter_);
            filterX += width + 4;
        }
        summary_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"", 32, 500, 480, statusTextHeight, font, SS_LEFT | SS_CENTERIMAGE);
        searchFrame_ = RECT{500, metrics.toolbarY + 1, 650, metrics.toolbarY + metrics.toolbarHeight - 1};
        searchEdit_ = ThemedControls::CreateSingleLineEdit(
            instance_,
            hwnd_,
            ID_PLUGIN_SEARCH,
            theme_,
            searchFrame_,
            L"",
            font,
            ES_AUTOHSCROLL);
        source_ = CreateWindowExW(
            0,
            L"STATIC",
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | SS_NOTIFY | SS_RIGHT | SS_ENDELLIPSIS | SS_CENTERIMAGE,
            520,
            500,
            204,
            statusTextHeight,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PLUGIN_SOURCE_LINK)),
            instance_,
            nullptr);
        if (source_) {
            SendMessageW(source_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }

        const int listY = layout_.listTop();
        listFrame_ = RECT{metrics.contentPaddingX - 1, listY - 1, metrics.contentPaddingX + 1, listY + 1};
        list_ = CreateWindowExW(
            0,
            L"LISTBOX",
            nullptr,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
            metrics.contentPaddingX,
            listY,
            1,
            1,
            hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_PLUGIN_LIST)),
            instance_,
            nullptr);
        SendMessageW(list_, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(list_, LB_SETITEMHEIGHT, 0, CurrentRowHeight());
        SetWindowSubclass(list_, PluginManagerDialog::ListProc, 1, reinterpret_cast<DWORD_PTR>(this));

        prevButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_PREV_PAGE, L"上一页", 496, 460, kButtonWidth, pagerControlHeight, font);
        pageText_ = ThemedControls::CreateStaticText(instance_, hwnd_, L"", 580, 465, kPageTextWidth, pagerControlHeight, font, SS_CENTER | SS_CENTERIMAGE);
        nextButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_NEXT_PAGE, L"下一页", 660, 460, kButtonWidth, pagerControlHeight, font);
        sortButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_SORT, L"", 24, 460, kSortButtonWidth, pagerControlHeight, font);
        viewButton_ = ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_VIEW, L"", 148, 460, kViewButtonWidth, pagerControlHeight, font);
        ThemedControls::CreateButton(instance_, hwnd_, ID_PLUGIN_REFRESH, L"刷新", 660, metrics.toolbarY + 1, 76, toolbarControlHeight, font);

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
        LayoutControls();
        LoadCachedPluginIcons();
        StartPluginIconRequests();
    }

    void Paint() {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

        layout_.DrawStatusBar(theme_, dc, rect);
        if (searchEdit_) {
            ThemedControls::DrawFieldFrame(theme_, dc, searchFrame_, searchEdit_);
        }
        ThemedControls::DrawListFrame(theme_, dc, listFrame_, list_);
        EndPaint(hwnd_, &ps);
    }

    void LayoutControls() {
        if (!hwnd_) {
            return;
        }
        RECT client{};
        GetClientRect(hwnd_, &client);
        std::vector<HWND> filters;
        for (HWND button : filterButtons_) {
            filters.push_back(button);
        }
        CatalogDialogControls controls{};
        controls.toolbarLeading = std::move(filters);
        controls.theme = &theme_;
        controls.searchEdit = searchEdit_;
        controls.searchFrame = &searchFrame_;
        controls.toolbarTrailing = GetDlgItem(hwnd_, ID_PLUGIN_REFRESH);
        controls.toolbarTrailingWidth = kRefreshWidth;
        controls.list = list_;
        controls.listFrame = &listFrame_;
        controls.listItemHeight = CurrentRowHeight();
        controls.pagerLeading = {{sortButton_, kSortButtonWidth}, {viewButton_, kViewButtonWidth}};
        controls.pagePrev = prevButton_;
        controls.pagePrevWidth = kButtonWidth;
        controls.pageText = pageText_;
        controls.pageTextWidth = kPageTextWidth;
        controls.pageNext = nextButton_;
        controls.pageNextWidth = kButtonWidth;
        controls.statusSummary = summary_;
        controls.statusSource = source_;
        layout_.Layout(hwnd_, client, controls);
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void DrawPluginRow(const DRAWITEMSTRUCT* draw) {
        if (!draw || draw->itemID == static_cast<UINT>(-1)) {
            return;
        }
        const int rowIndex = static_cast<int>(draw->itemID);
        if (rowIndex < 0 || rowIndex >= VisibleRowCount()) {
            return;
        }
        if (view_ == PluginView::Grid3) {
            DrawPluginGridRow(draw, rowIndex);
            return;
        }
        const int pluginIndex = PluginIndexForVisibleCell(rowIndex, 0);
        if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
            return;
        }
        const PluginRecord& plugin = plugins_[pluginIndex];
        const bool selected = (draw->itemState & ODS_SELECTED) != 0;
        const COLORREF listBg = ToColorRef(theme_.color(L"list", L"normal", L"bg"));
        const COLORREF selectedBg = ToColorRef(theme_.color(L"listItem", L"selected", L"bg"));
        const COLORREF rowBg = selected ? selectedBg : (rowIndex % 2 == 0 ? listBg : Blend(listBg, RGB(244, 247, 250), 0.45f));
        const COLORREF text = ToColorRef(theme_.color(L"listItem", selected ? L"selected" : L"normal", L"text"));
        const COLORREF muted = ToColorRef(theme_.color(L"label", L"muted", L"text"));
        const COLORREF line = ToColorRef(theme_.color(L"separator", L"normal", L"line"));

        RECT row = draw->rcItem;
        FillRectColor(draw->hDC, row, rowBg);
        DrawLine(draw->hDC, row.left + 10, row.bottom - 1, row.right - 10, row.bottom - 1, line);

        HFONT font = font_ ? font_ : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ oldFont = SelectObject(draw->hDC, font);
        SetBkMode(draw->hDC, TRANSPARENT);

        const bool compact = view_ == PluginView::Compact;
        RECT markRect{
            row.left + 14,
            compact ? row.top + 14 : row.top + 22,
            row.left + 32,
            compact ? row.top + 34 : row.top + 42};
        const COLORREF favoriteColor = plugin.favorite
            ? ToColorRef(theme_.color(L"global", L"warning", L"text"))
            : muted;
        SetTextColor(draw->hDC, favoriteColor);
        DrawTextW(draw->hDC, plugin.favorite ? L"★" : L"☆", -1, &markRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        RECT iconRect{
            row.left + 40,
            compact ? row.top + 5 : row.top + 13,
            row.left + 40 + kPluginIconBox,
            (compact ? row.top + 5 : row.top + 13) + kPluginIconBox};
        DrawPluginIcon(draw->hDC, plugin, iconRect);

        const int textLeft = row.left + 88;
        RECT titleRect{textLeft, compact ? row.top + 5 : row.top + 9, row.left + 370, compact ? row.top + 25 : row.top + 29};
        SetTextColor(draw->hDC, text);
        DrawTextW(draw->hDC, plugin.name.c_str(), static_cast<int>(plugin.name.size()), &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT descRect{textLeft, compact ? row.top + 25 : row.top + 34, row.left + 520, compact ? row.top + 45 : row.top + 54};
        SetTextColor(draw->hDC, muted);
        const std::wstring desc = plugin.description.empty() ? plugin.id : plugin.description;
        DrawTextW(draw->hDC, desc.c_str(), static_cast<int>(desc.size()), &descRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT metaRect{row.left + 374, compact ? row.top + 5 : row.top + 9, row.right - 18, compact ? row.top + 25 : row.top + 29};
        const std::wstring meta = CategoryLabel(plugin.category) + L" · v" + plugin.version;
        SetTextColor(draw->hDC, muted);
        DrawTextW(draw->hDC, meta.c_str(), static_cast<int>(meta.size()), &metaRect, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        DrawStatusBadge(draw->hDC, plugin, RECT{row.right - 138, compact ? row.top + 25 : row.top + 34, row.right - 18, compact ? row.top + 45 : row.top + 54}, text);

        if (oldFont) {
            SelectObject(draw->hDC, oldFont);
        }
    }

    void DrawPluginGridRow(const DRAWITEMSTRUCT* draw, int rowIndex) {
        const COLORREF listBg = ToColorRef(theme_.color(L"list", L"normal", L"bg"));
        const COLORREF selectedBg = ToColorRef(theme_.color(L"listItem", L"selected", L"bg"));
        const COLORREF text = ToColorRef(theme_.color(L"listItem", L"normal", L"text"));
        const COLORREF selectedText = ToColorRef(theme_.color(L"listItem", L"selected", L"text"));
        const COLORREF muted = ToColorRef(theme_.color(L"label", L"muted", L"text"));
        const COLORREF line = ToColorRef(theme_.color(L"separator", L"normal", L"line"));
        RECT row = draw->rcItem;
        FillRectColor(draw->hDC, row, listBg);

        HFONT font = font_ ? font_ : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        HGDIOBJ oldFont = SelectObject(draw->hDC, font);
        SetBkMode(draw->hDC, TRANSPARENT);

        for (int column = 0; column < CurrentColumns(); ++column) {
            const int pluginIndex = PluginIndexForVisibleCell(rowIndex, column);
            if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
                continue;
            }
            const PluginRecord& plugin = plugins_[pluginIndex];
            RECT card = GridCellRect(row, column);
            const bool selected = plugin.id == selectedPluginId_;
            FillRectColor(draw->hDC, card, selected ? selectedBg : (column % 2 == 0 ? Blend(listBg, RGB(248, 250, 252), 0.55f) : listBg));
            DrawRectOutline(draw->hDC, card, line);

            RECT markRect{card.left + 8, card.top + 8, card.left + 28, card.top + 28};
            SetTextColor(draw->hDC, plugin.favorite ? ToColorRef(theme_.color(L"global", L"warning", L"text")) : muted);
            DrawTextW(draw->hDC, plugin.favorite ? L"★" : L"☆", -1, &markRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            RECT iconRect{card.left + 34, card.top + 9, card.left + 34 + kPluginIconBox, card.top + 9 + kPluginIconBox};
            DrawPluginIcon(draw->hDC, plugin, iconRect);

            RECT titleRect{card.left + 80, card.top + 8, card.right - 10, card.top + 28};
            SetTextColor(draw->hDC, selected ? selectedText : text);
            DrawTextW(draw->hDC, plugin.name.c_str(), static_cast<int>(plugin.name.size()), &titleRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            RECT metaRect{card.left + 80, card.top + 30, card.right - 10, card.top + 50};
            const std::wstring meta = CategoryLabel(plugin.category) + L" · v" + plugin.version;
            SetTextColor(draw->hDC, muted);
            DrawTextW(draw->hDC, meta.c_str(), static_cast<int>(meta.size()), &metaRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            DrawStatusBadge(draw->hDC, plugin, RECT{card.left + 8, card.bottom - 28, card.right - 10, card.bottom - 8}, selected ? selectedText : text);
        }

        if (oldFont) {
            SelectObject(draw->hDC, oldFont);
        }
    }

    void DrawStatusBadge(HDC dc, const PluginRecord& plugin, RECT rect, COLORREF textColor) {
        const COLORREF dot = StatusColor(plugin);
        DrawCircle(dc, rect.left + 8, rect.top + 10, 5, dot, dot);
        rect.left += 22;
        SetTextColor(dc, textColor);
        const std::wstring state = StateLabel(plugin);
        DrawTextW(dc, state.c_str(), static_cast<int>(state.size()), &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void DrawPluginIcon(HDC dc, const PluginRecord& plugin, RECT rect) {
        auto found = pluginIcons_.find(plugin.id);
        if (found != pluginIcons_.end() && found->second) {
            DrawIconEx(
                dc,
                rect.left + (rect.right - rect.left - kPluginIconSize) / 2,
                rect.top + (rect.bottom - rect.top - kPluginIconSize) / 2,
                found->second,
                kPluginIconSize,
                kPluginIconSize,
                0,
                nullptr,
                DI_NORMAL);
            return;
        }

        const COLORREF fill = PluginIconFill(plugin);
        const COLORREF border = ToColorRef(theme_.color(L"separator", L"normal", L"line"));
        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, border);
        HGDIOBJ oldBrush = SelectObject(dc, brush);
        HGDIOBJ oldPen = SelectObject(dc, pen);
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, 10, 10);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);

        std::wstring letter = PluginIconLetter(plugin);
        SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
        DrawTextW(dc, letter.c_str(), static_cast<int>(letter.size()), &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    COLORREF PluginIconFill(const PluginRecord& plugin) const {
        if (plugin.category == L"link-pack") {
            return Blend(ToColorRef(theme_.color(L"global", L"success", L"text")), ToColorRef(theme_.color(L"dialog", L"normal", L"bg")), 0.82f);
        }
        if (plugin.category == L"theme" || plugin.category == L"theme-pack") {
            return Blend(ToColorRef(theme_.color(L"global", L"warning", L"text")), ToColorRef(theme_.color(L"dialog", L"normal", L"bg")), 0.84f);
        }
        return Blend(ToColorRef(theme_.color(L"comboBox", L"focused", L"border")), ToColorRef(theme_.color(L"dialog", L"normal", L"bg")), 0.80f);
    }

    std::wstring PluginIconLetter(const PluginRecord& plugin) const {
        if (plugin.category == L"link-pack") {
            return L"包";
        }
        if (plugin.category == L"builtin-tools") {
            return L"工";
        }
        if (plugin.category == L"theme" || plugin.category == L"theme-pack") {
            return L"主";
        }
        if (!plugin.name.empty()) {
            return plugin.name.substr(0, 1);
        }
        return L"插";
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

    void ApplyFilterAndPagination() {
        filteredPluginIndexes_.clear();
        for (int i = 0; i < static_cast<int>(plugins_.size()); ++i) {
            if (MatchesFilter(plugins_[i], filter_) && MatchesSearch(plugins_[i], searchText_)) {
                filteredPluginIndexes_.push_back(i);
            }
        }
        SortFilteredPlugins();
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

        RebuildListItems();
        UpdateSummary();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    int PageCount() const {
        if (filteredPluginIndexes_.empty()) {
            return 1;
        }
        return static_cast<int>((filteredPluginIndexes_.size() + kPageSize - 1) / kPageSize);
    }

    int CurrentColumns() const {
        return view_ == PluginView::Grid3 ? kGridColumns : 1;
    }

    int CurrentRowHeight() const {
        switch (view_) {
        case PluginView::Compact:
            return kCompactRowHeight;
        case PluginView::Grid3:
            return kGridRowHeight;
        case PluginView::List:
        default:
            return kListRowHeight;
        }
    }

    int VisibleRowCount() const {
        const int columns = CurrentColumns();
        if (columns <= 1) {
            return static_cast<int>(pagePluginIndexes_.size());
        }
        return static_cast<int>((pagePluginIndexes_.size() + columns - 1) / columns);
    }

    int PluginIndexForVisibleCell(int rowIndex, int columnIndex) const {
        const int columns = CurrentColumns();
        const int pageIndex = rowIndex * columns + columnIndex;
        if (pageIndex < 0 || pageIndex >= static_cast<int>(pagePluginIndexes_.size())) {
            return -1;
        }
        return pagePluginIndexes_[pageIndex];
    }

    int PageOffsetForPluginId(const std::wstring& pluginId) const {
        for (int i = 0; i < static_cast<int>(pagePluginIndexes_.size()); ++i) {
            if (plugins_[pagePluginIndexes_[i]].id == pluginId) {
                return i / CurrentColumns();
            }
        }
        return -1;
    }

    int HitTestPluginIndex(POINT point, int* rowOut = nullptr, int* columnOut = nullptr) const {
        if (!list_) {
            return -1;
        }
        const LRESULT hit = SendMessageW(list_, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
        if (HIWORD(hit) != 0) {
            return -1;
        }
        const int rowIndex = LOWORD(hit);
        if (rowIndex < 0 || rowIndex >= VisibleRowCount()) {
            return -1;
        }
        int column = 0;
        if (view_ == PluginView::Grid3) {
            RECT itemRect{};
            if (SendMessageW(list_, LB_GETITEMRECT, static_cast<WPARAM>(rowIndex), reinterpret_cast<LPARAM>(&itemRect)) == LB_ERR) {
                return -1;
            }
            column = -1;
            for (int i = 0; i < CurrentColumns(); ++i) {
                RECT cell = GridCellRect(itemRect, i);
                if (PtInRect(&cell, point)) {
                    column = i;
                    break;
                }
            }
            if (column < 0) {
                return -1;
            }
        }
        if (rowOut) {
            *rowOut = rowIndex;
        }
        if (columnOut) {
            *columnOut = column;
        }
        return PluginIndexForVisibleCell(rowIndex, column);
    }

    RECT GridCellRect(const RECT& row, int column) const {
        const int gap = 6;
        const int columns = std::max(1, CurrentColumns());
        const int rowWidth = static_cast<int>(row.right - row.left);
        const int available = std::max(1, rowWidth - gap * (columns + 1));
        const int width = std::max(1, available / columns);
        RECT rect{};
        rect.left = row.left + gap + column * (width + gap);
        rect.right = column == columns - 1 ? row.right - gap : rect.left + width;
        rect.top = row.top + 6;
        rect.bottom = row.bottom - 6;
        return rect;
    }

    void RebuildListItems() {
        SendMessageW(list_, LB_RESETCONTENT, 0, 0);
        SendMessageW(list_, LB_SETITEMHEIGHT, 0, CurrentRowHeight());
        const int rows = VisibleRowCount();
        for (int row = 0; row < rows; ++row) {
            std::wstring text;
            const int pluginIndex = PluginIndexForVisibleCell(row, 0);
            if (pluginIndex >= 0 && pluginIndex < static_cast<int>(plugins_.size())) {
                const PluginRecord& plugin = plugins_[pluginIndex];
                text = plugin.name + L"\t" + plugin.version + L"\t" + StateLabel(plugin) + L"\t" + CategoryLabel(plugin.category);
            }
            const LRESULT item = SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
            if (item >= 0) {
                SendMessageW(list_, LB_SETITEMDATA, static_cast<WPARAM>(item), static_cast<LPARAM>(pluginIndex));
            }
        }
    }

    void UpdateSummary() {
        int enabled = 0;
        int disabled = 0;
        int notInstalled = 0;
        int favorite = 0;
        for (const auto& plugin : plugins_) {
            if (plugin.favorite) {
                ++favorite;
            }
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
            L" 个 | 收藏 " + std::to_wstring(favorite) +
            L" | 启用 " + std::to_wstring(enabled) +
            L" / 禁用 " + std::to_wstring(disabled) +
            L" / 未安装 " + std::to_wstring(notInstalled);
        SetWindowTextW(summary_, summary.c_str());

        const std::wstring sourceText = L"来源：" + StoreSourceText(storeUrl_);
        SetWindowTextW(source_, sourceText.c_str());

        const std::wstring pageText = L"第 " + std::to_wstring(page_ + 1) + L" / " + std::to_wstring(PageCount()) + L" 页";
        SetWindowTextW(pageText_, pageText.c_str());
        const std::wstring sortText = SortLabel(sort_) + L" ▾";
        SetWindowTextW(sortButton_, sortText.c_str());
        const std::wstring viewText = ViewLabel(view_) + L" ▾";
        SetWindowTextW(viewButton_, viewText.c_str());
        EnableWindow(prevButton_, page_ > 0);
        EnableWindow(nextButton_, page_ + 1 < PageCount());
    }

    void SelectVisiblePlugin(int visibleIndex) {
        if (pagePluginIndexes_.empty()) {
            selectedPluginId_.clear();
            return;
        }
        visibleIndex = std::max(0, std::min(visibleIndex, VisibleRowCount() - 1));
        SendMessageW(list_, LB_SETCURSEL, visibleIndex, 0);
        const int pluginIndex = PluginIndexForVisibleCell(visibleIndex, 0);
        if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
            selectedPluginId_.clear();
            return;
        }
        const PluginRecord& plugin = plugins_[pluginIndex];
        selectedPluginId_ = plugin.id;
        InvalidateRect(list_, nullptr, TRUE);
    }

    int SelectedVisibleIndex() const {
        const int visibleIndex = static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0));
        if (visibleIndex < 0 || visibleIndex >= VisibleRowCount()) {
            return -1;
        }
        return visibleIndex;
    }

    int SelectedPluginIndex() const {
        if (!selectedPluginId_.empty()) {
            for (int pluginIndex : pagePluginIndexes_) {
                if (plugins_[pluginIndex].id == selectedPluginId_) {
                    return pluginIndex;
                }
            }
        }
        const int visibleIndex = SelectedVisibleIndex();
        return visibleIndex < 0 ? -1 : PluginIndexForVisibleCell(visibleIndex, 0);
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
                return i / CurrentColumns();
            }
        }
        return -1;
    }

    void UpdateFilterButtons() {
        for (int i = 0; i < static_cast<int>(PluginFilter::Count); ++i) {
            if (filterButtons_[i]) {
                SendMessageW(filterButtons_[i], BM_SETCHECK, i == static_cast<int>(filter_) ? BST_CHECKED : BST_UNCHECKED, 0);
            }
        }
    }

    void SortFilteredPlugins() {
        const auto nameKey = [](const PluginRecord& plugin) {
            return ToLower(plugin.name.empty() ? plugin.id : plugin.name);
        };
        const auto dateKey = [](const PluginRecord& plugin) {
            return FirstNonEmpty({plugin.updatedAt, plugin.addedAt, plugin.createdAt});
        };
        const auto textKey = [&](const PluginRecord& plugin) {
            switch (sort_) {
            case PluginSort::Category:
                return ToLower(CategoryLabel(plugin.category));
            case PluginSort::Version:
                return ToLower(plugin.version);
            case PluginSort::Name:
            default:
                return nameKey(plugin);
            }
        };
        std::stable_sort(filteredPluginIndexes_.begin(), filteredPluginIndexes_.end(), [&](int leftIndex, int rightIndex) {
            const PluginRecord& left = plugins_[leftIndex];
            const PluginRecord& right = plugins_[rightIndex];
            if (sort_ == PluginSort::State) {
                const int leftState = StateRank(left);
                const int rightState = StateRank(right);
                if (leftState != rightState) {
                    return leftState < rightState;
                }
            } else if (sort_ == PluginSort::Favorite) {
                if (left.favorite != right.favorite) {
                    return left.favorite && !right.favorite;
                }
            } else if (sort_ == PluginSort::UpdatedAt) {
                const std::wstring leftDate = dateKey(left);
                const std::wstring rightDate = dateKey(right);
                if (leftDate != rightDate) {
                    return leftDate > rightDate;
                }
            } else {
                const std::wstring leftText = textKey(left);
                const std::wstring rightText = textKey(right);
                if (leftText != rightText) {
                    return leftText < rightText;
                }
            }
            const std::wstring leftName = nameKey(left);
            const std::wstring rightName = nameKey(right);
            if (leftName != rightName) {
                return leftName < rightName;
            }
            return ToLower(left.id) < ToLower(right.id);
        });
    }

    void ShowSortMenu() {
        if (!sortButton_) {
            return;
        }
        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }
        const std::array<std::wstring, static_cast<std::size_t>(PluginSort::Count)> labels{
            L"按名称",
            L"按分类",
            L"按状态",
            L"按版本",
            L"收藏优先",
            L"最近更新",
        };
        for (int i = 0; i < static_cast<int>(PluginSort::Count); ++i) {
            const UINT flags = MF_STRING | (i == static_cast<int>(sort_) ? MF_CHECKED : 0);
            AppendMenuW(menu, flags, ID_PLUGIN_SORT_BASE + i, labels[static_cast<std::size_t>(i)].c_str());
        }
        RECT rect{};
        GetWindowRect(sortButton_, &rect);
        ActivateWindow(hwnd_);
        TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, rect.left, rect.bottom, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void ShowViewMenu() {
        if (!viewButton_) {
            return;
        }
        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }
        const std::array<std::wstring, static_cast<std::size_t>(PluginView::Count)> labels{
            L"列表",
            L"紧凑列表",
            L"三列卡片",
        };
        for (int i = 0; i < static_cast<int>(PluginView::Count); ++i) {
            const UINT flags = MF_STRING | (i == static_cast<int>(view_) ? MF_CHECKED : 0);
            AppendMenuW(menu, flags, ID_PLUGIN_VIEW_BASE + i, labels[static_cast<std::size_t>(i)].c_str());
        }
        RECT rect{};
        GetWindowRect(viewButton_, &rect);
        ActivateWindow(hwnd_);
        TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON, rect.left, rect.bottom, 0, hwnd_, nullptr);
        DestroyMenu(menu);
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

    void LoadCachedPluginIcons() {
        for (const auto& plugin : plugins_) {
            if (pluginIcons_.find(plugin.id) != pluginIcons_.end()) {
                continue;
            }
            const std::wstring path = PluginIconCachePath(appDirectory_, plugin);
            if (!FileExists(path)) {
                continue;
            }
            HICON icon = static_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON, kPluginIconSize, kPluginIconSize, LR_LOADFROMFILE));
            if (icon) {
                pluginIcons_[plugin.id] = icon;
            }
        }
    }

    void StartPluginIconRequests() {
        if (iconRequestStarted_) {
            return;
        }
        iconRequestStarted_ = true;
        const HWND target = hwnd_;
        const std::filesystem::path appDirectory = appDirectory_;
        const std::wstring storeUrl = storeUrl_;
        const std::vector<PluginRecord> plugins = plugins_;
        std::thread([target, appDirectory, storeUrl, plugins]() {
            bool changed = false;
            for (const auto& plugin : plugins) {
                const std::wstring cachePath = PluginIconCachePath(appDirectory, plugin);
                if (FileExists(cachePath)) {
                    continue;
                }
                for (const auto& url : IconCandidateUrls(plugin, storeUrl)) {
                    std::vector<std::uint8_t> bytes;
                    if (!ReadUrlBytes(url, bytes)) {
                        continue;
                    }
                    std::error_code ec;
                    std::filesystem::create_directories(std::filesystem::path(cachePath).parent_path(), ec);
                    std::ofstream file(cachePath, std::ios::binary);
                    if (!file) {
                        continue;
                    }
                    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
                    if (file.good()) {
                        changed = true;
                        break;
                    }
                }
            }
            if (changed && IsWindow(target)) {
                PostMessageW(target, WM_PLUGIN_ICONS_READY, 0, 0);
            }
        }).detach();
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

    void ShowPluginMenu(POINT screenPoint) {
        if (!list_ || pagePluginIndexes_.empty()) {
            return;
        }

        if (screenPoint.x == -1 && screenPoint.y == -1) {
            int visibleIndex = SelectedVisibleIndex();
            if (visibleIndex < 0) {
                visibleIndex = 0;
                SelectVisiblePlugin(visibleIndex);
            }
            RECT itemRect{};
            SendMessageW(list_, LB_GETITEMRECT, static_cast<WPARAM>(visibleIndex), reinterpret_cast<LPARAM>(&itemRect));
            screenPoint = POINT{itemRect.left + 24, itemRect.top + 18};
            ClientToScreen(list_, &screenPoint);
        } else {
            POINT clientPoint = screenPoint;
            ScreenToClient(list_, &clientPoint);
            RECT listRect{};
            GetClientRect(list_, &listRect);
            if (!PtInRect(&listRect, clientPoint)) {
                return;
            }
            int row = -1;
            const int hitPluginIndex = HitTestPluginIndex(clientPoint, &row);
            if (hitPluginIndex < 0 || hitPluginIndex >= static_cast<int>(plugins_.size())) {
                return;
            }
            SendMessageW(list_, LB_SETCURSEL, row, 0);
            selectedPluginId_ = plugins_[hitPluginIndex].id;
            InvalidateRect(list_, nullptr, TRUE);
        }

        const int pluginIndex = SelectedPluginIndex();
        if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
            return;
        }
        const PluginRecord& plugin = plugins_[pluginIndex];
        HMENU menu = CreatePopupMenu();
        if (!menu) {
            return;
        }

        AppendMenuW(menu, MF_STRING, ID_PLUGIN_TOGGLE_FAVORITE, plugin.favorite ? L"取消收藏" : L"收藏");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (plugin.installed && !plugin.enabled ? 0 : MF_GRAYED), ID_PLUGIN_ENABLE, L"启用");
        AppendMenuW(menu, MF_STRING | (plugin.installed && plugin.enabled ? 0 : MF_GRAYED), ID_PLUGIN_DISABLE, L"禁用");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | (!plugin.installed && !plugin.builtin ? 0 : MF_GRAYED), ID_PLUGIN_INSTALL, L"安装");
        AppendMenuW(menu, MF_STRING | (plugin.installed && plugin.deletable && !plugin.builtin ? 0 : MF_GRAYED), ID_PLUGIN_REMOVE, L"删除");

        ActivateWindow(hwnd_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void ToggleFavoriteSelected() {
        const int pluginIndex = SelectedPluginIndex();
        if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
            return;
        }
        const std::wstring pluginId = plugins_[pluginIndex].id;
        const bool favorite = !plugins_[pluginIndex].favorite;
        if (!registry_.SetFavorite(pluginId, favorite)) {
            MessageBoxW(hwnd_, registry_.lastError().c_str(), L"插件商店", MB_OK | MB_ICONWARNING);
            return;
        }
        changed_ = true;
        ReloadPluginsKeepingSelection(pluginId);
    }

    bool HandleListClick(POINT point) {
        if (!list_) {
            return false;
        }
        int row = -1;
        int column = -1;
        const int pluginIndex = HitTestPluginIndex(point, &row, &column);
        if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
            return false;
        }
        RECT itemRect{};
        if (SendMessageW(list_, LB_GETITEMRECT, static_cast<WPARAM>(row), reinterpret_cast<LPARAM>(&itemRect)) == LB_ERR) {
            return false;
        }
        RECT favoriteRect{};
        if (view_ == PluginView::Grid3) {
            RECT cell = GridCellRect(itemRect, column);
            favoriteRect = RECT{cell.left + 4, cell.top + 4, cell.left + 32, cell.top + 32};
        } else {
            favoriteRect = RECT{itemRect.left + 8, itemRect.top + 8, itemRect.left + 36, itemRect.bottom - 8};
        }
        if (!PtInRect(&favoriteRect, point)) {
            return false;
        }
        SendMessageW(list_, LB_SETCURSEL, row, 0);
        selectedPluginId_ = plugins_[pluginIndex].id;
        ToggleFavoriteSelected();
        return true;
    }

    bool SelectPluginAtPoint(POINT point) {
        int row = -1;
        const int pluginIndex = HitTestPluginIndex(point, &row);
        if (pluginIndex < 0 || pluginIndex >= static_cast<int>(plugins_.size())) {
            return false;
        }
        SendMessageW(list_, LB_SETCURSEL, row, 0);
        selectedPluginId_ = plugins_[pluginIndex].id;
        InvalidateRect(list_, nullptr, TRUE);
        return true;
    }

    void ShowSourceInfoDialog() {
        const int pluginIndex = SelectedPluginIndex();
        const PluginRecord* plugin = nullptr;
        if (pluginIndex >= 0 && pluginIndex < static_cast<int>(plugins_.size())) {
            plugin = &plugins_[pluginIndex];
        }
        ShowPluginSourceDialog(hwnd_, instance_, theme_, plugin, storeUrl_);
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
        text += L"收藏：" + std::wstring(plugin.favorite ? L"是" : L"否") + L"\r\n";
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
        if (list_) {
            RemoveWindowSubclass(list_, PluginManagerDialog::ListProc, 1);
        }
        if (tooltip_) {
            DestroyWindow(tooltip_);
            tooltip_ = nullptr;
        }
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
        for (auto& [_, icon] : pluginIcons_) {
            if (icon) {
                DestroyIcon(icon);
            }
        }
        pluginIcons_.clear();
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
        return HitTestPluginIndex(point);
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    PluginRegistry& registry_;
    StorageService& storage_;
    std::filesystem::path appDirectory_;
    std::wstring storeUrl_;
    CatalogDialogLayout layout_{PluginCatalogMetrics()};
    HWND hwnd_ = nullptr;
    HWND summary_ = nullptr;
    HWND source_ = nullptr;
    HWND searchEdit_ = nullptr;
    std::array<HWND, static_cast<std::size_t>(PluginFilter::Count)> filterButtons_{};
    HWND list_ = nullptr;
    HWND sortButton_ = nullptr;
    HWND viewButton_ = nullptr;
    HWND prevButton_ = nullptr;
    HWND nextButton_ = nullptr;
    HWND pageText_ = nullptr;
    HWND tooltip_ = nullptr;
    RECT listFrame_{};
    RECT searchFrame_{};
    HFONT font_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH listBrush_ = nullptr;
    std::vector<PluginRecord> plugins_;
    std::vector<int> filteredPluginIndexes_;
    std::vector<int> pagePluginIndexes_;
    std::vector<StorePluginDefinition> storePlugins_;
    std::map<std::wstring, HICON> pluginIcons_;
    PluginFilter filter_ = PluginFilter::All;
    PluginSort sort_ = PluginSort::Name;
    PluginView view_ = PluginView::List;
    int page_ = 0;
    std::wstring selectedPluginId_;
    std::wstring tooltipText_;
    std::wstring searchText_;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool iconRequestStarted_ = false;
    bool changed_ = false;
    bool done_ = false;
};

class PluginSourceDialog {
public:
    PluginSourceDialog(
        HWND owner,
        HINSTANCE instance,
        const Theme& theme,
        const PluginRecord* plugin,
        std::wstring storeUrl)
        : owner_(owner),
          instance_(instance),
          theme_(theme),
          plugin_(plugin),
          storeUrl_(std::move(storeUrl)) {}

    void Run() {
        const std::wstring className = L"QuattroPluginSourceDialog_" +
            std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = PluginSourceDialog::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = className.c_str();
        RegisterClassExW(&wc);

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            className.c_str(),
            L"来源信息",
            WS_CAPTION | WS_SYSMENU | WS_POPUP,
            ownerRect.left + 92,
            ownerRect.top + 92,
            kSourceDialogWidth,
            kSourceDialogHeight,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            return;
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
    }

private:
    static constexpr int ID_OPEN_SOURCE = ID_PLUGIN_OPEN_SOURCE;
    static constexpr int ID_COPY_SOURCE = ID_PLUGIN_COPY_SOURCE;

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        PluginSourceDialog* dialog = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            dialog = static_cast<PluginSourceDialog*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(dialog));
            dialog->hwnd_ = hwnd;
        } else {
            dialog = reinterpret_cast<PluginSourceDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return dialog ? dialog->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            CreateControls();
            return 0;
        case WM_COMMAND:
            return HandleCommand(LOWORD(wParam));
        case WM_DRAWITEM:
            if (ThemedControls::Draw(theme_, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
                return TRUE;
            }
            return 0;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
            return reinterpret_cast<LRESULT>(backgroundBrush_ ? backgroundBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_PAINT:
            Paint();
            return 0;
        case WM_ERASEBKGND:
            return 1;
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

    LRESULT HandleCommand(int id) {
        if (id == ID_OPEN_SOURCE) {
            const std::wstring url = OpenableUrl();
            if (!url.empty()) {
                ShellExecuteW(hwnd_, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }
        if (id == ID_COPY_SOURCE) {
            CopyTextToClipboard(hwnd_, InfoText());
            return 0;
        }
        if (id == IDCANCEL || id == IDOK) {
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        }
        return 0;
    }

    void CreateControls() {
        font_ = ThemedControls::CreateDialogFont();
        HFONT font = font_ ? font_ : reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
        titleFont_ = CreateFontW(
            -16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        ThemedControls::CreateStaticText(instance_, hwnd_, L"插件来源", 24, 18, 180, 24, titleFont_ ? titleFont_ : font);
        details_ = ThemedControls::CreateStaticText(instance_, hwnd_, InfoText().c_str(), 24, 52, 456, 124, font, SS_LEFT);
        const int buttonHeight = ThemedControls::ButtonHeight(theme_);
        ThemedControls::CreateButton(instance_, hwnd_, ID_OPEN_SOURCE, L"打开来源", 238, 184, 92, buttonHeight, font);
        ThemedControls::CreateButton(instance_, hwnd_, ID_COPY_SOURCE, L"复制信息", 342, 184, 92, buttonHeight, font);
        ThemedControls::CreateButton(instance_, hwnd_, IDCANCEL, L"关闭", 446, 184, 58, buttonHeight, font, true);
    }

    void Paint() {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd_, &ps);
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        EndPaint(hwnd_, &ps);
    }

    std::wstring OpenableUrl() const {
        if (plugin_) {
            const std::wstring url = FirstNonEmpty({plugin_->sourceUrl, plugin_->homepageUrl, plugin_->packageUrl});
            if (!url.empty()) {
                return url;
            }
        }
        return StoreSourceText(storeUrl_);
    }

    std::wstring InfoText() const {
        std::wstring text = L"商店索引：";
        text += StoreSourceText(storeUrl_);
        text += L"\r\n";
        if (plugin_) {
            text += L"当前插件：" + plugin_->name + L"\r\n";
            text += L"仓库/来源：" + FirstNonEmpty({plugin_->sourceUrl, plugin_->homepageUrl, L"-"}) + L"\r\n";
            text += L"主页：" + (plugin_->homepageUrl.empty() ? L"-" : plugin_->homepageUrl) + L"\r\n";
            text += L"包地址：" + (plugin_->packageUrl.empty() ? L"-" : plugin_->packageUrl) + L"\r\n";
            text += L"作者：" + (plugin_->author.empty() ? L"-" : plugin_->author);
        } else {
            text += L"当前插件：未选择";
        }
        return text;
    }

    void Cleanup() {
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
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    const Theme& theme_;
    const PluginRecord* plugin_ = nullptr;
    std::wstring storeUrl_;
    HWND hwnd_ = nullptr;
    HWND details_ = nullptr;
    HFONT font_ = nullptr;
    HFONT titleFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool done_ = false;
};

void ShowPluginSourceDialog(HWND owner, HINSTANCE instance, const Theme& theme, const PluginRecord* plugin, const std::wstring& storeUrl) {
    PluginSourceDialog dialog(owner, instance, theme, plugin, storeUrl);
    dialog.Run();
}
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
