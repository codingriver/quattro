#include "ThemedControls.h"
#include "ThemedUi.h"
#include "Theme.h"

#include <commctrl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr int kWidth = 980;
constexpr int kHeight = 1120;

COLORREF ToColorRef(Color color) {
    const auto byte = [](float value) -> BYTE {
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        return static_cast<BYTE>(value * 255.0f + 0.5f);
    };
    return RGB(byte(color.r), byte(color.g), byte(color.b));
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

void DrawLabel(HDC dc, HFONT font, const std::wstring& text, RECT rect, COLORREF color) {
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

void DrawThemedControl(const Theme& theme, HDC dc, HWND hwnd, UINT ctlType, UINT itemState, RECT rect, UINT itemId = 0) {
    DRAWITEMSTRUCT draw{};
    draw.CtlType = ctlType;
    draw.CtlID = static_cast<UINT>(GetDlgCtrlID(hwnd));
    draw.itemID = itemId;
    draw.itemState = itemState;
    draw.hwndItem = hwnd;
    draw.hDC = dc;
    draw.rcItem = rect;
    ThemedControls::Draw(theme, &draw);
}

void FillRoundRect(HDC dc, RECT rect, int radius, COLORREF fill, COLORREF border, int borderWidth = 1) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, borderWidth), border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void FillEllipseRect(HDC dc, RECT rect, COLORREF fill, COLORREF border, int borderWidth = 1) {
    HBRUSH brush = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, borderWidth), border);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    Ellipse(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, width), color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

HWND CreateParent(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = instance;
    wc.lpszClassName = L"QuattroThemeStateProbeWindow";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, instance, nullptr);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    HINSTANCE instance = GetModuleHandleW(nullptr);
    std::filesystem::path output = L"theme-state-real-controls.png";
    if (argc > 1 && argv[1] && argv[1][0] != L'\0') {
        output = argv[1];
    }

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

    Gdiplus::GdiplusStartupInput gdiplusInput{};
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
        return 2;
    }

    Theme theme = Theme::Load(std::filesystem::current_path() / L"theme", L"default");
    HWND parent = CreateParent(instance);
    HFONT font = CreateFontW(
        -14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    if (!font) {
        font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }

    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, kWidth, kHeight);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    ReleaseDC(nullptr, screen);

    HBRUSH bg = CreateSolidBrush(ToColorRef(theme.color(L"dialog", L"normal", L"bg")));
    RECT canvas{0, 0, kWidth, kHeight};
    FillRect(dc, &canvas, bg);
    DeleteObject(bg);

    HFONT bold = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    DrawLabel(dc, bold ? bold : font, L"real Win32 themed control state matrix", RECT{24, 18, 420, 42}, ToColorRef(theme.color(L"global", L"normal", L"text")));
    DrawLabel(dc, font, L"Generated by QuattroThemeStateProbe", RECT{24, 38, 420, 62}, ToColorRef(theme.color(L"content", L"empty", L"text")));

    const std::vector<std::wstring> columns{L"normal", L"hover", L"pressed", L"focused", L"disabled"};
    const int startX = 130;
    const int startY = 78;
    const int cellW = 154;
    for (int i = 0; i < static_cast<int>(columns.size()); ++i) {
        DrawLabel(dc, bold ? bold : font, columns[static_cast<std::size_t>(i)], RECT{startX + i * cellW, 48, startX + i * cellW + 120, 72}, ToColorRef(theme.color(L"content", L"empty", L"text")));
    }

    HWND button = ThemedControls::CreateButton(instance, parent, 101, L"button", 0, 0, 124, ThemedControls::ButtonHeight(theme), font);
    HWND mini = ThemedControls::CreateMiniButton(instance, parent, 105, L"up", 0, 0, 42, ThemedControls::MiniButtonHeight(theme), font);
    HWND check = ThemedControls::CreateCheckBox(instance, parent, 102, L"checkbox", 0, 0, 124, ThemedControls::CheckBoxHeight(theme), font, false);
    HWND tab = ThemedControls::CreateTabButton(instance, parent, 103, L"tab", 0, 0, 124, ThemedControls::TabButtonHeight(theme), font, false);
    HWND toggle = ThemedControls::CreateToggle(instance, parent, 106, L"toggle", 0, 0, 124, font, theme, true);
    HWND radio = ThemedControls::CreateRadioButton(instance, parent, 107, L"radio", 0, 0, 124, font, theme, 1, true);
    HWND slider = ThemedControls::CreateSlider(instance, parent, 108, theme, 0, 0, 124, 0.0, 100.0, 5.0, 60.0);
    HWND combo = ThemedControls::CreateComboBox(instance, parent, 104, 0, 0, 124, 180, font, theme);
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"combo"));
    SendMessageW(combo, CB_SETCURSEL, 0, 0);

    const struct Row {
        const wchar_t* label;
        HWND hwnd;
        UINT type;
        int height;
    } rows[] = {
        {L"Button", button, ODT_BUTTON, ThemedControls::ButtonHeight(theme)},
        {L"Mini", mini, ODT_BUTTON, ThemedControls::MiniButtonHeight(theme)},
        {L"Combo", combo, ODT_COMBOBOX, ThemedControls::ComboBoxHeight(theme)},
        {L"Checkbox", check, ODT_BUTTON, ThemedControls::CheckBoxHeight(theme)},
        {L"Tab", tab, ODT_BUTTON, ThemedControls::TabButtonHeight(theme)},
    };

    for (int row = 0; row < 5; ++row) {
        const int y = startY + row * 58;
        DrawLabel(dc, bold ? bold : font, rows[row].label, RECT{24, y, 112, y + 34}, ToColorRef(theme.color(L"global", L"normal", L"text")));
        for (int col = 0; col < 5; ++col) {
            const int x = startX + col * cellW;
            RECT rect{x, y, x + 124, y + rows[row].height};
            UINT state = 0;
            EnableWindow(rows[row].hwnd, TRUE);
            if (col == 2) state |= ODS_SELECTED;
            if (col == 3) state |= ODS_FOCUS;
            if (col == 4) {
                state |= ODS_DISABLED;
                EnableWindow(rows[row].hwnd, FALSE);
            }
            if (rows[row].hwnd == check && (col == 2 || col == 3)) {
                SendMessageW(check, BM_SETCHECK, BST_CHECKED, 0);
            } else if (rows[row].hwnd == tab && (col == 2 || col == 3)) {
                SendMessageW(tab, BM_SETCHECK, BST_CHECKED, 0);
            } else {
                SendMessageW(check, BM_SETCHECK, BST_UNCHECKED, 0);
                SendMessageW(tab, BM_SETCHECK, BST_UNCHECKED, 0);
            }
            SetWindowTextW(rows[row].hwnd, columns[static_cast<std::size_t>(col)].c_str());
            if (rows[row].hwnd == combo) {
                SetWindowPos(combo, nullptr, 0, 0, rect.right - rect.left, ThemedControls::ComboBoxDropdownHeight(theme), SWP_NOZORDER | SWP_NOACTIVATE);
                POINT oldOrigin{};
                SetViewportOrgEx(dc, rect.left, rect.top, &oldOrigin);
                SendMessageW(combo, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
                SetViewportOrgEx(dc, oldOrigin.x, oldOrigin.y, nullptr);
            } else {
                DrawThemedControl(theme, dc, rows[row].hwnd, rows[row].type, state, rect);
            }
        }
    }

    const int sampleY = 370;
    DrawLabel(dc, bold ? bold : font, L"Basic", RECT{24, sampleY, 112, sampleY + 34}, ToColorRef(theme.color(L"global", L"normal", L"text")));
    DrawLabel(dc, font, L"label text", RECT{startX, sampleY, startX + 124, sampleY + ThemedControls::LabelHeight(theme)}, ToColorRef(theme.color(L"label", L"normal", L"text")));
    RECT fieldRect{startX + cellW, sampleY, startX + cellW + 124, sampleY + ThemedControls::FieldFrameHeight(theme)};
    ThemedControls::DrawFieldFrame(theme, dc, fieldRect, nullptr, true);
    RECT fieldText = ThemedControls::FieldTextRect(theme, fieldRect);
    DrawLabel(dc, font, L"field", fieldText, ToColorRef(theme.color(L"field", L"readonly", L"text")));
    RECT listRect{startX + cellW * 2, sampleY, startX + cellW * 2 + 124, sampleY + 52};
    ThemedControls::DrawListFrame(theme, dc, listRect, nullptr, true);
    RECT listText{listRect.left + 8, listRect.top + 8, listRect.right - 8, listRect.top + 28};
    DrawLabel(dc, font, L"list", listText, ToColorRef(theme.color(L"list", L"normal", L"text")));

    const int reservedY = 440;
    DrawLabel(dc, bold ? bold : font, L"Reserved", RECT{24, reservedY, 112, reservedY + 34}, ToColorRef(theme.color(L"global", L"normal", L"text")));

    const int panelRadius = static_cast<int>(theme.metric(L"panel", L"radius", 7.0f));
    RECT panelNormal{startX, reservedY, startX + 124, reservedY + 44};
    FillRoundRect(dc, panelNormal, panelRadius, ToColorRef(theme.color(L"panel", L"normal", L"bg")), ToColorRef(theme.color(L"panel", L"normal", L"border")));
    DrawLabel(dc, font, L"panel", RECT{panelNormal.left + 10, panelNormal.top + 10, panelNormal.right - 10, panelNormal.bottom - 6}, ToColorRef(theme.color(L"text", L"normal", L"text")));

    RECT iconButton{startX + cellW, reservedY, startX + cellW + 32, reservedY + 32};
    FillRoundRect(dc, iconButton, static_cast<int>(theme.metric(L"iconButton", L"radius", 7.0f)), ToColorRef(theme.color(L"iconButton", L"hover", L"bg")), ToColorRef(theme.color(L"panel", L"normal", L"border")));
    const COLORREF iconColor = ToColorRef(theme.color(L"iconButton", L"hover", L"icon"));
    DrawLine(dc, iconButton.left + 10, iconButton.top + 16, iconButton.right - 10, iconButton.top + 16, iconColor, 2);
    DrawLine(dc, iconButton.left + 16, iconButton.top + 10, iconButton.left + 16, iconButton.bottom - 10, iconColor, 2);
    DrawLabel(dc, font, L"iconButton", RECT{iconButton.right + 8, reservedY + 4, iconButton.right + 100, reservedY + 28}, ToColorRef(theme.color(L"text", L"normal", L"text")));

    DrawThemedControl(theme, dc, toggle, ODT_BUTTON, 0, RECT{startX + cellW * 2, reservedY + 4, startX + cellW * 2 + 124, reservedY + 4 + ThemedControls::ToggleHeight(theme)});
    DrawThemedControl(theme, dc, radio, ODT_BUTTON, 0, RECT{startX + cellW * 3, reservedY + 4, startX + cellW * 3 + 124, reservedY + 4 + ThemedControls::RadioButtonHeight(theme)});

    const int secondY = reservedY + 62;
    DrawLabel(dc, bold ? bold : font, L"Reserved", RECT{24, secondY, 112, secondY + 34}, ToColorRef(theme.color(L"global", L"normal", L"text")));
    RECT listItemRect{startX, secondY, startX + 124, secondY + static_cast<int>(theme.metric(L"listItem", L"height", 28.0f))};
    FillRoundRect(dc, listItemRect, 5, ToColorRef(theme.color(L"listItem", L"selected", L"bg")), ToColorRef(theme.color(L"listItem", L"selected", L"bg")));
    DrawLabel(dc, font, L"listItem", RECT{listItemRect.left + static_cast<int>(theme.metric(L"listItem", L"paddingX", 8.0f)), listItemRect.top, listItemRect.right - 8, listItemRect.bottom}, ToColorRef(theme.color(L"listItem", L"selected", L"text")));

    RECT sliderRect{startX + cellW, secondY + 4, startX + cellW + 124, secondY + 4 + ThemedControls::SliderHeight(theme)};
    SetWindowPos(slider, nullptr, 0, 0, sliderRect.right - sliderRect.left, sliderRect.bottom - sliderRect.top, SWP_NOZORDER | SWP_NOACTIVATE);
    POINT sliderOrigin{};
    SetViewportOrgEx(dc, sliderRect.left, sliderRect.top, &sliderOrigin);
    SendMessageW(slider, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT);
    SetViewportOrgEx(dc, sliderOrigin.x, sliderOrigin.y, nullptr);
    DrawLabel(dc, font, L"slider", RECT{sliderRect.right + 8, secondY + 2, sliderRect.right + 70, secondY + 26}, ToColorRef(theme.color(L"text", L"normal", L"text")));

    RECT tooltipRect{startX + cellW * 3, secondY, startX + cellW * 3 + 124, secondY + 32};
    FillRoundRect(dc, tooltipRect, static_cast<int>(theme.metric(L"tooltip", L"radius", 6.0f)), ToColorRef(theme.color(L"tooltip", L"normal", L"bg")), ToColorRef(theme.color(L"tooltip", L"normal", L"border")));
    DrawLabel(dc, font, L"tooltip", RECT{tooltipRect.left + static_cast<int>(theme.metric(L"tooltip", L"paddingX", 8.0f)), tooltipRect.top, tooltipRect.right - 8, tooltipRect.bottom}, ToColorRef(theme.color(L"tooltip", L"normal", L"text")));

    RECT toastRect{startX + cellW * 4, secondY, startX + cellW * 4 + 150, secondY + 38};
    FillRoundRect(dc, toastRect, static_cast<int>(theme.metric(L"toast", L"radius", 7.0f)), ToColorRef(theme.color(L"toast", L"success", L"bg")), ToColorRef(theme.color(L"toast", L"success", L"border")));
    DrawLabel(dc, font, L"toast success", RECT{toastRect.left + static_cast<int>(theme.metric(L"toast", L"paddingX", 12.0f)), toastRect.top, toastRect.right - 8, toastRect.bottom}, ToColorRef(theme.color(L"toast", L"success", L"text")));

    const int separatorY = secondY + 56;
    const int separatorX = startX + cellW * 3;
    DrawLine(dc, separatorX, separatorY, separatorX + 124, separatorY, ToColorRef(theme.color(L"separator", L"normal", L"line")), static_cast<int>(theme.metric(L"separator", L"thickness", 1.0f)));
    DrawLabel(dc, font, L"separator", RECT{separatorX, separatorY + 8, separatorX + 124, separatorY + 30}, ToColorRef(theme.color(L"text", L"muted", L"text")));

    const int facadeY = 620;
    DrawLabel(dc, bold ? bold : font, L"Facade", RECT{24, facadeY, 112, facadeY + 34}, ToColorRef(theme.color(L"global", L"normal", L"text")));
    ThemedUi facade(instance, parent, theme, font, DialogLayoutKind::Compact, kWidth, kHeight);
    HWND link = facade.LinkText(201, L"themed link", 0, 0, 150, ThemedLinkOptions{ThemedLinkRole::External});
    DrawThemedControl(theme, dc, link, ODT_BUTTON, 0, RECT{startX, facadeY, startX + 150, facadeY + facade.labelHeight()});
    RECT tableFrame{startX + cellW, facadeY, startX + cellW * 4, facadeY + 140};
    HWND table = facade.Table(
        202,
        RECT{0, 0, tableFrame.right - tableFrame.left, tableFrame.bottom - tableFrame.top},
        {ThemedTableColumn{L"name", L"Name", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Fixed, 150},
         ThemedTableColumn{L"value", L"Value", ThemedTableColumnAlign::Start, ThemedTableColumnWidth::Remaining}},
        ThemedTableOptions{});
    ThemedUi::SetTableRows(table, {
        ThemedTableRow{1, {{L"First row"}, {L"Public Table facade"}}, false, true},
        ThemedTableRow{2, {{L"Selected row"}, {L"Theme-driven header and rows"}}, false, true},
    });
    ListView_SetItemState(table, 1, LVIS_SELECTED, LVIS_SELECTED);
    SetWindowPos(table, nullptr, 0, 0, tableFrame.right - tableFrame.left - 4, tableFrame.bottom - tableFrame.top - 4, SWP_NOZORDER | SWP_NOACTIVATE);
    ThemedControls::DrawListFrame(theme, dc, tableFrame, table);
    POINT tableOrigin{};
    SetViewportOrgEx(dc, tableFrame.left + 2, tableFrame.top + 2, &tableOrigin);
    SendMessageW(table, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_CHILDREN);
    SetViewportOrgEx(dc, tableOrigin.x, tableOrigin.y, nullptr);

    const int containerY = 790;
    DrawLabel(dc, bold ? bold : font, L"Containers", RECT{24, containerY, 112, containerY + 34}, ToColorRef(theme.color(L"global", L"normal", L"text")));
    HWND group = facade.GroupBox(203, L"GroupBox", RECT{0, 0, 220, 110});
    DrawThemedControl(theme, dc, group, ODT_BUTTON, 0, RECT{startX, containerY, startX + 220, containerY + 110});
    HWND tabs = facade.TabControl(204, RECT{0, 0, 260, 54}, {{801, L"First"}, {802, L"Second"}});
    DrawThemedControl(theme, dc, tabs, ODT_BUTTON, 0, RECT{startX + 240, containerY, startX + 500, containerY + 54});
    HWND toolbar = facade.ToolBar(205, RECT{0, 0, 280, 42}, {{901, L"Run"}, {902, L"Pin", ThemedToolItemKind::Toggle}});
    DrawThemedControl(theme, dc, toolbar, ODT_BUTTON, 0, RECT{startX + 520, containerY, startX + 800, containerY + 42});

    const int verticalY = 900;
    DrawLabel(dc, bold ? bold : font, L"Vertical TabControl appearances", RECT{24, verticalY, 280, verticalY + 28}, ToColorRef(theme.color(L"global", L"normal", L"text")));
    const std::vector<std::pair<std::wstring, ThemedTabControlAppearance>> verticalAppearances{
        {L"Standard", ThemedTabControlAppearance::Standard},
        {L"Emphasized", ThemedTabControlAppearance::EmphasizedSegmented},
        {L"Underline", ThemedTabControlAppearance::MinimalUnderline},
        {L"Soft pill", ThemedTabControlAppearance::SoftPill},
        {L"Connected", ThemedTabControlAppearance::ConnectedTabs},
    };
    for (std::size_t index = 0; index < verticalAppearances.size(); ++index) {
        const int x = 116 + static_cast<int>(index) * 168;
        DrawLabel(dc, font, verticalAppearances[index].first, RECT{x, verticalY + 26, x + 144, verticalY + 50}, ToColorRef(theme.color(L"text", L"muted", L"text")));
        ThemedTabControlOptions options{};
        options.activeIndex = 1;
        options.appearance = verticalAppearances[index].second;
        options.orientation = ThemedTabControlOrientation::Vertical;
        const int firstId = 1000 + static_cast<int>(index) * 3;
        HWND verticalTabs = facade.TabControl(
            300 + static_cast<int>(index),
            RECT{0, 0, 144, 150},
            {{firstId, L"First"}, {firstId + 1, L"Second"}, {firstId + 2, L"Third"}},
            options);
        RECT target{x, verticalY + 52, x + 144, verticalY + 202};
        DrawThemedControl(theme, dc, verticalTabs, ODT_BUTTON, 0, target);
        for (int item = 0; item < 3; ++item) {
            HWND tabButton = GetDlgItem(verticalTabs, firstId + item);
            RECT buttonRect{};
            GetWindowRect(tabButton, &buttonRect);
            MapWindowPoints(HWND_DESKTOP, verticalTabs, reinterpret_cast<POINT*>(&buttonRect), 2);
            OffsetRect(&buttonRect, target.left, target.top);
            DrawThemedControl(theme, dc, tabButton, ODT_BUTTON, 0, buttonRect);
        }
    }

    SelectObject(dc, oldBitmap);
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        CLSID pngClsid{};
        if (GetEncoderClsid(L"image/png", &pngClsid) >= 0) {
            image.Save(output.c_str(), &pngClsid, nullptr);
        }
    }

    DeleteObject(bitmap);
    DeleteDC(dc);
    if (bold) DeleteObject(bold);
    if (font && font != GetStockObject(DEFAULT_GUI_FONT)) DeleteObject(font);
    DestroyWindow(parent);
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return 0;
}
