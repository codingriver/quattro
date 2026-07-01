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
    SearchWindow(HWND owner, HINSTANCE instance, const Theme& theme, const AppModel& model, AppConfig& config)
        : owner_(owner), instance_(instance), theme_(theme), model_(model), config_(config) {}

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
        hwnd_ = CreateWindowExW(
            WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE,
            wc.lpszClassName,
            L"搜索",
            WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_CLIPCHILDREN,
            x,
            y,
            560,
            430,
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
            Refresh();
            SetFocus(config_.focusSearch ? edit_ : list_);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            PaintBackground(dc);
            ThemedControls::DrawFieldFrame(theme_, dc, editFrame_, edit_);
            ThemedControls::DrawListFrame(theme_, dc, listFrame_, list_, true);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_PRINTCLIENT:
            PaintBackground(reinterpret_cast<HDC>(wParam));
            ThemedControls::DrawFieldFrame(theme_, reinterpret_cast<HDC>(wParam), editFrame_, edit_);
            ThemedControls::DrawListFrame(theme_, reinterpret_cast<HDC>(wParam), listFrame_, list_, true);
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
                Refresh();
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

    void Refresh() {
        results_.clear();
        const std::wstring query = Trim(GetText(edit_));
        for (const auto& link : model_.links) {
            if (query.empty() ||
                ContainsText(link.name, query) ||
                ContainsText(link.path, query) ||
                ContainsText(link.parameter, query) ||
                ContainsText(link.remark, query)) {
                results_.push_back(link.id);
            }
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
    const Theme& theme_;
    const AppModel& model_;
    AppConfig& config_;
    std::vector<int> results_;
    int selectedId_ = 0;
    bool ownerWasEnabled_ = false;
    bool ownerRestored_ = false;
    bool done_ = false;
};
}

int SearchDialog::Show(HWND owner, HINSTANCE instance, const Theme& theme, const AppModel& model, AppConfig& config) {
    ++config.searchCount;
    SearchWindow window(owner, instance, theme, model, config);
    return window.Run();
}

