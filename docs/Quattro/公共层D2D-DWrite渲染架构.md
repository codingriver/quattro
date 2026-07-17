# 公共层 D2D/DWrite 渲染架构

## 目标

Quattro 和独立的 `AppLaunchLocker.exe` 继续保留原生 HWND、键盘/IME、滚动和辅助功能行为；所有公共控件的可视绘制统一经过 `ThemedUi`/`ThemedControls`，由 `ThemedD2D` 使用 Direct2D 与 DirectWrite 完成抗锯齿几何和文本绘制。

业务窗口只传递控件 ID、布局结果、文本、图标、业务数据和语义状态，不接触 D2D 类型、画刷、字体、圆角或颜色细节。

## 分层

1. `ThemedUi` / `ThemedWindowUi`：公共 facade、DPI、窗口消息、编辑框/表格 frame、Tooltip/Toast 生命周期。
2. `ThemedControls`：owner-draw 控件的语义绘制和状态映射。
3. `ThemedD2D`：线程本地 DC render target、DWrite text format/layout、brush/stroke 缓存、设备丢失重建、px/DIP 转换。
4. `ThemedGdiFallback`：仅在 D2D 工厂不可用、设备重建失败或测试强制切换时使用的隔离应急后端。正常路径不得直接调用 GDI 视觉原语。

## DPI 规则

- Theme token 始终是 96-DPI 逻辑值。
- 公共层使用控件所属窗口的 DPI 将 metric 转为物理像素；Tab/ToolBar padding、gap、图标尺寸、ComboBox/ListBox item 高度也必须走同一转换。
- 文本使用 DWrite layout 测量和绘制，确保测量宽度与实际字形一致。
- 直线和表格网格执行像素吸附；圆角和曲线保留 per-primitive antialiasing。
- `WM_DPICHANGED` 由 `ThemedWindowUi`/主窗口处理，业务窗口不保存缩放后的常量。

## Popup

Tooltip、Toast 和主窗口待办提醒统一使用公共 `ThemedWindowUi`。这些应用内浮层必须以当前宿主 `hwnd` 为 owner，使用 `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE`，不得依赖 `WS_EX_TOPMOST` 保持在宿主窗口上方。D2D 可用时采用 `WS_EX_LAYERED + UpdateLayeredWindow` 的预乘 alpha surface；透明 surface 使用 grayscale text antialiasing，避免 ClearType 彩边。D2D 不可用时才退回非 layered 的隔离 GDI 绘制。

原生 popup 菜单统一通过 `ThemedUi::ShowPopupMenu` 跟踪。公共入口负责解析顶层 owner、正式模式的前台准备、后台验收的激活抑制、语义对齐选项以及菜单关闭后的 `WM_NULL` 收尾；业务窗口、工具栏和 Shell 菜单服务不得直接调用 `TrackPopupMenu`/`TrackPopupMenuEx`。

## 验收

- `QuattroThemedUi`、`Quattro.exe`、`AppLaunchLocker.exe` 必须编译通过。
- 公共控件可视行为在验收进程内直接托管 HWND 验证。
- 主窗口、Process Tools、Table、Toast/Tooltip 和 AppLaunchLocker 分别保存 100%、125%、150% 截图。
- 单元测试覆盖 px/DIP、DWrite 测量、fallback 切换、ComboBox/ListBox 行高、设备资源重建和 layered popup alpha。
