# Agent Rules

## Feature Rules

- 搜索功能暂时不要开发；不要新增、恢复或暴露搜索入口、搜索热键、搜索窗口或与搜索相关的用户可见配置。

## Acceptance Rules

- 每次新增需求完成后，只验收与本次需求直接相关的功能和受影响范围，不要求运行完整冒烟测试。
- 自动化测试和自动化截图验收必须默认静默后台运行：测试窗口必须使用不激活方式创建和显示，保持在其他无关程序窗口之后，不得调用 `SetForegroundWindow`、`SetActiveWindow`、`SetFocus`、`BringWindowToTop`、`SwitchToThisWindow`，不得使用 `HWND_TOPMOST`、普通 `SW_SHOW` 或其它会抢占前台、焦点、输入法、任务栏注意状态的方式。测试需要可见 HWND 时应使用 `WS_EX_NOACTIVATE`、`SW_SHOWNOACTIVATE` 和 `HWND_BOTTOM` 等后台语义；测试结束后不得改变用户原有前台窗口和输入焦点。
- 自动化测试和自动化截图验收不得读取、移动、点击或依赖真实鼠标状态，包括 `GetCursorPos`、`GetCursorInfo`、`GetMessagePos`、`SetCursorPos`、`SendInput`、`mouse_event` 等桌面输入接口。Hover、pressed、选择、拖拽等状态应在验收进程内通过固定客户区坐标消息、公共语义状态接口或专用测试 registry 驱动；Tooltip 视觉验收必须在验收进程内直接调用公共 `ShowTooltip` 并传入固定屏幕坐标，Tooltip hover 绑定行为由 `WM_MOUSEMOVE` 配合测试 registry 的单元测试验证。
- 自动化截图必须按目标窗口 HWND 捕获并验证像素有效性。普通窗口、独立对话框和 Tooltip 使用各自 HWND；原生弹出菜单使用独立 `#32768` HWND。`PrintWindow` 返回成功后仍必须检查截图不是空白、纯色或明显无效；若目标 HWND 捕获无效，应优先增加进程内离屏绘制/专用验收通道，禁止回退到桌面 `BitBlt`、`CopyFromScreen` 或通过置顶、激活窗口来截取屏幕像素，因为后台窗口可能被其它程序遮挡。
- 自动化验收不得通过真实全局键盘、系统热键、托盘点击或桌面输入触发产品功能；应使用进程内窗口消息、公共命令接口、CLI/IPC 协议或测试专用语义入口。确需验证真实桌面输入、系统托盘或操作系统合成行为的用例，必须显式标记为交互式视觉验收，默认不得运行，并要求在独立、无人使用的 Windows 交互会话中执行。
- 测试启动必须使用独立测试运行根目录、独立配置目录和测试单例标识；测试清理只能按本次测试记录的 PID 和带标记的测试目录执行，禁止按进程名结束 `Quattro.exe`，禁止读取、覆盖或删除正式版配置，禁止向正式版窗口发送测试消息。正式版与测试版并存验收必须证明正式版 PID、窗口和配置在测试前后保持不受影响。
- 后台验收应禁用与目标无关的全局热键、托盘通知、自动启动、外部程序唤起和系统级修改，避免干扰用户当前桌面；网络、注册表、计划任务、服务、文件关联等外部副作用必须使用替身、临时范围或明确的专项隔离环境，不能因“后台运行”而扩大测试权限或影响范围。
- 表现向需求（包括界面、布局、样式、视觉状态、DPI 或交互表现变更）必须进行截图验收，并保留能够证明需求达成的相关界面截图；涉及多个关键状态、窗口或 DPI 档位时，应分别截图验收。
- 纯功能向需求必须通过相关自动化测试验收；测试应覆盖本次新增或修改的行为及必要的回归边界。
- 每次公共开发完成后都必须进行相关范围的自动化验收，包括公共组件、公共 helper、公共基础设施、共享模块或公共接口的新增与修改；只需运行与本次公共改动及其直接影响范围相关的自动化测试，无需运行完整冒烟测试。
- 测试或验收检查更新安装链路、`QuattroUpdater`、`AppLaunchLocker`、Quattro 工具箱启动 `AppLaunchLocker`、独立进程启动、内嵌释放或相关 launcher/打包行为时，必须使用 `tools/build.ps1 --all`（PowerShell 参数形式为 `-All`）构建完整组件包后再测试；禁止使用默认精简构建测试这些功能，否则缺少附属 EXE 或内嵌组件的结果只能视为构建范围不满足，不能作为产品缺陷或验收结论。
- 公共控件的可视行为（如 `Table` 隔行背景、选中态、hover 态、owner-draw 效果）必须在验收进程内直接托管窗口和控件来验证，禁止依赖跨进程读取另一个 EXE 的控件状态。`LVM_GETITEMRECT`、`LVM_GETITEMTEXTW`、`LVM_SETITEMSTATE` 等携带指针参数的 Win32 控件消息不会被系统跨进程 marshaling，跨进程调用只会返回空矩形、空文本或过期状态，无法作为验收依据。共享绘制路径只需在进程内验证一次即可覆盖所有复用该 facade 的界面（例如自启动表格与设置热键表格）。
- 定位可视缺陷时应尽早在运行期绘制路径打点（记录实际 row、selected、颜色距离等），不要长时间只做静态代码阅读；同时区分“产品缺陷”与“测试用例本身不可靠”，避免因测试断言恒不可达而误判产品未修复。

## AppLaunchLocker Architecture Rules

- `AppLaunchLocker` 必须作为完全独立的 `AppLaunchLocker.exe` 构建和运行，必须拥有自己的程序入口、进程、主窗口、消息循环和生命周期。禁止将其窗口、扫描、治理、CLI 或提权逻辑实现为 `Quattro.exe` 进程内的 `BuiltinTools` 工具、模态对话框、内嵌页面或同进程插件。
- Quattro 工具箱只允许提供 `AppLaunchLocker` 的发现、启动、唤起和必要的版本/可用性提示。工具箱入口必须通过独立进程启动接口调用 `AppLaunchLocker.exe`；禁止 Quattro 主进程代为执行扫描、禁用、恢复、服务/计划任务/注册表修改、阻止规则或后台监控。
- `AppLaunchLocker.exe` 必须能够在未启动 Quattro、不持有 Quattro 主窗口句柄且不读取 Quattro 运行时状态的情况下直接运行全部 GUI、CLI、扫描、备份和恢复功能。首期可与 Quattro 同包发布，但不得把 Quattro 进程、主窗口、配置库或数据库作为运行前置条件。
- `AppLaunchLocker` 的提权和高权限操作必须限制在自身进程边界内，通过按需自提权或独立的受限辅助执行器完成。禁止为了 `AppLaunchLocker` 而提权 Quattro 主进程，也禁止由 Quattro 传递可任意执行的高权限命令。
- `AppLaunchLocker` 必须复用 Quattro 的公共界面基础设施，包括 `Theme`、`ThemedUi`、`ThemedWindowUi`、公共布局 helper、公共组件 facade、默认主题资源与对应 lint/测试，以保持与 Quattro 一致的界面、DPI 和交互规则。禁止为 `AppLaunchLocker` 复制一套私有主题类、控件 facade、布局常量或 owner-draw 实现。
- 为 `AppLaunchLocker` 共享的界面代码必须保持为可独立链接、可独立初始化的公共层，不得反向依赖 `MainWindow`、`BuiltinTools`、`PluginRegistry`、Quattro 业务模型、Quattro 单例状态或主窗口消息。若现有主题代码无法被独立 EXE 安全复用，必须先将其抽取为公共 UI 库或无 Quattro 业务依赖的共享模块，再开发 `AppLaunchLocker` 界面。
- `AppLaunchLocker` 的领域与系统治理逻辑必须与 UI 分离，并收敛到不依赖 Quattro 窗口和业务状态的独立 core 模块。GUI、CLI、提权执行器和后续 Service 必须复用同一套操作、安全校验、备份、恢复与审计接口，禁止在各入口重复实现系统修改逻辑。
- Quattro 与 `AppLaunchLocker` 之间只允许使用有版本的命令行协议或明确定义的 IPC 协议传递启动意图和必要的业务数据。禁止跨进程传递内部指针、操作对方内部子控件、依赖未文档化的私有窗口消息，或共享无版本的内存/数据库结构。
- 必须分别验证 `AppLaunchLocker.exe` 直接启动和从 Quattro 工具箱启动两条路径；关闭、崩溃、提权取消或升级 `AppLaunchLocker` 不得导致 Quattro 退出、卡死、被提权或丢失主窗口状态。新增功能验收时必须检查这一进程隔离边界。

## Theme Rules

- `src/theme/ThemedUi.h` 是应用界面公共控件接口、语义 options 和状态方法的唯一权威清单。新增或修改界面前必须先检查 `ThemedUi`、`ThemedWindowUi` 是否已有对应能力；`AGENTS.md` 只定义选型和约束，不复制可能与头文件失配的完整函数签名或默认参数。
- 公共控件必须按类型使用对应 facade：文本使用 `Label`、`StatusText`、`StatusBadge`、`FramedStatic`；操作使用 `Button`、`FooterButton`、`LinkText`；输入使用 `Edit`、`HotKeyCapture`；选择使用 `CheckBox`、`Toggle`、`RadioButton`、`ComboBox`、`ListBox`；数值使用 `Slider`、`ProgressBar`；数据使用 `Table`；容器使用 `Panel`、`GroupBox`；导航使用 `TabControl`、`TabButton`；工具项使用 `ToolBar`；浮层提示使用 `Tooltip`。
- 界面只能向公共控件传递控件 ID、位置、公共布局计算后的可用宽高、文本、图标、业务数据、语义角色和业务状态。字体、字号、颜色、边框、圆角、padding、margin、gap、模板高度、状态色、hover/pressed 效果和 owner-draw 细节必须由公共主题分类或公共 helper 提供。
- 公共控件状态必须通过对应语义接口修改：勾选使用 `SetChecked`/`IsChecked`，Tab 使用 `SetActiveTab`/`SetTabEnabled`/`SetTabVisible`，ToolBar 使用 `SetTool*` 和集合更新接口，Table 使用 `SetTable*`，Slider 使用 `SetSliderValue`，ProgressBar 使用 `SetProgress`，Panel 使用 `SetPanel*`。禁止窗口直接发送底层状态消息或操作 facade 创建的内部子控件。
- 新需求若无法由现有公共控件表达，必须先扩展 `ThemedUi` options、公共运行时状态、主题公共分类、默认主题、文档、lint 和测试，再接入业务窗口；禁止先在窗口层实现特殊版本后再补公共接口。
- 主题只支持公共组件分类，不支持某个页面、某个窗口、某个具体控件的单独配置。
- 新增界面或调整界面时，布局必须优先采用紧凑方案，减少不必要的留白、重复容器和装饰性层级，保证信息密度和操作路径清晰。
- 紧凑工具窗口布局必须优先将同一行内容作为整体分组居中对齐；需要居中时必须使用公共布局 helper（例如 `DialogLayoutMetrics::CenteredGroupX`），禁止在单个工具界面内散落手写居中坐标。

### Window Layout Rules

- 所有窗口采用 96-DPI 逻辑像素和 4px 基准网格。界面设计必须先确定文本行高、控件模板高度和布局间距三种独立语义，禁止把字体高度、控件高度和行间距混为一个数值，也禁止通过增加控件高度代替行间距。
- 公共文本行高只允许使用三档：辅助说明/次要状态使用 16，普通正文、Label、输入内容、列表正文和按钮文字使用 20，窗口内标题、分组标题和强调文本使用 24。普通多行正文行高为 20，Tooltip 等紧密多行文本的行间附加间距使用 4；禁止窗口自行设置 `textHeight`、字体像素高度或多行行距。
- 公共控件高度必须收敛到语义模板：Small 为 24，Medium 为 28，Large 为 32；纯文本行保留 20，ProgressBar 保留 16。禁止新增 22、26、30、34 等中间控件高度，也禁止业务窗口向公共控件传入高度。
- Small 24 用于 CheckBox、RadioButton、Toggle、Slider、MiniButton、CompactButton 等紧凑控件；Medium 28 用于普通 Edit、HotKeyCapture、ComboBox、Button、TabButton、ListItem、TableHeader、MenuItem、MinorNavItem 和普通 framed field；Large 32 用于 FooterButton、主要/强调操作、MajorNavItem、标题区域和需要强调的导航项。双行结果列表项使用公共 `listItem.twoLineHeight`，默认 48，禁止使用 `max(40, itemHeight + 14)` 等手写公式。
- Footer 操作必须使用 `ThemedUi::FooterButton` 或公共 Large 按钮规格，高度统一为 32；普通行内按钮使用 Medium 28，紧凑工具按钮使用 Small 24。不得因为主按钮使用强调色就默认改变其高度，按钮高度由所在布局角色决定。
- 垂直布局间距只允许使用五档非零语义值：denseGap=4、compactRowGap=6、standardRowGap=8、sectionGap=12、majorSectionGap=16。0 仅用于明确无间距或相邻分段控件。禁止把 2、5、7、10、14、18、20 等数值作为新的行间距、控件间距或分组间距；图标笔画、命中区域、内部视觉偏移等非布局参数不受此条限制。
- denseGap 4 用于图标与文字、同一控件内部元素、紧密子选项、Tooltip 行间距和列表项之间；compactRowGap 6 用于高密度工具窗口和频繁操作区域；standardRowGap 8 用于普通表单字段和普通对话框行；sectionGap 12 用于字段组、标题与内容、正文与次级操作区；majorSectionGap 16 用于独立功能区、正文与 Footer、主窗口大区块之间。
- “所有界面间距一致”指相同语义必须取得相同数值，不要求不同语义全部使用同一个数值。普通对话框默认使用 standardRowGap=8，紧凑工具窗口默认使用 compactRowGap=6；任何偏离必须先形成可跨界面复用的公共布局语义，禁止在单个窗口中增加例外值。
- 新增布局必须从 `ThemedWindowUi::ui()` 取得已经按 DPI 缩放的 `DialogLayoutMetrics` 和控件高度访问器，使用实际 `clientWidth()`/`clientHeight()`、`contentRect()`、`RowStep()`、`FooterButtonX/Y()`、`CenteredGroupX()`、`nextRowY()` 等公共结果排布。禁止业务窗口重新调用 `GetDialogLayoutMetrics` 获取未缩放值后与实际客户区混用，也禁止用固定对话框宽度计算右对齐、居中或 Footer 坐标。
- 窗口内所有位置和尺寸必须来自公共 layout metric、公共控件模板、文本测量结果、业务数据尺寸或可用客户区。业务窗口出现确有必要的 96-DPI 逻辑宽高时，必须通过 `ThemedUi::scale(...)` 或更具体的公共 helper 转换；禁止保存缩放后常量、手写比例或让一部分坐标缩放而另一部分保持未缩放。
- 普通表单采用稳定的“Label 列 + 字段列”结构：Label 使用 `dialog.labelMinWidth` 和公共测量/helper 得到统一列宽，字段从 `layout.fieldX` 开始并延伸到内容区右边界；同一字段行内的 Label、Edit、ComboBox、Button 必须按该行最高公共模板垂直居中。禁止每行使用不同 Label 起点、字段起点或临时 Y 偏移修正视觉误差。
- 同一行包含多个控件时，必须先计算整组宽度，再使用 `controlGapX` 和 `CenteredGroupX`/公共对齐 helper 放置；同组控件之间使用同一种水平 gap。禁止散落 `+1`、`+4`、固定绝对 X 坐标或逐个控件试凑居中。文本测量、自适应宽度和可用剩余宽度必须通过 `ThemedUi`/公共布局 helper 计算。
- 相邻字段行使用 `RowStep(该行最高控件高度)` 推进；最后一行之后不得额外追加 rowGap。进入下一分组时只追加 sectionGap 或 majorSectionGap，禁止同时叠加“上一行 rowGap + 新分组 sectionGap”造成重复留白，除非公共 helper 明确定义组合规则。
- GroupBox/Panel 内部排布必须从其公共内容区域和 insets 开始；GroupBox 标题高度、标题与内容间距、内容行距使用公共 `groupBox` metric 或 `ThemedFormLayout`。禁止把容器边框、标题高度或 padding 当作业务窗口常量重复计算。普通内容不应为了视觉分层重复嵌套 Panel/GroupBox。
- Footer 必须与正文形成独立操作区：正文结束到 Footer 使用公共 `footerGap`/majorSectionGap，Footer 到客户区底部使用 `footerInsetY`，按钮组由公共 helper 右对齐或整体居中。对话框内不得手写 Footer Y、按钮间距或用普通按钮高度替代 Footer 高度。
- 多行 Label、说明文字和错误信息必须按公共行高计算占用高度；条件错误行隐藏时不得保留空白行。多行 Edit 的高度必须由公共 `editHeight(ThemedEditMode::MultiLine)` 或公共可复用规格决定，禁止使用“单行高度 + 文本高度 + 临时间距”的页面公式。
- 列表、表格和结果区应优先占用剩余可用高度，顶部工具行和底部状态/Footer 固定由公共布局计算；内容超出时使用公共 Panel/List/Table 滚动能力。禁止通过增大窗口固定高度、裁切控件、隐藏操作或使用负间距解决溢出。
- MainWindow 等非对话框布局也必须遵循相同语义：列表项高度 28、双行结果项 48、列表项间距 4、网格间距 4 或 8、主要导航高度 32、次要导航高度 28、独立功能区间距 16。不同显示模式可以使用不同公共语义，但不得各自引入一套私有高度和 gap。
- 日历、图表、画布等用户数据本身属于视觉内容时，可以保留专用单元格结构，但其标题行、操作行、正文行和区块间距仍应优先映射到 24/28/32 高度模板及 4/6/8/12/16 间距体系，并通过公共 DPI helper 缩放。命中测试与绘制必须消费同一组公共计算结果，禁止绘制尺寸与点击区域分别维护常量。
- 新增或改造界面必须验证 100%、125%、150% DPI 下：Label 与字段对齐、ComboBox 与 Edit 等高、Footer 不越界、按钮组不重叠、内容区可滚动、文本不裁切。涉及公共尺寸或间距变更时，必须同步更新 `theme/default.xml`、`Theme.cpp` fallback、主题 lint、单元测试和 UI 截图验收。
- 设计评审顺序固定为：先确认信息层级和操作路径，再选择 Standard/Compact 等公共布局种类，再确定行/分组语义，最后选择公共控件；禁止先画固定坐标再倒推窗口大小。优先减少无业务价值的标题、边框、重复说明和空白，确保主操作可见、次操作就近、危险操作有明确语义状态。

- 新增或修改主题参数时，必须优先映射到共享组件分类，例如 `text`、`label`、`panel`、`field`、`edit`、`button`、`iconButton`、`checkbox`、`toggle`、`radio`、`comboBox`、`tabButton`、`list`、`listItem`、`scrollbar`、`slider`、`dialog`、`menuItem`、`linkItem`、`majorNavItem`、`minorNavItem`、`tooltip`、`separator`。
- 禁止新增页面级、窗口级、控件实例级主题组件名，例如 `settings.searchInput`、`linkEdit.nameField`、`searchDialog.keywordEdit`、`newGroup.nameEdit`。
- 只有当一个主题组件代表跨界面复用的组件类型时，才允许新增公共分类。
- 新增公共分类时，必须同步更新 `src/theme/Theme.cpp` 的主题白名单、`theme/default.xml`、主题文档和相关测试。
- 开发或调整任意界面时，布局和组件必须优先使用主题驱动的公共布局 helper 与公共组件 helper；如果现有公共布局或公共组件不支持需求，必须先扩展主题公共分类、公共布局 metric 或公共组件 helper，再接入界面，禁止在界面内部自行处理布局规则或定义控件样式细节参数。
- 文本自适应 Label 的最小宽度必须由主题公共布局参数 `dialog.labelMinWidth` 统一控制，默认值为 20；禁止在界面或调用处为文本自适应 Label 传入自定义 `minWidth`、`minLabelWidth` 等最小宽度参数。需要多行 Label 对齐时，应先通过公共布局 helper 计算统一固定宽度，再使用固定宽度 Label 接口。
- 所有使用 `ThemedWindowUi` 的窗口，必须在窗口 `Handle(...)`/WndProc 进入后、任何业务 `switch`/`WM_NOTIFY`/`WM_COMMAND` 分支之前，优先调用 `ThemedWindowUi::HandleCommonMessage(...)` 等公共消息处理入口；禁止先由业务分支消费或转发消息后再补调公共处理，避免跳过 Table header、owner-draw、公共主题状态和资源释放等公共逻辑。公共主题消息、背景绘制、Label 透明背景、owner-draw 控件绘制和资源释放禁止在单个界面内重复处理；如需新增公共消息处理，必须扩展公共处理方法。
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
- 新增或改造标签组时必须先按功能语义选择一种公共 `ThemedTabControlAppearance`，并显式设置横向或纵向方向；禁止仅凭个人视觉偏好选择样式，也禁止在窗口层混合、覆写或手绘第五种之外的页面私有标签样式。同一种功能语义在不同界面应保持相同样式；确因空间结构需要改变方向时可以保留样式，仅切换公共 orientation。
- `Standard` 用于表单内部、局部设置或紧凑工具区域中的互斥选项组，强调每一项均可点击，但不承担窗口主导航；例如重复规则、对齐方式、局部模式选择。不得用 `Standard` 模拟主要页面导航或内容容器标题栏。
- `EmphasizedSegmented` 用于窗口或功能模块的主要页面导航，需要让当前页面具有最强识别度的场景；例如设置窗口的一级分类。若标签只是筛选条件、视图模式或次级内容切换，不得使用该高强调样式。
- `MinimalUnderline` 用于信息密度较高的次级页面导航或内容分类，要求减少边框和背景噪声、主要依靠文字与指示线表达当前位置的场景。横向时指示线位于底部，纵向时位于右侧；不得用于需要明显按钮可点击感的操作型选项组。
- `SoftPill` 用于筛选范围、显示模式、内容视图、轻量分类等弱导航或状态切换，要求界面友好、低压且各项彼此独立的场景。不得作为危险操作、主要窗口导航或必须高对比确认当前位置的页面入口。
- `ConnectedTabs` 仅用于标签条与其对应内容容器直接相邻、需要明确表达“当前标签打开当前内容面板”的传统页签结构；横向标签向下连接内容，纵向标签向右连接内容。标签条与内容之间存在独立区块间距、无共同容器或切换不控制紧邻内容时，禁止使用该样式。
- 标签组方向必须服从信息结构：顶部单行导航、少量并列分类和横向空间充足时使用 `Horizontal`；类别较多、名称长度较稳定、界面具有左侧导航栏或横向空间不足时使用 `Vertical`。纵向标签必须为导航栏提供足够高度，不得压缩 28px 公共标签高度；横向标签不得通过裁切文字、减小 padding 或手写宽度解决溢出，应改用纵向布局、减少层级或调整公共容器结构。
- 横向 `equalWidth` 仅用于数量少、语义地位相同且文本长度接近的标签；纵向标签由公共组件统一填充导航栏可用宽度，不得在业务窗口逐项指定不同宽度。标签方向键、`Ctrl+Tab`、选中状态、disabled/hidden 状态和页面绑定必须继续由 `ThemedUi` 公共接口托管。
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
