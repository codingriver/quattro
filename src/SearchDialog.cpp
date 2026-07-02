#include "SearchDialog.h"

#include "../resources/resource.h"

#include "ShellItemService.h"
#include "ThemedControls.h"
#include "Utilities.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {
constexpr UINT ID_SEARCH_RUN = 50001;
constexpr UINT ID_SEARCH_OPEN_LOCATION = 50002;
constexpr UINT ID_SEARCH_COPY_PATH = 50003;
constexpr UINT_PTR ID_SEARCH_DEBOUNCE_TIMER = 1;
constexpr int kSearchDefaultWidth = 560;
constexpr int kSearchDefaultHeight = 430;
constexpr int kSearchMinWidth = 420;
constexpr int kSearchMinHeight = 300;

std::wstring GetText(HWND hwnd) {
    const int length = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
    if (length > 0) {
        GetWindowTextW(hwnd, text.data(), length + 1);
    }
    text.resize(static_cast<std::size_t>(length));
    return text;
}

void SetFont(HWND hwnd, HFONT font) {
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

float ClampFloat(float value, float minValue, float maxValue) {
    return std::max(minValue, std::min(maxValue, value));
}

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        return static_cast<BYTE>(ClampFloat(value, 0.0f, 1.0f) * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
}

bool ContainsText(const std::wstring& haystack, const std::wstring& needle) {
    return ToLower(haystack).find(ToLower(needle)) != std::wstring::npos;
}

int MatchScore(const Link& link, const std::wstring& query) {
    const std::wstring needle = ToLower(Trim(query));
    if (needle.empty()) {
        return 1;
    }
    const std::wstring name = ToLower(link.name);
    const std::wstring path = ToLower(link.path);
    const std::wstring parameter = ToLower(link.parameter);
    const std::wstring remark = ToLower(link.remark);
    if (name.rfind(needle, 0) == 0) {
        return 400;
    }
    if (name.find(needle) != std::wstring::npos) {
        return 300;
    }
    if (path.find(needle) != std::wstring::npos) {
        return 200;
    }
    if (remark.find(needle) != std::wstring::npos) {
        return 100;
    }
    if (parameter.find(needle) != std::wstring::npos) {
        return 80;
    }
    return 0;
}

POINT ClampDialogPosition(int x, int y, int width, int height) {
    RECT proposed{x, y, x + width, y + height};
    HMONITOR monitor = MonitorFromRect(&proposed, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return POINT{x, y};
    }
    const RECT work = info.rcWork;
    const int clampedX = std::max(static_cast<int>(work.left), std::min(x, static_cast<int>(work.right) - width));
    const int clampedY = std::max(static_cast<int>(work.top), std::min(y, static_cast<int>(work.bottom) - height));
    return POINT{clampedX, clampedY};
}

void CopyText(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) {
        return;
    }
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory) {
        void* data = GlobalLock(memory);
        if (data) {
            memcpy(data, text.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
            memory = nullptr;
        }
    }
    if (memory) {
        GlobalFree(memory);
    }
    CloseClipboard();
}

class SearchWindow {
public:
    SearchWindow(HWND owner, HINSTANCE instance, const Theme& theme, const AppModel& model, AppConfig& config, std::wstring initialQuery)
        : owner_(owner), instance_(instance), theme_(theme), model_(model), config_(config), initialQuery_(std::move(initialQuery)) {}

    int Run() {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SearchWindow::Proc;
        wc.hInstance = instance_;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hIconSm = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_QUATTRO_APP_ICON));
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"QuattroSearchDialog";
        RegisterClassExW(&wc);

        RECT ownerRect{};
        GetWindowRect(owner_, &ownerRect);
        int x = ownerRect.left + 40;
        int y = ownerRect.top + 60;
        if (config_.searchX >= 0 && config_.searchY >= 0) {
            x = config_.searchX;
            y = config_.searchY;
        }
        const POINT position = ClampDialogPosition(x, y, kSearchDefaultWidth, kSearchDefaultHeight);
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            wc.lpszClassName,
            L"搜索",
            WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN | WS_THICKFRAME,
            position.x,
            position.y,
            kSearchDefaultWidth,
            kSearchDefaultHeight,
            owner_,
            nullptr,
            instance_,
            this);
        if (!hwnd_) {
            return 0;
        }
        ownerWasEnabled_ = ShowModalWindow(owner_, hwnd_);
        UpdateWindow(hwnd_);
        RedrawWindow(hwnd_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (!IsDialogMessageW(hwnd_, &message)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }
        RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
        return selectedId_;
    }

private:
    static LRESULT CALLBACK EditProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR refData) {
        auto* self = reinterpret_cast<SearchWindow*>(refData);
        if (self && message == WM_KEYDOWN &&
            (wParam == VK_UP || wParam == VK_DOWN || wParam == VK_PRIOR || wParam == VK_NEXT)) {
            switch (wParam) {
            case VK_UP:
                self->MoveListSelection(-1);
                break;
            case VK_DOWN:
                self->MoveListSelection(1);
                break;
            case VK_PRIOR:
                self->MoveListSelection(-8);
                break;
            case VK_NEXT:
                self->MoveListSelection(8);
                break;
            }
            return 0;
        }
        return DefSubclassProc(hwnd, message, wParam, lParam);
    }

    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        SearchWindow* window = nullptr;
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            window = static_cast<SearchWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
            window->hwnd_ = hwnd;
        } else {
            window = reinterpret_cast<SearchWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }
        return window ? window->Handle(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
    }

    LRESULT Handle(UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
        case WM_CREATE: {
            font_ = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            editFont_ = ThemedControls::CreateEditFont(theme_);
            backgroundBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"dialog", L"normal", L"bg")));
            fieldBrush_ = CreateSolidBrush(ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            const int fieldHeight = ThemedControls::EditFrameHeight(theme_);
            editFrame_ = RECT{18, 16, 522, 16 + fieldHeight};
            listFrame_ = RECT{18, 62, 522, 350};
            edit_ = ThemedControls::CreateSingleLineEdit(instance_, hwnd_, 100, theme_, editFrame_, L"", editFont_ ? editFont_ : font_);
            SetWindowSubclass(edit_, EditProc, 1, reinterpret_cast<DWORD_PTR>(this));
            list_ = CreateWindowExW(0, WC_LISTVIEWW, L"",
                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                                    20, 64, 500, 284, hwnd_, reinterpret_cast<HMENU>(101), instance_, nullptr);
            SetFont(list_, font_);
            ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            ListView_SetBkColor(list_, ToColorRef(theme_.color(L"list", L"normal", L"bg")));
            ListView_SetTextBkColor(list_, ToColorRef(theme_.color(L"list", L"normal", L"bg")));
            ListView_SetTextColor(list_, ToColorRef(theme_.color(L"list", L"normal", L"text")));
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = const_cast<LPWSTR>(L"名称");
            col.cx = 160;
            ListView_InsertColumn(list_, 0, &col);
            col.pszText = const_cast<LPWSTR>(L"路径");
            col.cx = 320;
            ListView_InsertColumn(list_, 1, &col);
            LayoutControls();
            Refresh();
            SetFocus(config_.focusSearch ? edit_ : list_);
            if (!initialQuery_.empty()) {
                SetWindowTextW(edit_, initialQuery_.c_str());
                SetFocus(edit_);
                const int len = GetWindowTextLengthW(edit_);
                SendMessageW(edit_, EM_SETSEL, static_cast<WPARAM>(len), static_cast<LPARAM>(len));
                Refresh();
            }
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            PaintBackground(dc);
            ThemedControls::DrawFieldFrame(theme_, dc, editFrame_, edit_);
            ThemedControls::DrawListFrame(theme_, dc, listFrame_, list_, true);
            DrawStatus(dc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintBackground(reinterpret_cast<HDC>(wParam));
            ThemedControls::DrawFieldFrame(theme_, reinterpret_cast<HDC>(wParam), editFrame_, edit_);
            ThemedControls::DrawListFrame(theme_, reinterpret_cast<HDC>(wParam), listFrame_, list_, true);
            DrawStatus(reinterpret_cast<HDC>(wParam));
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"text")));
            SetBkColor(dc, ToColorRef(theme_.color(L"edit", L"normal", L"bg")));
            return reinterpret_cast<LRESULT>(fieldBrush_ ? fieldBrush_ : GetStockObject(WHITE_BRUSH));
        }
        case WM_COMMAND:
            if (LOWORD(wParam) == 100 && HIWORD(wParam) == EN_CHANGE) {
                SetTimer(hwnd_, ID_SEARCH_DEBOUNCE_TIMER, 100, nullptr);
                return 0;
            }
            if (LOWORD(wParam) == 100 && (HIWORD(wParam) == EN_SETFOCUS || HIWORD(wParam) == EN_KILLFOCUS)) {
                InvalidateRect(hwnd_, &editFrame_, TRUE);
                return 0;
            }
            if (LOWORD(wParam) == ID_SEARCH_RUN) {
                AcceptSelected();
                return 0;
            }
            if (LOWORD(wParam) == ID_SEARCH_OPEN_LOCATION) {
                OpenSelectedLocation();
                return 0;
            }
            if (LOWORD(wParam) == ID_SEARCH_COPY_PATH) {
                CopySelectedPath();
                return 0;
            }
            return 0;
        case WM_TIMER:
            if (wParam == ID_SEARCH_DEBOUNCE_TIMER) {
                KillTimer(hwnd_, ID_SEARCH_DEBOUNCE_TIMER);
                Refresh();
                return 0;
            }
            return 0;
        case WM_SIZE:
            LayoutControls();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = kSearchMinWidth;
            info->ptMinTrackSize.y = kSearchMinHeight;
            return 0;
        }
        case WM_NOTIFY: {
            auto* hdr = reinterpret_cast<NMHDR*>(lParam);
            if (hdr->hwndFrom == list_ && hdr->code == NM_DBLCLK) {
                AcceptSelected();
                return 0;
            }
            return 0;
        }
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wParam) == list_) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (point.x == -1 && point.y == -1) {
                    RECT rect{};
                    GetWindowRect(list_, &rect);
                    point = POINT{rect.left + 24, rect.top + 24};
                }
                ShowResultMenu(point);
                return 0;
            }
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_RETURN) {
                AcceptSelected();
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                done_ = true;
                DestroyWindow(hwnd_);
                return 0;
            }
            return 0;
        case WM_CLOSE:
            SavePosition();
            done_ = true;
            DestroyWindow(hwnd_);
            return 0;
        case WM_DESTROY:
            SavePosition();
            done_ = true;
            KillTimer(hwnd_, ID_SEARCH_DEBOUNCE_TIMER);
            if (edit_) {
                RemoveWindowSubclass(edit_, EditProc, 1);
            }
            RestoreModalOwner(owner_, ownerWasEnabled_, ownerRestored_);
            if (editFont_) {
                DeleteObject(editFont_);
                editFont_ = nullptr;
            }
            if (backgroundBrush_) {
                DeleteObject(backgroundBrush_);
                backgroundBrush_ = nullptr;
            }
            if (fieldBrush_) {
                DeleteObject(fieldBrush_);
                fieldBrush_ = nullptr;
            }
            return 0;
        default:
            return DefWindowProcW(hwnd_, message, wParam, lParam);
        }
    }

    void PaintBackground(HDC dc) {
        RECT rect{};
        GetClientRect(hwnd_, &rect);
        FillRect(dc, &rect, backgroundBrush_ ? backgroundBrush_ : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    }

    void DrawStatus(HDC dc) {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ToColorRef(theme_.color(L"label", L"normal", L"text")));
        RECT statusRect = statusFrame_;
        const std::wstring text = resultCount_ == 0
            ? L"无匹配结果"
            : (std::to_wstring(resultCount_) + L" 条结果");
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &statusRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    void LayoutControls() {
        if (!hwnd_) {
            return;
        }
        RECT client{};
        if (!GetClientRect(hwnd_, &client)) {
            return;
        }
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        const int margin = 18;
        const int fieldHeight = ThemedControls::EditFrameHeight(theme_);
        editFrame_ = RECT{margin, 16, std::max(margin + 80, width - margin), 16 + fieldHeight};
        const int listTop = 62;
        const int statusTop = std::max(listTop + 80, height - 42);
        listFrame_ = RECT{margin, listTop, std::max(margin + 80, width - margin), statusTop - 8};
        statusFrame_ = RECT{margin + 2, statusTop, std::max(margin + 80, width - margin), statusTop + 24};
        if (edit_) {
            const int paddingX = ThemedControls::EditPaddingX(theme_);
            const int paddingY = std::max(2, (ThemedControls::EditFrameHeight(theme_) - 20) / 2);
            SetWindowPos(edit_, nullptr, editFrame_.left + paddingX, editFrame_.top + paddingY,
                         std::max(20, static_cast<int>(editFrame_.right - editFrame_.left) - paddingX * 2),
                         std::max(20, static_cast<int>(editFrame_.bottom - editFrame_.top) - paddingY * 2),
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (list_) {
            SetWindowPos(list_, nullptr, listFrame_.left + 2, listFrame_.top + 2,
                         std::max(20, static_cast<int>(listFrame_.right - listFrame_.left) - 4),
                         std::max(20, static_cast<int>(listFrame_.bottom - listFrame_.top) - 4),
                         SWP_NOZORDER | SWP_NOACTIVATE);
            const int listWidth = std::max(40, static_cast<int>(listFrame_.right - listFrame_.left) - 8);
            const int nameWidth = std::max(120, listWidth / 3);
            ListView_SetColumnWidth(list_, 0, nameWidth);
            ListView_SetColumnWidth(list_, 1, std::max(120, listWidth - nameWidth));
        }
    }

    void Refresh() {
        results_.clear();
        const std::wstring query = Trim(GetText(edit_));
        struct ScoredResult {
            int id = 0;
            int score = 0;
            std::wstring name;
        };
        std::vector<ScoredResult> scored;
        for (const auto& link : model_.links) {
            const int score = MatchScore(link, query);
            if (score > 0) {
                scored.push_back(ScoredResult{link.id, score, ToLower(link.name)});
            }
        }
        std::sort(scored.begin(), scored.end(), [](const ScoredResult& left, const ScoredResult& right) {
            if (left.score != right.score) {
                return left.score > right.score;
            }
            if (left.name != right.name) {
                return left.name < right.name;
            }
            return left.id < right.id;
        });
        for (const auto& item : scored) {
            results_.push_back(item.id);
        }
        ListView_DeleteAllItems(list_);
        int row = 0;
        for (int id : results_) {
            const Link* link = FindLink(id);
            if (!link) {
                continue;
            }
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = row;
            item.pszText = const_cast<LPWSTR>(link->name.c_str());
            item.lParam = id;
            ListView_InsertItem(list_, &item);
            ListView_SetItemText(list_, row, 1, const_cast<LPWSTR>(link->path.c_str()));
            ++row;
        }
        if (row > 0) {
            ListView_SetItemState(list_, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
        resultCount_ = row;
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    const Link* FindLink(int id) const {
        for (const auto& link : model_.links) {
            if (link.id == id) {
                return &link;
            }
        }
        return nullptr;
    }

    void AcceptSelected() {
        int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (selected < 0) {
            return;
        }
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = selected;
        if (ListView_GetItem(list_, &item)) {
            selectedId_ = static_cast<int>(item.lParam);
            SavePosition();
            done_ = true;
            DestroyWindow(hwnd_);
        }
    }

    void MoveListSelection(int delta) {
        const int count = ListView_GetItemCount(list_);
        if (count <= 0) {
            return;
        }
        int current = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        int next = current < 0 ? (delta > 0 ? 0 : count - 1) : current + delta;
        next = std::max(0, std::min(count - 1, next));
        if (next == current) {
            return;
        }
        ListView_SetItemState(list_, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_SetItemState(list_, next, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, next, FALSE);
    }

    const Link* SelectedLink() const {
        int selected = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
        if (selected < 0) {
            return nullptr;
        }
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = selected;
        if (!ListView_GetItem(list_, &item)) {
            return nullptr;
        }
        return FindLink(static_cast<int>(item.lParam));
    }

    void ShowResultMenu(POINT screenPoint) {
        const Link* link = SelectedLink();
        HMENU menu = CreatePopupMenu();
        AppendMenuW(menu, link ? MF_STRING : (MF_STRING | MF_GRAYED), ID_SEARCH_RUN, L"运行");
        AppendMenuW(menu, link ? MF_STRING : (MF_STRING | MF_GRAYED), ID_SEARCH_OPEN_LOCATION, L"打开所在目录");
        AppendMenuW(menu, link ? MF_STRING : (MF_STRING | MF_GRAYED), ID_SEARCH_COPY_PATH, L"复制路径");
        ActivateWindow(hwnd_);
        TrackPopupMenu(menu, TPM_RIGHTBUTTON, screenPoint.x, screenPoint.y, 0, hwnd_, nullptr);
        DestroyMenu(menu);
    }

    void OpenSelectedLocation() {
        const Link* link = SelectedLink();
        if (!link) {
            return;
        }
        std::wstring error;
        ShellItemService::OpenContainingLocation(hwnd_, *link, error);
    }

    void CopySelectedPath() {
        if (const Link* link = SelectedLink()) {
            CopyText(hwnd_, link->path);
        }
    }

    void SavePosition() {
        if (!hwnd_) {
            return;
        }
        RECT rect{};
        if (GetWindowRect(hwnd_, &rect)) {
            config_.searchX = rect.left;
            config_.searchY = rect.top;
        }
    }

    HWND owner_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND edit_ = nullptr;
    HWND list_ = nullptr;
    HFONT font_ = nullptr;
    HFONT editFont_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    HBRUSH fieldBrush_ = nullptr;
    RECT editFrame_{};
    RECT listFrame_{};
    RECT statusFrame_{};
    const Theme& theme_;
    const AppModel& model_;
    AppConfig& config_;
    std::vector<int> results_;
    int resultCount_ = 0;
    int selectedId_ = 0;
    std::wstring initialQuery_;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool done_ = false;
};
}

int SearchDialog::Show(HWND owner, HINSTANCE instance, const Theme& theme, const AppModel& model, AppConfig& config, const std::wstring& initialQuery) {
    ++config.searchCount;
    SearchWindow window(owner, instance, theme, model, config, initialQuery);
    return window.Run();
}

