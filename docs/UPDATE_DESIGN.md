# 检查更新与热更新设计文档

本文记录 Quattro 当前的检查更新、下载校验和自更新安装方案，并给出抽象成通用更新能力时的建议。目标是让其它 Windows 原生程序、Tauri/Rust 程序或脚本型宿主可以借鉴同一套流程，而不把 Quattro 主窗口、配置数据库或主题 UI 作为依赖。

## 目标与边界

### 目标

- 以尽量短的用户路径完成“检查新版本 -> 下载更新包 -> 校验 -> 替换当前程序 -> 重启”。
- 让检查、下载、校验逻辑可以脱离 Quattro 主窗口复用。
- 让最终替换当前 EXE 的动作由独立进程执行，避免宿主进程仍占用自身文件。
- 支持 GitHub Release 静态清单、GitHub API 清单和 GitHub 下载镜像 fallback。
- 下载完成后使用 SHA256 校验，校验失败时不得继续静默安装。

### 非目标

- 不把更新器做成通用包管理器。
- 不实现复杂差分更新、热补丁注入或进程内二进制 patch。
- 不把 Quattro 的主题 UI、主窗口模型、配置数据库强行纳入通用更新核心。
- 不让 DLL 在宿主进程内直接替换宿主 EXE。

本文中的“热更新”指桌面程序常见的自更新：下载新版本，退出当前进程，由外部更新器替换 EXE，再重启应用。它不是进程内代码热替换。

## 当前实现概览

当前实现由四层组成：

| 层级 | 现有文件 | 职责 |
| --- | --- | --- |
| 发布产物元数据 | `tools/build.ps1` | 生成 `latest.json` 和 `SHA256SUMS.txt` |
| 检查/下载服务 | `src/services/UpdateCheckService.*` | 版本比较、清单解析、镜像候选、下载、SHA256 校验 |
| 主程序 UI 编排 | `src/windows/MainWindow.cpp`、`src/windows/UpdateCheckDialog.*`、`src/windows/UpdateDownloadDialog.*` | 检查更新菜单、确认弹窗、下载进度、用户决策 |
| 安装替换进程 | `src/services/UpdateInstaller.*`、`src/updater/UpdaterMain.cpp` | 准备并启动独立更新器，等待旧进程退出，备份、替换、重启 |

### 发布阶段

构建脚本的 `Write-ReleaseMetadata` 会为正式发布产物生成：

- `dist/latest.json`
- `dist/SHA256SUMS.txt`

`latest.json` 使用静态 manifest 格式，核心字段如下：

```json
{
  "version": "1.2.3",
  "releaseUrl": "https://github.com/codingriver/quattro/releases/tag/v1.2.3",
  "notes": "",
  "checksumUrl": "https://github.com/codingriver/quattro/releases/download/v1.2.3/SHA256SUMS.txt",
  "assets": [
    {
      "name": "Quattro-x64.exe",
      "url": "https://github.com/codingriver/quattro/releases/download/v1.2.3/Quattro-x64.exe",
      "size": 12345678,
      "sha256": "..."
    }
  ]
}
```

`SHA256SUMS.txt` 每行记录：

```text
<sha256>  <file-name>
```

运行时优先使用 `latest.json` 中 asset 自带的 `sha256`。如果 asset 没有内联 `sha256`，再尝试下载 `checksumUrl` 并从 `SHA256SUMS.txt` 查找当前包的校验值。

### 检查更新阶段

`UpdateCheckService::CheckLatest` 的核心流程：

1. 根据用户配置的更新地址生成有效 manifest URL。
2. 确保 `update-mirrors.json` 存在并按当前版本刷新内置镜像配置。
3. 构建更新源候选：主 GitHub URL 在前，镜像 URL 在后。
4. 逐个下载 manifest，直到有一个源成功解析。
5. 支持两种 manifest：
   - 静态 `latest.json`。
   - GitHub Releases API 返回的 latest release JSON。
6. 根据当前架构选择资产：
   - x64: `Quattro-x64.exe` 或 `Quattro.exe`
   - x86: `Quattro-x86.exe` 或 `Quattro.exe`
7. 使用语义化数字版本比较判断是否有更新。

版本比较会忽略开头的 `v` 或 `V`，并按数字段比较，例如：

- `v1.2.10` > `1.2.9`
- `1.2` == `1.2.0`

### 下载与校验阶段

`UpdateCheckService::DownloadUpdate` 的核心流程：

1. 根据版本号和 asset 名称生成本地文件名，例如 `1.2.3-Quattro-x64.exe`。
2. 下载到临时文件 `<target>.tmp`。
3. 通过 libcurl 进行下载，支持：
   - 下载进度回调。
   - 取消回调。
   - 连接超时。
   - 低速或长时间无进展超时。
   - GitHub 镜像 fallback。
4. 下载完成后检查文件存在且大小大于 0。
5. 计算临时文件 SHA256。
6. 与 manifest 内联 sha256 或 `SHA256SUMS.txt` 中的值比对。
7. 校验通过后将临时文件 rename 为最终下载文件。

如果下载失败，会清理临时文件。取消下载返回明确错误文本，UI 层据此不弹失败警告。

### UI 编排阶段

`MainWindow::CheckForUpdates` 是 Quattro 主程序中的用户路径编排：

1. 如果已有下载任务，提示用户稍候。
2. 写入检查开始日志。
3. 调用 `UpdateCheckService::CheckLatest`。
4. 失败时记录日志并显示错误。
5. 没有更新时显示 toast。
6. 有更新时显示 `UpdateCheckDialog`，让用户选择：
   - 下载更新。
   - 打开 Release 页面。
   - 取消。
7. 用户选择下载后打开 `UpdateDownloadDialog`。
8. 下载成功但未完成 SHA256 校验时，必须再次询问用户是否继续。
9. 调用 `LaunchEmbeddedUpdater` 启动独立更新器。
10. 当前主窗口销毁，当前进程退出。

UI 层只处理用户决策、日志和窗口反馈，不应该复制下载、hash、镜像或版本解析逻辑。

### 安装替换阶段

最终文件替换由 `QuattroUpdater.exe` 完成。主程序通过 `LaunchEmbeddedUpdater` 构造命令行并启动它。

启动参数示例：

```powershell
QuattroUpdater.exe `
  --pid 1234 `
  --source "C:\Users\...\updates\1.2.3-Quattro-x64.exe" `
  --target "C:\Program Files\Quattro\Quattro.exe" `
  --backup "C:\Program Files\Quattro\Quattro.exe.bak" `
  --restart "C:\Program Files\Quattro\Quattro.exe" `
  --restart-args "--quattro-update-restart" `
  --version "1.2.3" `
  --asset "Quattro-x64.exe" `
  --asset-size 12345678 `
  --log "C:\Program Files\Quattro\logs\update.log"
```

`QuattroUpdater.exe` 的执行流程：

1. 解析命令行参数。
2. 如果提供 `--pid`，等待目标进程退出。
3. 删除旧 backup。
4. 将当前目标 EXE move 到 backup。
5. 将下载好的新版 EXE copy 到目标路径。
6. 如果复制失败，尝试把 backup 恢复回目标。
7. 启动新版程序。
8. 删除下载源文件。
9. 写入更新日志。

独立更新器不依赖 Quattro 主窗口，也不链接主题 UI。这样它可以在宿主退出后继续运行，并且避免主程序自己替换自己。

## 为什么安装阶段必须独立进程

Windows 上正在运行的 EXE 通常仍被进程映像占用。宿主进程内的 DLL 或普通函数很难安全完成“替换宿主 EXE 并重启”：

- 宿主进程未退出时，目标文件可能无法覆盖。
- DLL 跟随宿主进程生命周期，宿主退出后 DLL 也消失，无法继续复制和重启。
- 更新失败时需要独立日志和回滚逻辑。
- 对 Tauri/Rust、C#、脚本宿主来说，EXE 协议比 C++ ABI 更稳定。

因此推荐拆成：

- 检查/下载/校验：可做核心库或独立 agent。
- 安装替换：必须由独立 EXE 完成。

## 通用化建议

当前 `UpdateCheckService` 还包含一些 Quattro 专属假设。为了给其它项目复用，建议抽象为三层：

| 新组件 | 推荐形态 | 职责 |
| --- | --- | --- |
| `QuattroUpdateCore` | 静态库或内部 C++ 库 | manifest、版本比较、镜像、下载、hash 校验 |
| `QuattroUpdateAgent.exe` | 独立 EXE | 对外暴露 `check`、`download`、`verify` JSON 命令 |
| `QuattroUpdater.exe` 或 `UpdateApply.exe` | 独立 EXE | 等待进程、备份、替换、重启 |

### 需要从 Quattro 中参数化的内容

| 当前假设 | 通用化参数 |
| --- | --- |
| 当前版本来自 `QuattroVersionText()` | `currentVersion` |
| 默认 manifest URL 指向 Quattro GitHub Release | `manifestUrl` |
| asset 名称只匹配 `Quattro-x64.exe`、`Quattro-x86.exe`、`Quattro.exe` | `assetNamePatterns` |
| 下载目录位于 Quattro 用户配置目录 | `downloadDirectory` |
| User-Agent 固定为 Quattro | `userAgent` |
| GitHub 镜像配置文件固定为 `update-mirrors.json` | `mirrorConfigPath` 或 `mirrorBases` |
| 重启参数固定为 `--quattro-update-restart` | `restartArgs` |

建议核心选项：

```json
{
  "appId": "my-app",
  "appName": "My App",
  "currentVersion": "1.0.0",
  "architecture": "x64",
  "manifestUrl": "https://example.com/releases/latest/download/latest.json",
  "assetNamePatterns": ["MyApp-${arch}.exe", "MyApp.exe"],
  "downloadDirectory": "C:\\Users\\...\\AppData\\Local\\MyApp\\updates",
  "userAgent": "MyApp Update Agent/1.0",
  "mirrorBases": [],
  "requireChecksum": true
}
```

## 推荐的 EXE 协议

对其它项目，尤其是 Tauri/Rust 项目，推荐先提供独立 EXE，而不是 DLL。

### check

```powershell
UpdateAgent.exe check --options options.json --out release.json
```

输出：

```json
{
  "ok": true,
  "error": "",
  "updateAvailable": true,
  "currentVersion": "1.0.0",
  "latestVersion": "1.1.0",
  "releaseUrl": "https://example.com/releases/tag/v1.1.0",
  "releaseNotes": "",
  "source": {
    "name": "github",
    "manifestUrl": "https://example.com/releases/latest/download/latest.json",
    "mirrorBase": ""
  },
  "asset": {
    "name": "MyApp-x64.exe",
    "url": "https://example.com/releases/download/v1.1.0/MyApp-x64.exe",
    "size": 12345678,
    "sha256": "..."
  },
  "checksumUrl": "https://example.com/releases/download/v1.1.0/SHA256SUMS.txt"
}
```

### download

```powershell
UpdateAgent.exe download --options options.json --release release.json --out download.json
```

进度建议通过 stdout 输出 JSON Lines：

```jsonl
{"type":"progress","downloadedBytes":0,"totalBytes":12345678}
{"type":"progress","downloadedBytes":1048576,"totalBytes":12345678}
{"type":"done","filePath":"C:\\Users\\...\\updates\\1.1.0-MyApp-x64.exe","checksumVerified":true}
```

最终输出：

```json
{
  "ok": true,
  "error": "",
  "filePath": "C:\\Users\\...\\updates\\1.1.0-MyApp-x64.exe",
  "checksumVerified": true,
  "checksumMessage": "SHA256 校验通过。"
}
```

### apply

安装替换继续使用独立更新器：

```powershell
UpdateApply.exe `
  --pid 1234 `
  --source "C:\Users\...\updates\1.1.0-MyApp-x64.exe" `
  --target "C:\Program Files\MyApp\MyApp.exe" `
  --backup "C:\Program Files\MyApp\MyApp.exe.bak" `
  --restart "C:\Program Files\MyApp\MyApp.exe" `
  --restart-args "--updated" `
  --version "1.1.0" `
  --log "C:\Program Files\MyApp\logs\update.log"
```

## Tauri/Rust 项目接入建议

Tauri/Rust 项目优先使用独立 EXE 作为 sidecar，而不是直接加载 C++ DLL。

推荐接入方式：

1. 将 `UpdateAgent.exe` 和 `UpdateApply.exe` 随 Tauri 应用一起打包。
2. Rust 层封装 `check_update`、`download_update`、`apply_update` 命令。
3. 前端只接收 Rust 层转发的结构化状态，不直接拼接更新器命令行。
4. 下载进度通过 stdout JSON Lines 或 Rust 后端事件转发到前端。
5. 触发安装时，Rust 层启动 `UpdateApply.exe`，随后主动退出当前 Tauri 进程。

不建议 Tauri 首选 DLL 的原因：

- Rust 调 C++ DLL 需要稳定 C ABI，不能直接使用 C++ class、`std::wstring` 或 STL 容器。
- DLL 运行在 Tauri 宿主进程内，宿主进程退出后无法继续替换宿主 EXE。
- DLL 内存分配和释放必须跨语言约定，容易出现泄漏或 CRT 不匹配。
- 独立 EXE 的 JSON 协议更容易被其它语言和脚本复用。

只有当项目明确要求进程内 API、实时回调、无子进程策略或企业 SDK 分发时，才建议额外提供 DLL。即便提供 DLL，也应该导出 C ABI：

```cpp
extern "C" __declspec(dllexport)
int Update_CheckLatestJson(const wchar_t* optionsJson, wchar_t** resultJson);

extern "C" __declspec(dllexport)
int Update_DownloadJson(
    const wchar_t* optionsJson,
    const wchar_t* releaseJson,
    UpdateProgressCallback progress,
    wchar_t** resultJson);

extern "C" __declspec(dllexport)
void Update_FreeString(wchar_t* value);
```

DLL 不应该负责最终替换宿主 EXE。最终替换仍应委托独立 `UpdateApply.exe`。

## 安全与可靠性要求

### 下载安全

- 发布时必须生成 `SHA256SUMS.txt`。
- `latest.json` 中每个 asset 推荐内联 `sha256`。
- 运行时如果无法完成 SHA256 校验，默认不应静默安装。
- 文件名必须清洗，禁止把远端 asset 名称直接拼到本地路径中。
- 下载必须先写临时文件，校验通过后再 rename 成正式文件。

### 安装可靠性

- 替换目标前先备份旧版本。
- 新版本复制失败时必须尝试恢复 backup。
- 等待旧进程退出需要超时，不能无限等待。
- 日志应记录 source、target、backup、version、asset、size 和失败原因。
- 安装器不应依赖主程序 UI、数据库或复杂运行时状态。

### 兼容性

- manifest 格式需要向后兼容，新增字段不能破坏老客户端。
- CLI JSON 输出应包含 `ok` 和 `error`，调用方不用解析本地化错误文本判断成败。
- 命令行参数中的路径必须 quote。
- 版本比较规则需要固定并有单元测试。

### 镜像策略

- 主源应始终排在第一位。
- 镜像源只用于 GitHub URL 或明确支持的 URL。
- 镜像失败时继续尝试下一个候选。
- 记录实际成功的 `sourceName`、`sourceManifestUrl` 和 `mirrorBase`，下载 asset 与 checksum 时优先使用同一镜像。

## 推荐测试清单

### 核心单元测试

- `1.2.10` 大于 `1.2.9`。
- `v1.2.0` 等于 `1.2`。
- 静态 `latest.json` 能解析版本、releaseUrl、asset、sha256。
- GitHub API JSON 能解析 tag、body、asset、checksumUrl。
- 当前架构能选择正确 asset。
- 未找到当前架构 asset 时返回明确错误。
- GitHub URL 能被镜像 URL 重写，非 GitHub URL 不被镜像重写。
- `SHA256SUMS.txt` 能按 asset 名称提取 hash。

### 下载测试

- 成功下载后生成最终文件。
- 下载中断时删除 `.tmp`。
- hash 不匹配时删除 `.tmp` 并返回失败。
- 取消回调生效。
- 进度从 0 到 total 单调推进。

### 安装器测试

- 目标进程已退出时能替换成功。
- 目标进程不存在时能继续更新。
- 复制新版本失败时能恢复 backup。
- 启动参数 quote 后能处理带空格路径。
- 失败时写入日志并返回非 0 exit code。

### 集成测试

- 使用本地 HTTP 测试服务器提供 `latest.json`、asset 和 `SHA256SUMS.txt`。
- 使用独立测试根目录，禁止覆盖真实应用配置和正式 EXE。
- 自动化测试不得依赖真实鼠标、系统热键或当前桌面前台窗口。

## 推荐演进路线

1. 把 `UpdateCheckService` 中的 Quattro 专属常量抽成 `UpdateClientOptions`。
2. 新增 `QuattroUpdateCore`，只保留纯检查、下载、校验逻辑。
3. Quattro 主窗口继续使用核心库，但 UI 对话框保持在窗口层。
4. 新增 `QuattroUpdateAgent.exe`，用 JSON 协议包装核心库。
5. 保留 `QuattroUpdater.exe` 作为最终安装替换器，必要时重命名为 `UpdateApply.exe`。
6. 为 Rust/Tauri 项目提供一个轻量 Rust wrapper crate，内部调用 sidecar EXE。
7. 如果后续确实需要进程内 SDK，再额外提供 C ABI DLL，但不让 DLL 承担安装替换。

## 最小可复用方案

其它项目最小借鉴时，只需要实现以下三个文件/组件：

1. `latest.json`
   - 描述最新版本、下载地址、文件大小、SHA256。
2. `UpdateAgent.exe`
   - `check`：下载并解析 `latest.json`。
   - `download`：下载 asset 并校验 SHA256。
3. `UpdateApply.exe`
   - 等待宿主退出。
   - 备份旧 EXE。
   - 复制新 EXE。
   - 重启宿主。

宿主程序只负责：

- 提供当前版本、manifest URL、目标 EXE 路径。
- 展示检查结果和下载进度。
- 用户确认后启动安装器并退出。

这个边界最适合跨项目复用，也最适合 Tauri/Rust、C#、C++、脚本程序共享。
