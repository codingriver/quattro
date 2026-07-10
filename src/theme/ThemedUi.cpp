#include "ThemedUi.h"

#include <algorithm>

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
    int clientHeight)
    : instance_(instance),
      parent_(parent),
      theme_(theme),
      font_(font),
      layout_(GetDialogLayoutMetrics(theme, layoutKind)),
      clientWidth_(clientWidth),
      clientHeight_(clientHeight) {}

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

// 创建主题标签文本控件。
// 参数：
// - text：标签显示的文本。
// - x：左侧 x 坐标。
// - y：顶部 y 坐标。
// - width：标签宽度，单位为像素。
// - style：Win32 静态控件样式标志，例如 SS_LEFT。
// 返回值：创建成功时返回标签 HWND，失败时返回 nullptr。
HWND ThemedUi::Label(const std::wstring& text, int x, int y, int width, DWORD style) const {
    return ThemedControls::CreateLabelText(instance_, parent_, text.c_str(), x, y, width, theme_, font_, style);
}

HWND ThemedUi::StatusText(const std::wstring& text, int x, int y, int width, const wchar_t* state, DWORD style) const {
    return BindTheme(ThemedControls::CreateStatusText(instance_, parent_, text.c_str(), x, y, width, theme_, font_, state, style));
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
HWND ThemedUi::CheckBox(int id, const std::wstring& text, int x, int y, int width, int height, bool checked) const {
    return BindTheme(ThemedControls::CreateCheckBox(instance_, parent_, id, text.c_str(), x, y, width, height, font_, checked));
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

// 创建主题下拉框。
// 参数：
// - id：子控件 ID，用于 WM_COMMAND/WM_DRAWITEM。
// - x：左侧 x 坐标。
// - y：顶部 y 坐标。
// - width：下拉框宽度，单位为像素。
// - height：下拉框高度，单位为像素。
// 返回值：创建成功时返回下拉框 HWND，失败时返回 nullptr。
HWND ThemedUi::ComboBox(int id, int x, int y, int width, int height) const {
    return ThemedControls::CreateComboBox(instance_, parent_, id, x, y, width, height, font_, theme_);
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
HWND ThemedUi::ListBox(int id, int x, int y, int width, int height, DWORD extraStyle) const {
    return ThemedControls::CreateListBox(instance_, parent_, id, x, y, width, height, font_, theme_, extraStyle);
}

// 在主题字段框内创建单行编辑框。
// 参数：
// - id：子控件 ID，用于 WM_COMMAND。
// - frame：外层字段框矩形；编辑框子控件会按主题参数内缩。
// - value：初始文本值。
// - extraStyle：附加 Win32 编辑框样式标志。
// 返回值：创建成功时返回编辑框 HWND，失败时返回 nullptr。
HWND ThemedUi::SingleLineEdit(int id, RECT frame, const std::wstring& value, DWORD extraStyle) const {
    return ThemedControls::CreateSingleLineEdit(instance_, parent_, id, theme_, frame, value, font_, extraStyle);
}

// 在主题字段框内创建多行编辑框。
// 参数：
// - id：子控件 ID，用于 WM_COMMAND。
// - frame：外层字段框矩形；编辑框子控件会按主题参数内缩。
// - value：初始文本值。
// - extraStyle：附加 Win32 编辑框样式标志。
// 返回值：创建成功时返回编辑框 HWND，失败时返回 nullptr。
HWND ThemedUi::MultiLineEdit(int id, RECT frame, const std::wstring& value, DWORD extraStyle) const {
    return ThemedControls::CreateMultiLineEdit(instance_, parent_, id, theme_, frame, value, font_, extraStyle);
}

// 在字段框内部创建主题静态文本控件。
// 参数：
// - value：静态文本内容。
// - frame：外层字段框矩形；静态文本会按主题参数内缩。
// - style：Win32 静态控件样式标志。
// 返回值：创建成功时返回静态文本 HWND，失败时返回 nullptr。
HWND ThemedUi::FramedStatic(const std::wstring& value, RECT frame, DWORD style) const {
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
HWND ThemedUi::ProgressBar(int id, int x, int y, int width, int height) const {
    return ThemedControls::CreateProgressBar(
        instance_,
        parent_,
        id,
        theme_,
        x,
        y,
        width,
        height > 0 ? height : progressBarHeight());
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
