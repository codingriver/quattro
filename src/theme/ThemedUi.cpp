#include "ThemedUi.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <unordered_map>

namespace {
DWORD StaticStyle(ThemedTextAlign align) {
    switch (align) {
    case ThemedTextAlign::Center:
        return SS_CENTER;
    case ThemedTextAlign::End:
        return SS_RIGHT;
    case ThemedTextAlign::Start:
    default:
        return SS_LEFT;
    }
}

const wchar_t* StatusState(ThemedStatusRole role) {
    switch (role) {
    case ThemedStatusRole::Info:
        return L"info";
    case ThemedStatusRole::Success:
        return L"success";
    case ThemedStatusRole::Warning:
        return L"warning";
    case ThemedStatusRole::Danger:
        return L"danger";
    case ThemedStatusRole::Normal:
    default:
        return L"normal";
    }
}

const wchar_t* LinkState(ThemedLinkRole role) {
    switch (role) {
    case ThemedLinkRole::External: return L"external";
    case ThemedLinkRole::Muted: return L"muted";
    case ThemedLinkRole::Danger: return L"danger";
    case ThemedLinkRole::Normal:
    default: return L"normal";
    }
}
} // namespace

ThemedEditFrameCollection::Entry* ThemedEditFrameCollection::Find(HWND child) {
    auto it = std::find_if(entries_.begin(), entries_.end(), [child](const Entry& entry) {
        return entry.child == child;
    });
    return it == entries_.end() ? nullptr : &*it;
}

void ThemedEditFrameCollection::RegisterEditFrame(HWND child, RECT frame, const ThemedEditOptions& options) {
    if (!child) return;
    if (Entry* entry = Find(child)) {
        entry->frame = frame;
        entry->options = options;
    } else {
        entries_.push_back(Entry{child, frame, options});
    }
}

void ThemedEditFrameCollection::Remove(HWND child) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [child](const Entry& entry) {
        return entry.child == child;
    }), entries_.end());
}

void ThemedEditFrameCollection::SetFrame(HWND child, RECT frame) {
    if (Entry* entry = Find(child)) entry->frame = frame;
}

void ThemedEditFrameCollection::SetReadOnly(HWND child, bool readOnly) {
    if (Entry* entry = Find(child)) {
        entry->options.readOnly = readOnly;
        SendMessageW(child, EM_SETREADONLY, readOnly ? TRUE : FALSE, 0);
    }
}

void ThemedEditFrameCollection::SetError(HWND child, bool error) {
    if (Entry* entry = Find(child)) entry->options.error = error;
}

void ThemedEditFrameCollection::Draw(const Theme& theme, HDC dc) const {
    for (const Entry& entry : entries_) {
        if (IsWindow(entry.child) && IsWindowVisible(entry.child)) {
            ThemedControls::DrawEditFrame(theme, dc, entry.frame, entry.child, entry.options.readOnly, entry.options.error);
        }
    }
}

namespace {
int TextWidth(HWND parent, HFONT font, const std::wstring& text) {
    if (text.empty()) {
        return 0;
    }
    HDC dc = GetDC(parent);
    if (!dc) {
        return 0;
    }
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    if (oldFont) {
        SelectObject(dc, oldFont);
    }
    ReleaseDC(parent, dc);
    return size.cx;
}

struct GroupBoxRuntime {
    std::vector<HWND> children;
};

struct PanelChildRuntime {
    HWND hwnd = nullptr;
    RECT original{};
};

struct PanelRuntime {
    const Theme* theme = nullptr;
    std::vector<PanelChildRuntime> children;
    ThemedPanelRole role = ThemedPanelRole::Normal;
    bool enabled = true;
    bool visible = true;
    bool scrollable = false;
    int scrollPosition = 0;
    int maximumScroll = 0;
};

struct TabControlRuntime {
    HWND owner = nullptr;
    int controlId = 0;
    const Theme* theme = nullptr;
    std::vector<HWND> buttons;
    std::vector<std::vector<HWND>> pages;
    std::vector<HWND> pageRoots;
    int activeIndex = -1;
};

struct ToolRuntimeItem {
    HWND hwnd = nullptr;
    HWND separator = nullptr;
    int id = 0;
    ThemedToolItemKind kind = ThemedToolItemKind::Command;
    ThemedToolItemAlignment alignment = ThemedToolItemAlignment::Leading;
    ThemedToolItemDisplay display = ThemedToolItemDisplay::TextOnly;
    std::wstring text;
    std::wstring tooltip;
    HICON icon = nullptr;
    ThemedIconOwnership iconOwnership = ThemedIconOwnership::Borrowed;
    int width = 0;
    bool requestedVisible = true;
    bool overflowed = false;
};

struct ToolBarRuntime {
    HWND owner = nullptr;
    int controlId = 0;
    const Theme* theme = nullptr;
    ThemedTooltipRegistry* tooltipRegistry = nullptr;
    HINSTANCE instance = nullptr;
    HFONT font = nullptr;
    bool enabled = true;
    bool compact = true;
    bool automaticOverflow = true;
    HWND overflowButton = nullptr;
    int updateDepth = 0;
    bool layoutPending = false;
    std::vector<int> order;
    std::unordered_map<int, ToolRuntimeItem> items;
};

constexpr int kToolBarOverflowId = 0x7ff0;

LRESULT CALLBACK ToolItemProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR refData);

std::unordered_map<HWND, GroupBoxRuntime>& GroupBoxStates() {
    static std::unordered_map<HWND, GroupBoxRuntime> states;
    return states;
}

std::unordered_map<HWND, PanelRuntime>& PanelStates() {
    static std::unordered_map<HWND, PanelRuntime> states;
    return states;
}

const wchar_t* PanelRoleState(ThemedPanelRole role) {
    switch (role) {
    case ThemedPanelRole::Subtle: return L"subtle";
    case ThemedPanelRole::Raised: return L"raised";
    case ThemedPanelRole::Inset: return L"inset";
    case ThemedPanelRole::Warning: return L"warning";
    case ThemedPanelRole::Danger: return L"danger";
    case ThemedPanelRole::Normal:
    default: return L"normal";
    }
}

void ApplyPanelScroll(HWND panel) {
    auto it = PanelStates().find(panel);
    if (it == PanelStates().end()) return;
    RECT panelWindow{};
    GetWindowRect(panel, &panelWindow);
    MapWindowPoints(nullptr, GetParent(panel), reinterpret_cast<POINT*>(&panelWindow), 2);
    for (const auto& child : it->second.children) {
        if (!child.hwnd) continue;
        SetWindowPos(child.hwnd, nullptr,
            child.original.left,
            child.original.top - it->second.scrollPosition,
            child.original.right - child.original.left,
            child.original.bottom - child.original.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
    }
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS};
    RECT client{};
    GetClientRect(panel, &client);
    info.nMin = 0;
    info.nMax = it->second.maximumScroll + (client.bottom - client.top);
    info.nPage = client.bottom - client.top;
    info.nPos = it->second.scrollPosition;
    SetScrollInfo(panel, SB_VERT, &info, TRUE);
}

void SetPanelScrollInternal(HWND panel, int position) {
    auto it = PanelStates().find(panel);
    if (it == PanelStates().end() || !it->second.scrollable) return;
    position = std::clamp(position, 0, it->second.maximumScroll);
    if (position == it->second.scrollPosition) return;
    it->second.scrollPosition = position;
    ApplyPanelScroll(panel);
}

LRESULT CALLBACK PanelRuntimeProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
    auto it = PanelStates().find(hwnd);
    if (it != PanelStates().end() && it->second.scrollable) {
        if (message == WM_MOUSEWHEEL) {
            const int step = static_cast<int>(it->second.theme->metric(L"panel", L"scrollStep", 24.0f));
            SetPanelScrollInternal(hwnd, it->second.scrollPosition - GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * step);
            return 0;
        }
        if (message == WM_VSCROLL) {
            int next = it->second.scrollPosition;
            const int step = static_cast<int>(it->second.theme->metric(L"panel", L"scrollStep", 24.0f));
            switch (LOWORD(wParam)) {
            case SB_LINEUP: next -= step; break;
            case SB_LINEDOWN: next += step; break;
            case SB_PAGEUP: next -= step * 4; break;
            case SB_PAGEDOWN: next += step * 4; break;
            case SB_THUMBPOSITION:
            case SB_THUMBTRACK: next = HIWORD(wParam); break;
            case SB_TOP: next = 0; break;
            case SB_BOTTOM: next = it->second.maximumScroll; break;
            default: break;
            }
            SetPanelScrollInternal(hwnd, next);
            return 0;
        }
    }
    if (message == WM_SIZE) ApplyPanelScroll(hwnd);
    if (message == WM_NCDESTROY) {
        PanelStates().erase(hwnd);
        RemoveWindowSubclass(hwnd, PanelRuntimeProc, id);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

std::unordered_map<HWND, TabControlRuntime>& TabControlStates() {
    static std::unordered_map<HWND, TabControlRuntime> states;
    return states;
}

std::unordered_map<HWND, ToolBarRuntime>& ToolBarStates() {
    static std::unordered_map<HWND, ToolBarRuntime> states;
    return states;
}

void ApplyActiveTab(HWND tabControl, int index, bool notify) {
    auto it = TabControlStates().find(tabControl);
    if (it == TabControlStates().end() || index < 0 || index >= static_cast<int>(it->second.buttons.size())) return;
    auto& state = it->second;
    state.activeIndex = index;
    for (int i = 0; i < static_cast<int>(state.buttons.size()); ++i) {
        ThemedControls::SetTabButtonSelected(state.buttons[static_cast<std::size_t>(i)], i == index);
        if (i < static_cast<int>(state.pages.size())) {
            for (HWND child : state.pages[static_cast<std::size_t>(i)]) {
                if (child) ShowWindow(child, i == index ? SW_SHOWNA : SW_HIDE);
            }
        }
    }
    if (notify && state.owner) {
        SendMessageW(state.owner, WM_COMMAND, MAKEWPARAM(state.controlId, CBN_SELCHANGE), reinterpret_cast<LPARAM>(tabControl));
    }
}

bool NavigateTabControl(HWND tabControl, int direction, bool focusTabButton) {
    auto it = TabControlStates().find(tabControl);
    if (it == TabControlStates().end() || it->second.buttons.empty()) return false;
    const int count = static_cast<int>(it->second.buttons.size());
    for (int offset = 1; offset <= count; ++offset) {
        const int next = (it->second.activeIndex + direction * offset + count * 2) % count;
        HWND candidate = it->second.buttons[static_cast<std::size_t>(next)];
        if (IsWindowEnabled(candidate) && IsWindowVisible(candidate)) {
            ApplyActiveTab(tabControl, next, true);
            if (focusTabButton) SetFocus(candidate);
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK TabPageNavigationProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR refData) {
    if (message == WM_KEYDOWN && wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        const int direction = (GetKeyState(VK_SHIFT) & 0x8000) != 0 ? -1 : 1;
        if (NavigateTabControl(reinterpret_cast<HWND>(refData), direction, false)) return 0;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, TabPageNavigationProc, id);
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK TabContainerProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_DRAWITEM) {
        auto it = TabControlStates().find(hwnd);
        if (it != TabControlStates().end() && it->second.theme
            && ThemedControls::Draw(*it->second.theme, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
    }
    if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED) {
        auto it = TabControlStates().find(hwnd);
        if (it != TabControlStates().end()) {
            for (int i = 0; i < static_cast<int>(it->second.buttons.size()); ++i) {
                if (it->second.buttons[static_cast<std::size_t>(i)] == reinterpret_cast<HWND>(lParam)) {
                    ApplyActiveTab(hwnd, i, true);
                    return 0;
                }
            }
        }
    }
    if (message == WM_NCDESTROY) {
        TabControlStates().erase(hwnd);
        RemoveWindowSubclass(hwnd, TabContainerProc, id);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK GroupBoxRuntimeProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCDESTROY) {
        GroupBoxStates().erase(hwnd);
        RemoveWindowSubclass(hwnd, GroupBoxRuntimeProc, id);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK TabButtonNavigationProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR refData) {
    HWND tabControl = reinterpret_cast<HWND>(refData);
    if (message == WM_KEYDOWN) {
        auto it = TabControlStates().find(tabControl);
        if (it != TabControlStates().end()) {
            int direction = 0;
            if (wParam == VK_LEFT || wParam == VK_UP) direction = -1;
            else if (wParam == VK_RIGHT || wParam == VK_DOWN) direction = 1;
            else if (wParam == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
                direction = (GetKeyState(VK_SHIFT) & 0x8000) != 0 ? -1 : 1;
            }
            if (direction != 0 && NavigateTabControl(tabControl, direction, true)) return 0;
        }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, TabButtonNavigationProc, id);
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

void LayoutToolBar(HWND toolbar) {
    auto stateIt = ToolBarStates().find(toolbar);
    if (stateIt == ToolBarStates().end()) return;
    auto& state = stateIt->second;
    if (state.updateDepth > 0) {
        state.layoutPending = true;
        return;
    }
    RECT client{};
    GetClientRect(toolbar, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int padding = static_cast<int>(state.theme->metric(L"toolbar", L"paddingX", 6.0f));
    const int gap = static_cast<int>(state.theme->metric(L"toolbar", L"itemGap", 4.0f));
    const int itemHeight = state.compact
        ? ThemedControls::CompactButtonHeight(*state.theme)
        : ThemedControls::ButtonHeight(*state.theme);
    const int y = std::max(0, (height - itemHeight) / 2);
    const int overflowWidth = itemHeight;

    for (auto& pair : state.items) pair.second.overflowed = false;
    int required = padding * 2;
    for (int itemId : state.order) {
        auto& item = state.items[itemId];
        if (item.requestedVisible) required += item.width + gap;
    }
    const bool needsOverflow = state.automaticOverflow && required > width;
    int leadingX = padding;
    int trailingX = width - padding - (needsOverflow ? overflowWidth + gap : 0);

    for (int itemId : state.order) {
        auto& item = state.items[itemId];
        HWND child = item.hwnd ? item.hwnd : item.separator;
        if (!child) continue;
        if (!item.requestedVisible) {
            ShowWindow(child, SW_HIDE);
            continue;
        }
        int x = item.alignment == ThemedToolItemAlignment::Leading ? leadingX : trailingX - item.width;
        const bool fits = item.alignment == ThemedToolItemAlignment::Leading
            ? x + item.width <= trailingX
            : x >= leadingX;
        if (needsOverflow && !fits) {
            item.overflowed = true;
            ShowWindow(child, SW_HIDE);
            continue;
        }
        MoveWindow(child, x, y, item.width, itemHeight, TRUE);
        ShowWindow(child, SW_SHOWNA);
        if (item.alignment == ThemedToolItemAlignment::Leading) leadingX += item.width + gap;
        else trailingX -= item.width + gap;
    }
    if (state.overflowButton) {
        MoveWindow(state.overflowButton, width - padding - overflowWidth, y, overflowWidth, itemHeight, TRUE);
        ShowWindow(state.overflowButton, needsOverflow ? SW_SHOWNA : SW_HIDE);
    }
}

void RecalculateToolWidth(HWND toolbar, ToolRuntimeItem& item) {
    auto stateIt = ToolBarStates().find(toolbar);
    if (stateIt == ToolBarStates().end() || item.kind == ThemedToolItemKind::Separator) return;
    auto& state = stateIt->second;
    RECT client{};
    GetClientRect(toolbar, &client);
    ThemedUi nested(state.instance, toolbar, *state.theme, state.font, DialogLayoutKind::Compact, client.right, client.bottom);
    const ThemedButtonSize size = state.compact ? ThemedButtonSize::Compact : ThemedButtonSize::Normal;
    const int iconSize = static_cast<int>(state.theme->metric(L"toolbarItem", L"iconSize", 16.0f));
    const int iconGap = static_cast<int>(state.theme->metric(L"toolbarItem", L"iconGap", 6.0f));
    const int iconPadding = static_cast<int>(state.theme->metric(L"toolbarItem", L"iconPaddingX", 8.0f));
    const std::wstring visibleText = item.display == ThemedToolItemDisplay::IconOnly ? L"" : item.text;
    item.width = nested.buttonWidth(visibleText,
        item.kind == ThemedToolItemKind::Toggle ? ThemedButtonRole::Tab : ThemedButtonRole::Normal,
        size, ThemedButtonWidthMode::Text);
    if (item.display == ThemedToolItemDisplay::IconOnly) item.width = iconSize + iconPadding * 2;
    else if (item.display == ThemedToolItemDisplay::IconAndText) item.width += iconSize + iconGap;
    SetWindowTextW(item.hwnd, visibleText.c_str());
}

bool InsertRuntimeTool(HWND toolbar, int index, const ThemedToolItem& source) {
    auto stateIt = ToolBarStates().find(toolbar);
    if (stateIt == ToolBarStates().end()) return false;
    auto& state = stateIt->second;
    int key = source.id;
    if (source.kind == ThemedToolItemKind::Separator) {
        key = -1;
        while (state.items.find(key) != state.items.end()) --key;
    } else if (key == kToolBarOverflowId || state.items.find(key) != state.items.end()) {
        return false;
    }
    ToolRuntimeItem item{};
    item.id = source.id;
    item.kind = source.kind;
    item.alignment = source.alignment;
    item.text = source.text;
    item.tooltip = source.tooltip;
    item.icon = source.icon;
    item.iconOwnership = source.iconOwnership;
    item.requestedVisible = true;
    if (source.kind == ThemedToolItemKind::Separator) {
        item.width = static_cast<int>(state.theme->metric(L"toolbar", L"separatorWidth", 9.0f));
        item.separator = ThemedControls::CreateSeparator(state.instance, toolbar, RECT{0, 0, item.width, 1}, *state.theme, true);
    } else {
        item.display = source.display == ThemedToolItemDisplay::Automatic
            ? (source.icon ? (source.text.empty() ? ThemedToolItemDisplay::IconOnly : ThemedToolItemDisplay::IconAndText) : ThemedToolItemDisplay::TextOnly)
            : source.display;
        RECT client{}; GetClientRect(toolbar, &client);
        ThemedUi nested(state.instance, toolbar, *state.theme, state.font, DialogLayoutKind::Compact, client.right, client.bottom);
        const ThemedButtonSize size = state.compact ? ThemedButtonSize::Compact : ThemedButtonSize::Normal;
        item.hwnd = source.kind == ThemedToolItemKind::Toggle
            ? nested.TabButton(source.id, L"", 0, 0, 1, source.checked)
            : nested.Button(source.id, L"", 0, 0, ThemedButtonRole::Normal, size, ThemedButtonWidthMode::Fixed, 1);
        ThemedControls::SetControlBackgroundComponent(item.hwnd, L"toolbar");
        EnableWindow(item.hwnd, state.enabled && source.enabled ? TRUE : FALSE);
        if (source.icon) SendMessageW(item.hwnd, BM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(source.icon));
        SetWindowSubclass(item.hwnd, ToolItemProc, 3, reinterpret_cast<DWORD_PTR>(toolbar));
    }
    state.items[key] = std::move(item);
    if (source.kind != ThemedToolItemKind::Separator) RecalculateToolWidth(toolbar, state.items[key]);
    index = std::clamp(index, 0, static_cast<int>(state.order.size()));
    state.order.insert(state.order.begin() + index, key);
    LayoutToolBar(toolbar);
    return true;
}

void ShowToolBarOverflow(HWND toolbar) {
    auto stateIt = ToolBarStates().find(toolbar);
    if (stateIt == ToolBarStates().end()) return;
    auto& state = stateIt->second;
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    for (int itemId : state.order) {
        auto& item = state.items[itemId];
        if (!item.overflowed || item.kind == ThemedToolItemKind::Separator) continue;
        UINT flags = MF_STRING;
        if (!item.hwnd || !IsWindowEnabled(item.hwnd)) flags |= MF_GRAYED;
        if (item.kind == ThemedToolItemKind::Toggle && ThemedUi::IsChecked(item.hwnd)) flags |= MF_CHECKED;
        AppendMenuW(menu, flags, static_cast<UINT_PTR>(item.id), item.text.c_str());
    }
    RECT button{};
    GetWindowRect(state.overflowButton, &button);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, button.right, button.bottom, 0, toolbar, nullptr);
    DestroyMenu(menu);
    if (command != 0) {
        auto item = state.items.find(command);
        if (item != state.items.end() && item->second.kind == ThemedToolItemKind::Toggle) {
            ThemedUi::SetChecked(item->second.hwnd, !ThemedUi::IsChecked(item->second.hwnd));
        }
        if (state.owner) SendMessageW(state.owner, WM_COMMAND, MAKEWPARAM(command, BN_CLICKED), reinterpret_cast<LPARAM>(toolbar));
    }
}

LRESULT CALLBACK ToolBarProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR) {
    if (message == WM_DRAWITEM) {
        auto it = ToolBarStates().find(hwnd);
        if (it != ToolBarStates().end() && it->second.theme
            && ThemedControls::Draw(*it->second.theme, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
            return TRUE;
        }
    }
    if (message == WM_COMMAND && HIWORD(wParam) == BN_CLICKED) {
        auto it = ToolBarStates().find(hwnd);
        if (it != ToolBarStates().end()) {
            if (LOWORD(wParam) == kToolBarOverflowId) {
                ShowToolBarOverflow(hwnd);
                return 0;
            }
            auto item = it->second.items.find(LOWORD(wParam));
            if (item != it->second.items.end() && item->second.kind == ThemedToolItemKind::Toggle) {
                const bool checked = !ThemedUi::IsChecked(item->second.hwnd);
                ThemedUi::SetChecked(item->second.hwnd, checked);
            }
            if (it->second.owner) {
                SendMessageW(it->second.owner, WM_COMMAND, wParam, reinterpret_cast<LPARAM>(hwnd));
            }
            return 0;
        }
    }
    if (message == WM_SIZE) LayoutToolBar(hwnd);
    if (message == WM_NCDESTROY) {
        auto it = ToolBarStates().find(hwnd);
        if (it != ToolBarStates().end()) {
            for (auto& pair : it->second.items) {
                if (pair.second.icon && pair.second.iconOwnership == ThemedIconOwnership::Transfer) DestroyIcon(pair.second.icon);
            }
        }
        ToolBarStates().erase(hwnd);
        RemoveWindowSubclass(hwnd, ToolBarProc, id);
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK ToolItemProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR refData) {
    HWND toolbar = reinterpret_cast<HWND>(refData);
    auto toolbarIt = ToolBarStates().find(toolbar);
    if (toolbarIt != ToolBarStates().end()) {
        auto item = toolbarIt->second.items.find(GetDlgCtrlID(hwnd));
        if (item != toolbarIt->second.items.end() && toolbarIt->second.tooltipRegistry && !item->second.tooltip.empty()) {
            if (message == WM_MOUSEMOVE) {
                TRACKMOUSEEVENT track{};
                track.cbSize = sizeof(track);
                track.dwFlags = TME_LEAVE;
                track.hwndTrack = hwnd;
                TrackMouseEvent(&track);
                POINT point{};
                GetCursorPos(&point);
                ThemedTooltipOptions options{};
                options.placement = ThemedTooltipPlacement::Cursor;
                options.multiline = false;
                toolbarIt->second.tooltipRegistry->ShowTooltip(item->second.tooltip, point, options);
            } else if (message == WM_MOUSELEAVE) {
                toolbarIt->second.tooltipRegistry->HideTooltip();
            }
        }
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, ToolItemProc, id);
    return DefSubclassProc(hwnd, message, wParam, lParam);
}
}

// 创建绑定到单个父窗口的主题 UI 门面对象。
// 参数：
// - instance：创建子控件时使用的 Win32 模块实例。
// - parent：承载此门面创建的所有控件的父窗口。
// - theme：当前主题，用于读取尺寸、颜色并进行 owner-draw 绘制。
// - font：分配给文本类控件的字体。
// - layoutKind：对话框布局预设，用于解析共享间距参数。
// - clientWidth：父窗口客户区宽度，供布局 helper 计算位置。
// - clientHeight：父窗口客户区高度，供底部按钮布局 helper 计算位置。
// 返回值：无。
ThemedUi::ThemedUi(
    HINSTANCE instance,
    HWND parent,
    const Theme& theme,
    HFONT font,
    DialogLayoutKind layoutKind,
    int clientWidth,
    int clientHeight,
    ThemedEditFrameRegistry* editFrameRegistry,
    ThemedTableFrameRegistry* tableFrameRegistry,
    ThemedTooltipRegistry* tooltipRegistry)
    : instance_(instance),
      parent_(parent),
      theme_(theme),
      font_(font),
      layout_(GetDialogLayoutMetrics(theme, layoutKind)),
      clientWidth_(clientWidth),
      clientHeight_(clientHeight),
      editFrameRegistry_(editFrameRegistry),
      tableFrameRegistry_(tableFrameRegistry),
      tooltipRegistry_(tooltipRegistry) {}

// 返回主题定义的单行标签高度。
// 参数：无。
// 返回值：当前主题中的标签高度，单位为像素。
int ThemedUi::labelHeight() const {
    return ThemedControls::LabelHeight(theme_);
}

// 返回主题定义的普通按钮高度。
// 参数：无。
// 返回值：当前主题中的按钮高度，单位为像素。
int ThemedUi::buttonHeight() const {
    return ThemedControls::ButtonHeight(theme_);
}

// 根据按钮角色和尺寸规格返回按钮高度。
// 参数：
// - role：按钮语义角色，例如普通按钮、主按钮、迷你按钮或标签按钮。
// - size：按钮尺寸规格，例如普通、紧凑或迷你。
// 返回值：匹配角色和尺寸规格的按钮高度，单位为像素。
int ThemedUi::buttonHeight(ThemedButtonRole role, ThemedButtonSize size) const {
    if (role == ThemedButtonRole::Mini || size == ThemedButtonSize::Mini) {
        return ThemedControls::MiniButtonHeight(theme_);
    }
    if (role == ThemedButtonRole::Tab) {
        return ThemedControls::TabButtonHeight(theme_);
    }
    if (size == ThemedButtonSize::Compact) {
        return ThemedControls::CompactButtonHeight(theme_);
    }
    return ThemedControls::ButtonHeight(theme_);
}

// 返回主题定义的紧凑按钮高度。
// 参数：无。
// 返回值：当前主题中的紧凑按钮高度，单位为像素。
int ThemedUi::compactButtonHeight() const {
    return ThemedControls::CompactButtonHeight(theme_);
}

// 返回主题定义的进度条高度。
// 参数：无。
// 返回值：当前主题中的进度条高度，单位为像素。
int ThemedUi::progressBarHeight() const {
    return ThemedControls::ProgressBarHeight(theme_);
}

int ThemedUi::editHeight(ThemedEditMode mode) const {
    if (mode == ThemedEditMode::MultiLine) {
        return ThemedControls::EditFrameHeight(theme_) * 2 + layout_.rowGap;
    }
    return ThemedControls::EditFrameHeight(theme_);
}

// 根据按钮角色、尺寸规格和宽度策略返回按钮宽度。
// 参数：
// - text：按钮标题，用于 Text 自适应宽度策略。
// - role：按钮语义角色，例如普通按钮、主按钮、迷你按钮或标签按钮。
// - size：按钮尺寸规格，例如普通、紧凑或迷你。
// - widthMode：宽度策略；Fixed 使用固定宽度，Text 按文本测量。
// - fixedWidth：固定宽度，或 Text 模式下的最小宽度；小于等于 0 时使用默认值。
// 返回值：计算后的按钮宽度，单位为像素。
int ThemedUi::buttonWidth(
    const std::wstring& text,
    ThemedButtonRole role,
    ThemedButtonSize size,
    ThemedButtonWidthMode widthMode,
    int fixedWidth) const {
    if (role == ThemedButtonRole::Mini || size == ThemedButtonSize::Mini) {
        return fixedWidth > 0 ? fixedWidth : static_cast<int>(theme_.metric(L"miniButton", L"width", 26.0f));
    }
    if (widthMode == ThemedButtonWidthMode::Fixed) {
        if (fixedWidth > 0) {
            return fixedWidth;
        }
        if (role == ThemedButtonRole::Tab) {
            return static_cast<int>(theme_.metric(L"tabButton", L"groupItemWidth", 58.0f));
        }
        return layout_.footerButtonWidth;
    }

    const wchar_t* component = role == ThemedButtonRole::Tab ? L"tabButton" : L"button";
    const int paddingX = static_cast<int>(theme_.metric(component, L"paddingX", 12.0f));
    const int measured = TextWidth(parent_, font_, text) + paddingX * 2;
    const int minWidth = fixedWidth > 0 ? fixedWidth : buttonHeight(role, size) * 2;
    if (role == ThemedButtonRole::Tab) {
        const int minTextWidth = static_cast<int>(theme_.metric(L"tabButton", L"minTextWidth", 18.0f));
        return std::max({measured, minTextWidth + paddingX * 2, minWidth});
    }
    return std::max(measured, minWidth);
}

int ThemedUi::textWidth(const std::wstring& text) const {
    return TextWidth(parent_, font_, text);
}

// 使用共享对话框参数计算底部按钮的 x 坐标。
// 参数：
// - buttonIndex：按钮在底部按钮组中的从 0 开始的索引。
// - buttonCount：底部按钮组中的按钮总数。
// 返回值：指定底部按钮的左侧 x 坐标。
int ThemedUi::footerButtonX(int buttonIndex, int buttonCount) const {
    return layout_.FooterButtonX(clientWidth_, buttonIndex, buttonCount);
}

// 使用共享对话框参数计算底部按钮的 y 坐标。
// 参数：
// - buttonHeight：要放置的底部按钮高度。
// 返回值：底部按钮的顶部 y 坐标。
int ThemedUi::footerButtonY(int buttonHeight) const {
    return layout_.FooterButtonY(clientHeight_, buttonHeight);
}

// 计算一组控件整体居中时的 x 坐标。
// 参数：
// - groupWidth：需要整体居中的控件组总宽度。
// 返回值：控件组左侧 x 坐标，并限制在对话框内容区域内。
int ThemedUi::centeredGroupX(int groupWidth) const {
    return layout_.CenteredGroupX(clientWidth_, groupWidth);
}

// 根据共享行间距计算下一行的 y 坐标。
// 参数：
// - y：当前行顶部 y 坐标。
// - controlHeight：当前行控件高度。
// 返回值：下一行顶部 y 坐标。
int ThemedUi::nextRowY(int y, int controlHeight) const {
    return y + controlHeight + layout_.rowGap;
}

// 根据位置和尺寸构造 Win32 RECT。
// 参数：
// - x：左侧 x 坐标。
// - y：顶部 y 坐标。
// - width：矩形宽度，单位为像素。
// - height：矩形高度，单位为像素。
// 返回值：已填充 left/top/right/bottom 的 RECT。
RECT ThemedUi::rect(int x, int y, int width, int height) const {
    return RECT{x, y, x + width, y + height};
}

RECT ThemedUi::editFrame(int x, int y, int width, ThemedEditMode mode) const {
    return rect(x, y, width, editHeight(mode));
}

// 创建主题标签文本控件。
// 参数：
// - text：标签显示的文本。
// - x：左侧 x 坐标。
// - y：顶部 y 坐标。
// - width：标签宽度，单位为像素。
// - style：Win32 静态控件样式标志，例如 SS_LEFT。
// 返回值：创建成功时返回标签 HWND，失败时返回 nullptr。
HWND ThemedUi::Label(const std::wstring& text, int x, int y, int width, ThemedLabelOptions options) const {
    int lineCount = 1;
    if (options.lines == ThemedLabelLines::Two) {
        lineCount = 2;
    } else if (options.lines == ThemedLabelLines::Three) {
        lineCount = 3;
    }
    if (lineCount == 1) {
        return ThemedControls::CreateLabelText(
            instance_, parent_, text.c_str(), x, y, width, theme_, font_, StaticStyle(options.align));
    }
    return ThemedControls::CreateStaticText(
        instance_, parent_, text.c_str(), x, y, width, labelHeight() * lineCount, font_, StaticStyle(options.align));
}

HWND ThemedUi::StatusText(const std::wstring& text, int x, int y, int width, ThemedStatusTextOptions options) const {
    return BindTheme(ThemedControls::CreateStatusText(
        instance_, parent_, text.c_str(), x, y, width, theme_, font_, StatusState(options.role), StaticStyle(options.align)));
}

void ThemedUi::SetStatusTextRole(HWND hwnd, ThemedStatusRole role) const {
    ThemedControls::SetStatusTextState(hwnd, StatusState(role));
}

HWND ThemedUi::StatusBadge(const std::wstring& text, int x, int y, int width, ThemedStatusRole role) const {
    return BindTheme(ThemedControls::CreateStatusBadge(
        instance_, parent_, text.c_str(), x, y, width, theme_, font_, StatusState(role)));
}

void ThemedUi::SetStatusBadgeRole(HWND hwnd, ThemedStatusRole role) const {
    ThemedControls::SetStatusBadgeState(hwnd, StatusState(role));
}

void ThemedUi::SetEnabled(HWND hwnd, bool enabled) const {
    if (!hwnd) {
        return;
    }
    EnableWindow(hwnd, enabled ? TRUE : FALSE);
    InvalidateRect(hwnd, nullptr, TRUE);
    if (HWND parent = GetParent(hwnd)) {
        InvalidateRect(parent, nullptr, FALSE);
    }
}

void ThemedUi::MoveControl(HWND hwnd, int x, int y, int width) const {
    if (!hwnd) {
        return;
    }
    RECT rect{};
    GetWindowRect(hwnd, &rect);
    SetWindowPos(
        hwnd, nullptr, x, y, width, rect.bottom - rect.top,
        SWP_NOACTIVATE | SWP_NOZORDER);
}

void ThemedUi::MoveComboBox(HWND hwnd, int x, int y, int width) const {
    if (!hwnd) {
        return;
    }
    SetWindowPos(
        hwnd, nullptr, x, y, width, ThemedControls::ComboBoxDropdownHeight(theme_),
        SWP_NOACTIVATE | SWP_NOZORDER);
}

HWND ThemedUi::GroupBox(int id, const std::wstring& title, RECT frame, ThemedGroupBoxOptions options) const {
    HWND group = BindTheme(ThemedControls::CreateGroupBox(
        instance_, parent_, id, title.c_str(), frame, font_, theme_, options.raised));
    if (group) {
        EnableWindow(group, options.enabled ? TRUE : FALSE);
        GroupBoxStates()[group] = GroupBoxRuntime{};
        SetWindowSubclass(group, GroupBoxRuntimeProc, 2, 0);
    }
    return group;
}

RECT ThemedUi::GroupContentRect(HWND groupBox) {
    return ThemedControls::GroupBoxContentRect(groupBox);
}

void ThemedUi::BindGroupChildren(HWND groupBox, const std::vector<HWND>& children) {
    if (!groupBox) return;
    GroupBoxStates()[groupBox].children = children;
    SetGroupEnabled(groupBox, IsWindowEnabled(groupBox) != FALSE);
}

void ThemedUi::SetGroupEnabled(HWND groupBox, bool enabled) {
    if (!groupBox) return;
    EnableWindow(groupBox, enabled ? TRUE : FALSE);
    auto it = GroupBoxStates().find(groupBox);
    if (it != GroupBoxStates().end()) {
        for (HWND child : it->second.children) {
            if (child) EnableWindow(child, enabled ? TRUE : FALSE);
        }
    }
    InvalidateRect(groupBox, nullptr, TRUE);
}

HWND ThemedUi::Panel(int id, RECT frame, ThemedPanelOptions options) const {
    HWND panel = BindTheme(ThemedControls::CreatePanel(
        instance_, parent_, id, frame, font_, theme_, PanelRoleState(options.role), options.scrollable));
    if (!panel) return nullptr;
    auto& runtime = PanelStates()[panel];
    runtime.theme = &theme_;
    runtime.role = options.role;
    runtime.enabled = options.enabled;
    runtime.visible = options.visible;
    runtime.scrollable = options.scrollable;
    SetWindowSubclass(panel, PanelRuntimeProc, 2, 0);
    SetPanelEnabled(panel, options.enabled);
    SetPanelVisible(panel, options.visible);
    return panel;
}

RECT ThemedUi::PanelContentRect(HWND panel) {
    return ThemedControls::PanelContentRect(panel);
}

void ThemedUi::BindPanelChildren(HWND panel, const std::vector<HWND>& children) {
    auto it = PanelStates().find(panel);
    if (it == PanelStates().end()) return;
    it->second.children.clear();
    it->second.maximumScroll = 0;
    RECT panelRect{};
    GetWindowRect(panel, &panelRect);
    MapWindowPoints(nullptr, GetParent(panel), reinterpret_cast<POINT*>(&panelRect), 2);
    RECT content = PanelContentRect(panel);
    const int visibleHeight = content.bottom - content.top;
    for (HWND child : children) {
        if (!child) continue;
        RECT rect{};
        GetWindowRect(child, &rect);
        MapWindowPoints(nullptr, GetParent(child), reinterpret_cast<POINT*>(&rect), 2);
        it->second.children.push_back(PanelChildRuntime{child, rect});
        it->second.maximumScroll = std::max(
            it->second.maximumScroll,
            static_cast<int>(rect.bottom - (panelRect.top + content.top) - visibleHeight));
        EnableWindow(child, it->second.enabled ? TRUE : FALSE);
        ShowWindow(child, it->second.visible ? SW_SHOWNA : SW_HIDE);
    }
    ApplyPanelScroll(panel);
}

void ThemedUi::SetPanelEnabled(HWND panel, bool enabled) {
    auto it = PanelStates().find(panel);
    if (it == PanelStates().end()) return;
    it->second.enabled = enabled;
    EnableWindow(panel, enabled ? TRUE : FALSE);
    for (const auto& child : it->second.children) if (child.hwnd) EnableWindow(child.hwnd, enabled ? TRUE : FALSE);
    InvalidateRect(panel, nullptr, TRUE);
}

bool ThemedUi::IsPanelEnabled(HWND panel) {
    auto it = PanelStates().find(panel);
    return it != PanelStates().end() && it->second.enabled;
}

void ThemedUi::SetPanelVisible(HWND panel, bool visible) {
    auto it = PanelStates().find(panel);
    if (it == PanelStates().end()) return;
    it->second.visible = visible;
    ShowWindow(panel, visible ? SW_SHOWNA : SW_HIDE);
    for (const auto& child : it->second.children) if (child.hwnd) ShowWindow(child.hwnd, visible ? SW_SHOWNA : SW_HIDE);
}

bool ThemedUi::IsPanelVisible(HWND panel) {
    auto it = PanelStates().find(panel);
    return it != PanelStates().end() && it->second.visible;
}

void ThemedUi::SetPanelRole(HWND panel, ThemedPanelRole role) {
    auto it = PanelStates().find(panel);
    if (it == PanelStates().end()) return;
    it->second.role = role;
    ThemedControls::SetPanelRole(panel, PanelRoleState(role));
}

void ThemedUi::SetPanelScrollPosition(HWND panel, int position) { SetPanelScrollInternal(panel, position); }

int ThemedUi::PanelScrollPosition(HWND panel) {
    auto it = PanelStates().find(panel);
    return it == PanelStates().end() ? 0 : it->second.scrollPosition;
}

void ThemedUi::EnsurePanelChildVisible(HWND panel, HWND child) {
    auto it = PanelStates().find(panel);
    if (it == PanelStates().end() || !child) return;
    auto childIt = std::find_if(it->second.children.begin(), it->second.children.end(), [child](const PanelChildRuntime& item) { return item.hwnd == child; });
    if (childIt == it->second.children.end()) return;
    RECT panelRect{};
    GetWindowRect(panel, &panelRect);
    MapWindowPoints(nullptr, GetParent(panel), reinterpret_cast<POINT*>(&panelRect), 2);
    RECT content = PanelContentRect(panel);
    const int top = childIt->original.top - (panelRect.top + content.top);
    const int bottom = childIt->original.bottom - (panelRect.top + content.top);
    if (top < it->second.scrollPosition) SetPanelScrollInternal(panel, top);
    else if (bottom > it->second.scrollPosition + (content.bottom - content.top)) {
        SetPanelScrollInternal(panel, bottom - (content.bottom - content.top));
    }
}

HWND ThemedUi::TabControl(
    int id,
    RECT frame,
    const std::vector<ThemedTabItem>& items,
    ThemedTabControlOptions options) const {
    HWND tab = BindTheme(ThemedControls::CreateTabControlFrame(instance_, parent_, id, frame, font_, theme_));
    if (!tab) return nullptr;
    EnableWindow(tab, options.enabled ? TRUE : FALSE);
    auto& runtime = TabControlStates()[tab];
    runtime.owner = parent_;
    runtime.controlId = id;
    runtime.theme = &theme_;
    runtime.pages.resize(items.size());
    runtime.pageRoots.resize(items.size());
    SetWindowSubclass(tab, TabContainerProc, 2, 0);

    const int width = frame.right - frame.left;
    const int padding = static_cast<int>(theme_.metric(L"tabControl", L"paddingX", 4.0f));
    const int gap = static_cast<int>(theme_.metric(L"tabControl", L"itemGap", 2.0f));
    const int tabHeight = buttonHeight(ThemedButtonRole::Tab, ThemedButtonSize::Normal);
    int equalWidth = 0;
    std::vector<int> itemWidths;
    itemWidths.reserve(items.size());
    if (options.equalWidth && !items.empty()) {
        equalWidth = std::max(24, (width - padding * 2 - gap * (static_cast<int>(items.size()) - 1)) / static_cast<int>(items.size()));
        itemWidths.assign(items.size(), equalWidth);
    } else {
        int totalWidth = 0;
        std::vector<int> minimumWidths;
        minimumWidths.reserve(items.size());
        const int textPadding = static_cast<int>(theme_.metric(L"tabButton", L"paddingX", 12.0f)) * 2;
        for (const auto& item : items) {
            const int desired = buttonWidth(item.text, ThemedButtonRole::Tab, ThemedButtonSize::Normal, ThemedButtonWidthMode::Text);
            const int minimum = std::max(24, textWidth(item.text) + textPadding);
            itemWidths.push_back(desired);
            minimumWidths.push_back(minimum);
            totalWidth += desired;
        }
        const int available = std::max(1, width - padding * 2 - gap * std::max(0, static_cast<int>(items.size()) - 1));
        int excess = std::max(0, totalWidth - available);
        while (excess > 0) {
            bool reduced = false;
            for (std::size_t i = 0; i < itemWidths.size() && excess > 0; ++i) {
                if (itemWidths[i] > minimumWidths[i]) {
                    --itemWidths[i];
                    --excess;
                    reduced = true;
                }
            }
            if (!reduced) break;
        }
    }
    ThemedUi nested(instance_, tab, theme_, font_, DialogLayoutKind::Compact, width, frame.bottom - frame.top);
    int x = padding;
    for (std::size_t index = 0; index < items.size(); ++index) {
        const auto& item = items[index];
        const int itemWidth = itemWidths[index];
        HWND button = nested.TabButton(item.id, item.text, x, padding, itemWidth, false);
        ThemedControls::SetControlBackgroundComponent(button, L"tabControl");
        EnableWindow(button, options.enabled && item.enabled ? TRUE : FALSE);
        SetWindowSubclass(button, TabButtonNavigationProc, 2, reinterpret_cast<DWORD_PTR>(tab));
        runtime.buttons.push_back(button);
        x += itemWidth + gap;
    }
    if (!items.empty()) ApplyActiveTab(tab, std::clamp(options.activeIndex, 0, static_cast<int>(items.size()) - 1), false);
    (void)tabHeight;
    return tab;
}

void ThemedUi::BindTabPage(HWND tabControl, int index, const std::vector<HWND>& children) {
    auto it = TabControlStates().find(tabControl);
    if (it == TabControlStates().end() || index < 0 || index >= static_cast<int>(it->second.pages.size())) return;
    it->second.pages[static_cast<std::size_t>(index)] = children;
    for (HWND child : children) {
        if (child) {
            ShowWindow(child, index == it->second.activeIndex ? SW_SHOWNA : SW_HIDE);
            SetWindowSubclass(child, TabPageNavigationProc, 4, reinterpret_cast<DWORD_PTR>(tabControl));
        }
    }
}

void ThemedUi::BindTabPageRoot(HWND tabControl, int index, HWND pageRoot) {
    auto it = TabControlStates().find(tabControl);
    if (it == TabControlStates().end() || index < 0 || index >= static_cast<int>(it->second.pageRoots.size())) return;
    it->second.pageRoots[static_cast<std::size_t>(index)] = pageRoot;
    BindTabPage(tabControl, index, pageRoot ? std::vector<HWND>{pageRoot} : std::vector<HWND>{});
}

bool ThemedUi::PreTranslateMessage(const MSG& message) {
    if (message.message != WM_KEYDOWN || message.wParam != VK_TAB || (GetKeyState(VK_CONTROL) & 0x8000) == 0) return false;
    HWND focus = message.hwnd ? message.hwnd : GetFocus();
    for (auto& pair : TabControlStates()) {
        auto& state = pair.second;
        for (HWND root : state.pageRoots) {
            if (root && (focus == root || IsChild(root, focus))) {
                const int direction = (GetKeyState(VK_SHIFT) & 0x8000) != 0 ? -1 : 1;
                return NavigateTabControl(pair.first, direction, false);
            }
        }
        for (const auto& page : state.pages) {
            for (HWND child : page) {
                if (child && (focus == child || IsChild(child, focus))) {
                    const int direction = (GetKeyState(VK_SHIFT) & 0x8000) != 0 ? -1 : 1;
                    return NavigateTabControl(pair.first, direction, false);
                }
            }
        }
    }
    return false;
}

void ThemedUi::SetActiveTab(HWND tabControl, int index, bool notify) { ApplyActiveTab(tabControl, index, notify); }

int ThemedUi::ActiveTab(HWND tabControl) {
    auto it = TabControlStates().find(tabControl);
    return it == TabControlStates().end() ? -1 : it->second.activeIndex;
}

void ThemedUi::SetTabEnabled(HWND tabControl, int index, bool enabled) {
    auto it = TabControlStates().find(tabControl);
    if (it == TabControlStates().end() || index < 0 || index >= static_cast<int>(it->second.buttons.size())) return;
    EnableWindow(it->second.buttons[static_cast<std::size_t>(index)], enabled ? TRUE : FALSE);
}

void ThemedUi::SetTabVisible(HWND tabControl, int index, bool visible) {
    auto it = TabControlStates().find(tabControl);
    if (it == TabControlStates().end() || index < 0 || index >= static_cast<int>(it->second.buttons.size())) return;
    ShowWindow(it->second.buttons[static_cast<std::size_t>(index)], visible ? SW_SHOWNA : SW_HIDE);
    if (!visible && it->second.activeIndex == index) {
        for (int next = 0; next < static_cast<int>(it->second.buttons.size()); ++next) {
            if (next != index && IsWindowVisible(it->second.buttons[static_cast<std::size_t>(next)])) {
                ApplyActiveTab(tabControl, next, true);
                break;
            }
        }
    }
}

HWND ThemedUi::ToolBar(
    int id,
    RECT frame,
    const std::vector<ThemedToolItem>& items,
    ThemedToolBarOptions options) const {
    HWND toolbar = BindTheme(ThemedControls::CreateToolBarFrame(instance_, parent_, id, frame, font_, theme_));
    if (!toolbar) return nullptr;
    EnableWindow(toolbar, options.enabled ? TRUE : FALSE);
    auto& runtime = ToolBarStates()[toolbar];
    runtime.owner = parent_;
    runtime.controlId = id;
    runtime.theme = &theme_;
    runtime.tooltipRegistry = tooltipRegistry_;
    runtime.instance = instance_;
    runtime.font = font_;
    runtime.enabled = options.enabled;
    runtime.compact = options.compact;
    runtime.automaticOverflow = options.automaticOverflow;
    SetWindowSubclass(toolbar, ToolBarProc, 2, 0);

    const int width = frame.right - frame.left;
    const int height = frame.bottom - frame.top;
    const int separatorWidth = static_cast<int>(theme_.metric(L"toolbar", L"separatorWidth", 9.0f));
    const ThemedButtonSize size = options.compact ? ThemedButtonSize::Compact : ThemedButtonSize::Normal;
    const int itemHeight = buttonHeight(ThemedButtonRole::Normal, size);
    const int y = std::max(0, (height - itemHeight) / 2);
    ThemedUi nested(instance_, toolbar, theme_, font_, DialogLayoutKind::Compact, width, height);
    for (const auto& item : items) {
        if (item.kind == ThemedToolItemKind::Separator) {
            HWND separator = ThemedControls::CreateSeparator(
                instance_, toolbar,
                RECT{0, y, separatorWidth, y + itemHeight},
                theme_, true);
            const int runtimeKey = -1 - static_cast<int>(runtime.order.size());
            runtime.order.push_back(runtimeKey);
            ToolRuntimeItem runtimeItem{};
            runtimeItem.separator = separator;
            runtimeItem.id = item.id;
            runtimeItem.kind = item.kind;
            runtimeItem.alignment = item.alignment;
            runtimeItem.width = separatorWidth;
            runtime.items[runtimeKey] = runtimeItem;
            continue;
        }
        ThemedToolItemDisplay display = item.display;
        if (display == ThemedToolItemDisplay::Automatic) {
            display = item.icon ? (item.text.empty() ? ThemedToolItemDisplay::IconOnly : ThemedToolItemDisplay::IconAndText)
                                : ThemedToolItemDisplay::TextOnly;
        }
        const int iconSize = static_cast<int>(theme_.metric(L"toolbarItem", L"iconSize", 16.0f));
        const int iconGap = static_cast<int>(theme_.metric(L"toolbarItem", L"iconGap", 6.0f));
        const int iconPadding = static_cast<int>(theme_.metric(L"toolbarItem", L"iconPaddingX", 8.0f));
        const std::wstring displayText = display == ThemedToolItemDisplay::IconOnly ? L"" : item.text;
        int itemWidth = nested.buttonWidth(
            displayText,
            item.kind == ThemedToolItemKind::Toggle ? ThemedButtonRole::Tab : ThemedButtonRole::Normal,
            size,
            ThemedButtonWidthMode::Text);
        if (display == ThemedToolItemDisplay::IconOnly) itemWidth = iconSize + iconPadding * 2;
        else if (display == ThemedToolItemDisplay::IconAndText) itemWidth += iconSize + iconGap;
        HWND child = item.kind == ThemedToolItemKind::Toggle
            ? nested.TabButton(item.id, displayText, 0, y, itemWidth, item.checked)
            : nested.Button(item.id, displayText, 0, y, ThemedButtonRole::Normal, size, ThemedButtonWidthMode::Fixed, itemWidth);
        if (item.icon && display != ThemedToolItemDisplay::TextOnly) {
            SendMessageW(child, BM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(item.icon));
        }
        ThemedControls::SetControlBackgroundComponent(child, L"toolbar");
        EnableWindow(child, options.enabled && item.enabled ? TRUE : FALSE);
        runtime.order.push_back(item.id);
        ToolRuntimeItem runtimeItem{};
        runtimeItem.hwnd = child;
        runtimeItem.id = item.id;
        runtimeItem.kind = item.kind;
        runtimeItem.alignment = item.alignment;
        runtimeItem.display = display;
        runtimeItem.text = item.text;
        runtimeItem.tooltip = item.tooltip;
        runtimeItem.icon = item.icon;
        runtimeItem.iconOwnership = item.iconOwnership;
        runtimeItem.width = itemWidth;
        runtime.items[item.id] = std::move(runtimeItem);
        SetWindowSubclass(child, ToolItemProc, 3, reinterpret_cast<DWORD_PTR>(toolbar));
    }
    runtime.overflowButton = nested.Button(
        kToolBarOverflowId, L"...", 0, y, ThemedButtonRole::Normal, size,
        ThemedButtonWidthMode::Fixed, itemHeight);
    ThemedControls::SetControlBackgroundComponent(runtime.overflowButton, L"toolbar");
    ShowWindow(runtime.overflowButton, SW_HIDE);
    LayoutToolBar(toolbar);
    return toolbar;
}

void ThemedUi::SetToolEnabled(HWND toolbar, int itemId, bool enabled) {
    auto it = ToolBarStates().find(toolbar);
    if (it == ToolBarStates().end()) return;
    auto item = it->second.items.find(itemId);
    if (item != it->second.items.end()) EnableWindow(item->second.hwnd, enabled ? TRUE : FALSE);
}

void ThemedUi::SetToolChecked(HWND toolbar, int itemId, bool checked) {
    auto it = ToolBarStates().find(toolbar);
    if (it == ToolBarStates().end()) return;
    auto item = it->second.items.find(itemId);
    if (item != it->second.items.end()) SetChecked(item->second.hwnd, checked);
}

bool ThemedUi::IsToolChecked(HWND toolbar, int itemId) {
    auto it = ToolBarStates().find(toolbar);
    if (it == ToolBarStates().end()) return false;
    auto item = it->second.items.find(itemId);
    return item != it->second.items.end() && IsChecked(item->second.hwnd);
}

void ThemedUi::SetToolVisible(HWND toolbar, int itemId, bool visible) {
    auto it = ToolBarStates().find(toolbar);
    if (it == ToolBarStates().end()) return;
    auto item = it->second.items.find(itemId);
    if (item != it->second.items.end()) {
        item->second.requestedVisible = visible;
        LayoutToolBar(toolbar);
    }
}

bool ThemedUi::HasTool(HWND toolbar, int itemId) {
    auto it = ToolBarStates().find(toolbar);
    return it != ToolBarStates().end() && it->second.items.find(itemId) != it->second.items.end();
}

bool ThemedUi::IsToolEnabled(HWND toolbar, int itemId) {
    auto it = ToolBarStates().find(toolbar);
    if (it == ToolBarStates().end()) return false;
    auto item = it->second.items.find(itemId);
    return item != it->second.items.end() && item->second.hwnd && IsWindowEnabled(item->second.hwnd);
}

bool ThemedUi::IsToolVisible(HWND toolbar, int itemId) {
    auto it = ToolBarStates().find(toolbar);
    if (it == ToolBarStates().end()) return false;
    auto item = it->second.items.find(itemId);
    return item != it->second.items.end() && item->second.requestedVisible;
}

bool ThemedUi::IsToolOverflowed(HWND toolbar, int itemId) {
    auto it = ToolBarStates().find(toolbar);
    if (it == ToolBarStates().end()) return false;
    auto item = it->second.items.find(itemId);
    return item != it->second.items.end() && item->second.overflowed;
}

int ThemedUi::ToolCount(HWND toolbar) {
    auto it = ToolBarStates().find(toolbar);
    if (it == ToolBarStates().end()) return 0;
    return static_cast<int>(std::count_if(it->second.order.begin(), it->second.order.end(), [&it](int key) {
        return it->second.items.at(key).kind != ThemedToolItemKind::Separator;
    }));
}

int ThemedUi::ToolIndex(HWND toolbar, int itemId) {
    auto it = ToolBarStates().find(toolbar);
    if (it == ToolBarStates().end()) return -1;
    auto position = std::find(it->second.order.begin(), it->second.order.end(), itemId);
    return position == it->second.order.end() ? -1 : static_cast<int>(position - it->second.order.begin());
}

void ThemedUi::SetToolText(HWND toolbar, int itemId, const std::wstring& text) {
    auto it = ToolBarStates().find(toolbar); if (it == ToolBarStates().end()) return;
    auto item = it->second.items.find(itemId); if (item == it->second.items.end()) return;
    item->second.text = text; RecalculateToolWidth(toolbar, item->second); LayoutToolBar(toolbar);
}

void ThemedUi::SetToolTooltip(HWND toolbar, int itemId, const std::wstring& tooltip) {
    auto it = ToolBarStates().find(toolbar); if (it == ToolBarStates().end()) return;
    auto item = it->second.items.find(itemId); if (item != it->second.items.end()) item->second.tooltip = tooltip;
}

void ThemedUi::SetToolIcon(HWND toolbar, int itemId, HICON icon, ThemedIconOwnership ownership) {
    auto it = ToolBarStates().find(toolbar); if (it == ToolBarStates().end()) return;
    auto item = it->second.items.find(itemId); if (item == it->second.items.end()) return;
    if (item->second.icon && item->second.iconOwnership == ThemedIconOwnership::Transfer) DestroyIcon(item->second.icon);
    item->second.icon = icon; item->second.iconOwnership = ownership;
    if (item->second.hwnd) SendMessageW(item->second.hwnd, BM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(icon));
    RecalculateToolWidth(toolbar, item->second); LayoutToolBar(toolbar);
}

void ThemedUi::SetToolDisplay(HWND toolbar, int itemId, ThemedToolItemDisplay display) {
    auto it = ToolBarStates().find(toolbar); if (it == ToolBarStates().end()) return;
    auto item = it->second.items.find(itemId); if (item == it->second.items.end()) return;
    item->second.display = display == ThemedToolItemDisplay::Automatic
        ? (item->second.icon ? (item->second.text.empty() ? ThemedToolItemDisplay::IconOnly : ThemedToolItemDisplay::IconAndText) : ThemedToolItemDisplay::TextOnly)
        : display;
    RecalculateToolWidth(toolbar, item->second); LayoutToolBar(toolbar);
}

void ThemedUi::SetToolAlignment(HWND toolbar, int itemId, ThemedToolItemAlignment alignment) {
    auto it = ToolBarStates().find(toolbar); if (it == ToolBarStates().end()) return;
    auto item = it->second.items.find(itemId); if (item == it->second.items.end()) return;
    item->second.alignment = alignment; LayoutToolBar(toolbar);
}

bool ThemedUi::RemoveTool(HWND toolbar, int itemId) {
    auto it = ToolBarStates().find(toolbar); if (it == ToolBarStates().end()) return false;
    auto item = it->second.items.find(itemId); if (item == it->second.items.end()) return false;
    if (item->second.icon && item->second.iconOwnership == ThemedIconOwnership::Transfer) DestroyIcon(item->second.icon);
    if (item->second.hwnd) DestroyWindow(item->second.hwnd);
    if (item->second.separator) DestroyWindow(item->second.separator);
    it->second.order.erase(std::remove(it->second.order.begin(), it->second.order.end(), itemId), it->second.order.end());
    it->second.items.erase(item); LayoutToolBar(toolbar); return true;
}

bool ThemedUi::InsertTool(HWND toolbar, int index, ThemedToolItem item) { return InsertRuntimeTool(toolbar, index, item); }

bool ThemedUi::MoveTool(HWND toolbar, int itemId, int targetIndex) {
    auto it = ToolBarStates().find(toolbar); if (it == ToolBarStates().end()) return false;
    auto position = std::find(it->second.order.begin(), it->second.order.end(), itemId);
    if (position == it->second.order.end()) return false;
    int key = *position; it->second.order.erase(position);
    targetIndex = std::clamp(targetIndex, 0, static_cast<int>(it->second.order.size()));
    it->second.order.insert(it->second.order.begin() + targetIndex, key); LayoutToolBar(toolbar); return true;
}

void ThemedUi::ClearTools(HWND toolbar) {
    auto it = ToolBarStates().find(toolbar); if (it == ToolBarStates().end()) return;
    BeginToolBarUpdate(toolbar);
    const auto keys = it->second.order;
    for (int key : keys) {
        auto item = it->second.items.find(key);
        if (item == it->second.items.end()) continue;
        if (item->second.icon && item->second.iconOwnership == ThemedIconOwnership::Transfer) DestroyIcon(item->second.icon);
        if (item->second.hwnd) DestroyWindow(item->second.hwnd);
        if (item->second.separator) DestroyWindow(item->second.separator);
    }
    it->second.items.clear(); it->second.order.clear(); it->second.layoutPending = true;
    EndToolBarUpdate(toolbar);
}

void ThemedUi::SetTools(HWND toolbar, const std::vector<ThemedToolItem>& items) {
    BeginToolBarUpdate(toolbar); ClearTools(toolbar);
    for (int index = 0; index < static_cast<int>(items.size()); ++index) InsertRuntimeTool(toolbar, index, items[static_cast<std::size_t>(index)]);
    EndToolBarUpdate(toolbar);
}

void ThemedUi::BeginToolBarUpdate(HWND toolbar) {
    auto it = ToolBarStates().find(toolbar); if (it != ToolBarStates().end()) ++it->second.updateDepth;
}

void ThemedUi::EndToolBarUpdate(HWND toolbar) {
    auto it = ToolBarStates().find(toolbar); if (it == ToolBarStates().end() || it->second.updateDepth <= 0) return;
    if (--it->second.updateDepth == 0 && it->second.layoutPending) { it->second.layoutPending = false; LayoutToolBar(toolbar); }
}

// 使用统一按钮规格创建主题按钮，并绑定当前主题。
// 参数：
// - id：子控件 ID，用于 WM_COMMAND/WM_DRAWITEM。
// - text：按钮标题。
// - x：左侧 x 坐标。
// - y：顶部 y 坐标。
// - role：按钮语义角色，决定使用普通、主操作、迷你或标签按钮。
// - size：按钮尺寸规格，决定按钮高度和默认尺寸倾向。
// - widthMode：宽度策略，决定使用固定宽度还是按文本自适应宽度。
// - fixedWidth：固定宽度，或 Text 模式下的最小宽度；小于等于 0 时使用默认值。
// - defaultButton：为 true 时创建默认按钮样式；对标签/迷你按钮无特殊效果。
// - selected：标签按钮的初始选中状态；其它按钮角色忽略此参数。
// 返回值：创建成功时返回按钮 HWND，失败时返回 nullptr。
HWND ThemedUi::Button(
    int id,
    const std::wstring& text,
    int x,
    int y,
    ThemedButtonRole role,
    ThemedButtonSize size,
    ThemedButtonWidthMode widthMode,
    int fixedWidth,
    bool defaultButton,
    bool selected) const {
    const int width = buttonWidth(text, role, size, widthMode, fixedWidth);
    const int height = buttonHeight(role, size);
    switch (role) {
    case ThemedButtonRole::Primary:
        return PrimaryButton(id, text, x, y, width, height, defaultButton);
    case ThemedButtonRole::Mini:
        return BindTheme(ThemedControls::CreateMiniButton(instance_, parent_, id, text.c_str(), x, y, width, height, font_));
    case ThemedButtonRole::Tab:
        return BindTheme(ThemedControls::CreateTabButton(instance_, parent_, id, text.c_str(), x, y, width, height, font_, selected));
    case ThemedButtonRole::Normal:
    default:
        return NormalButton(id, text, x, y, width, height, defaultButton);
    }
}

// 创建主题普通按钮，并绑定当前主题（内部 helper，高度由模板决定）。
// 参数：
// - id：子控件 ID，用于 WM_COMMAND/WM_DRAWITEM。
// - text：按钮标题。
// - x：左侧 x 坐标。
// - y：顶部 y 坐标。
// - width：按钮宽度，单位为像素。
// - height：按钮高度，由 (role, size) 模板解析得出。
// - defaultButton：为 true 时创建默认按钮样式。
// 返回值：创建成功时返回按钮 HWND，失败时返回 nullptr。
HWND ThemedUi::NormalButton(int id, const std::wstring& text, int x, int y, int width, int height, bool defaultButton) const {
    return BindTheme(ThemedControls::CreateButton(instance_, parent_, id, text.c_str(), x, y, width, height, font_, defaultButton));
}

// 创建主题主操作按钮，并绑定当前主题。
// 参数：
// - id：子控件 ID，用于 WM_COMMAND/WM_DRAWITEM。
// - text：按钮标题。
// - x：左侧 x 坐标。
// - y：顶部 y 坐标。
// - width：按钮宽度，单位为像素。
// - height：按钮高度，单位为像素。
// - defaultButton：为 true 时创建默认按钮样式。
// 返回值：创建成功时返回主操作按钮 HWND，失败时返回 nullptr。
HWND ThemedUi::PrimaryButton(int id, const std::wstring& text, int x, int y, int width, int height, bool defaultButton) const {
    return BindTheme(ThemedControls::CreatePrimaryButton(instance_, parent_, id, text.c_str(), x, y, width, height, font_, defaultButton));
}

// 使用共享底部布局参数创建主题底部按钮。
// 参数：
// - id：子控件 ID，用于 WM_COMMAND/WM_DRAWITEM。
// - text：按钮标题。
// - buttonIndex：按钮在底部按钮组中的从 0 开始的索引。
// - buttonCount：底部按钮组中的按钮总数。
// - primary：为 true 时使用主操作按钮视觉分类。
// - defaultButton：为 true 时创建默认按钮样式。
// 返回值：创建成功时返回底部按钮 HWND，失败时返回 nullptr。
HWND ThemedUi::FooterButton(int id, const std::wstring& text, int buttonIndex, int buttonCount, bool primary, bool defaultButton) const {
    const ThemedButtonRole role = primary ? ThemedButtonRole::Primary : ThemedButtonRole::Normal;
    const int height = buttonHeight(role, ThemedButtonSize::Normal);
    const int x = footerButtonX(buttonIndex, buttonCount);
    const int y = footerButtonY(height);
    return Button(
        id,
        text,
        x,
        y,
        role,
        ThemedButtonSize::Normal,
        ThemedButtonWidthMode::Fixed,
        layout_.footerButtonWidth,
        defaultButton);
}

// 创建主题复选框，并绑定当前主题。
// 参数：
// - id：子控件 ID，用于 WM_COMMAND/WM_DRAWITEM。
// - text：复选框标签文本。
// - x：左侧 x 坐标。
// - y：顶部 y 坐标。
// - width：复选框控件宽度，单位为像素。
// - height：复选框控件高度，单位为像素。
// - checked：初始勾选状态。
// 返回值：创建成功时返回复选框 HWND，失败时返回 nullptr。
HWND ThemedUi::CheckBox(int id, const std::wstring& text, int x, int y, int width, ThemedCheckBoxOptions options) const {
    const bool multiline = options.size == ThemedCheckBoxSize::TwoLines;
    HWND hwnd = BindTheme(ThemedControls::CreateCheckBox(
        instance_, parent_, id, text.c_str(), x, y, width,
        ThemedControls::CheckBoxHeight(theme_) * (multiline ? 2 : 1), font_, options.checked));
    if (hwnd) {
        EnableWindow(hwnd, options.enabled ? TRUE : FALSE);
        ThemedControls::SetControlMultiline(hwnd, multiline);
    }
    return hwnd;
}

void ThemedUi::SetChecked(HWND hwnd, bool checked) {
    if (hwnd) {
        SendMessageW(hwnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    }
}

bool ThemedUi::IsChecked(HWND hwnd) {
    return hwnd && SendMessageW(hwnd, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

HWND ThemedUi::Toggle(int id, const std::wstring& text, int x, int y, int width, ThemedToggleOptions options) const {
    HWND hwnd = BindTheme(ThemedControls::CreateToggle(
        instance_, parent_, id, text.c_str(), x, y, width, font_, theme_, options.checked));
    if (hwnd) {
        EnableWindow(hwnd, options.enabled ? TRUE : FALSE);
    }
    return hwnd;
}

HWND ThemedUi::RadioButton(int id, const std::wstring& text, int x, int y, int width, ThemedRadioButtonOptions options) const {
    HWND hwnd = BindTheme(ThemedControls::CreateRadioButton(
        instance_, parent_, id, text.c_str(), x, y, width, font_, theme_, options.group, options.checked));
    if (hwnd) {
        EnableWindow(hwnd, options.enabled ? TRUE : FALSE);
    }
    return hwnd;
}

HWND ThemedUi::HotKeyCapture(int id, const std::wstring& text, int x, int y, int width, ThemedHotKeyCaptureOptions options) const {
    HWND hwnd = BindTheme(ThemedControls::CreateHotKeyCapture(
        instance_, parent_, id, text.c_str(), x, y, width, font_, theme_));
    if (hwnd) {
        EnableWindow(hwnd, options.enabled ? TRUE : FALSE);
    }
    return hwnd;
}

HWND ThemedUi::LinkText(int id, const std::wstring& text, int x, int y, int width, ThemedLinkOptions options) const {
    DWORD style = BS_LEFT;
    if (options.align == ThemedTextAlign::Center) style = BS_CENTER;
    else if (options.align == ThemedTextAlign::End) style = BS_RIGHT;
    HWND hwnd = BindTheme(ThemedControls::CreateLinkText(
        instance_, parent_, id, text.c_str(), x, y, width, labelHeight(), font_, theme_,
        LinkState(options.role), options.visited, style));
    if (hwnd) EnableWindow(hwnd, options.enabled ? TRUE : FALSE);
    return hwnd;
}

void ThemedUi::SetLinkRole(HWND hwnd, ThemedLinkRole role) {
    ThemedControls::SetLinkRole(hwnd, LinkState(role));
}

void ThemedUi::SetLinkVisited(HWND hwnd, bool visited) {
    ThemedControls::SetLinkVisited(hwnd, visited);
}

// 创建主题标签/分段按钮，并绑定当前主题。
// 参数：
// - id：子控件 ID，用于 WM_COMMAND/WM_DRAWITEM。
// - text：按钮标题。
// - x：左侧 x 坐标。
// - y：顶部 y 坐标。
// - width：按钮宽度，单位为像素。
// - selected：初始选中状态。
// 返回值：创建成功时返回标签按钮 HWND，失败时返回 nullptr。
HWND ThemedUi::TabButton(int id, const std::wstring& text, int x, int y, int width, bool selected) const {
    const int height = buttonHeight(ThemedButtonRole::Tab, ThemedButtonSize::Normal);
    return BindTheme(ThemedControls::CreateTabButton(instance_, parent_, id, text.c_str(), x, y, width, height, font_, selected));
}

void ThemedUi::SetTabSelected(HWND hwnd, bool selected) {
    ThemedControls::SetTabButtonSelected(hwnd, selected);
}

bool ThemedUi::IsTabSelected(HWND hwnd) {
    return ThemedControls::IsTabButtonSelected(hwnd);
}

// 创建主题下拉框。
// 参数：
// - id：子控件 ID，用于 WM_COMMAND/WM_DRAWITEM。
// - x：左侧 x 坐标。
// - y：顶部 y 坐标。
// - width：下拉框宽度，单位为像素。
// - height：下拉框高度，单位为像素。
// 返回值：创建成功时返回下拉框 HWND，失败时返回 nullptr。
HWND ThemedUi::ComboBox(int id, int x, int y, int width, ThemedComboBoxOptions options) const {
    HWND hwnd = ThemedControls::CreateComboBox(
        instance_, parent_, id, x, y, width, ThemedControls::ComboBoxDropdownHeight(theme_), font_, theme_);
    if (hwnd) {
        EnableWindow(hwnd, options.enabled ? TRUE : FALSE);
    }
    return hwnd;
}

// 创建主题列表框。
// 参数：
// - id：子控件 ID，用于 WM_COMMAND/WM_DRAWITEM。
// - x：左侧 x 坐标。
// - y：顶部 y 坐标。
// - width：列表框宽度，单位为像素。
// - height：列表框高度，单位为像素。
// - extraStyle：附加 Win32 列表框样式标志。
// 返回值：创建成功时返回列表框 HWND，失败时返回 nullptr。
HWND ThemedUi::ListBox(int id, int x, int y, int width, int height, ThemedListBoxOptions options) const {
    DWORD style = LBS_HASSTRINGS;
    if (options.notify) {
        style |= LBS_NOTIFY;
    }
    if (options.selection == ThemedListSelection::Multiple) {
        style |= LBS_EXTENDEDSEL;
    }
    if (options.scroll == ThemedListScroll::Vertical || options.scroll == ThemedListScroll::Both) {
        style |= WS_VSCROLL;
    }
    if (options.scroll == ThemedListScroll::Both) {
        style |= WS_HSCROLL;
    }
    HWND hwnd = ThemedControls::CreateListBox(instance_, parent_, id, x, y, width, height, font_, theme_, style);
    if (hwnd) {
        EnableWindow(hwnd, options.enabled ? TRUE : FALSE);
    }
    return hwnd;
}

HWND ThemedUi::Table(int id, RECT frame, const std::vector<ThemedTableColumn>& columns, ThemedTableOptions options) const {
    const RECT inner = ThemedControls::ListFrameInnerRect(theme_, frame);
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_SHOWSELALWAYS | LVS_SHAREIMAGELISTS;
    style |= options.view == ThemedTableView::Details ? LVS_REPORT : LVS_ICON;
    if (options.selection == ThemedTableSelection::Single) style |= LVS_SINGLESEL;
    if (!options.showHeader) style |= LVS_NOCOLUMNHEADER;
    if (!options.allowColumnResize) style |= LVS_NOSORTHEADER;
    HWND table = CreateWindowExW(
        0, WC_LISTVIEWW, L"", style,
        inner.left, inner.top, inner.right - inner.left, inner.bottom - inner.top,
        parent_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    if (!table) return nullptr;

    SendMessageW(table, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    ThemedControls::RegisterTable(table, theme_);
    DWORD extended = LVS_EX_DOUBLEBUFFER;
    if (options.checkable) extended |= LVS_EX_CHECKBOXES;
    if (options.fullRowSelect) extended |= LVS_EX_FULLROWSELECT;
    ListView_SetExtendedListViewStyle(table, extended);
    EnableWindow(table, options.enabled ? TRUE : FALSE);

    int fixedWidth = 0;
    int remainingCount = 0;
    std::vector<int> widthModes;
    widthModes.reserve(columns.size());
    for (const auto& column : columns) {
        if (column.widthMode == ThemedTableColumnWidth::Remaining) ++remainingCount;
        else fixedWidth += std::max(0, column.fixedWidth);
        widthModes.push_back(static_cast<int>(column.widthMode));
    }
    const int available = std::max(40, static_cast<int>(inner.right - inner.left) - fixedWidth - 4);
    for (std::size_t i = 0; i < columns.size(); ++i) {
        const auto& source = columns[i];
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_FMT;
        column.pszText = const_cast<wchar_t*>(source.title.c_str());
        column.iSubItem = static_cast<int>(i);
        column.cx = source.widthMode == ThemedTableColumnWidth::Remaining
            ? available / std::max(1, remainingCount)
            : std::max(24, source.fixedWidth);
        column.fmt = source.align == ThemedTableColumnAlign::Center ? LVCFMT_CENTER
            : (source.align == ThemedTableColumnAlign::End ? LVCFMT_RIGHT : LVCFMT_LEFT);
        ListView_InsertColumn(table, static_cast<int>(i), &column);
    }
    ThemedControls::ConfigureTableColumns(table, widthModes);
    if (tableFrameRegistry_) tableFrameRegistry_->RegisterTableFrame(table, frame);
    return table;
}

void ThemedUi::SetTableRows(HWND table, const std::vector<ThemedTableRow>& rows) {
    if (!table) return;
    ListView_DeleteAllItems(table);
    std::vector<bool> enabledStates;
    enabledStates.reserve(rows.size());
    for (const auto& row : rows) enabledStates.push_back(row.enabled);
    ThemedControls::SetTableRowEnabledStates(table, enabledStates);
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& source = rows[index];
        const std::wstring first = source.cells.empty() ? L"" : source.cells.front().text;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        if (!source.cells.empty() && source.cells.front().image >= 0) {
            item.mask |= LVIF_IMAGE;
            item.iImage = source.cells.front().image;
        }
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(first.c_str());
        item.lParam = static_cast<LPARAM>(source.key);
        ListView_InsertItem(table, &item);
        for (std::size_t cell = 1; cell < source.cells.size(); ++cell) {
            ListView_SetItemText(table, static_cast<int>(index), static_cast<int>(cell), const_cast<wchar_t*>(source.cells[cell].text.c_str()));
        }
        ListView_SetCheckState(table, static_cast<int>(index), source.checked && source.enabled ? TRUE : FALSE);
    }
    const int columnCount = Header_GetItemCount(ListView_GetHeader(table));
    for (int column = 0; column < columnCount; ++column) {
        LVCOLUMNW info{};
        info.mask = LVCF_WIDTH;
        if (ListView_GetColumn(table, column, &info) && info.cx <= 24) {
            ListView_SetColumnWidth(table, column, LVSCW_AUTOSIZE_USEHEADER);
        }
    }
}

void ThemedUi::SetTableView(HWND table, ThemedTableView view) {
    if (!table) return;
    DWORD_PTR style = static_cast<DWORD_PTR>(GetWindowLongPtrW(table, GWL_STYLE));
    style &= ~LVS_TYPEMASK;
    style |= view == ThemedTableView::Details ? LVS_REPORT : LVS_ICON;
    SetWindowLongPtrW(table, GWL_STYLE, static_cast<LONG_PTR>(style));
    ListView_SetView(table, view == ThemedTableView::Details ? LV_VIEW_DETAILS : LV_VIEW_ICON);
    SetWindowPos(table, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(table, nullptr, TRUE);
}

void ThemedUi::SetTableChecked(HWND table, int index, bool checked) {
    if (table && IsTableRowEnabled(table, index)) ListView_SetCheckState(table, index, checked ? TRUE : FALSE);
}
bool ThemedUi::IsTableChecked(HWND table, int index) { return table && ListView_GetCheckState(table, index) != FALSE; }
bool ThemedUi::IsTableRowEnabled(HWND table, int index) { return ThemedControls::IsTableRowEnabled(table, index); }
int ThemedUi::TableRowCount(HWND table) { return table ? ListView_GetItemCount(table) : 0; }
int ThemedUi::TableSelectedIndex(HWND table) { return table ? ListView_GetNextItem(table, -1, LVNI_SELECTED) : -1; }
std::intptr_t ThemedUi::TableRowKey(HWND table, int index) {
    if (!table || index < 0) return 0;
    LVITEMW item{};
    item.mask = LVIF_PARAM;
    item.iItem = index;
    return ListView_GetItem(table, &item) ? static_cast<std::intptr_t>(item.lParam) : 0;
}
int ThemedUi::TableHitTest(HWND table, POINT point, bool fullRow, bool* stateIcon) {
    if (!table) return -1;
    LVHITTESTINFO hit{};
    hit.pt = point;
    int index = ListView_SubItemHitTest(table, &hit);
    if (stateIcon) *stateIcon = (hit.flags & LVHT_ONITEMSTATEICON) != 0;
    if (index >= 0 || !fullRow) return index;
    RECT client{};
    GetClientRect(table, &client);
    if (!PtInRect(&client, point)) return -1;
    const int count = ListView_GetItemCount(table);
    const int top = std::max(0, ListView_GetTopIndex(table));
    const int bottom = std::min(count, top + ListView_GetCountPerPage(table) + 1);
    for (int rowIndex = top; rowIndex < bottom; ++rowIndex) {
        RECT row{};
        if (!ListView_GetItemRect(table, rowIndex, &row, LVIR_BOUNDS)) continue;
        row.left = client.left;
        row.right = client.right;
        if (PtInRect(&row, point)) return rowIndex;
    }
    return -1;
}
void ThemedUi::SetTableIconSpacing(HWND table, int x, int y) { if (table) ListView_SetIconSpacing(table, x, y); }
void ThemedUi::ClearTable(HWND table) { if (table) ListView_DeleteAllItems(table); }
void ThemedUi::SetTableImageLists(HWND table, HIMAGELIST smallImages, HIMAGELIST largeImages) {
    if (!table) return;
    ListView_SetImageList(table, smallImages, LVSIL_SMALL);
    ListView_SetImageList(table, largeImages, LVSIL_NORMAL);
}

bool ThemedUi::DecodeTableEvent(HWND table, LPARAM lParam, ThemedTableEvent& event) {
    auto* header = reinterpret_cast<NMHDR*>(lParam);
    if (!table || !header || header->hwndFrom != table) return false;
    if (header->code == NM_CLICK || header->code == NM_DBLCLK) {
        auto* activate = reinterpret_cast<NMITEMACTIVATE*>(lParam);
        event.kind = header->code == NM_DBLCLK ? ThemedTableEventKind::Activated : ThemedTableEventKind::Click;
        event.row = activate->iItem;
        event.column = activate->iSubItem;
        event.point = activate->ptAction;
        event.rowKey = TableRowKey(table, event.row);
        return true;
    }
    if (header->code == LVN_COLUMNCLICK) {
        auto* view = reinterpret_cast<NMLISTVIEW*>(lParam);
        event.kind = ThemedTableEventKind::SortRequested;
        event.column = view->iSubItem;
        return true;
    }
    if (header->code == LVN_ITEMCHANGED) {
        auto* changed = reinterpret_cast<NMLISTVIEW*>(lParam);
        if (changed->iItem < 0 || (changed->uChanged & LVIF_STATE) == 0) return false;
        const UINT oldCheck = changed->uOldState & LVIS_STATEIMAGEMASK;
        const UINT newCheck = changed->uNewState & LVIS_STATEIMAGEMASK;
        event.row = changed->iItem;
        event.rowKey = TableRowKey(table, event.row);
        if (oldCheck != newCheck) {
            event.kind = ThemedTableEventKind::CheckChanged;
            event.checked = IsTableChecked(table, event.row);
        } else {
            event.kind = ThemedTableEventKind::SelectionChanged;
        }
        return true;
    }
    return false;
}

void ThemedUi::ShowTooltip(const std::wstring& text, POINT screenPoint, ThemedTooltipOptions options) const {
    if (tooltipRegistry_) tooltipRegistry_->ShowTooltip(text, screenPoint, options);
}

void ThemedUi::HideTooltip() const {
    if (tooltipRegistry_) tooltipRegistry_->HideTooltip();
}

HWND ThemedUi::Edit(int id, RECT frame, const std::wstring& value, ThemedEditOptions options) const {
    const bool multiline = options.mode == ThemedEditMode::MultiLine;
    DWORD style = multiline
        ? ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | WS_CLIPSIBLINGS
        : ES_AUTOHSCROLL;
    if (multiline && options.acceptsReturn) {
        style |= ES_WANTRETURN;
    }
    switch (options.content) {
    case ThemedEditContent::Integer:
        style |= ES_NUMBER;
        break;
    case ThemedEditContent::Password:
        style |= ES_PASSWORD;
        break;
    case ThemedEditContent::Text:
    default:
        break;
    }
    if (options.readOnly) {
        style |= ES_READONLY;
    }

    HWND hwnd = multiline
        ? ThemedControls::CreateMultiLineEdit(instance_, parent_, id, theme_, frame, value, font_, style)
        : ThemedControls::CreateSingleLineEdit(instance_, parent_, id, theme_, frame, value, font_, style);
    if (hwnd) {
        EnableWindow(hwnd, options.enabled ? TRUE : FALSE);
        ThemedControls::ConfigureEditBehavior(hwnd, options.selectAllOnFocus);
        if (options.maxLength > 0) {
            SendMessageW(hwnd, EM_SETLIMITTEXT, options.maxLength, 0);
        }
        if (!options.placeholder.empty()) {
            SendMessageW(hwnd, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(options.placeholder.c_str()));
        }
    }
    if (editFrameRegistry_ && hwnd) {
        editFrameRegistry_->RegisterEditFrame(hwnd, frame, options);
    }
    return hwnd;
}

// 在字段框内部创建主题静态文本控件。
// 参数：
// - value：静态文本内容。
// - frame：外层字段框矩形；静态文本会按主题参数内缩。
// - style：Win32 静态控件样式标志。
// 返回值：创建成功时返回静态文本 HWND，失败时返回 nullptr。
HWND ThemedUi::FramedStatic(const std::wstring& value, RECT frame, ThemedFramedTextOptions options) const {
    DWORD style = StaticStyle(options.align);
    if (!options.wrap && options.align == ThemedTextAlign::Start) {
        style = SS_LEFTNOWORDWRAP;
    }
    return ThemedControls::CreateFramedStatic(instance_, parent_, theme_, frame, value, font_, style);
}

// 创建主题进度条。
// 参数：
// - id：子控件 ID，用于 WM_DRAWITEM。
// - x：左侧 x 坐标。
// - y：顶部 y 坐标。
// - width：进度条宽度，单位为像素。
// - height：进度条高度，单位为像素；小于等于 0 时使用主题默认高度。
// 返回值：创建成功时返回进度条 HWND，失败时返回 nullptr。
HWND ThemedUi::ProgressBar(int id, int x, int y, int width, ThemedProgressBarOptions options) const {
    HWND hwnd = ThemedControls::CreateProgressBar(
        instance_,
        parent_,
        id,
        theme_,
        x,
        y,
        width,
        progressBarHeight());
    if (hwnd) {
        EnableWindow(hwnd, options.enabled ? TRUE : FALSE);
        ThemedControls::SetProgressBarValue(hwnd, options.value, options.indeterminate);
    }
    return hwnd;
}

void ThemedUi::SetProgress(HWND hwnd, double value, bool indeterminate) {
    ThemedControls::SetProgressBarValue(hwnd, value, indeterminate);
}

HWND ThemedUi::Slider(int id, int x, int y, int width, ThemedSliderOptions options) const {
    HWND hwnd = ThemedControls::CreateSlider(
        instance_, parent_, id, theme_, x, y, width,
        options.minimum, options.maximum, options.step, options.value);
    if (hwnd) {
        EnableWindow(hwnd, options.enabled ? TRUE : FALSE);
    }
    return hwnd;
}

void ThemedUi::SetSliderValue(HWND hwnd, double value, bool notify) {
    ThemedControls::SetSliderValue(hwnd, value, notify);
}

double ThemedUi::SliderValue(HWND hwnd) {
    return ThemedControls::SliderValue(hwnd);
}

bool ThemedUi::IsNativeEditShortcut(const MSG& message) {
    if (message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) {
        return false;
    }
    HWND target = message.hwnd;
    if (!target || target != GetFocus()) {
        return false;
    }
    wchar_t className[64]{};
    if (!GetClassNameW(target, className, static_cast<int>(sizeof(className) / sizeof(className[0])))) {
        return false;
    }
    const bool edit = _wcsicmp(className, L"Edit") == 0;
    const bool richEdit = _wcsnicmp(className, L"RichEdit", 8) == 0;
    if (!edit && !richEdit) {
        return false;
    }

    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (!ctrl || alt) {
        return false;
    }
    switch (message.wParam) {
    case 'A':
    case 'C':
    case 'V':
    case 'X':
    case 'Y':
    case 'Z':
    case VK_INSERT:
        return true;
    default:
        return false;
    }
}

// 处理主题 owner-draw 控件需要的父窗口消息。
// 参数：
// - theme：当前主题，用于绘制控件。
// - message：父窗口 WndProc 收到的窗口消息。
// - wParam：原始消息 wParam；当前保留给后续扩展处理。
// - lParam：原始消息 lParam，例如 DRAWITEMSTRUCT* 或 NMHDR*。
// - result：输出参数，接收父窗口 WndProc 应返回的 LRESULT。
// 返回值：消息已处理且 result 有效时返回 true，否则返回 false。
bool ThemedUi::HandleParentMessage(const Theme& theme, UINT message, WPARAM, LPARAM lParam, LRESULT& result) {
    switch (message) {
    case WM_DRAWITEM:
        if (ThemedControls::Draw(theme, reinterpret_cast<const DRAWITEMSTRUCT*>(lParam))) {
            result = TRUE;
            return true;
        }
        break;
    case WM_NOTIFY:
        if (ThemedControls::HandleListViewCustomDraw(theme, lParam, result)) {
            return true;
        }
        break;
    default:
        break;
    }
    return false;
}

// 将已创建的控件绑定到当前门面对象持有的主题。
// 参数：
// - hwnd：需要绑定主题的控件窗口句柄；允许传入 nullptr。
// 返回值：原样返回传入的 HWND，便于创建函数直接内联返回。
HWND ThemedUi::BindTheme(HWND hwnd) const {
    ThemedControls::SetControlTheme(hwnd, theme_);
    return hwnd;
}
