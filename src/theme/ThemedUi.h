#pragma once

#include "DialogLayout.h"
#include "ThemedControls.h"
#include "TablerIconManifest.h"

#include <string>
#include <string_view>
#include <cstdint>
#include <vector>
#include <commctrl.h>
#include <filesystem>
#include <functional>
#include <memory>
#include <windows.h>

// Shared Tabler icon font facade. Callers provide a semantic icon name from
// tools/tabler-icons.json; font loading and glyph rendering stay in the theme
// layer so local full fonts and official subset fonts use the same API.
using TablerIconId = TablerIconManifest::Id;

bool EnsureTablerIconFontLoaded(const std::filesystem::path& appDirectory = {});
wchar_t TablerIconGlyph(TablerIconId id);
HICON CreateTablerIconHandle(
    const std::filesystem::path& appDirectory,
    TablerIconId id,
    int fontHeight = 52,
    COLORREF color = RGB(31, 41, 55));
bool DrawTablerIcon(
    HDC dc,
    const RECT& rect,
    const std::filesystem::path& appDirectory,
    TablerIconId id,
    COLORREF color,
    int fontHeight = 0);

enum class ThemedPaintComponent {
    Window,
    Dialog,
    Panel,
    List,
    ListItem,
    Menu,
    MenuItem,
    TabButton,
    CalendarDay,
    Text,
    Label,
    Separator,
};

enum class ThemedPaintState {
    Normal,
    Hover,
    Selected,
    Focused,
    Disabled,
    Error,
    Muted,
    Accent,
    Danger,
    Warning,
    Success,
    Today,
};

enum class ThemedPaintShape {
    Rectangle,
    RoundedRectangle,
    Ellipse,
};

enum class ThemedPaintTextAlign {
    Start,
    Center,
    End,
};

enum class ThemedPaintVerticalAlign {
    Top,
    Center,
    Bottom,
};

struct ThemedPaintTextOptions {
    ThemedPaintTextAlign align = ThemedPaintTextAlign::Start;
    ThemedPaintVerticalAlign verticalAlign = ThemedPaintVerticalAlign::Top;
    bool multiline = false;
    bool ellipsis = false;
    bool noPrefix = true;
};

// Semantic custom-paint facade. Business windows provide only shared component
// roles and runtime states; theme tokens, DPI conversion, D2D/DWrite resource
// management, and the isolated GDI emergency backend remain in the public UI
// layer.
class ThemedPaint final {
public:
    ThemedPaint(HWND hwnd, HDC dc, const Theme& theme, HFONT font);
    ~ThemedPaint();

    ThemedPaint(const ThemedPaint&) = delete;
    ThemedPaint& operator=(const ThemedPaint&) = delete;

    bool d2dReady() const;
    void Fill(
        RECT rect,
        ThemedPaintComponent component,
        ThemedPaintState state = ThemedPaintState::Normal,
        ThemedPaintShape shape = ThemedPaintShape::Rectangle) const;
    void DrawText(
        const std::wstring& text,
        RECT rect,
        ThemedPaintComponent component = ThemedPaintComponent::Text,
        ThemedPaintState state = ThemedPaintState::Normal,
        ThemedPaintTextOptions options = {}) const;
    void DrawLine(
        POINT start,
        POINT end,
        ThemedPaintComponent component = ThemedPaintComponent::Separator,
        ThemedPaintState state = ThemedPaintState::Normal,
        bool pixelSnap = true) const;
    void DrawPolyline(
        const POINT* points,
        int pointCount,
        ThemedPaintComponent component,
        ThemedPaintState state = ThemedPaintState::Normal) const;
    void DrawIcon(HICON icon, RECT rect, bool disabled = false) const;
    bool DrawBitmap(HBITMAP bitmap, RECT rect, bool disabled = false) const;
    bool DrawTablerIcon(
        const std::filesystem::path& appDirectory,
        TablerIconId id,
        RECT rect,
        ThemedPaintComponent component = ThemedPaintComponent::MenuItem,
        ThemedPaintState state = ThemedPaintState::Normal) const;
    SIZE MeasureText(const std::wstring& text, int maxWidth = 0, bool multiline = false) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class ThemedButtonRole {
    Normal,
    Primary,
    Mini,
    Tab,
};

enum class ThemedButtonSize {
    Normal,
    Compact,
    Mini,
    Large,
};

enum class ThemedButtonWidthMode {
    Fixed,
    Text,
};

enum class ThemedTextAlign {
    Start,
    Center,
    End,
};

enum class ThemedLabelLines {
    One,
    Two,
    Three,
};

enum class ThemedStatusRole {
    Normal,
    Info,
    Success,
    Warning,
    Danger,
};

struct ThemedLabelOptions {
    ThemedTextAlign align = ThemedTextAlign::Start;
    ThemedLabelLines lines = ThemedLabelLines::One;
};

struct ThemedFramedTextOptions {
    ThemedTextAlign align = ThemedTextAlign::Start;
    bool wrap = false;
    bool multiline = false;
};

struct ThemedStatusTextOptions {
    ThemedStatusRole role = ThemedStatusRole::Normal;
    ThemedTextAlign align = ThemedTextAlign::Center;
};

struct ThemedComboBoxOptions {
    bool enabled = true;
};

enum class ThemedListSelection {
    Single,
    Multiple,
};

enum class ThemedListScroll {
    None,
    Vertical,
    Both,
};

struct ThemedListBoxOptions {
    ThemedListSelection selection = ThemedListSelection::Single;
    ThemedListScroll scroll = ThemedListScroll::Vertical;
    bool notify = true;
    bool enabled = true;
};

enum class ThemedCheckBoxSize {
    Normal,
    TwoLines,
};

struct ThemedCheckBoxOptions {
    bool checked = false;
    bool enabled = true;
    ThemedCheckBoxSize size = ThemedCheckBoxSize::Normal;
};

struct ThemedProgressBarOptions {
    double value = 0.0;
    bool indeterminate = false;
    bool enabled = true;
};

struct ThemedToggleOptions {
    bool checked = false;
    bool enabled = true;
};

struct ThemedRadioButtonOptions {
    int group = 0;
    bool checked = false;
    bool enabled = true;
};

struct ThemedSliderOptions {
    double minimum = 0.0;
    double maximum = 100.0;
    double step = 1.0;
    double value = 0.0;
    bool enabled = true;
};

struct ThemedHotKeyCaptureOptions {
    bool enabled = true;
};

enum class ThemedLinkRole {
    Normal,
    External,
    Muted,
    Danger,
};

struct ThemedLinkOptions {
    ThemedLinkRole role = ThemedLinkRole::Normal;
    ThemedTextAlign align = ThemedTextAlign::Start;
    bool enabled = true;
    bool visited = false;
};

enum class ThemedTableSelection {
    Single,
    Multiple,
};

enum class ThemedTableView {
    Details,
    Icons,
};

enum class ThemedTableRowPresentation {
    SingleLine,
    TwoLine,
};

enum class ThemedTableColumnAlign {
    Start,
    Center,
    End,
};

enum class ThemedTableColumnWidth {
    Fixed,
    Content,
    Remaining,
};

struct ThemedTableColumn {
    std::wstring key;
    std::wstring title;
    ThemedTableColumnAlign align = ThemedTableColumnAlign::Start;
    ThemedTableColumnWidth widthMode = ThemedTableColumnWidth::Remaining;
    int fixedWidth = 0;
    bool sortable = false;
};

struct ThemedTableOptions {
    ThemedTableSelection selection = ThemedTableSelection::Single;
    ThemedTableView view = ThemedTableView::Details;
    bool checkable = false;
    bool fullRowSelect = true;
    bool enabled = true;
    bool showHeader = true;
    bool allowColumnResize = false;
    bool allowHorizontalScroll = false;
    bool showRowGridLines = false;
    bool showColumnGridLines = false;
    // 行数会超出可见高度、必然出现垂直滚动条时置 true：
    // 列宽分配预扣滚动条宽度，避免出现横向滚动条。
    bool reserveScrollBarGutter = false;
    ThemedTableRowPresentation rowPresentation = ThemedTableRowPresentation::SingleLine;
};

enum class ThemedTableCellRole {
    Text,
    Action,
    DestructiveAction,
};

struct ThemedTableCell {
    std::wstring text;
    int image = -1;
    ThemedTableCellRole role = ThemedTableCellRole::Text;
    int actionId = 0;
    std::wstring secondaryText;
};

struct ThemedTableRow {
    std::intptr_t key = 0;
    std::vector<ThemedTableCell> cells;
    bool checked = false;
    bool enabled = true;
};

enum class ThemedTableEventKind {
    Click,
    Activated,
    SelectionChanged,
    CheckChanged,
    SortRequested,
    ActionInvoked,
};

struct ThemedTableEvent {
    ThemedTableEventKind kind = ThemedTableEventKind::Click;
    int row = -1;
    int column = -1;
    POINT point{};
    bool checked = false;
    std::intptr_t rowKey = 0;
    int actionId = 0;
};

enum class ThemedTooltipPlacement {
    Auto,
    Above,
    Below,
    Cursor,
};

enum class ThemedTooltipRole {
    Normal,
    Info,
    Warning,
    Danger,
};

struct ThemedTooltipOptions {
    ThemedTooltipPlacement placement = ThemedTooltipPlacement::Auto;
    ThemedTooltipRole role = ThemedTooltipRole::Normal;
    bool multiline = true;
    bool enabled = true;
    int maxWidth = 0;
};

using ThemedTableRowTooltipProvider = std::function<std::wstring(int row, std::intptr_t rowKey)>;

enum class ThemedToastAnchor {
    OwnerBottomRight,
    OwnerTopRight,
    ScreenBottomRight,
};

enum class ThemedToastRole {
    Normal,
    Info,
    Success,
    Warning,
    Danger,
};

struct ThemedToastOptions {
    ThemedToastAnchor anchor = ThemedToastAnchor::OwnerBottomRight;
    ThemedToastRole role = ThemedToastRole::Normal;
    bool multiline = true;
    bool enabled = true;
    int durationMs = 3000;
    int maxWidth = 0;
};

struct ThemedGroupBoxOptions {
    bool enabled = true;
    bool raised = false;
};

struct ThemedContentInsets {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

enum class ThemedPanelRole {
    Normal,
    Subtle,
    Raised,
    Inset,
    Warning,
    Danger,
};

enum class ThemedControlSurface {
    Dialog,
    Panel,
    GroupBox,
};

struct ThemedPanelOptions {
    ThemedPanelRole role = ThemedPanelRole::Normal;
    bool enabled = true;
    bool visible = true;
    bool scrollable = false;
};

struct ThemedTabItem {
    int id = 0;
    std::wstring text;
    bool enabled = true;
};

enum class ThemedTabControlAppearance {
    // 默认公共 TabButton 外观：每个标签具有独立浅色背景、边框和圆角，
    // 选中项使用浅强调色。适合表单内的小型互斥选择组。
    Standard,

    // 方案 A，高对比分段式：标签位于统一容器中，普通项弱化边框，
    // 选中项使用强调色实心背景和白色文字。适合主要页面导航。
    EmphasizedSegmented,

    // 方案 B，极简指示线式：移除常态按钮边框，以强调色文字和指示线表示选中项；
    // 横向时指示线位于底部，纵向时位于右侧。适合内容密集的设置页。
    MinimalUnderline,

    // 方案 C，柔和胶囊式：每个标签为彼此分离的无硬边框胶囊，
    // 选中项使用浅强调色背景。适合轻量筛选和友好的工具界面。
    SoftPill,

    // 方案 D，内容连接式：选中标签使用卡片边框并向内容方向开放；
    // 横向时向下开放，纵向时向右开放。适合强调标签与内容归属关系的布局。
    ConnectedTabs,
};

enum class ThemedTabControlOrientation {
    Horizontal, // 标签从左到右排列，使用 Left/Right 导航。
    Vertical,   // 标签从上到下排列，使用 Up/Down 导航。
};

// 标签组外层容器样式。AppearanceDefault 保持由 appearance 决定的默认行为；
// Framed 强制显示公共容器外框；Borderless 仅保留容器背景，移除整体外框。
enum class ThemedTabControlContainerStyle {
    AppearanceDefault,
    Framed,
    Borderless,
};

struct ThemedTabControlOptions {
    int activeIndex = 0;
    bool enabled = true;
    // 仅控制横向标签是否平分可用宽度；纵向标签始终填充导航栏可用宽度。
    bool equalWidth = false;
    ThemedTabControlAppearance appearance = ThemedTabControlAppearance::Standard;
    ThemedTabControlOrientation orientation = ThemedTabControlOrientation::Horizontal;
    ThemedTabControlContainerStyle containerStyle = ThemedTabControlContainerStyle::AppearanceDefault;
};

enum class ThemedToolItemKind {
    Command,
    Toggle,
    Separator,
};

enum class ThemedToolItemAlignment {
    Leading,
    Trailing,
};

enum class ThemedToolItemDisplay {
    Automatic,
    TextOnly,
    IconOnly,
    IconAndText,
};

enum class ThemedIconOwnership {
    Borrowed,
    Transfer,
};

struct ThemedToolItem {
    int id = 0;
    std::wstring text;
    ThemedToolItemKind kind = ThemedToolItemKind::Command;
    ThemedToolItemAlignment alignment = ThemedToolItemAlignment::Leading;
    bool enabled = true;
    bool checked = false;
    std::wstring tooltip;
    HICON icon = nullptr;
    ThemedToolItemDisplay display = ThemedToolItemDisplay::Automatic;
    ThemedIconOwnership iconOwnership = ThemedIconOwnership::Borrowed;
};

struct ThemedToolBarOptions {
    bool enabled = true;
    bool compact = true;
    bool automaticOverflow = true;
};

struct ThemedSplitButton {
    HWND primary = nullptr;
    HWND menu = nullptr;
};

struct ThemedSplitButtonMenuItem {
    int id = 0;
    std::wstring text;
    bool enabled = true;
    TablerIconId icon{};
};

enum class ThemedEditMode {
    SingleLine,
    MultiLine,
};

enum class ThemedEditContent {
    Text,
    Integer,
    Password,
};

struct ThemedEditOptions {
    ThemedEditMode mode = ThemedEditMode::SingleLine;
    ThemedEditContent content = ThemedEditContent::Text;
    bool readOnly = false;
    bool enabled = true;
    bool error = false;
    bool selectAllOnFocus = false;
    bool acceptsReturn = true;
    UINT maxLength = 0;
    std::wstring placeholder;
};

class ThemedEditFrameRegistry {
public:
    virtual ~ThemedEditFrameRegistry() = default;
    virtual void RegisterEditFrame(HWND child, RECT frame, const ThemedEditOptions& options) = 0;
};

class ThemedTableFrameRegistry {
public:
    virtual ~ThemedTableFrameRegistry() = default;
    virtual void RegisterTableFrame(HWND child, RECT frame) = 0;
    virtual void UnregisterTableFrame(HWND child) = 0;
};

class ThemedTooltipRegistry {
public:
    virtual ~ThemedTooltipRegistry() = default;
    virtual void ShowTooltip(const std::wstring& text, POINT screenPoint, const ThemedTooltipOptions& options) = 0;
    virtual void HideTooltip() = 0;
};

class ThemedToastRegistry {
public:
    virtual ~ThemedToastRegistry() = default;
    virtual void ShowToast(const std::wstring& text, const ThemedToastOptions& options) = 0;
    virtual void HideToast() = 0;
};

class ThemedEditFrameCollection final : public ThemedEditFrameRegistry {
public:
    void RegisterEditFrame(HWND child, RECT frame, const ThemedEditOptions& options) override;
    void Remove(HWND child);
    void SetFrame(HWND child, RECT frame);
    void SetReadOnly(HWND child, bool readOnly);
    void SetError(HWND child, bool error);
    void Draw(const Theme& theme, HDC dc) const;

private:
    struct Entry {
        HWND child = nullptr;
        RECT frame{};
        ThemedEditOptions options{};
    };
    Entry* Find(HWND child);
    std::vector<Entry> entries_;
};

// Cached system menu font for owner-draw popup menus. The font is resolved
// from NONCLIENTMETRICS for the target monitor DPI so measuring and painting
// use the same Windows menu typography.
class ThemedMenuFontCache final {
public:
    ThemedMenuFontCache() = default;
    ~ThemedMenuFontCache();

    ThemedMenuFontCache(const ThemedMenuFontCache&) = delete;
    ThemedMenuFontCache& operator=(const ThemedMenuFontCache&) = delete;

    HFONT FontForScreenPoint(POINT screenPoint, HWND fallbackWindow = nullptr);
    HFONT FontForDpi(UINT dpi);
    SIZE MeasureText(HWND owner, const std::wstring& text) const;
    int Scale(int logicalPixels) const;
    UINT dpi() const { return dpi_; }
    void Reset();

private:
    HFONT font_ = nullptr;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
};

enum class ThemedPopupMenuSource {
    ClientArea,
    TrayIcon,
    ToolBar,
    NativeShell,
};

enum class ThemedPopupMenuHorizontalAlign {
    Left,
    Right,
};

enum class ThemedPopupMenuVerticalAlign {
    Top,
    Bottom,
};

struct ThemedPopupMenuOptions {
    ThemedPopupMenuSource source = ThemedPopupMenuSource::ClientArea;
    ThemedPopupMenuHorizontalAlign horizontalAlign = ThemedPopupMenuHorizontalAlign::Left;
    ThemedPopupMenuVerticalAlign verticalAlign = ThemedPopupMenuVerticalAlign::Top;
    bool rightButton = true;
    bool returnCommand = false;
};

struct ThemedPopupMenuResult {
    UINT command = 0;
    bool opened = false;
    bool foregroundReady = false;
    bool foregroundSuppressed = false;
};

// ThemedUi is the preferred facade for dialog/tool-window UI code.
//
// It keeps three responsibilities together:
// 1. Common component creation, so controls always receive the same theme
//    binding, font, subclass, owner-draw flags, and cached text behavior.
// 2. Common compact dialog layout helpers, so windows do not hand-roll footer
//    button alignment or content insets.
// 3. Parent message dispatch for owner-draw controls, so a new window does not
//    accidentally create a themed button/progress bar that nobody paints.
//
// Window classes can still use ThemedControls directly for low-level drawing or
// special cases. New ordinary controls should go through this facade first.
class ThemedUi {
public:
    static COLORREF ListSurfaceColor(const Theme& theme);
    static ThemedPopupMenuResult ShowPopupMenu(
        HWND notificationWindow,
        HMENU menu,
        POINT screenPoint,
        const ThemedPopupMenuOptions& options = {});
    ThemedUi(
        HINSTANCE instance,
        HWND parent,
        const Theme& theme,
        HFONT font,
        DialogLayoutKind layoutKind,
        int clientWidth,
        int clientHeight,
        ThemedEditFrameRegistry* editFrameRegistry = nullptr,
        ThemedTableFrameRegistry* tableFrameRegistry = nullptr,
        ThemedTooltipRegistry* tooltipRegistry = nullptr,
        ThemedToastRegistry* toastRegistry = nullptr,
        UINT dpi = 0);

    const DialogLayoutMetrics& layout() const { return layout_; }
    const Theme& theme() const { return theme_; }
    int clientWidth() const { return clientWidth_; }
    int clientHeight() const { return clientHeight_; }
    UINT dpi() const { return dpi_; }
    int scale(int logicalPixels) const;

    // Layout accessors. They expose the shared dialog metrics instead of
    // letting each window duplicate inset, footer, and row-spacing math.
    int contentLeft() const { return layout_.contentInsetX; }
    int contentTop() const { return layout_.contentInsetY; }
    int contentWidth() const { return clientWidth_ - layout_.contentInsetX * 2; }
    RECT contentRect() const { return layout_.ContentRect(clientWidth_, clientHeight_); }
    int denseGap() const;

    int labelHeight() const;
    int buttonHeight() const;
    int buttonHeight(ThemedButtonRole role, ThemedButtonSize size) const;
    int compactButtonHeight() const;
    int footerButtonHeight() const { return layout_.footerButtonHeight; }
    int timeDisplayHeight() const;
    SIZE timeDisplayPreferredSize(const std::wstring& text) const;
    int checkBoxHeight(ThemedCheckBoxSize size = ThemedCheckBoxSize::Normal) const;
    int toggleHeight() const;
    int tabButtonHeight() const;
    int comboBoxHeight() const;
    int listItemHeight(bool twoLines = false) const;
    int tabButtonWidth(const std::wstring& text) const;
    ThemedContentInsets groupBoxInsets() const;
    int progressBarHeight() const;
    int editHeight(ThemedEditMode mode = ThemedEditMode::SingleLine) const;
    int buttonWidth(
        const std::wstring& text,
        ThemedButtonRole role,
        ThemedButtonSize size,
        ThemedButtonWidthMode widthMode,
        int fixedWidth = 0) const;
    int splitButtonWidth(
        const std::wstring& text,
        ThemedButtonRole role,
        ThemedButtonSize size,
        ThemedButtonWidthMode widthMode,
        int fixedWidth = 0) const;
    int textWidth(const std::wstring& text) const;
    int comboBoxWidth(const std::vector<std::wstring>& items) const;
    int tableColumnWidth(const std::wstring& widestText) const;
    int tableColumnWidth(std::initializer_list<std::wstring_view> candidateTexts) const;
    int tableHeightForRows(int visibleRows, bool showHeader = true, bool twoLines = false) const;
    RECT tabStripRect(RECT bounds) const;
    int tabPageTop(RECT tabStrip) const;
    int footerButtonX(int buttonIndex, int buttonCount) const;
    int footerButtonY(int buttonHeight) const;
    int centeredGroupX(int groupWidth) const;
    int nextRowY(int y, int controlHeight) const;
    RECT rect(int x, int y, int width, int height) const;
    RECT editFrame(int x, int y, int width, ThemedEditMode mode = ThemedEditMode::SingleLine) const;

    // Common component factories. These wrap ThemedControls and perform the
    // extra theme binding needed by owner-draw controls. Callers should pass
    // only semantic values, text, and geometry; visual styling stays in Theme.
    HWND Label(const std::wstring& text, int x, int y, int width, ThemedLabelOptions options = {}) const;
    HWND StatusText(const std::wstring& text, int x, int y, int width, ThemedStatusTextOptions options = {}) const;
    void SetStatusTextRole(HWND hwnd, ThemedStatusRole role) const;
    HWND StatusBadge(const std::wstring& text, int x, int y, int width, ThemedStatusRole role = ThemedStatusRole::Normal) const;
    HWND TimeDisplay(const std::wstring& text, int x, int y, int width) const;
    void SetStatusBadgeRole(HWND hwnd, ThemedStatusRole role) const;
    static void SetText(HWND hwnd, const std::wstring& text);
    static void SetVisible(HWND hwnd, bool visible);
    void SetEnabled(HWND hwnd, bool enabled) const;
    static void SetControlSurface(HWND hwnd, ThemedControlSurface surface);
    void MoveControl(HWND hwnd, int x, int y, int width) const;
    void MoveComboBox(HWND hwnd, int x, int y, int width) const;
    HWND GroupBox(int id, const std::wstring& title, RECT frame, ThemedGroupBoxOptions options = {}) const;
    static RECT GroupContentRect(HWND groupBox);
    static void BindGroupChildren(HWND groupBox, const std::vector<HWND>& children);
    static void SetGroupEnabled(HWND groupBox, bool enabled);
    HWND Panel(int id, RECT frame, ThemedPanelOptions options = {}) const;
    static RECT PanelContentRect(HWND panel);
    static void BindPanelChildren(HWND panel, const std::vector<HWND>& children);
    static void SetPanelEnabled(HWND panel, bool enabled);
    static bool IsPanelEnabled(HWND panel);
    static void SetPanelVisible(HWND panel, bool visible);
    static bool IsPanelVisible(HWND panel);
    static void SetPanelRole(HWND panel, ThemedPanelRole role);
    static void SetPanelScrollPosition(HWND panel, int position);
    static int PanelScrollPosition(HWND panel);
    static void EnsurePanelChildVisible(HWND panel, HWND child);
    HWND TabControl(int id, RECT frame, const std::vector<ThemedTabItem>& items, ThemedTabControlOptions options = {}) const;
    static void BindTabPage(HWND tabControl, int index, const std::vector<HWND>& children);
    static void BindTabPageRoot(HWND tabControl, int index, HWND pageRoot);
    static void SetActiveTab(HWND tabControl, int index, bool notify = false);
    static int ActiveTab(HWND tabControl);
    static void SetTabEnabled(HWND tabControl, int index, bool enabled);
    static void SetTabVisible(HWND tabControl, int index, bool visible);
    static bool PreTranslateMessage(const MSG& message);
    HWND ToolBar(int id, RECT frame, const std::vector<ThemedToolItem>& items, ThemedToolBarOptions options = {}) const;
    static void SetToolEnabled(HWND toolbar, int itemId, bool enabled);
    static void SetToolChecked(HWND toolbar, int itemId, bool checked);
    static bool IsToolChecked(HWND toolbar, int itemId);
    static void SetToolVisible(HWND toolbar, int itemId, bool visible);
    static bool HasTool(HWND toolbar, int itemId);
    static bool IsToolEnabled(HWND toolbar, int itemId);
    static bool IsToolVisible(HWND toolbar, int itemId);
    static bool IsToolOverflowed(HWND toolbar, int itemId);
    static int ToolCount(HWND toolbar);
    static int ToolIndex(HWND toolbar, int itemId);
    static void SetToolText(HWND toolbar, int itemId, const std::wstring& text);
    static void SetToolTooltip(HWND toolbar, int itemId, const std::wstring& tooltip);
    static void SetToolIcon(HWND toolbar, int itemId, HICON icon, ThemedIconOwnership ownership = ThemedIconOwnership::Borrowed);
    static void SetToolDisplay(HWND toolbar, int itemId, ThemedToolItemDisplay display);
    static void SetToolAlignment(HWND toolbar, int itemId, ThemedToolItemAlignment alignment);
    static bool RemoveTool(HWND toolbar, int itemId);
    static bool InsertTool(HWND toolbar, int index, ThemedToolItem item);
    static bool MoveTool(HWND toolbar, int itemId, int targetIndex);
    static void SetTools(HWND toolbar, const std::vector<ThemedToolItem>& items);
    static void ClearTools(HWND toolbar);
    static void BeginToolBarUpdate(HWND toolbar);
    static void EndToolBarUpdate(HWND toolbar);
    // Button is the single public entry point for creating any themed button.
    // Height is fixed by (role, size) templates and cannot be overridden by
    // callers; only width is adjustable (fixed width, or measured from text).
    HWND Button(
        int id,
        const std::wstring& text,
        int x,
        int y,
        ThemedButtonRole role = ThemedButtonRole::Normal,
        ThemedButtonSize size = ThemedButtonSize::Normal,
        ThemedButtonWidthMode widthMode = ThemedButtonWidthMode::Fixed,
        int fixedWidth = 0,
        bool defaultButton = false,
        bool selected = false) const;
    ThemedSplitButton SplitButton(
        int primaryId,
        int menuId,
        const std::wstring& text,
        int x,
        int y,
        ThemedButtonRole role = ThemedButtonRole::Normal,
        ThemedButtonSize size = ThemedButtonSize::Normal,
        ThemedButtonWidthMode widthMode = ThemedButtonWidthMode::Fixed,
        int fixedWidth = 0,
        bool defaultButton = false) const;
    UINT ShowSplitButtonMenu(
        HWND notificationWindow,
        HWND menuButton,
        const std::vector<ThemedSplitButtonMenuItem>& items) const;

    // FooterButton uses DialogLayoutMetrics::FooterButtonX/Y internally, so
    // every dialog gets the same compact footer alignment and spacing.
    HWND FooterButton(int id, const std::wstring& text, int buttonIndex, int buttonCount, bool primary = false, bool defaultButton = false) const;
    HWND CheckBox(int id, const std::wstring& text, int x, int y, int width, ThemedCheckBoxOptions options = {}) const;
    static void SetChecked(HWND hwnd, bool checked);
    static bool IsChecked(HWND hwnd);
    HWND Toggle(int id, const std::wstring& text, int x, int y, int width, ThemedToggleOptions options = {}) const;
    HWND RadioButton(int id, const std::wstring& text, int x, int y, int width, ThemedRadioButtonOptions options = {}) const;
    HWND HotKeyCapture(int id, const std::wstring& text, int x, int y, int width, ThemedHotKeyCaptureOptions options = {}) const;
    HWND LinkText(int id, const std::wstring& text, int x, int y, int width, ThemedLinkOptions options = {}) const;
    static void SetLinkRole(HWND hwnd, ThemedLinkRole role);
    static void SetLinkVisited(HWND hwnd, bool visited);
    // TabButton height is fixed by the Tab template; only width is adjustable.
    HWND TabButton(int id, const std::wstring& text, int x, int y, int width, bool selected = false) const;
    static void SetTabSelected(HWND hwnd, bool selected);
    static bool IsTabSelected(HWND hwnd);
    HWND ComboBox(int id, int x, int y, int width, ThemedComboBoxOptions options = {}) const;
    static void SetComboBoxItems(HWND comboBox, const std::vector<std::wstring>& items, int selectedIndex = 0);
    static void SetComboBoxSelectedIndex(HWND comboBox, int selectedIndex, bool notify = false);
    static int ComboBoxSelectedIndex(HWND comboBox);
    HWND ListBox(int id, int x, int y, int width, int height, ThemedListBoxOptions options = {}) const;
    void MoveListBox(HWND listBox, int x, int y, int width, int height) const;
    HWND Table(int id, RECT frame, const std::vector<ThemedTableColumn>& columns, ThemedTableOptions options = {}) const;
    void MoveTable(HWND table, RECT frame) const;
    static void SetTableRows(HWND table, const std::vector<ThemedTableRow>& rows);
    static int AppendTableRow(HWND table, const ThemedTableRow& row);
    static bool UpdateTableRow(HWND table, int index, const ThemedTableRow& row);
    static bool RemoveTableRow(HWND table, int index);
    static int FindTableRowByKey(HWND table, std::intptr_t key);
    static void SetTableView(HWND table, ThemedTableView view);
    static void SetTableChecked(HWND table, int index, bool checked);
    static bool IsTableChecked(HWND table, int index);
    static bool IsTableRowEnabled(HWND table, int index);
    static int TableRowCount(HWND table);
    static int TableSelectedIndex(HWND table);
    static void SetTableSelectedIndex(HWND table, int index);
    static std::intptr_t TableRowKey(HWND table, int index);
    static int TableHitTest(HWND table, POINT point, bool fullRow = false, bool* stateIcon = nullptr);
    static int TableScreenHitTest(HWND table, POINT screenPoint, bool fullRow = false, bool* stateIcon = nullptr);
    static bool TableCellScreenRect(HWND table, int row, int column, RECT& screenRect);
    static void SetTableIconSpacing(HWND table, int x, int y);
    static void ClearTable(HWND table);
    static void SetTableImageLists(HWND table, HIMAGELIST smallImages, HIMAGELIST largeImages);
    static bool DecodeTableEvent(HWND table, LPARAM lParam, ThemedTableEvent& event);
    // Bind dynamic tooltip content to table rows. Hit testing, hover delay,
    // placement, and cleanup remain owned by the public UI layer.
    void SetTableRowTooltip(
        HWND table,
        ThemedTableRowTooltipProvider provider,
        ThemedTooltipOptions options = {},
        UINT hoverDelayMs = 400) const;
    // Bind a themed tooltip to any facade-created control. Hover tracking,
    // placement, drawing, and cleanup remain owned by the public UI layer.
    void SetTooltip(HWND control, const std::wstring& text, ThemedTooltipOptions options = {}) const;
    // Infrastructure lifecycle hook used before a tooltip registry is
    // destroyed, so attached child controls cannot retain a stale callback.
    static void DetachTooltips(ThemedTooltipRegistry* registry);
    void ShowTooltip(const std::wstring& text, POINT screenPoint, ThemedTooltipOptions options = {}) const;
    void HideTooltip() const;
    void ShowToast(const std::wstring& text, ThemedToastOptions options = {}) const;
    void HideToast() const;
    HWND Edit(int id, RECT frame, const std::wstring& value, ThemedEditOptions options = {}) const;
    HWND FramedStatic(const std::wstring& value, RECT frame, ThemedFramedTextOptions options = {}) const;
    HWND ProgressBar(int id, int x, int y, int width, ThemedProgressBarOptions options = {}) const;
    static void SetProgress(HWND hwnd, double value, bool indeterminate = false);
    HWND Slider(int id, int x, int y, int width, ThemedSliderOptions options = {}) const;
    static void SetSliderValue(HWND hwnd, double value, bool notify = false);
    static double SliderValue(HWND hwnd);

    // Native editing shortcuts take priority over window-level commands when
    // an Edit or RichEdit control owns keyboard focus.
    static bool IsNativeEditShortcut(const MSG& message);

    // Call this from a themed window's WndProc before falling back to
    // DefWindowProc. It centralizes the owner-draw/custom-draw bridge required
    // by common controls created through this facade.
    static bool HandleParentMessage(const Theme& theme, UINT message, WPARAM wParam, LPARAM lParam, LRESULT& result);

private:
    // Internal creators that take an already-resolved template height. Kept
    // private so that dialog code cannot bypass the (role, size) height
    // templates and hand-roll button heights.
    HWND NormalButton(int id, const std::wstring& text, int x, int y, int width, int height, bool defaultButton) const;
    HWND PrimaryButton(int id, const std::wstring& text, int x, int y, int width, int height, bool defaultButton) const;
    HWND BindTheme(HWND hwnd) const;

    HINSTANCE instance_ = nullptr;
    HWND parent_ = nullptr;
    const Theme& theme_;
    HFONT font_ = nullptr;
    DialogLayoutMetrics layout_{};
    int clientWidth_ = 0;
    int clientHeight_ = 0;
    UINT dpi_ = USER_DEFAULT_SCREEN_DPI;
    ThemedEditFrameRegistry* editFrameRegistry_ = nullptr;
    ThemedTableFrameRegistry* tableFrameRegistry_ = nullptr;
    ThemedTooltipRegistry* tooltipRegistry_ = nullptr;
    ThemedToastRegistry* toastRegistry_ = nullptr;
};
