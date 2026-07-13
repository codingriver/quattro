# Agent Rules

## Feature Rules

- 搜索功能暂时不要开发；不要新增、恢复或暴露搜索入口、搜索热键、搜索窗口或与搜索相关的用户可见配置。

## Theme Rules

- `src/theme/ThemedUi.h` 是应用界面公共控件接口、语义 options 和状态方法的唯一权威清单。新增或修改界面前必须先检查 `ThemedUi`、`ThemedWindowUi` 是否已有对应能力；`AGENTS.md` 只定义选型和约束，不复制可能与头文件失配的完整函数签名或默认参数。
- 公共控件必须按类型使用对应 facade：文本使用 `Label`、`StatusText`、`StatusBadge`、`FramedStatic`；操作使用 `Button`、`FooterButton`、`LinkText`；输入使用 `Edit`、`HotKeyCapture`；选择使用 `CheckBox`、`Toggle`、`RadioButton`、`ComboBox`、`ListBox`；数值使用 `Slider`、`ProgressBar`；数据使用 `Table`；容器使用 `Panel`、`GroupBox`；导航使用 `TabControl`、`TabButton`；工具项使用 `ToolBar`；浮层提示使用 `Tooltip`。
- 界面只能向公共控件传递控件 ID、位置、公共布局计算后的可用宽高、文本、图标、业务数据、语义角色和业务状态。字体、字号、颜色、边框、圆角、padding、margin、gap、模板高度、状态色、hover/pressed 效果和 owner-draw 细节必须由公共主题分类或公共 helper 提供。
- 公共控件状态必须通过对应语义接口修改：勾选使用 `SetChecked`/`IsChecked`，Tab 使用 `SetActiveTab`/`SetTabEnabled`/`SetTabVisible`，ToolBar 使用 `SetTool*` 和集合更新接口，Table 使用 `SetTable*`，Slider 使用 `SetSliderValue`，ProgressBar 使用 `SetProgress`，Panel 使用 `SetPanel*`。禁止窗口直接发送底层状态消息或操作 facade 创建的内部子控件。
- 新需求若无法由现有公共控件表达，必须先扩展 `ThemedUi` options、公共运行时状态、主题公共分类、默认主题、文档、lint 和测试，再接入业务窗口；禁止先在窗口层实现特殊版本后再补公共接口。
- 主题只支持公共组件分类，不支持某个页面、某个窗口、某个具体控件的单独配置。
- 新增界面或调整界面时，布局必须优先采用紧凑方案，减少不必要的留白、重复容器和装饰性层级，保证信息密度和操作路径清晰。
- 紧凑工具窗口布局必须优先将同一行内容作为整体分组居中对齐；需要居中时必须使用公共布局 helper（例如 `DialogLayoutMetrics::CenteredGroupX`），禁止在单个工具界面内散落手写居中坐标。
- 新增或修改主题参数时，必须优先映射到共享组件分类，例如 `text`、`label`、`panel`、`field`、`edit`、`button`、`iconButton`、`checkbox`、`toggle`、`radio`、`comboBox`、`tabButton`、`list`、`listItem`、`scrollbar`、`slider`、`dialog`、`menuItem`、`linkItem`、`majorNavItem`、`minorNavItem`、`tooltip`、`separator`。
- 禁止新增页面级、窗口级、控件实例级主题组件名，例如 `settings.searchInput`、`linkEdit.nameField`、`searchDialog.keywordEdit`、`newGroup.nameEdit`。
- 只有当一个主题组件代表跨界面复用的组件类型时，才允许新增公共分类。
- 新增公共分类时，必须同步更新 `src/theme/Theme.cpp` 的主题白名单、`theme/default.xml`、主题文档和相关测试。
- 开发或调整任意界面时，布局和组件必须优先使用主题驱动的公共布局 helper 与公共组件 helper；如果现有公共布局或公共组件不支持需求，必须先扩展主题公共分类、公共布局 metric 或公共组件 helper，再接入界面，禁止在界面内部自行处理布局规则或定义控件样式细节参数。
- 文本自适应 Label 的最小宽度必须由主题公共布局参数 `dialog.labelMinWidth` 统一控制，默认值为 20；禁止在界面或调用处为文本自适应 Label 传入自定义 `minWidth`、`minLabelWidth` 等最小宽度参数。需要多行 Label 对齐时，应先通过公共布局 helper 计算统一固定宽度，再使用固定宽度 Label 接口。
- 所有使用 `ThemedWindowUi` 的窗口，必须在窗口 `Handle(...)` 开头调用 `ThemedWindowUi::HandleCommonMessage(...)`。公共主题消息、背景绘制、Label 透明背景、owner-draw 控件绘制和资源释放禁止在单个界面内重复处理；如需新增公共消息处理，必须扩展公共处理方法。
- 所有新加界面必须使用 `ThemedWindowUi::DialogOptions(...)` 创建统一窗口参数，并使用 `ThemedWindowUi::HandleCommonMessage(...)` 处理公共消息；禁止新界面自定义窗口尺寸、窗口样式、布局规则、控件样式或特殊界面配置。新增界面内的布局必须使用公共布局 helper，控件必须使用公共组件 helper，并且所有视觉、尺寸、间距、状态、圆角等参数必须来自主题公共组件分类或公共 helper；禁止在新界面内部自定义组件、手写特殊参数或绕过主题公共组件。
- 新增窗口和改造既有窗口时，窗口类注册、窗口样式组合、客户区到窗口尺寸换算、显示器范围内定位以及窗口句柄创建必须统一通过 `ThemedWindowUi::DialogOptions(...)`、`ThemedWindowUi::CreateWindowHandle(...)` 等公共窗口接口完成；禁止业务窗口直接调用 `RegisterClassExW`、`CreateWindowExW` 或自行实现等价创建流程。公共窗口接口参数或语义能力不足时，必须先扩展 `ThemedWindowCreateOptions`、`ThemedWindowUi` 及其测试，再由业务窗口使用，禁止在业务窗口绕过公共接口补特殊创建逻辑。
- DPI 缩放必须由 `ThemedWindowUi`、`ThemedUi` 和公共布局/组件 helper 统一处理：窗口与界面只提供 96-DPI 逻辑尺寸，公共层负责目标显示器 DPI 换算、`WM_DPICHANGED`、字体、客户区、公共 layout metric、组件模板和注册 frame 的缩放。禁止业务窗口调用 `GetDpiForWindow`、`MulDiv` 或保存页面级 DPI 比例和缩放后常量；公共 DPI 能力不足时必须先扩展公共接口及 100%/125%/150% 测试。
- 控件外观必须优先通过统一 helper 控件消费主题 token，禁止每个界面自行设计按钮、输入框、列表、标签页等控件的独立样式。
- 输入框必须优先使用 `ThemedUi::Edit(...)` 及 `ThemedEditOptions`、`ThemedEditMode`、`ThemedEditContent` 语义接口创建，并由 `ThemedWindowUi` 注册和绘制公共输入框 frame；禁止界面直接拼接输入框视觉相关 Win32 style、手写输入框字体、margins、padding、height、radius、border、颜色或内缩矩形。确有新的输入框需求时，必须先扩展公共 options、公共 layout metric 或 `edit`/`field` 主题 token，再接入界面。
- 使用 `ThemedWindowUi` 的窗口绘制或更新输入框外框状态时，必须调用 `ThemedWindowUi::DrawRegisteredEditFrames(...)`、`ThemedWindowUi::SetEditFrameState(...)` 或其它公共托管接口；禁止在界面内直接调用 `ThemedControls::DrawFieldFrame(...)` 并自行维护输入框状态，除非该窗口尚未接入 `ThemedWindowUi` 且本次改动明确属于旧代码兼容迁移。
- Button、Edit、Label、CheckBox、Toggle、Radio、ComboBox、Tab、List、Tree、Table、Slider、ProgressBar、Link、Tooltip、MenuItem、Panel、Separator 等公共控件必须通过 `ThemedUi` 或对应公共组件 facade 创建；界面只能提供文本、图标、数据、控件 ID、语义角色和业务状态，禁止提供字体、字号、颜色、控件高度、padding、margin、gap、radius、border width、状态色、owner-draw 参数或视觉 Win32 style。
- Label 对齐必须使用 `ThemedLabelOptions`/`ThemedTextAlign`，状态文本必须使用 `ThemedStatusTextOptions`/`ThemedStatusRole`，ComboBox 下拉高度必须由公共主题 helper 决定，ListBox 选择与滚动行为必须使用 `ThemedListBoxOptions`；禁止界面向这些公共接口传递 `SS_*`、`CBS_*`、`LBS_*`、`WS_VSCROLL` 等视觉或行为 style。
- CheckBox 必须使用 `ThemedCheckBoxOptions` 创建并通过 `ThemedUi::SetChecked`/`IsChecked` 更新和读取状态；TabButton 必须通过 `ThemedUi::SetTabSelected`/`IsTabSelected` 管理选中状态；ProgressBar 必须使用 `ThemedProgressBarOptions` 创建并通过 `ThemedUi::SetProgress` 更新。禁止界面传入这些控件的高度或直接调用 `BM_SETCHECK`、`ThemedControls::SetTabButtonSelected`、`ThemedControls::SetProgressBarValue`。
- Toggle、RadioButton、Slider 必须分别使用 `ThemedToggleOptions`、`ThemedRadioButtonOptions`、`ThemedSliderOptions` 和 `ThemedUi` 公共接口；Radio 分组只能使用公共 `group` 语义，Slider 范围、步长和值只能通过公共 options 与 `SetSliderValue`/`SliderValue` 管理。禁止界面自行绘制轨道、thumb、圆点或直接使用 Trackbar style/message。
- Table 必须使用 `ThemedUi::Table`、`ThemedTableColumn`、`ThemedTableRow` 和公共状态接口；禁止窗口直接使用 `WC_LISTVIEW`、`LVS_*`、`LVCOLUMN`、`LVITEM` 或 `ListView_*` 创建和维护表格。Link 文本必须使用 `ThemedUi::LinkText` 与 `ThemedLinkOptions`，禁止页面自行定义链接颜色、下划线、hover 或 hand cursor。Tooltip 必须通过 `ThemedUi::ShowTooltip/HideTooltip` 及 `ThemedTooltipOptions` 托管，禁止页面自行计算 tooltip 字体、padding、圆角、边框、尺寸和显示器避让。
- TabControl、ToolBar、GroupBox 必须分别通过 `ThemedUi::TabControl`、`ThemedUi::ToolBar`、`ThemedUi::GroupBox` 创建，并使用公共页面绑定、工具项状态和分组子控件接口；禁止应用界面直接使用 `WC_TABCONTROL`、`TCM_*`、`TabCtrl_*`、`TOOLBARCLASSNAME`、`TB_*`、`BS_GROUPBOX`，也禁止界面用 `DrawTabGroupFrame`/`DrawPanelFrame` 或散落 Button 手工模拟这些公共组件。读取操作系统已有 Toolbar（例如系统托盘检查）属于系统集成例外，但不得用于创建应用界面。
- ToolBar 图标项必须通过 `ThemedToolItem::icon`、`display` 和 `iconOwnership` 传递，图标尺寸、图文间距与溢出按钮尺寸由公共主题控制；禁止界面自行绘制图标工具项或计算工具项宽度。工具栏空间不足时必须使用公共自动溢出能力，禁止窗口手工隐藏工具项或另建视觉参数不同的备用按钮。TabControl 页内 `Ctrl+Tab` 导航必须由 `BindTabPage` 或 `BindTabPageRoot` 公共绑定提供，禁止各页面重复注册同类快捷键。
- `ThemedToolItem::iconOwnership` 为 `Borrowed` 时调用方必须保证图标在 ToolBar 使用期间有效且 ToolBar 不负责释放；为 `Transfer` 时所有权交给 ToolBar，调用方不得再次释放。多个工具项连续变化时必须使用 `BeginToolBarUpdate`/`EndToolBarUpdate`，单项和集合变化必须使用 `SetTool*`、`InsertTool`、`RemoveTool`、`MoveTool`、`SetTools`、`ClearTools`，禁止通过 `GetDlgItem` 查找或修改 ToolBar 内部按钮。
- 普通无标题容器必须通过 `ThemedUi::Panel` 创建，并通过公共内容区域、子控件绑定、状态传播和滚动接口管理；禁止界面直接创建 `STATIC` 容器、调用 `DrawPanelFrame`、设置圆角区域或手写 Panel padding。ToolBar 运行时变化必须使用公共动态查询、修改、插入、删除、重排和批量更新接口，禁止窗口直接操作内部按钮。Tab 页包含容器或多层后代控件时必须使用 `BindTabPageRoot`，所有消息循环必须先调用 `ThemedUi::PreTranslateMessage`。
- `ThemedUi::SingleLineEdit`、`ThemedUi::MultiLineEdit` 已删除，所有输入框必须使用 `ThemedUi::Edit` 和 `ThemedEditOptions`；禁止恢复允许界面传入 Win32 style 的输入框接口。只读 framed text 必须使用 `ThemedFramedTextOptions`，禁止传入 `SS_*`。
- 热键录入必须使用 `ThemedUi::HotKeyCapture` 和 `ThemedControls::WM_HOTKEY_CAPTURED` 公共通知；禁止页面用普通 Static/Edit 模拟热键控件或重复实现按键过滤。多行说明文本必须使用 `ThemedLabelLines` 公共规格。
- 窗口级快捷键处理必须优先调用 `ThemedUi::IsNativeEditShortcut(...)` 为获得焦点的 Edit/RichEdit 让路；禁止窗口抢占输入框的 `Ctrl+A/C/V/X/Y/Z`、`Ctrl+Insert` 等原生编辑快捷键。业务快捷键与原生编辑快捷键冲突时，输入框原生行为优先。
- 所有拥有独立 Win32 消息循环的窗口必须在 `IsDialogMessageW`、`TranslateMessage`、`DispatchMessageW` 之前调用 `ThemedUi::PreTranslateMessage(...)`；Tab 页面存在容器或多层后代控件时必须使用 `BindTabPageRoot`。禁止窗口自行注册或消费 `Ctrl+Tab`、`Ctrl+Shift+Tab` 实现页面切换。
- 公共控件 facade 迁移已经完成；`QuattroThemeFacadeLint` 对所有窗口实行零容忍检查，不再允许 legacy 基线。任何窗口不得直接调用受限 `ThemedControls::Create*`/`DrawFieldFrame`、已删除的 style-based Edit facade 或底层公共控件状态消息；出现新需求必须先扩展 `ThemedUi`、`ThemedWindowUi` 或公共语义 options。
- 只有用户数据本身就是视觉内容（例如颜色选择、图片、二维码、图表、画布）、操作系统强制托管且公共层无法控制的原生控件、辅助功能/高对比度覆盖、或者明确标注迁移 TODO 的旧窗口兼容代码可以例外。即使属于例外，也必须优先通过公共语义 token、palette 或专用公共组件接口传递，禁止在新界面中直接散落 RGB、字体、圆角和间距常量。
- 所有公共控件必须强制使用主题统一配置，禁止在各个界面内为同类公共控件单独定义样式、圆角、间距、字号、状态色或交互效果。
- 单个界面只允许保留显示文本、图标、业务数据、控件 ID、语义角色和业务状态；背景颜色、前景颜色、边框颜色也必须使用公共组件状态或语义 palette，除上述明确例外外，禁止界面直接传入任何视觉参数。
