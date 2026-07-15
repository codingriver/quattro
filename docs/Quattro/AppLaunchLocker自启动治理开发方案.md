# AppLaunchLocker 简化版开发方案

## 1. 产品目标

`AppLaunchLocker` 是独立的 Windows 自启动管理程序，构建产物为 `AppLaunchLocker.exe`，正式发布时作为版本化组件内嵌在 `Quattro.exe` 中。

- 可以直接运行，不依赖 Quattro 主进程、主窗口、配置库或数据库。
- Quattro 仍按单 EXE 发布；用户第一次打开“自启动管理”时，主程序才释放并启动该 EXE。
- Quattro 工具箱只负责查找、校验、释放和启动，不执行任何扫描、禁用或恢复操作。
- GUI、CLI 和管理员操作复用同一套核心逻辑。
- 界面复用 Quattro 的 `Theme`、`ThemedUi`、`ThemedWindowUi`、公共布局和组件 facade。
- 不承诺阻止所有程序启动，不替代杀毒软件或系统修复工具。
- 不提供搜索框、搜索热键、搜索窗口或搜索配置。

第一版只解决三个问题：

1. 查看 Windows 中现有的自启动入口。
2. 禁用常见且可以稳定恢复的自启动入口。
3. 查看并恢复由本工具禁用的项目。

## 2. 第一版范围

### 2.1 全部扫描来源

| 来源 | 展示 | 禁用/恢复 |
|---|---:|---:|
| 注册表 Run、RunOnce、RunOnceEx（HKCU/HKLM 及 WOW6432Node 32 位视图） | 是 | 是，仅字符串值 |
| 注册表 RunServices、RunServicesOnce（HKCU/HKLM） | 是 | 是，仅字符串值 |
| 注册表 Policies\Explorer\Run（HKCU/HKLM 双 hive） | 是 | 是，仅字符串值 |
| 注册表 Windows NT\CurrentVersion\Windows 的 Load/Run | 是 | 是，仅字符串值 |
| Active Setup Installed Components 的 StubPath | 是 | 是，备份并清除 StubPath |
| 当前用户和公共启动文件夹 | 是 | 是 |
| 自动触发的计划任务 | 是 | 是，Microsoft Windows 系统任务只读 |
| 自动启动服务 | 是 | 是，改为手动且不停止服务 |
| 驱动服务 | 是 | 否 |
| WMI 永久事件订阅 | 是 | 否 |
| Winlogon Shell、Userinit（HKCU/HKLM 双 hive） | 是 | 否 |
| Winlogon Notify 子键 | 是 | 否 |
| AppInit_DLLs（64 位与 32 位视图） | 是 | 否 |
| AppCertDLLs | 是 | 否 |
| Session Manager BootExecute | 是 | 否 |
| Session Manager KnownDLLs | 是 | 否 |
| Shell 扩展（图标叠加、右键菜单等 COM 加载项） | 是 | 否 |
| 已有 IFEO Debugger 项（64 位与 32 位视图） | 是 | 否 |

应用自身设置中的“开机启动”最终写入上述入口时会被发现。第一版不为浏览器、更新器或具体应用开发专用配置适配器。

扫描 Run、启动文件夹项时会读取 `StartupApproved` 中系统“设置→启动”开关的状态；若某项已被系统标记为禁用，界面显示“已被系统禁用”，本工具不再重复操作。第一版只读取该状态，不写入。

> 边界声明：本版覆盖上述枚举来源，力求涵盖常见持久化/自启动点，但不声称枚举 Windows 全部机制。界面中的“全部项目”指“本工具支持的全部来源”。未列入的高级或罕见机制（如 COM 劫持的全部变体、部分内核级持久化）不在扫描范围内。

### 2.2 明确不做

- 风险评分、低/中/高风险标签和自动安全结论。
- 可配置策略、允许列表和复杂规则引擎。
- IFEO 阻止规则、AppLocker、WDAC 和策略导出。
- 实时 kill、后台 Service、实时监控和系统变更通知。
- 快照差异、独立审计数据库、系统还原点和事务框架。
- 文件签名、发布者和信誉判断。
- 驱动、WMI、Winlogon（含 Notify）、AppInit、AppCert、BootExecute、KnownDLLs、Shell 扩展或 IFEO 的修改能力。
- 向 `StartupApproved` 写入开关状态（第一版只读取用于状态对齐）。
- WinRT/UWP StartupTask 的枚举与禁用（后续版本处理）。

## 3. 固定操作规则

第一版不提供策略设置页面，能力由来源类型固定决定。

### 3.1 注册表启动项

- 只操作 `REG_SZ` 和 `REG_EXPAND_SZ`。
- 标准 `Run` 键（含 HKCU/HKLM、64 位与 WOW6432Node 视图）采用系统原生 `StartupApproved` 机制禁用（方案 B1，见 §3.7），不删除原 value。
- `RunOnce`、`RunOnceEx`、`RunServices`、`RunServicesOnce`、`Policies\Explorer\Run` 及 `Windows` Load/Run 等键仍走删除 value 机制：
  - 禁用前记录 hive、key、value name、value type 和 value data。
  - 禁用时删除原 value。
  - 恢复时如果原位置已经存在同名 value，不覆盖并提示用户。

### 3.2 启动文件夹

- 支持 `.lnk`、`.url`、`.bat`、`.cmd`、`.ps1`、`.vbs`、`.js` 和 `.exe`。
- 采用系统原生 `StartupApproved\StartupFolder` 机制禁用（方案 B1，见 §3.7），不移动文件；文件保留在原位置。
- 恢复时清除 `StartupApproved` 中的禁用标记或写回原始 blob。

### 3.7 StartupApproved 原生禁用机制（方案 B1）

标准 `Run` 键与启动文件夹项复用 Windows「设置→应用→启动」和任务管理器所用的 `StartupApproved` 开关，实现非破坏性、与系统 UI 一致的禁用。

- 目标键：`HKCU/HKLM\...\CurrentVersion\Explorer\StartupApproved\Run`（32 位视图为 `Run32`）与 `...\StartupApproved\StartupFolder`。
- 存储为 12 字节 `REG_BINARY` blob：字节 0 最低位为 1 表示禁用（0x03/0x07），为 0 表示启用（0x02/0x06）；字节 4-11 为禁用时刻的 `FILETIME`，启用时为全零。
- 禁用时备份原有 blob 到恢复记录（`saOriginalBlob`/`saHadOriginal` 等字段），写入禁用 blob；原 value 与 `.lnk` 文件保持不变。
- 恢复时若存在原始 blob 则写回，否则删除该值（等价于启用）。
- 因原项目未被删除，扫描仍会枚举到它：对已由本工具禁用且系统仍标记为禁用的项目从「当前自启动」中剔除；若用户已在系统设置中重新启用，则保留在「当前自启动」并放弃过期记录（状态漂移处理）。
- 覆盖边界：仅标准 `Run` 与启动文件夹走此机制；`RunOnce/RunOnceEx/RunServices/Policies/Active Setup` 仍走各自的删除/清空机制，两套禁用机制按项目类型分派并存。

### 3.3 计划任务

- 只显示已启用且包含自动触发器的任务。
- 禁用时将 `Enabled` 设置为 false。
- 恢复时重新启用。
- `\Microsoft\Windows\` 下的系统任务只读。

### 3.4 Windows 服务

- 只将普通第三方自动服务列为可操作项目。
- Windows 目录中的服务、无明确可执行路径的服务、受保护服务和驱动服务只读。
- 禁用时只把启动类型从自动改为手动，不停止当前服务。
- 恢复时写回原启动类型和延迟自动启动状态。

### 3.5 Active Setup

- 来源为 `HKLM\SOFTWARE\Microsoft\Active Setup\Installed Components\<GUID>`（含 WOW6432Node 视图）子键的 `StubPath` 字符串值。
- 只操作 `REG_SZ` 和 `REG_EXPAND_SZ` 的 `StubPath`，不删除子键、不改动其它值。
- 禁用前记录 hive、子键路径、`StubPath` 类型和值；禁用时删除 `StubPath` 值。
- 恢复时如原位置已存在 `StubPath`，不覆盖并提示用户。
- HKLM 写入按操作触发 UAC。

### 3.6 只读诊断来源

- Winlogon Notify、AppCertDLLs、BootExecute、KnownDLLs 和 Shell 扩展一律只读，只展示名称、路径和命令。
- 界面显示“仅查看”，不提供禁用或恢复操作，也不写入恢复存储。

## 4. 简单恢复存储

只保存一个文件：

```text
%LOCALAPPDATA%\AppLaunchLocker\disabled-items.json
```

格式：

```json
{
  "version": 1,
  "items": [
    {
      "recordId": "记录 ID",
      "itemId": "启动项 ID",
      "source": "registry",
      "name": "示例程序",
      "disabledAt": "2026-07-13T10:00:00Z",
      "requiresAdmin": false,
      "original": {
        "hive": "HKCU",
        "key": "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        "valueName": "Example",
        "valueType": "1",
        "valueData": "C:\\Example.exe"
      }
    }
  ]
}
```

不同来源只保存恢复所需字段：

- 注册表：原 hive、key、名称、类型和值。
- 启动文件夹：原路径和备份路径。
- 计划任务：任务完整路径和原启用状态。
- 服务：服务名、原启动类型和延迟启动状态。

操作顺序：

1. 读取原状态并把恢复记录写入 JSON。
2. 执行禁用。
3. 操作失败时删除刚写入的记录。
4. 操作成功后直接重新读取目标状态。
5. 恢复成功后删除对应记录；失败时保留记录。

JSON 通过临时文件加替换保存。文件损坏时保留原文件、停止修改操作并显示错误，不自动重建或覆盖。

禁用时间本身就是简单操作记录。第一版不建立审计数据库；普通运行错误只写简单文本日志。

## 5. 发布、释放、进程和权限

### 5.1 单 EXE 发布与版本化释放

- `AppLaunchLocker` 与 `QuattroUpdater` 使用同一套内嵌可执行组件框架，不建立 AppLaunchLocker 私有释放逻辑。
- 构建时读取组件 PE 的 `ProductVersion`/`FileVersion`；版本必须与 `QUATTRO_VERSION` 完全一致，否则构建失败。
- 构建时记录组件 ID、文件名、版本、SHA-256、字节内容和大小，统一注册到内嵌组件目录。
- 正式发布目录和 zip 中只包含 `Quattro-<arch>.exe`，不附带顶层 `AppLaunchLocker.exe`。
- 第一次使用时释放到：

```text
<Quattro 用户配置目录>\tools\app-launch-locker\<Quattro版本>\AppLaunchLocker.exe
```

- 同版本再次使用时先校验文件大小和 SHA-256；完全一致则直接复用，不重复写盘。
- 文件缺失或内容被修改时，从主程序内嵌内容重新释放并校验。
- 不同版本使用独立目录，避免升级时覆盖仍在运行的旧进程，也不要求首次使用前预释放。
- 释放使用进程间命名锁、临时文件、落盘刷新和原子替换，多个入口同时触发时只生成一个有效文件。
- 当前版本不自动删除旧版本目录；旧版本清理作为统一组件框架的后续维护能力处理，不放入 AppLaunchLocker 业务逻辑。

### 5.2 独立进程边界

```text
Quattro.exe
    └─ 工具箱“自启动管理”
          ├─ 按组件 ID 请求统一释放器准备文件
          └─ CreateProcess 启动版本目录中的 AppLaunchLocker.exe

AppLaunchLocker.exe
    ├─ GUI
    ├─ CLI
    └─ 需要时以 runas 启动自身执行单次管理员命令
```

- GUI 默认普通权限运行。
- 当前用户注册表和当前用户启动文件夹通常不需要管理员权限。
- HKLM、公共启动文件夹、计划任务和服务修改按操作触发 UAC。
- 提权进程只执行指定的 `disable` 或 `restore` 命令，完成后退出。
- 用户取消 UAC 时不生成新的禁用记录，也不改变当前界面状态。
- 不新增管理员 Helper 和 Windows Service。

### 5.3 后续内嵌 EXE 的统一接入

新增独立 EXE 时必须复用同一流程：

1. 创建独立 CMake 可执行目标，并为目标加入与 Quattro 共用的版本资源。
2. 调用 `quattro_register_embedded_executable(TARGET ... SYMBOL ... ID ... FILE_NAME ...)` 注册组件；构建系统自动生成字节源文件并将组件加入目录。
3. 业务入口只通过稳定组件 ID 调用 `PrepareEmbeddedExecutable(...)`，取得校验后的绝对路径。
4. 使用独立进程启动接口运行该路径；组件自己的参数使用有版本的命令行协议定义。

禁止为新组件复制 PowerShell 生成器、哈希校验、临时文件替换、释放目录计算或版本判断代码。组件 ID 和文件名必须是安全的单路径段，发布版本由 `QUATTRO_VERSION` 统一控制。

## 6. CLI

```text
AppLaunchLocker.exe scan --format plain|json
AppLaunchLocker.exe list-disabled --format plain|json
AppLaunchLocker.exe disable --id <item-id>
AppLaunchLocker.exe restore --record-id <record-id>
```

- `scan` 即使部分来源读取失败，也返回其他来源并输出 warnings。
- 破坏性命令失败时返回非零退出码。
- GUI 和 CLI 调用相同的 `StartupManager`，不重复实现修改逻辑。

## 7. 界面

主界面只保留两个页面。默认显示用户能够管理的项目；需要诊断时可将“显示”切换为“全部项目”。不提供来源树、风险筛选和专家选项。

### 7.1 当前自启动

```text
┌──────────────────────────────────────────────────────────┐
│ 自启动管理                                               │
├──────────────────────────────────────────────────────────┤
│ [当前自启动]  [已禁用]                                   │
│                                                          │
│ 选择不需要开机启动的项目，然后点击“禁用”。               │
│ 显示：[可管理项目 ▼]                           [刷新]     │
│                                                          │
│ ┌──────────────────────────────────────────────────────┐ │
│ │ 名称                         来源          状态       │ │
│ ├──────────────────────────────────────────────────────┤ │
│ │ 示例更新程序                 计划任务      可禁用     │ │
│ │ 示例助手                     注册表        可禁用     │ │
│ │ 系统驱动                     驱动          仅查看     │ │
│ └──────────────────────────────────────────────────────┘ │
│                                                          │
│ 共 32 项                                                  │
│ [详情]                                          [禁用]   │
└──────────────────────────────────────────────────────────┘
```

### 7.2 已禁用

```text
┌──────────────────────────────────────────────────────────┐
│ [当前自启动]  [已禁用]                                   │
│                                                          │
│ 这里显示由本工具禁用的项目，可随时恢复。                  │
│                                                          │
│ ┌──────────────────────────────────────────────────────┐ │
│ │ 名称                    来源             禁用时间     │ │
│ ├──────────────────────────────────────────────────────┤ │
│ │ 示例更新程序            计划任务         2026-07-13  │ │
│ └──────────────────────────────────────────────────────┘ │
│                                                          │
│ [详情]                                          [恢复]   │
└──────────────────────────────────────────────────────────┘
```

### 7.3 交互

- 未选择项目时禁用“详情”“禁用”或“恢复”。
- 双击行与点击“详情”效果一致。
- 详情窗口只显示名称、来源、状态、路径、命令和原始位置。
- 禁用前只显示一次简短确认。
- 服务确认文案明确说明“改为手动启动，不会停止当前服务”。
- 只读项目显示“仅查看”，不出现无效的高级操作。
- 扫描在后台线程执行，界面显示“正在扫描”，不会因单个来源失败而中断。

## 8. 验收要求

- `AppLaunchLocker.exe` 可直接启动，也可从 Quattro 工具箱启动。
- AppLaunchLocker、QuattroUpdater 的文件版本和产品版本与 Quattro 完全一致；不一致时构建必须失败。
- 发布包中不存在外置 AppLaunchLocker；首次使用后只在版本化工具目录出现。
- 同版本第二次打开不重写已校验文件，文件损坏后能自动恢复为内嵌内容。
- AppLaunchLocker 的退出、崩溃和 UAC 取消不影响 Quattro。
- 全部来源扫描失败隔离，单个来源失败不丢失其他结果。
- 四类可操作来源可以禁用并从本工具恢复。
- 服务禁用不会调用停止服务。
- 敏感来源始终只读。
- JSON 损坏、备份缺失、恢复目标冲突时不覆盖现有系统状态。
- CLI plain/json 输出和退出码可用于脚本。
- 100%、125%、150% DPI 下表格、Tab、详情窗口和 Footer 不裁切、不重叠。
