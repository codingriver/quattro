# 待办事项

## 交互体验(UX)

### 悬停激活视觉预告

**现状**：`mouseEnterActiveGroup` / `mouseEnterActiveTag` 配合 `activeGroupDelay` / `activeTagDelay`（[MainWindow.cpp](src/MainWindow.cpp) `WM_MOUSEMOVE` 中的 `ID_TIMER_HOVER_ACTIVATE` 逻辑）实现了鼠标悬停延时后自动切换分组/标签，但等待期间没有任何视觉反馈，用户会觉得“怎么还没切”。

**改进**：在延时等待窗口内给目标项一个渐强的高亮（例如随延时进度插值的背景色或进度指示），到达延时时再真正切换，让“悬停即将激活”变得可感知。

**涉及点**：
- 悬停激活计时器：`pendingHoverActivationKind_` / `pendingHoverActivationId_` / `hoverActivationTimerId_`
- 绘制：`DrawGroups` / `DrawTags` 中目标项的 hover 态渲染
- 需要一个动画重绘节奏（计时器周期性 `InvalidateRect`），并记录激活起始 tick 以计算进度

**优先级**：低（锦上添花，改动面相对较大，依赖动画重绘）。
