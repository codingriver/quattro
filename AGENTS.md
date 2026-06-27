# Agent Rules

## Theme Rules

- 主题只支持公共组件分类，不支持某个页面、某个窗口、某个具体控件的单独配置。
- 新增界面或调整界面时，布局必须优先采用紧凑方案，减少不必要的留白、重复容器和装饰性层级，保证信息密度和操作路径清晰。
- 新增或修改主题参数时，必须优先映射到共享组件分类，例如 `text`、`label`、`panel`、`field`、`edit`、`button`、`iconButton`、`checkbox`、`toggle`、`radio`、`comboBox`、`tabButton`、`list`、`listItem`、`scrollbar`、`slider`、`dialog`、`menuItem`、`linkItem`、`majorNavItem`、`minorNavItem`、`tooltip`、`separator`。
- 禁止新增页面级、窗口级、控件实例级主题组件名，例如 `settings.searchInput`、`linkEdit.nameField`、`searchDialog.keywordEdit`、`newGroup.nameEdit`。
- 只有当一个主题组件代表跨界面复用的组件类型时，才允许新增公共分类。
- 新增公共分类时，必须同步更新 `src/Theme.cpp` 的主题白名单、`theme/default.xml`、主题文档和相关测试。
- 控件外观必须优先通过统一 helper 控件消费主题 token，禁止每个界面自行设计按钮、输入框、列表、标签页等控件的独立样式。
- 单个界面只允许保留基础自定义信息，例如显示文本、背景颜色、前景颜色、边框颜色等必要参数；圆角、间距、字号、状态色、悬停/按下效果等其他视觉样式必须走公共组件统一风格，方便主题设计和管理。
