# Agent Rules

## Theme Rules

- 主题只支持公共组件分类，不支持某个页面、某个窗口、某个具体控件的单独配置。
- 新增或修改主题参数时，必须优先映射到共享组件分类，例如 `text`、`label`、`panel`、`field`、`edit`、`button`、`iconButton`、`checkbox`、`toggle`、`radio`、`comboBox`、`tabButton`、`list`、`listItem`、`scrollbar`、`slider`、`dialog`、`menuItem`、`linkItem`、`majorNavItem`、`minorNavItem`、`tooltip`、`separator`。
- 禁止新增页面级、窗口级、控件实例级主题组件名，例如 `settings.searchInput`、`linkEdit.nameField`、`searchDialog.keywordEdit`、`newGroup.nameEdit`。
- 只有当一个主题组件代表跨界面复用的组件类型时，才允许新增公共分类。
- 新增公共分类时，必须同步更新 `src/Theme.cpp` 的主题白名单、`theme/default.xml`、主题文档和相关测试。
- 控件外观应通过统一 helper 控件消费主题 token，避免在单个界面内硬编码独立视觉参数。
