#pragma once

#include "DialogLayout.h"
#include "ThemedControls.h"

#include <string>
#include <cstdint>
#include <vector>
#include <commctrl.h>
#include <windows.h>

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
    bool allowColumnResize = true;
};

struct ThemedTableCell {
    std::wstring text;
    int image = -1;
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
};

struct ThemedTableEvent {
    ThemedTableEventKind kind = ThemedTableEventKind::Click;
    int row = -1;
    int column = -1;
    POINT point{};
    bool checked = false;
    std::intptr_t rowKey = 0;
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

struct ThemedTabControlOptions {
    int activeIndex = 0;
    bool enabled = true;
    bool equalWidth = false;
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
};

class ThemedTooltipRegistry {
public:
    virtual ~ThemedTooltipRegistry() = default;
    virtual void ShowTooltip(const std::wstring& text, POINT screenPoint, const ThemedTooltipOptions& options) = 0;
    virtual void HideTooltip() = 0;
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

    int labelHeight() const;
    int buttonHeight() const;
    int buttonHeight(ThemedButtonRole role, ThemedButtonSize size) const;
    int compactButtonHeight() const;
    int checkBoxHeight(ThemedCheckBoxSize size = ThemedCheckBoxSize::Normal) const;
    int toggleHeight() const;
    int tabButtonHeight() const;
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
    int textWidth(const std::wstring& text) const;
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
    void SetStatusBadgeRole(HWND hwnd, ThemedStatusRole role) const;
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
    HWND ListBox(int id, int x, int y, int width, int height, ThemedListBoxOptions options = {}) const;
    HWND Table(int id, RECT frame, const std::vector<ThemedTableColumn>& columns, ThemedTableOptions options = {}) const;
    static void SetTableRows(HWND table, const std::vector<ThemedTableRow>& rows);
    static void SetTableView(HWND table, ThemedTableView view);
    static void SetTableChecked(HWND table, int index, bool checked);
    static bool IsTableChecked(HWND table, int index);
    static bool IsTableRowEnabled(HWND table, int index);
    static int TableRowCount(HWND table);
    static int TableSelectedIndex(HWND table);
    static std::intptr_t TableRowKey(HWND table, int index);
    static int TableHitTest(HWND table, POINT point, bool fullRow = false, bool* stateIcon = nullptr);
    static void SetTableIconSpacing(HWND table, int x, int y);
    static void ClearTable(HWND table);
    static void SetTableImageLists(HWND table, HIMAGELIST smallImages, HIMAGELIST largeImages);
    static bool DecodeTableEvent(HWND table, LPARAM lParam, ThemedTableEvent& event);
    void ShowTooltip(const std::wstring& text, POINT screenPoint, ThemedTooltipOptions options = {}) const;
    void HideTooltip() const;
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
};
