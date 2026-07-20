# AppLaunchLocker 新版自启动管理与独立应用完整方案

## 0. 文档状态与使用说明

- 文档状态：**后续开发权威方案**。
- 编写日期：2026-07-18。
- 适用范围：新版“自启动管理”界面、应用聚合、扫描快照、禁用/恢复存储、Quattro 工具箱接入，以及 AppLaunchLocker 后续独立发布。
- 目标读者：后续接手开发、评审、测试、打包和发布的开发人员。
- 本文不依赖任何聊天上下文；实现前应完整阅读本文及仓库根目录 `AGENTS.md`。

当本文与以下旧文档发生冲突时，以本文为准：

- `AppLaunchLocker自启动治理开发方案.md` 中的旧主界面、旧“当前/已禁用”页面、只保存一个 JSON、版本必须永久跟随 Quattro、正式包只能内嵌等方案。
- `AppLaunchLocker自启动来源补全方案.md` 中针对旧分类界面的展示方案。
- `AppLaunchLocker-B2与广告拦截开发方案.md` 中针对旧自启动窗口的 UI 增量。

以下内容仍可作为专项资料继续参考：

- `AppLaunchLocker自启动来源补全方案.md` 中的来源枚举、注册表位置和来源安全边界。
- `AppLaunchLocker广告拦截简化版方案.md` 中的广告拦截窗口、IFEO 拦截和 `--ad-block` 模式。广告拦截与新版自启动管理并存，互不替代。
- `公共层D2D-DWrite渲染架构.md` 和根目录 `AGENTS.md` 中的公共主题、控件、DPI、Tabler 字体及后台验收规则。

本文的核心结论是：

> 从本次改造开始，将 AppLaunchLocker 视为一个完整独立应用，只是当前阶段仍可随 Quattro 分发。Quattro 工具箱中的“自启动管理”只是该独立应用的发现、启动和唤起入口，不是内嵌页面或同进程工具。

---

## 1. 最终产品定义

最终只保留一个用户可见入口名称：

```text
自启动管理
```

不增加“自启动管理2”，不保留“经典版”，不提供切换回旧界面的配置或隐藏参数。

最终形成：

```text
一个 AppLaunchLocker.exe
一个独立数据目录
一套扫描、聚合、治理和恢复核心
一套新版自启动管理界面
两种启动路径：直接启动 / Quattro 工具箱启动
```

### 1.1 AppLaunchLocker 的职责

`AppLaunchLocker.exe` 独立负责：

- 扫描 Windows 自启动和持久化来源。
- 将来源条目聚合为用户可理解的“应用”。
- 展示服务、计划任务、驱动和系统高级项。
- 读取真实启用状态并计算聚合状态。
- 禁用、恢复、备份、校验和操作审计。
- GUI、CLI、单实例、版本化 IPC 和按需提权。
- 独立读取主题资源、写入日志和维护数据格式。

必须满足：

- 未启动 Quattro 时可以直接运行全部 GUI、CLI、扫描、禁用和恢复能力。
- 不持有 Quattro 主窗口句柄作为运行前提。
- 不读取 Quattro 配置库、数据库、插件状态或运行时单例。
- Quattro 退出或崩溃后继续正常运行。
- AppLaunchLocker 退出、崩溃或取消 UAC 不影响 Quattro。

### 1.2 Quattro 的职责

Quattro 只负责：

- 在工具箱中显示“自启动管理”。
- 查找、释放或定位兼容的 `AppLaunchLocker.exe`。
- 启动新进程，或通过有版本的协议唤起已有实例。
- 在组件缺失、签名无效或版本不兼容时给出提示。

Quattro 禁止：

- 扫描自启动、服务、计划任务或驱动。
- 读取或写入 AppLaunchLocker 的 JSON。
- 代为禁用、恢复、备份或审计。
- 为 AppLaunchLocker 提权 Quattro 主进程。
- 跨进程操作 AppLaunchLocker 内部控件。
- 传递内部指针、无版本私有消息或共享业务对象。

---

## 2. 已确定的产品决策

以下内容视为已确定，不应在实现时重新退回旧方案：

1. 删除旧“左侧范围与分类 + 右侧来源明细”主界面。
2. 删除旧纯文本“项目详情”窗口。
3. 新主界面使用五个标签页：自启动项、服务、计划任务、驱动、系统高级项。
4. 不增加单独的“已禁用”标签页；每个列表的已禁用项固定排在末尾。
5. “自启动项”页每行代表一个应用，而不是一个系统入口。
6. 应用详情显示该应用的全部已识别入口及每个入口的具体信息。
7. 刷新必须发现新增、移除和状态变化。
8. 系统实时扫描结果是当前状态的权威来源；JSON 不是系统状态数据库。
9. 恢复记录、扫描快照和操作审计分别持久化。
10. AppLaunchLocker 从现在开始保持独立应用边界，同时继续作为 Quattro 工具箱工具使用。
11. 不新增搜索框、搜索热键、搜索窗口或搜索配置。
12. 驱动及高风险系统来源默认只读，除非未来另有经过安全评审的专项方案。

---

## 3. 当前代码基线与主要差距

实现前应先确认代码仍与以下基线一致；若已有后续修改，应以实际代码为准并更新本文。

### 3.1 当前可复用能力

- `src/applaunchlocker/AppLaunchLockerMain.cpp`
  - 已有独立程序入口。
  - 支持 GUI 和 CLI 分流。
  - 已有 `scan`、`list-disabled`、`disable`、`restore` 等 CLI。
  - 已有 `--ad-block` 独立窗口模式。
- `src/applaunchlocker/AppLaunchLockerCore.{h,cpp}`
  - 已有 `StartupManager`、`DisabledItemStore` 和来源扫描。
  - 已扫描注册表、启动目录、计划任务、服务、驱动、WMI、Winlogon、DLL 类来源、Shell 扩展和 IFEO 等来源。
  - 标准 Run 和启动目录已支持 `StartupApproved` 禁用/恢复。
  - 服务禁用语义为改成手动启动，不停止当前服务。
- `src/applaunchlocker/AppLaunchLockerWindow.{h,cpp}`
  - 已经是独立窗口、独立消息循环。
  - 已接入 `ThemedUi`、`ThemedWindowUi`、公共 Table 和后台扫描线程。
- `src/theme/ThemedUi.{h,cpp}`
  - 已有 TabControl、Table、Table 图片列表、操作单元格、PopupMenu、Panel、状态文本、Toast 和 Tooltip。
  - Table 第一列支持图标。
- `src/windows/MainWindow.cpp`
  - 已通过独立进程启动 `AppLaunchLocker.exe`。
- `src/domain/PluginRegistry.cpp`
  - 已存在 `quattro.builtin.app-launch-locker` 和 `app-launch-locker` 引擎。

### 3.2 当前必须修正的差距

1. 当前 `StartupItem` 是“来源条目”，没有“应用聚合”模型。
2. `canDisable/readOnly` 混合表达能力和状态，无法区分：
   - 当前已启用；
   - 被系统禁用；
   - 被本工具禁用；
   - 仅查看；
   - 扫描未知。
3. 系统禁用的 Run/启动目录项通过修改名称追加“已被系统禁用”表达，不是结构化状态。
4. 服务扫描当前主要返回自动服务；外部改成手动或禁用的相关服务可能从结果中消失。
5. 计划任务扫描当前主要返回已启用任务；外部禁用任务可能从结果中消失。
6. 计划任务“自动触发”判定范围过宽，需要区分开机、登录、会话触发与普通定时维护任务。
7. 当前主窗口以系统来源为列表粒度，普通用户难以理解。
8. 当前详情窗口只显示一段文本，无法浏览同一应用的多个入口。
9. 当前 `disabled-items.json` 是 version 1，`original` 为自由字符串映射，长期维护和迁移能力不足。
10. 当前没有跨重启扫描快照，不能可靠显示“上次完整扫描之后新增了什么”。
11. 当前 JSON 原子替换已有实现，但缺少明确的跨进程写锁和 revision 冲突控制。
12. 当前工具箱入口在 OfficialBuild 中被隐藏；若新版是正式产品功能，必须解除该限制并更新测试。

---

## 4. 目标模块架构

逻辑上应形成以下边界。文件可分阶段迁移，不要求一次性完成目录重组，但依赖方向必须成立。

```text
AppLaunchLocker.exe
│
├─ AppLaunchLockerApp
│  ├─ 程序入口
│  ├─ GUI/CLI模式分发
│  ├─ 单实例与版本化IPC
│  ├─ 日志、崩溃和退出管理
│  └─ 独立版本和产品信息
│
├─ AppLaunchLockerUi
│  ├─ 新版主窗口
│  ├─ 应用详情窗口
│  ├─ 服务/任务/高级项详情
│  ├─ 批量操作确认和结果
│  └─ 扫描状态展示
│
├─ AppLaunchLockerCore
│  ├─ 来源扫描编排
│  ├─ 目标解析与应用聚合
│  ├─ 状态计算与排序
│  ├─ 快照比较
│  ├─ 禁用/恢复/批量操作
│  ├─ 恢复记录和审计
│  └─ JSON存储与迁移
│
├─ AppLaunchLockerWindows
│  ├─ RegistryScanner
│  ├─ StartupFolderScanner
│  ├─ ServiceScanner
│  ├─ ScheduledTaskScanner
│  ├─ DriverScanner
│  ├─ AdvancedStartupScanner
│  └─ Windows操作执行器
│
└─ SharedThemeUi
   ├─ Theme
   ├─ ThemedUi
   ├─ ThemedWindowUi
   ├─ 公共布局和控件
   └─ Tabler图标公共接口
```

### 4.1 依赖方向

允许：

```text
AppLaunchLockerUi → AppLaunchLockerCore
AppLaunchLockerApp → AppLaunchLockerUi / AppLaunchLockerCore
AppLaunchLockerWindows → Windows API
AppLaunchLockerUi → SharedThemeUi
QuattroIntegration → 版本化启动协议
```

禁止：

```text
AppLaunchLockerCore → Quattro MainWindow/BuiltinTools/PluginRegistry
AppLaunchLockerUi → Quattro数据库或业务模型
Quattro → AppLaunchLocker内部扫描器或系统操作器
```

### 4.2 公共 UI 独立链接

AppLaunchLocker 必须继续复用 Quattro 公共 UI 基础设施，但这些公共模块必须可以被独立初始化和链接。AppLaunchLocker 正式独立包不得回头读取 Quattro 安装目录中的主题或字体资源。

---

## 5. 主界面设计

窗口标题建议：

```text
AppLaunchLocker 自启动管理
```

Quattro 工具箱入口名称仍为：

```text
自启动管理
```

### 5.1 标签页

固定五个一级标签页：

1. 自启动项
2. 服务
3. 计划任务
4. 驱动
5. 系统高级项

默认打开“自启动项”。不提供“全部项目”大杂烩页面，不提供“已禁用”独立页。

### 5.2 主界面草图

```text
┌──────────────────────────── AppLaunchLocker 自启动管理 ───────────────────────────┐
│ [自启动项 38] [服务 91] [计划任务 55] [驱动 450] [系统高级项 60]         [刷新] │
│ 上次刷新：14:32:08    新增 2 · 状态变化 1 · 扫描完整                            │
├──────────────────────────────────────────────────────────────────────────────────┤
│ 应用                   应用路径                       入口  状态      详情  操作   │
├──────────────────────────────────────────────────────────────────────────────────┤
│ [图标] OneDrive        C:\...\OneDrive.exe            3个   已启用    详情  禁用   │
│ [图标] Example App     C:\...\Example.exe             4个   部分禁用  详情  管理   │
│ [图标] New Tool        D:\Tools\Tool.exe               1个   新增      详情  禁用   │
│ [图标] System Helper   C:\Windows\...\Helper.exe       2个   仅查看    详情   —     │
│                                                                                    │
│ [图标] Old Client      C:\...\OldClient.exe            2个   已禁用    详情  恢复   │
├──────────────────────────────────────────────────────────────────────────────────┤
│ 共 38 个应用：启用 29 · 部分禁用 2 · 仅查看 5 · 已禁用 2                         │
└──────────────────────────────────────────────────────────────────────────────────┘
```

### 5.3 布局约束

- 使用 `ThemedWindowUi::DialogOptions(...)` 和 `CreateWindowHandle(...)`。
- WndProc 必须先调用 `ThemedWindowUi::HandleCommonMessage(...)`。
- 使用 `ThemedUi::TabControl`、`Table`、`StatusText`、`Button`、`FooterButton`、`Panel` 和公共 PopupMenu。
- 使用公共 `DialogLayoutMetrics`、模板高度和 4/6/8/12/16 间距体系。
- 不在业务窗口里自定义字体、颜色、边框、圆角、控件高度或 DPI 比例。
- 应用图标通过 Table 图片列表显示；图标获取失败使用公共通用应用图标。
- 不增加装饰性统计卡片、重复 GroupBox 或无业务价值的大面积留白。
- 不增加任何搜索入口。

---

## 6. 各标签页的数据粒度和列设计

### 6.1 自启动项

每行代表一个应用。

| 列 | 内容 |
|---|---|
| 应用 | 应用图标 + 应用显示名 |
| 应用路径 | 规范化后的目标路径；无法解析时显示可理解的原始目标 |
| 入口 | `N 个`，可点击打开详情 |
| 状态 | 新增、已启用、部分禁用、已禁用、仅查看、状态未知 |
| 详情 | 打开应用详情 |
| 操作 | 禁用、恢复、管理或不可操作 |

禁止用单一 Toggle 或 CheckBox 表示应用状态，因为一个应用可能同时拥有多个不同状态的入口。

### 6.2 服务

每行代表一个服务，不按 `svchost.exe` 聚合。

| 列 | 内容 |
|---|---|
| 服务 | 显示名，详情中同时显示服务名 |
| 程序路径 | BinaryPath 或服务 DLL 的可解析目标 |
| 启动类型 | 自动、延迟自动、手动、禁用、启动/系统/引导 |
| 运行状态 | 运行中、已停止、暂停、状态未知 |
| 状态 | 可管理、仅查看、由本工具改为手动、外部状态变化 |
| 详情 | 打开结构化详情 |
| 操作 | 改为手动、恢复自动或不可操作 |

“禁用自启动”对服务的语义是“改为手动启动”，不得停止当前服务。

### 6.3 计划任务

每行代表一个计划任务。

| 列 | 内容 |
|---|---|
| 任务 | 任务名称，详情中显示完整任务路径 |
| 程序/命令 | ExecAction 路径和参数；多操作任务标记为“多个操作” |
| 触发器 | 开机、登录、会话、定时等摘要 |
| 状态 | 已启用、已禁用、运行中、只读、状态未知 |
| 详情 | 打开任务详情 |
| 操作 | 禁用、恢复或不可操作 |

计划任务页应优先展示与自动运行相关的任务。不能继续把所有非 Custom Trigger 都简单等同为开机自启动。至少需要结构化记录触发器类型，使 UI 能区分：

- Boot；
- Logon；
- SessionStateChange；
- Registration；
- Event；
- Time/Daily/Weekly/Monthly；
- Idle；
- 其它。

“自启动项”应用聚合默认只关联目标明确且具有开机、登录或会话类触发器的任务；普通定时维护任务保留在计划任务页，不应误导为开机自启动。

### 6.4 驱动

每行代表一个驱动服务。

| 列 | 内容 |
|---|---|
| 驱动 | 驱动显示名/服务名 |
| 驱动路径 | 解析后的驱动文件路径 |
| 启动类型 | Boot、System、Auto、Demand、Disabled |
| 状态 | 运行状态、签名/系统属性摘要 |
| 详情 | 打开详情 |
| 操作 | 第一阶段只读 |

驱动数量通常远高于其他来源，因此单独成页。没有经过专项安全设计、备份和恢复验证前，不开放禁用。

### 6.5 系统高级项

包括：

- WMI 永久订阅；
- Winlogon Shell/Userinit；
- Winlogon Notify；
- AppInit DLL；
- AppCert DLL；
- BootExecute；
- KnownDLL；
- Shell 扩展；
- 第三方 IFEO Debugger；
- 后续增加的高风险系统来源。

| 列 | 内容 |
|---|---|
| 项目 | 名称 |
| 来源 | WMI、Winlogon、KnownDLL 等 |
| 路径/命令 | 可解析路径或完整命令摘要 |
| 状态 | 仅查看、异常、状态未知 |
| 详情 | 打开来源详情 |
| 操作 | 默认不可操作 |

允许提供“来源”下拉筛选，但不允许提供搜索。

---

## 7. 结构化详情设计

旧纯文本详情窗口删除，改为“入口列表 + 当前入口具体信息”。

### 7.1 应用详情草图

```text
┌──────────────────────── 应用自启动详情 ─────────────────────────┐
│ [图标] Example App                    状态：部分禁用             │
│ 路径：C:\Program Files\Example\Example.exe                      │
│ 发布者：Example Corporation                                    │
│ 共发现 4 个入口：启用 2 · 禁用 1 · 仅查看 1                    │
├──────────────────────────────────────────────────────────────────┤
│ 来源       范围       入口名称              状态        操作     │
│ 注册表     当前用户   Example               已启用      [禁用]   │
│ 启动目录   当前用户   Example.lnk           已禁用      [恢复]   │
│ 服务       系统       Example Update        已启用      [改手动] │
│ 计划任务   系统       \Example\Update       仅查看         —     │
├──────────────────────────────────────────────────────────────────┤
│ 当前入口详细信息                                                │
│ 注册表位置：HKCU\Software\Microsoft\Windows\CurrentVersion\Run  │
│ 值名称：Example                                                 │
│ 命令："C:\Program Files\Example\Example.exe" --background       │
│ 系统启动开关：启用                                              │
├──────────────────────────────────────────────────────────────────┤
│                                               [复制详情] [关闭] │
└──────────────────────────────────────────────────────────────────┘
```

### 7.2 各来源详情字段

注册表：

- HKCU/HKLM；
- 注册表键路径；
- 值名称；
- 值类型；
- 32/64 位视图；
- 完整命令；
- StartupApproved 位置和状态；
- 是否需要管理员权限。

启动目录：

- 当前用户/所有用户；
- 启动目录文件路径；
- 快捷方式目标；
- 参数；
- 工作目录；
- StartupApproved 状态。

服务：

- 服务名和显示名；
- 启动类型；
- 是否延迟自动启动；
- 当前运行状态；
- 登录账户；
- 二进制路径；
- 服务 DLL（适用时）；
- 是否受保护、是否系统服务、只读原因。

计划任务：

- 完整任务路径；
- Enabled 状态；
- 当前任务状态；
- 运行账户和权限级别；
- 全部触发器；
- 全部操作；
- 上次/下次运行时间（可安全读取时）；
- 只读原因。

高级项：

- 准确来源类型；
- 注册表或 WMI 位置；
- 32/64 位视图；
- DLL、CLSID、命令或脚本文本；
- 只读安全原因。

---

## 8. 右键菜单和操作入口

### 8.1 自启动项

```text
详情
────────────────
禁用全部可管理入口
恢复全部已禁用入口
管理各启动入口...
────────────────
复制应用路径
复制启动信息
打开文件所在位置
文件属性
```

### 8.2 服务

```text
详情
改为手动启动 / 恢复自动启动
────────────────
复制服务名
复制程序路径
打开文件所在位置
```

### 8.3 计划任务

```text
详情
禁用任务 / 恢复任务
────────────────
复制任务路径
复制执行命令
打开程序所在位置
```

### 8.4 驱动和系统高级项

第一阶段只提供：

```text
详情
复制名称
复制路径/命令
打开文件所在位置（目标存在时）
```

所有菜单必须使用公共主题菜单接口。自动化测试不得真实打开资源管理器或文件属性窗口，应使用替身记录启动意图。

---

## 9. 领域模型设计

现有 `StartupItem` 可以分阶段迁移，但最终需要区分“原始入口”和“聚合应用”。建议模型如下。

### 9.1 原始入口

```cpp
enum class StartupEntryState {
    Enabled,
    DisabledBySystem,
    DisabledByAppLaunchLocker,
    Missing,
    Unknown,
};

enum class StartupManagementCapability {
    Manageable,
    RequiresElevation,
    ReadOnlySystem,
    ReadOnlyProtected,
    Unsupported,
};

struct StartupEntry {
    std::wstring entryId;          // 来源原生稳定标识
    StartupSourceType source;
    std::wstring displayName;
    std::wstring location;
    std::wstring command;
    std::wstring resolvedTarget;
    std::wstring appIdentity;
    StartupEntryState state;
    StartupManagementCapability capability;
    bool requiresAdmin;
    std::map<std::wstring, TypedValue> details;
};
```

`TypedValue` 不应长期退化为全字符串 map；至少 JSON 持久化恢复字段应保持 bool、number、string、array 和 object 类型。

### 9.2 应用聚合

```cpp
enum class StartupApplicationState {
    New,
    Enabled,
    PartiallyDisabled,
    Disabled,
    ReadOnly,
    Unknown,
};

struct StartupApplication {
    std::wstring appId;
    std::wstring appIdentity;
    std::wstring displayName;
    std::wstring targetPath;
    std::wstring publisher;
    StartupApplicationState state;
    std::vector<std::wstring> entryIds;
};
```

### 9.3 扫描结果和差异

```cpp
struct StartupScanResult {
    std::vector<StartupEntry> entries;
    std::vector<StartupApplication> applications;
    std::vector<ScanWarning> warnings;
    bool complete;
    std::wstring scanId;
    std::wstring capturedAt;
};

struct StartupSnapshotDiff {
    std::vector<std::wstring> addedEntryIds;
    std::vector<std::wstring> removedEntryIds;
    std::vector<std::wstring> changedEntryIds;
};
```

扫描器只产生来源入口；目标解析、应用聚合、状态计算、排序和快照比较属于 core，不属于窗口。

---

## 10. 稳定标识和应用聚合规则

### 10.1 入口 ID

入口 ID 必须基于来源原生身份，不应基于可变显示名称：

| 来源 | 入口身份 |
|---|---|
| 注册表 | hive + view + key + valueName |
| 启动目录 | Known Folder 范围 + 规范化文件路径 |
| 服务/驱动 | serviceName |
| 计划任务 | 完整 taskPath |
| Active Setup | hive + view + component key + valueName |
| WMI | namespace + class + relPath |
| Winlogon/DLL类 | hive + view + key + valueName/subkey |
| Shell扩展 | hive + handler location + CLSID |
| IFEO | view + imageName + debugger value |

显示名或命令变化时应被识别为“状态/内容变化”，不能误判成完全不同的入口。

### 10.2 应用身份

应用聚合优先级：

1. 规范化的实际目标文件路径。
2. 解析后的脚本路径加宿主类型。
3. 可验证的包/应用身份（未来 UWP 支持）。
4. 无法解析时使用来源特定身份，不跨来源强行聚合。

规范化至少包括：

- 展开环境变量；
- 去除外围引号；
- 分离可执行文件与参数；
- 统一路径分隔符；
- 使用不区分大小写的比较键；
- 在安全且不会阻塞时解析快捷方式目标；
- 不把显示名当作唯一身份。

### 10.3 必须处理的边界

- `svchost.exe`：不能把所有服务聚成一个应用；服务以 serviceName 为身份。只有能安全解析 ServiceDll 时，才可作为应用详情的关联入口。
- `powershell.exe/script.exe`：按脚本文件路径聚合，不能把所有 PowerShell 启动项合并。
- `cmd.exe /c script.cmd`、`wscript.exe script.vbs`：提取脚本路径。
- 多 ExecAction 计划任务：任务保持独立；只有单一目标明确时才关联应用。
- `.lnk`：以解析后的目标聚合，原快捷方式路径仍保留为入口详情。
- `.url`：如果目标是网页，不伪装成本地应用；显示为网页启动入口。
- 无引号且带空格的服务命令：必须使用可靠解析策略，不能简单取第一个空格前文本。
- 目标不存在：入口仍显示，状态标记异常/缺失，不能直接丢弃。
- 同一 EXE 不同参数：默认聚为同一应用，但详情保留每条命令；未来如参数代表完全不同产品，再增加经过评审的分组策略。

---

## 11. 状态、排序与“已禁用在最后”

### 11.1 入口状态

- `Enabled`：系统当前确认启用。
- `DisabledBySystem`：系统开关或来源本身显示禁用，但不是由本工具恢复记录独占管理。
- `DisabledByAppLaunchLocker`：存在有效恢复记录且系统状态核验为禁用。
- `Missing`：恢复记录对应的系统入口已不存在，或目标文件缺失。
- `Unknown`：来源扫描失败或权限不足，不能可靠判断。

### 11.2 应用状态计算

- 全部可管理入口均启用：`已启用`。
- 同时存在启用和禁用入口：`部分禁用`。
- 全部可管理入口均禁用：`已禁用`。
- 没有可管理入口且状态均可读：`仅查看`。
- 关键入口状态未知：`状态未知`，不得误判已禁用。
- 本次与上一个成功快照比较出现新增入口：额外显示 `新增` 标识。

### 11.3 固定排序

主排序分组：

```text
新增且启用
已启用
部分禁用
状态未知
仅查看
已禁用
```

组内按本地化显示名排序，名称相同时按稳定 ID 排序。

服务、计划任务和其他标签页同样把确认已禁用或由本工具改为非自动状态的项目放在末尾。只读不是禁用，不能混入已禁用分组。

---

## 12. 刷新、扫描快照和变化检测

### 12.1 刷新流程

```text
开始异步扫描
  ↓
逐来源扫描并收集warning
  ↓
对账恢复记录和系统真实状态
  ↓
解析目标并聚合应用
  ↓
与上一次成功快照比较
  ↓
计算新增、移除、状态变化
  ↓
更新界面并恢复选中/滚动状态
  ↓
只有完整成功时保存新快照
```

### 12.2 行为要求

- 首次运行没有历史快照时，不把全部项目标记为新增。
- 刷新过程中禁用重复刷新、禁用和恢复操作。
- 保持当前标签页。
- 如果原选中入口仍存在，保持选中行和滚动位置。
- 移除项不保留幽灵行，只在摘要和审计中显示数量。
- 新增标识保留到下一次完整刷新或本次窗口生命周期结束。
- 任一关键来源扫描失败时，快照不得覆盖上一次完整快照。
- 扫描 warning 必须写日志并在状态栏显示“扫描不完整”。
- 操作完成后必须重新扫描，不允许只修改内存状态假装成功。

---

## 13. 禁用、恢复和批量操作

### 13.1 操作必须位于 core

应用级操作不能由 UI 循环调用单项接口。必须提供 core 批量接口，使 GUI、CLI 和提权执行器复用相同逻辑。

建议接口语义：

```cpp
BatchOperationResult DisableEntries(const std::vector<std::wstring>& entryIds);
BatchOperationResult RestoreRecords(const std::vector<std::wstring>& recordIds);
BatchOperationResult DisableApplication(const std::wstring& appId);
BatchOperationResult RestoreApplication(const std::wstring& appId);
```

### 13.2 操作前验证

每次修改前必须：

1. 重新读取目标来源的当前状态。
2. 用来源原生身份确认目标仍是同一入口。
3. 检查恢复记录是否冲突。
4. 检查能力、保护状态和管理员权限。
5. 生成恢复数据并安全持久化。
6. 执行系统修改。
7. 验证修改后的真实状态。

### 13.3 批量结果

批量操作必须返回逐项结果：

```text
发现4个入口
可管理3个
只读1个

成功2个
失败1个
只读跳过1个
最终结果：部分完成
```

不能把部分成功显示为“应用已禁用”。操作后根据真实扫描重新计算应用状态。

### 13.4 各来源固定语义

- 标准 Run/StartupFolder：使用 StartupApproved 系统开关，保留原注册表值或文件。
- 其他可恢复注册表项：备份值后删除；恢复冲突时不覆盖。
- Active Setup：按现有经过验证的 StubPath 备份/恢复机制。
- 计划任务：修改 Enabled；保留原状态。
- 服务：自动/延迟自动改为手动，不停止服务；恢复原启动类型和延迟状态。
- 驱动和高级项：第一阶段只读。

### 13.5 回滚原则

- 恢复记录写入失败：不得执行系统修改。
- 系统修改失败：删除本次未生效的恢复记录。
- 系统修改成功但验证失败：保留恢复记录并返回“结果未确认”。
- 批量操作中途失败：对可以安全回滚的本批次已完成项执行回滚；无法安全回滚时保留逐项恢复记录并明确报告部分完成。

---

## 14. 权限与提权安全

主 GUI 始终以普通用户运行。Quattro 主进程也不得为此提权。

当前阶段可以继续使用同一个签名 EXE 的受限 CLI 提权模式，但必须收紧协议：

```text
普通 GUI
  ↓ 生成受限操作请求
AppLaunchLocker.exe runas-operation --request <request-file> --token <one-time-token>
  ↓ 验证版本、token、来源和允许动作
执行单次或单批系统修改
  ↓ 写入结构化结果并退出
```

安全要求：

- 提权入口只接受白名单动作，不接受任意命令。
- 请求中以 entryId/recordId 为主，不能只信任 UI 传入的任意路径。
- 提权进程必须重新扫描验证目标。
- 请求文件位于当前用户独立数据目录，具有一次性 token 和过期时间。
- 结果文件必须关联 requestId。
- 用户取消 UAC 不得创建禁用记录或改变 UI 状态。
- 后续如拆出 `AppLaunchLockerElevated.exe`，仍复用相同版本化协议和 core 执行接口。

---

## 15. JSON 持久化设计

### 15.1 数据根目录

安装、随 Quattro 分发和后期独立发布都使用同一独立目录：

```text
%LOCALAPPDATA%\AppLaunchLocker\
```

禁止使用 Quattro 配置目录作为业务数据目录。

建议结构：

```text
AppLaunchLocker\
├─ disabled-items.json
├─ disabled-items.json.bak
├─ startup-snapshot.json
├─ operation-audit.jsonl
├─ AppLaunchLocker.log
├─ requests\
├─ disabled\
└─ icon-cache\
```

### 15.2 权威边界

| 数据 | 是否持久化 | 是否为系统真实状态权威来源 |
|---|---:|---:|
| Windows 当前入口和状态 | 快照仅用于比较 | 否，实时扫描才是权威 |
| 恢复记录 | 是 | 是，本工具恢复所需权威数据 |
| 操作审计 | 是 | 是，记录本工具执行历史 |
| 应用聚合结果 | 否 | 否，每次扫描重算 |
| UI排序和状态 | 否 | 否，每次重算 |
| HICON/位图 | 不写JSON | 否 |

### 15.3 `disabled-items.json` version 2

现有 version 1 必须兼容迁移。建议新格式：

```json
{
  "schemaVersion": 2,
  "revision": 12,
  "updatedAt": "2026-07-18T14:32:08Z",
  "records": [
    {
      "recordId": "rec_xxx",
      "entryId": "registry_xxx",
      "appIdentity": "path:c:\\program files\\example\\example.exe",
      "source": "registry",
      "displayName": "Example App",
      "disabledAt": "2026-07-18T14:30:00Z",
      "requiresAdmin": false,
      "restore": {
        "mechanism": "startup-approved",
        "hive": "HKCU",
        "key": "Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run",
        "valueName": "Example",
        "hadOriginalValue": true,
        "originalValueHex": "020000000000000000000000"
      }
    }
  ]
}
```

`restore` 应保持来源特定的类型化字段：

- 注册表：hive、view、key、valueName、valueType、valueData 或 StartupApproved 原 blob。
- 启动目录：originalPath、backupPath 或 StartupApproved 原 blob。
- 服务：serviceName、startType(number)、delayed(bool)。
- 计划任务：taskPath、wasEnabled(bool)。
- Active Setup：注册表值恢复字段。

不建议长期继续扩充自由字符串 `original` map。

### 15.4 version 1 迁移

迁移要求：

1. 读取 version 1。
2. 按 source 和 `original` 字段生成 version 2 类型化 `restore`。
3. 在内存中验证每条记录。
4. 先写 `.tmp` 并重新解析验证。
5. 原文件复制/替换为 `.bak`。
6. 原子替换正式文件。
7. 迁移失败时保留 version 1，不允许覆盖。

迁移必须有单元测试覆盖所有现有可操作来源。

### 15.5 `startup-snapshot.json`

只保存上一次完整成功扫描的精简信息：

```json
{
  "schemaVersion": 1,
  "capturedAt": "2026-07-18T14:32:08Z",
  "scanId": "scan_xxx",
  "entries": [
    {
      "entryId": "registry_xxx",
      "source": "registry",
      "appIdentity": "path:c:\\program files\\example\\example.exe",
      "displayName": "Example App",
      "targetPath": "C:\\Program Files\\Example\\Example.exe",
      "state": "enabled",
      "fingerprint": "sha256:..."
    }
  ]
}
```

快照不保存恢复密钥，不替代 `disabled-items.json`。

### 15.6 `operation-audit.jsonl`

使用追加式 JSON Lines：

```json
{"time":"2026-07-18T14:30:00Z","requestId":"req_xxx","action":"disable","entryId":"registry_xxx","source":"registry","success":true}
{"time":"2026-07-18T14:31:12Z","requestId":"req_yyy","action":"restore","entryId":"task_xxx","source":"scheduled-task","success":false,"error":"access-denied"}
```

至少记录：

- 时间；
- requestId；
- GUI/CLI/Quattro 启动来源；
- 动作；
- entryId/recordId；
- 来源；
- 是否提权；
- success/partial；
- 错误码和安全文案。

禁止记录不必要的敏感凭据或任意环境变量。

### 15.7 写入安全

- 临时文件写入后原子替换。
- `MOVEFILE_WRITE_THROUGH` 或等效落盘保证。
- 正式文件保留上一份有效 `.bak`。
- 使用当前用户范围命名互斥量或文件锁协调 GUI、CLI 和提权进程。
- 使用 `revision` 防止丢失更新。
- 写入后重新读取和 schema 校验。
- JSON 损坏时禁止自动清空覆盖。
- 多实例或并行操作必须串行化恢复存储写入。

### 15.8 图标缓存

图标不能 Base64 写进 JSON。允许使用：

```text
%LOCALAPPDATA%\AppLaunchLocker\icon-cache\
```

缓存键基于目标路径、文件时间和图标来源。缓存失败不影响扫描结果。

---

## 16. CLI 和协议

现有命令保持兼容：

```text
AppLaunchLocker.exe scan --format plain|json
AppLaunchLocker.exe list-disabled --format plain|json
AppLaunchLocker.exe disable --id <entry-id>
AppLaunchLocker.exe restore --record-id <record-id>
```

新版建议增加：

```text
AppLaunchLocker.exe scan --format json --schema-version 2
AppLaunchLocker.exe disable-application --app-id <app-id>
AppLaunchLocker.exe restore-application --app-id <app-id>
AppLaunchLocker.exe diff --format json
AppLaunchLocker.exe version --format json
```

CLI JSON 输出必须有独立 schemaVersion，不应随 UI 内部结构任意变化。

广告拦截命令和 `--ad-block` 继续按专项方案维护，不混入自启动恢复记录。

---

## 17. 单实例与版本化 IPC

### 17.1 直接启动

```text
AppLaunchLocker.exe
```

直接打开新版自启动管理主窗口。

### 17.2 Quattro 工具箱启动

```text
AppLaunchLocker.exe --launch-source quattro --protocol-version 1
```

`launch-source` 只用于日志和启动意图，不授予额外权限。

### 17.3 单实例

- GUI 使用当前用户/当前会话范围单实例。
- 已有 GUI 时，新进程通过版本化 IPC 请求已有实例显示主窗口。
- CLI 模式不受 GUI 单实例限制。
- 广告拦截是否与自启动主窗口共实例可独立决定，但不得共享未版本化私有消息。

### 17.4 IPC 安全

- 协议包含 `protocolVersion`、`requestId`、`intent`。
- 仅支持白名单启动意图，例如 `show-main`、`show-ad-block`、`select-tab`。
- 不传递 HWND、指针、控件 ID 或内部对象地址。
- 不允许 IPC 执行任意高权限系统命令。

---

## 18. Quattro 工具箱接入

### 18.1 插件标识

继续复用：

```text
id       = quattro.builtin.app-launch-locker
engine   = app-launch-locker
name     = 自启动管理
kind     = builtin-tool
```

不创建 v2 插件 ID，避免丢失已有启用状态和形成两个入口。

### 18.2 当前随 Quattro 分发

当前阶段可以继续使用嵌入释放：

```text
Quattro.exe
  └─ 内嵌 AppLaunchLocker.exe
       └─ 首次使用释放到版本化组件目录
```

但被释放的 EXE 必须：

- 可以直接运行；
- 不依赖 Quattro 正在运行；
- 使用 `%LOCALAPPDATA%\AppLaunchLocker`；
- 携带独立所需主题和资源；
- 拥有自己的日志和生命周期。

### 18.3 正式构建入口

若新版“自启动管理”是正式产品功能：

- 删除 `!QuattroIsOfficialBuild()` 对该插件入口的隐藏。
- 更新 `tests/UnitTests.cpp` 中“正式构建不含 AppLaunchLocker”的旧断言。
- 正式包必须包含或内嵌有效组件。
- 发布包不得通过构建参数绕过正式产品功能。

### 18.4 后期独立安装后的定位顺序

未来 AppLaunchLocker 有独立安装包后，Quattro 建议按以下顺序定位：

1. 已安装、签名有效且协议兼容的独立 AppLaunchLocker。
2. Quattro 随包提供的兼容版本。
3. 都不存在时提示“组件未安装或不可用”。

独立安装路径应通过明确安装清单或 Windows App Paths 发现。禁止从任意用户可写、无签名校验的注册表值启动未知 EXE。

---

## 19. 独立版本、打包和更新

### 19.1 版本分层

从本次改造开始区分：

```text
AppLaunchLocker 产品版本
启动/IPC协议版本
CLI JSON schema版本
恢复存储 schema版本
扫描快照 schema版本
```

当前随 Quattro 分发时，产品版本可以暂时与 Quattro 一致，但代码和协议不得假设永远一致。后期应允许：

```text
AppLaunchLocker 1.0.0
protocolVersion 1
disabledStore schemaVersion 2
snapshot schemaVersion 1
```

### 19.2 独立安装包目标

```text
AppLaunchLocker-Setup.exe
├─ AppLaunchLocker.exe
├─ 必要运行库
├─ 默认主题资源
├─ Tabler字体子集资源
├─ 版本/卸载信息
└─ 可选独立更新组件
```

### 19.3 更新边界

- 当前随 Quattro 分发时，可以随 Quattro 更新组件。
- 后期独立安装时，AppLaunchLocker 使用自己的安装/更新通道。
- AppLaunchLocker 运行不能依赖 QuattroUpdater。
- Quattro 可以提示版本不兼容，但不能代替 AppLaunchLocker 执行系统治理。
- 数据 schema 迁移由 AppLaunchLocker 自己负责。

### 19.4 资源和字体

正式构建必须继续遵守根目录规则：

- Tabler 图标只通过强类型 `TablerIconId` 和公共 facade 使用。
- `tools/tabler-icons.json` 是唯一图标配置源。
- 正式构建按实际 glyph 子集化并校验 cmap 和字体名称。
- 正式包不得回退完整字体。
- XPRESS 默认关闭；不要与 UPX 默认叠加双重压缩。
- AppLaunchLocker 独立正式包也必须执行同等级资源校验。

---

## 20. 预期文件改动范围

下面是实现时的建议改动表，不要求文件名完全不变，但职责必须对应。

| 文件/模块 | 预期改动 |
|---|---|
| `src/applaunchlocker/AppLaunchLockerCore.h` | 增加明确 EntryState、Capability、Application、Diff、BatchResult 等模型和接口 |
| `src/applaunchlocker/AppLaunchLockerCore.cpp` | 扫描状态补全、应用聚合、快照比较、批量操作、JSON v2 和迁移 |
| `src/applaunchlocker/AppLaunchLockerWindow.h` | 删除旧 Category 模型，改为五标签页和新版窗口状态 |
| `src/applaunchlocker/AppLaunchLockerWindow.cpp` | 完整替换旧分类界面，增加应用图标、详情、右键菜单和刷新差异 |
| `src/applaunchlocker/AppLaunchLockerMain.cpp` | 保持直接启动新版；扩展版本化CLI、launch-source和单实例协议 |
| `src/applaunchlocker/AdBlockWindow.*` | 原则上不改；只处理共享core模型迁移造成的编译适配 |
| `src/domain/PluginRegistry.cpp` | 保留原ID；正式功能时解除OfficialBuild隐藏；更新描述 |
| `src/windows/MainWindow.cpp` | 保持独立进程启动；补版本化启动参数和未来独立安装定位 |
| `src/theme/ThemedUi.*` | 仅在公共Table无法表达状态徽标/结构化操作时先扩公共能力及测试 |
| `tools/tabler-icons.json` | 新增图标引用时同步更新，禁止散落glyph |
| `CMakeLists.txt` | 新增源文件/库拆分、独立版本资源、测试目标和正式资源依赖 |
| `tests/AppLaunchLockerTests.cpp` | 聚合、状态、快照、JSON迁移、批量操作和边界单测 |
| `tests/UiScreenshotAcceptance.cpp` | 新五标签页、详情、右键菜单和DPI后台截图验收 |
| `tests/UnitTests.cpp` | 更新工具箱注册及OfficialBuild旧断言 |
| `docs/Quattro/00-文档索引.md` | 把本文列为权威方案 |

### 20.1 旧代码删除清单

确认新版覆盖后删除：

- 旧 `CategoryKind`、`CategoryEntry`。
- 旧 `categoryTable_` 左侧分类表。
- `RebuildCategories()` 和按旧来源分类切换逻辑。
- 旧“当前自启动/已禁用”主页面语义。
- 旧纯文本 `DetailsText(...)` 和旧详情窗口布局。
- 旧界面的截图基准和断言。
- 任何打开旧界面的备用入口。

删除旧 UI 不能删除：

- 现有恢复记录兼容逻辑；
- `StartupManager` 已实现的安全操作；
- 广告拦截窗口；
- CLI兼容命令；
- 独立进程入口。

---

## 21. 推荐实施顺序

### 阶段 A：领域和存储基础

1. 增加明确入口状态和能力枚举。
2. 为各来源补充真实启用/禁用状态。
3. 定义稳定 entryId 和 appIdentity。
4. 实现应用聚合纯函数。
5. 实现状态计算和固定排序纯函数。
6. 实现 JSON v2、v1 迁移、锁、revision 和 `.bak`。
7. 实现完整扫描快照和 diff。

完成标准：不改主 UI 也能通过 core/CLI 测试输出新模型。

### 阶段 B：批量治理

1. 增加逐项和应用级批量接口。
2. 实现操作前重新验证。
3. 实现逐项恢复记录和部分结果。
4. 收紧提权协议。
5. 增加审计 JSONL。

完成标准：CLI 可以对测试来源执行批量操作并准确报告部分成功。

### 阶段 C：新版 UI 完整替换

1. 实现五标签页。
2. 实现自启动项应用表和图标。
3. 实现服务、计划任务、驱动和高级项表。
4. 实现结构化详情窗口。
5. 实现右键菜单。
6. 实现刷新、差异摘要、选中和滚动保持。
7. 删除旧界面代码和旧验收。

完成标准：无任何用户路径能打开旧分类界面。

### 阶段 D：Quattro 正式接入

1. 保留原插件ID和入口名称。
2. 解除正式构建隐藏（若确认正式发布）。
3. 补版本化启动参数和单实例唤起。
4. 使用 `tools/build.ps1 -All` 构建完整组件包。
5. 验证直接启动与工具箱启动。

### 阶段 E：独立发布准备

1. 独立产品版本资源。
2. 确保主题、字体和运行库自包含。
3. 增加独立安装清单/App Paths。
4. 定义 Quattro 查找独立安装版的兼容规则。
5. 增加独立安装包和更新通道。

---

## 22. 测试和验收方案

### 22.1 Core 单元测试

必须覆盖：

- 全部 `StartupSourceType` key/text/round-trip。
- Registry、Folder、Service、Task 等稳定 ID。
- EXE、快捷方式、脚本、svchost、多操作任务聚合边界。
- 启用、部分禁用、已禁用、仅查看、未知状态计算。
- 已禁用项固定排在末尾。
- 首次快照、新增、删除、内容变化、状态变化。
- 不完整扫描不覆盖快照。
- JSON version 1 到 version 2 的所有来源迁移。
- JSON 损坏、`.bak`、revision 冲突和原子写入。
- 批量操作全部成功、部分成功、回滚和结果未确认。
- 服务操作不停止服务。
- 只读来源不能进入修改分派。

真实 HKLM、服务和计划任务修改不应在普通单元测试中执行，使用替身或隔离专项环境。

### 22.2 CLI 测试

- `scan --format json` schema 稳定。
- warnings 不丢失其他来源。
- 非法参数返回非零退出码。
- 批量结果和逐项错误可解析。
- GUI/CLI 共用同一 core，不出现两套实现漂移。

### 22.3 UI 截图验收

必须按 100%、125%、150% DPI 保存截图：

- 自启动项：启用、部分禁用、仅查看、已禁用、新增。
- 服务页。
- 计划任务页。
- 驱动页。
- 系统高级项页及来源筛选。
- 应用详情。
- 服务/任务详情。
- 右键菜单。
- 扫描中、扫描完整、扫描不完整。
- 空列表和长路径。
- 已禁用项位于末尾。

GUI 自动化必须遵守根目录后台测试规则：不激活、不置顶、不读取真实输入、不打开外部 GUI，以目标 HWND 后台截图并验证有效性。

### 22.4 进程隔离验收

分别验证：

1. 直接启动 `AppLaunchLocker.exe`。
2. 从 Quattro 工具箱启动。
3. 已运行时再次从工具箱唤起。
4. Quattro 退出后 AppLaunchLocker 继续运行。
5. AppLaunchLocker 正常关闭不影响 Quattro。
6. AppLaunchLocker 崩溃不影响 Quattro。
7. UAC 取消不影响两个主进程。
8. AppLaunchLocker 不读取 Quattro 正式配置。
9. 测试版和正式版并存时 PID、窗口和配置互不影响。

工具箱启动、内嵌释放、独立进程和完整组件链路必须使用：

```powershell
tools/build.ps1 -All
```

默认精简构建不能作为该链路的验收结论。

### 22.5 正式资源验收

- Tabler JSON 覆盖扫描通过。
- 正式字体子集生成和 cmap 校验通过。
- AppLaunchLocker 正式包不回退完整字体。
- XPRESS 未默认启用。
- 直接启动和 Quattro 启动均能加载主题和图标。

---

## 23. 风险和防错要求

### 23.1 把缓存当真实状态

风险：外部工具修改服务、任务或 StartupApproved 后界面错误。

要求：所有显示和操作前以实时扫描为准；快照只用于 diff。

### 23.2 应用误聚合

风险：把多个服务都聚为 svchost，或把所有脚本聚为 PowerShell。

要求：遵守本文稳定身份规则；无法确定时宁可不聚合。

### 23.3 批量操作误报

风险：部分入口失败但应用显示已禁用。

要求：逐项结果 + 操作后重新扫描 + 聚合状态重算。

### 23.4 恢复记录损坏

风险：系统已修改但无法恢复。

要求：先安全保存恢复记录，再修改系统；使用锁、revision、`.bak` 和写后校验。

### 23.5 旧文档误导

风险：后续开发按旧“两页分类界面”继续扩展。

要求：文档索引把本文标为权威方案；实现评审首先核对本文第 2 节。

### 23.6 独立发布时反向依赖 Quattro

风险：独立安装后缺主题、版本、配置或更新能力。

要求：从当前阶段保持数据、版本、资源和生命周期独立；Quattro 只做 launcher。

---

## 24. 完成定义

只有同时满足以下条件，才能认为新版自启动管理完成：

- 工具箱只显示一个“自启动管理”入口。
- 无任何路径可打开旧分类界面。
- `AppLaunchLocker.exe` 无参数直接打开新版五标签页界面。
- 自启动项每行代表应用，并能显示全部关联入口详情。
- 服务、计划任务、驱动和系统高级项具有独立页面。
- 已禁用项目在对应列表末尾。
- 刷新能显示新增和状态变化，且不完整扫描不覆盖快照。
- JSON version 1 恢复记录无损迁移并仍可恢复。
- 批量操作正确处理只读项、部分失败和回滚。
- Quattro 不执行任何治理逻辑，也不被 AppLaunchLocker 提权。
- 直接启动和工具箱启动两条路径均通过隔离验收。
- 100%、125%、150% DPI 截图验收通过。
- 完整组件包使用 `tools/build.ps1 -All` 验收通过。
- AppLaunchLocker 的数据、主题资源和运行生命周期已具备后续独立发布条件。

---

## 25. 最终架构摘要

```text
用户直接启动                    用户从 Quattro 工具箱启动
      │                                  │
      └──────────────┬───────────────────┘
                     ▼
             AppLaunchLocker.exe
                     │
       ┌─────────────┼─────────────┐
       ▼             ▼             ▼
   新版GUI         CLI/IPC       按需提权执行
       │             │             │
       └─────────────┴─────────────┘
                     ▼
            AppLaunchLockerCore
                     │
       ┌─────────────┼─────────────┐
       ▼             ▼             ▼
   Windows实时扫描   JSON恢复/快照   禁用/恢复/审计
```

实现应始终保持“一套独立应用核心、两个启动入口、零 Quattro 业务依赖”。这样后期从“随 Quattro 分发的工具箱工具”升级为“独立安装的 App”时，只需要调整发现、安装和更新方式，不需要重写界面、数据、权限或治理架构。
