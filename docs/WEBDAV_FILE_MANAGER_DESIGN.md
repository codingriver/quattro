# WebDAV 文件管理设计文档

本文从 Quattro 当前 WebDAV 文件管理模块抽象出一套可独立实现的设计。目标是：不依赖 Quattro 主窗口、主题 UI、配置格式或数据库，仅根据本文即可在其它桌面程序、命令行工具或服务端管理后台中实现同等的 WebDAV 文件管理能力。

## 目标与边界

### 目标

- 管理上传到 WebDAV 的本地文件快照，支持远端清单刷新、本地状态识别、上传、下载、删除、批量确认和传输队列。
- 使用稳定的远端记录目录和元数据文件描述每个文件，避免只依赖 WebDAV 目录文件名。
- 支持异常记录可见化：远端目录存在但元数据缺失、元数据无效或读取失败时，仍在文件管理中展示并允许查看详情或删除。
- 上传和下载都带进度、停止、校验和明确的失败反馈。
- 刷新清单时优先保留本地缓存和已有表格内容，再做后台增量更新，避免界面闪烁、选择丢失或空白等待。
- 提供与 Shell 或文件管理器集成的“上传到 WebDAV”入口，并复用同一传输队列。

### 非目标

- 不实现通用双向同步客户端；这里管理的是“本地文件路径对应的一份远端快照”。
- 不做冲突自动合并；本地较新、远端较新、缺失等状态只提示用户决策。
- 不做文件夹结构镜像；上传文件夹时只递归展开普通文件，逐个记录为独立文件快照。
- 不要求 WebDAV 服务端支持自定义属性、锁、版本历史或 MOVE/COPY。
- 不把传输队列设计成系统级下载器；队列只服务 WebDAV 文件管理。

## 当前实现来源

Quattro 当前相关模块分层如下：

| 层级 | 当前文件 | 可独立实现时的职责 |
| --- | --- | --- |
| 协议客户端 | src/services/WebDavClient.* | WebDAV 请求、路径拼接、目录创建、PROPFIND 解析、PUT/GET/DELETE |
| 文件记录服务 | src/services/WebDavFileService.* | 记录 ID、元数据 JSON、上传/下载/删除、刷新枚举、本地状态判断 |
| 本地索引缓存 | src/services/WebDavFileIndexCache.* | 按账号和远端目录隔离的清单缓存 |
| 传输队列协调 | src/services/WebDavTransferCoordinator.* | 外部入口提交上传/下载/显示队列请求 |
| 传输队列执行 | src/services/WebDavTransferQueueController.* | 排队、去重、并发、停止、队列快照 |
| 文件管理窗口 | src/windows/SimpleDialogs.cpp 的 WebDavFileManagerDialog | 表格展示、刷新、批量确认、上传/下载/删除入口 |
| 详情/确认窗口 | src/windows/WebDavFileDetailsDialog.*、src/windows/WebDavFileBatchConfirmDialog.* | 详情展示、批量操作确认 |
| 队列 UI | src/theme/ThemedFileTransferQueueDialog.* | 总进度、当前项进度、传输历史、停止/清空 |
| Shell 入口 | src/services/ExplorerWebDavUploadContextMenuService.*、src/main.cpp | 文件管理器右键上传入口和命令行转发 |

独立实现时可以替换 UI、线程库、HTTP 库、凭据存储和 IPC 机制，但应保留本文的数据模型、远端目录结构、状态机和失败处理语义。

## 总体架构

建议拆成 7 个模块：

| 模块 | 主要职责 |
| --- | --- |
| WebDavConfig | 保存启用状态、服务地址、用户名、文件目录、备份目录、保留数量等配置 |
| CredentialStore | 保存、读取、删除 WebDAV 密码或应用密码，避免明文写入普通配置 |
| WebDavClient | 封装 HTTP/WebDAV 方法：MKCOL、PROPFIND、PUT、GET、DELETE |
| WebDavFileRepository | 负责远端记录格式、枚举、上传、下载、删除、校验、本地状态判断 |
| WebDavFileIndexCache | 保存最近一次成功或部分成功的远端清单，按账号和目录隔离 |
| TransferQueue | 接收上传和下载任务，执行进度、停止、失败记录和历史展示 |
| FileManagerPresenter | 组织界面或 API：加载缓存、后台刷新、增量更新、批量确认、操作分发 |

推荐依赖方向：

1. FileManagerPresenter 依赖 WebDavFileRepository、WebDavFileIndexCache、TransferQueue。
2. TransferQueue 依赖 WebDavFileRepository。
3. WebDavFileRepository 依赖 WebDavClient、CredentialStore 和本地文件系统。
4. WebDavClient 只依赖 HTTP 库和配置，不依赖 UI。

## 配置与凭据

### 配置字段

最小配置模型：

| 字段 | 类型 | 默认值 | 说明 |
| --- | --- | --- | --- |
| enabled | bool | false | 是否启用 WebDAV 功能 |
| url | string | 空 | WebDAV 服务根地址，例如 https://example.com/dav |
| userName | string | 空 | 登录用户名 |
| remoteRootPath | string | /Quattro/ | 兼容旧配置的根目录，可选 |
| backupPath | string | /Quattro/backups/ | 备份文件目录，文件管理可不使用 |
| filesPath | string | /Quattro/files/ | 文件管理的远端根目录 |
| keepCount | int | 10 | 备份保留数量，文件管理可不使用 |
| lastSyncAt | string | 空 | 最近一次备份同步时间，文件管理可不使用 |
| registerUploadContextMenu | bool | false | 是否注册文件管理器右键上传入口 |

目录配置应归一化：

- 去掉首尾空白。
- 将反斜杠转为正斜杠。
- 补齐开头正斜杠。
- 折叠重复正斜杠。
- 补齐末尾正斜杠。
- 空值回退到默认值。

### 凭据存储

密码或应用密码必须由系统凭据仓库保存：

- Windows：Credential Manager，Generic Credential。
- macOS：Keychain。
- Linux：Secret Service、libsecret 或受保护的系统密钥环。

凭据键建议至少包含应用标识。若一个应用只维护一个 WebDAV 账号，可以使用固定键；若支持多账号，应使用 lower(url) + 竖线 + lower(userName) 的哈希作为键的一部分。

读取密码失败时不要继续发起网络请求；用户可见错误为“WebDAV 密码未配置”或“读取 WebDAV 密码失败”。详细系统错误写入日志。

## WebDAV 协议层

### 基本请求

协议层需要支持：

| 方法 | 用途 | 成功状态码 |
| --- | --- | --- |
| PROPFIND + Depth: 1 | 列出目录直接子项 | 207 Multi-Status，兼容 200 |
| MKCOL | 创建目录 | 201 Created；目录已存在可接受 405 Method Not Allowed |
| PUT | 上传 metadata.json 或 content | 任意 2xx |
| GET | 下载 metadata.json 或 content | 200 OK |
| DELETE 文件 | 删除 content 或 metadata.json | 200、202、204、404 |
| DELETE 目录 | 删除记录目录 | 200、202、204、404 |

PROPFIND 请求体：

~~~xml
<?xml version="1.0" encoding="utf-8"?>
<d:propfind xmlns:d="DAV:">
  <d:prop>
    <d:resourcetype/>
    <d:getcontentlength/>
    <d:getlastmodified/>
  </d:prop>
</d:propfind>
~~~

客户端解析每个 response：

- href：远端路径。
- resourcetype/collection：是否目录。
- getcontentlength：字节数，解析失败按 0。
- getlastmodified：服务端修改时间字符串。

### 路径拼接

实现 combineRemotePath(directory, name)：

1. 将两侧路径归一化为正斜杠分隔。
2. 右侧去掉开头正斜杠。
3. 左侧为空或根路径时返回根路径加右侧。
4. 左侧去掉末尾正斜杠后拼接 left + 正斜杠 + right。

实现 urlForRemotePath(remotePath)：

1. url 去掉末尾正斜杠。
2. remotePath 去掉开头正斜杠。
3. 空路径返回 url + 正斜杠，否则返回 url + 正斜杠 + remotePath。

生产实现还应对路径片段做 URL percent-encoding。Quattro 当前实现主要面向已可用的服务端路径；若独立实现需要完整兼容特殊字符，建议在 URL 层编码每个 path segment，而不是编码分隔用的正斜杠。

### 目录创建

上传前必须确保文件目录存在：

1. 将目标目录 /Quattro/files/ 拆成路径段。
2. 依次对 /Quattro、/Quattro/files 发起 MKCOL。
3. 201 表示创建成功，405 表示已存在并继续。
4. 其它状态码视为失败。

每个文件上传还要确保记录目录存在：filesPath/recordId/。

## 远端记录模型

### 记录目录

每个本地文件对应一个远端记录目录：

~~~text
filesPath/
  recordId/
    metadata.json
    content
~~~

其中：

- recordId 是本地绝对路径规范化后的 SHA-256 十六进制小写字符串，长度 64。
- metadata.json 是 UTF-8 JSON 文本。
- content 是文件二进制内容。

这样的目录结构能解决两个问题：

1. 文件名可以重复，不同目录下同名文件不会冲突。
2. 远端文件名始终固定为 content，复杂字符、路径分隔符和大小写差异都放入元数据处理。

### 本地路径规范化

Windows 语义下的规范化：

1. 将正斜杠替换为反斜杠。
2. 调用系统 API 解析为绝对路径。
3. 转为小写。

跨平台实现应定义自己的等价规则：

- Windows：大小写不敏感，建议小写化。
- macOS/Linux：是否小写化取决于目标文件系统策略；若要与 Windows 记录兼容，保持 Windows 规则。
- 不要使用当前列表下标、文件名或排序位置作为记录 ID。

记录 ID 计算规则：

~~~text
recordId = hex(sha256(utf8(canonicalAbsolutePath)))
~~~

若系统 SHA-256 不可用，可失败并提示；不要退化成短 hash，除非只为兼容历史数据且能区分新旧格式。

### 元数据格式

当前 schema 版本为 3：

~~~json
{
  "schemaVersion": 3,
  "absolutePath": "C:\\Users\\demo\\Documents\\report.docx",
  "displayName": "report.docx",
  "size": 123456,
  "sha256": "64位十六进制sha256",
  "uploadedAtUtc": "2026-07-21T08:31:35.872Z",
  "sourceLastWriteTimeUtc": "2026-07-21T08:31:35.8720000Z",
  "contentName": "content",
  "uploadState": "complete",
  "contentReady": true
}
~~~

字段说明：

| 字段 | 必需 | 说明 |
| --- | --- | --- |
| schemaVersion | 建议 | 当前写入 3；读取时可宽松处理 |
| absolutePath | 是 | 源文件规范化绝对路径 |
| displayName | 否 | 展示名；缺失时使用 absolutePath 的文件名 |
| size | 是 | 源文件字节数 |
| sha256 | 是 | 源文件内容 SHA-256 |
| uploadedAtUtc | 否 | 上传完成或开始时间，UTC ISO-like 字符串 |
| sourceLastWriteTimeUtc | 否 | 上传时本地文件最后修改时间，UTC |
| contentName | 建议 | 固定为 content |
| uploadState | 否 | pending 或 complete；缺失按 complete |
| contentReady | 否 | 内容是否可下载；缺失按 true |

读取元数据的最低有效条件：

- JSON 是对象。
- absolutePath 是字符串。
- size 存在。
- sha256 是字符串。

读取成功后应重新计算 record.id = recordId(canonicalPath(absolutePath))，但枚举目录时最终以目录名作为记录 ID，以便异常记录和历史目录仍可定位。

## 状态模型

### 记录健康状态

| 状态 | 含义 |
| --- | --- |
| Healthy | 元数据读取和解析成功 |
| MissingMetadata | 记录目录存在，但 metadata.json 返回 404 |
| InvalidMetadata | metadata.json 存在但 JSON 或必需字段无效 |
| MetadataReadFailed | metadata.json 因网络、鉴权、服务端错误等原因读取失败 |

异常记录应保留在列表里，展示名可用“异常记录 · id前12位”，uploadState = invalid，contentReady = false，并保留 recordError。

### 本地同步状态

本地状态根据健康状态、上传状态、元数据时间和本地文件状态推导：

| 状态 | 推导规则 | 展示文本 |
| --- | --- | --- |
| LocalMissing | 本地路径无文件 | 本地不存在 |
| RemoteNewer | 本地修改时间早于 sourceLastWriteTimeUtc | 远端较新 |
| Same | 本地修改时间等于 sourceLastWriteTimeUtc | 相同 |
| LocalNewer | 本地修改时间晚于 sourceLastWriteTimeUtc | 本地较新 |
| Unknown | 无法解析远端记录时间 | 无法判断 |
| Incomplete | contentReady=false 或 uploadState 不是 complete | 未完成 |
| MissingMetadata | 健康状态为 MissingMetadata | Meta 缺失 |
| InvalidMetadata | 健康状态为 InvalidMetadata | Meta 无效 |
| MetadataReadFailed | 健康状态为 MetadataReadFailed | Meta 读取失败 |

状态排序建议使用上述顺序。升序时把“本地不存在、远端较新、相同、本地较新、无法判断、未完成、Meta 异常”依次展示；降序反向。相同状态内保持稳定排序。

## 清单刷新与缓存

### 缓存格式

本地缓存按账号和远端文件目录隔离。缓存 key 建议：

~~~text
fingerprint = lower(url) + "|" + lower(userName) + "|" + lower(filesPath)
cacheFile = userConfig/cache/webdav-files/sha256(fingerprint).json
~~~

缓存内容：

~~~json
{
  "refreshedAtUtc": "2026-07-21T14:36:01.000Z",
  "records": [
    {
      "id": "64位记录目录名",
      "absolutePath": "C:\\...",
      "displayName": "report.docx",
      "size": 123456,
      "sha256": "...",
      "uploadedAtUtc": "...",
      "sourceLastWriteTimeUtc": "...",
      "uploadState": "complete",
      "contentReady": true,
      "health": 0,
      "recordError": ""
    }
  ]
}
~~~

同进程或多进程都可能更新缓存时，需要用互斥锁或文件锁保护读写。写入建议先写临时文件，再原子替换。

### 刷新流程

文件管理界面打开时：

1. 读取缓存并立即展示。
2. 启动后台刷新任务。
3. UI 显示“远端目录：filesPath · 正在后台刷新…”。
4. 禁用“刷新”按钮，文字改为“刷新中”。
5. 后台任务完成后恢复按钮和目录文案。

后台刷新：

1. 读取密码和配置，失败则返回错误。
2. 对 filesPath 发起 PROPFIND Depth: 1。
3. 过滤子目录：必须是 collection、不是目录自身响应、名称为 64 位十六进制。
4. 去重并排序 recordId。
5. 并发读取每个 filesPath/recordId/metadata.json，并发上限建议 4，绝对上限 8。
6. 对每个记录生成 WebDavFileRecord 或异常记录。
7. 按批次回调 UI，默认批大小 20，或间隔超过 100ms 也刷新一次。
8. 全部完成后返回完整 records 和刷新时间。

部分失败策略：

- metadata.json 缺失或无效：作为异常记录展示，整体刷新仍可成功。
- 网络等读取失败：保留异常记录，但整体结果应标记为“不完全成功”，提示用户稍后重试。
- 不完全成功时可以保存当前已合并清单缓存，但不要删除未看到的旧记录，避免网络抖动导致缓存丢失。
- 完全成功时，用最终 seenIds 删除本地缓存和表格中远端已不存在的记录。

### 增量更新 UI

UI 维护：

- records：当前业务记录列表。
- naturalOrderIds：刷新自然顺序，用于取消排序后恢复。
- rowKeyMap：recordId 到 stable row key 的映射，表格行 key 不随排序变化。
- checkedIds：已勾选记录 ID。
- deletingIds：正在删除的记录 ID。
- deletedTombstones：本轮删除成功或待确认的墓碑，避免刷新把正在删除的记录又加回来。

批次到达时：

1. 若 batch generation 不是当前刷新 generation，丢弃。
2. 对每条记录按 record.id 查找。
3. 新记录追加到 records 和表格。
4. 已存在且内容变化的记录原位更新。
5. 未变化不写 UI。
6. 若新增或更新导致排序变化，重新排序并重建表格，同时恢复选中 key、焦点 key 和顶部可见 key。

完成时：

1. 成功刷新：删除未在 seenIds 中出现、且不在删除中或墓碑中的记录。
2. 保存缓存。
3. 清理非删除中的墓碑。
4. 更新选择状态。
5. 写入刷新日志。

## 上传流程

### 单文件上传

输入是本地文件路径。流程：

1. 校验路径是普通文件。
2. 读取凭据和配置。
3. 计算规范化绝对路径、recordId、展示名、文件大小、SHA-256、最后修改时间。
4. 构造初始元数据：uploadState = pending，contentReady = false。
5. 报告 Preparing 进度。
6. 确保 filesPath 和 filesPath/recordId/ 存在。
7. 上传 pending 版 metadata.json。
8. 上传 content。
9. 上传完成后重新读取本地文件大小和最后修改时间；若上传期间文件发生变化，返回失败，提示用户重新上传。
10. 更新元数据：uploadState = complete，contentReady = true。
11. 再次上传 metadata.json 作为最终完成标记。
12. 返回成功记录并更新本地缓存。

阶段枚举：

| 阶段 | 说明 |
| --- | --- |
| Preparing | 校验文件、读取大小、计算 hash、准备目录 |
| UploadingMeta | 上传 pending 元数据 |
| UploadingContent | 上传内容文件 |
| FinalizingMeta | 上传 complete 元数据 |

pending 元数据非常重要：如果内容上传中断，刷新列表会看到“未完成”记录，而不是误认为可以下载。

### 文件夹上传

文件夹上传只在提交队列前展开：

1. 对输入路径去重。
2. 普通文件直接加入。
3. 目录递归遍历，加入所有普通文件。
4. 不上传空目录。
5. 用规范化绝对路径去重。

每个展开后的普通文件作为独立上传任务。

## 下载流程

输入是远端记录。流程：

1. 读取凭据和配置。
2. 拒绝异常记录、contentReady=false 或 uploadState 不是 complete 的记录。
3. 校验 absolutePath 是有效绝对文件路径，文件名和每个路径段不得为空、单点、双点、含非法字符或控制字符。
4. 创建目标父目录。
5. 下载 filesPath/recordId/content 到临时文件：target.quattro-download.tmp。
6. 报告 DownloadingContent 进度。
7. 报告 Verifying。
8. 校验临时文件大小等于 metadata size。
9. 计算临时文件 SHA-256，必须等于 metadata sha256。
10. 删除旧目标文件，并将临时文件重命名为目标文件。
11. 如果 sourceLastWriteTimeUtc 可解析，将目标文件最后修改时间设置为该时间。
12. 返回成功。

失败时必须删除临时文件。覆盖本地文件前，UI 应进行二次确认。

## 删除流程

删除只删除远端记录，不删除本地文件。

单条记录删除流程：

1. 删除 filesPath/recordId/content。
2. 删除 filesPath/recordId/metadata.json。
3. 删除 filesPath/recordId/ 目录。

文件和目录 DELETE 可接受 404，以便恢复半删除状态。进度阶段：

| 阶段 | 说明 |
| --- | --- |
| DeletingContent | 删除内容文件 |
| DeletingMetadata | 删除元数据 |
| DeletingDirectory | 删除记录目录 |

批量删除：

1. 展示危险确认，说明不会删除本地文件。
2. 将记录 ID 加入 deletingIds 和 deletedTombstones。
3. 禁用这些行或将其显示为处理中。
4. 后台逐条删除。
5. 每条成功后，从表格、records、缓存和勾选集合移除。
6. 每条失败后，移出删除集合和墓碑，恢复该行，并记录最后错误。
7. 支持停止；停止后当前文件完成再停，未开始项恢复。

## 传输队列

### 任务模型

每个队列任务：

| 字段 | 说明 |
| --- | --- |
| id | 队列内递增 ID，作为 UI 行 key |
| kind | Upload 或 Download |
| uploadPath | 上传源路径 |
| downloadRecord | 下载源记录 |
| fileName | 展示名 |
| absolutePath | 本地路径 |
| size | 内容大小 |
| status | 等待、准备、上传中、下载中、校验、完成、失败、停止 |
| phase | 文件服务阶段 |
| transferred / total | 当前阶段进度 |
| contentTransferred / contentTotal | 内容传输进度，用于总进度 |
| error | 失败原因 |
| stopRequested | 停止标记 |
| queuedAt / startedAt / finishedAt | 时间戳 |

并发规则：

- 默认并发 1。
- 用户可配置时建议限制在 1 到 4。
- 元数据枚举并发上限可到 8，但文件传输更消耗带宽和磁盘，建议不超过 4。

去重规则：

- 上传：同一规范化绝对路径已有未结束上传任务时跳过。
- 下载：同一 record.id 已有未结束下载任务时跳过。
- 同一次提交内也要去重。

停止规则：

- 等待中的任务直接变为 Stopped。
- 正在传输的任务设置 stopRequested，由进度回调返回 false 或取消 token 让 HTTP 请求中断。
- 析构或宿主退出时，请求停止全部任务，不在 UI 线程等待长时间 I/O。

### 状态与进度

上传 UI 状态映射：

| 服务阶段 | 队列状态 | 阶段序号 | 总体进度权重 |
| --- | --- | --- | --- |
| Preparing | Preparing | 1 / 4 | 0% |
| UploadingMeta | UploadingMeta | 2 / 4 | 25% + meta ratio × 25% |
| UploadingContent | Uploading | 3 / 4 | 50% + content ratio × 25% |
| FinalizingMeta | Confirming | 4 / 4 | 75% + meta ratio × 25% |

下载 UI 状态映射：

| 服务阶段 | 队列状态 | 阶段序号 | 总体进度权重 |
| --- | --- | --- | --- |
| DownloadingContent | Downloading | 1 / 2 | content ratio × 50% |
| Verifying | Verifying | 2 / 2 | 50% |
| 完成 | DownloadCompleted | - | 100% |

队列快照应提供：

- 标题：WebDAV 文件传输。
- 总状态：总字节进度、百分比、成功数、失败数、处理中数、等待数、停止数。
- 当前详情：当前第几项、文件名、阶段、阶段字节进度、并行任务数。
- 总进度条。
- 当前任务进度条。
- 任务表格：文件名、方向、绝对路径、大小、状态、完成时间。

终态任务排序建议：活动和等待任务在前，历史任务按完成时间倒序。用户触发表头排序时，按列排序并以 id 稳定打破平局。

## 文件管理界面/API

### 主视图

最小交互元素：

- 远端目录标签：显示 filesPath，刷新中追加“正在后台刷新”。
- “刷新”按钮。
- “队列”按钮。
- “全选”“清除选择”。
- 选择状态：已选择 X 项 · 共 Y 项。
- “上传所选”“下载所选”“删除所选”。
- 文件表格列：
  - 文件名
  - 大小
  - 上传时间
  - 本地状态
  - 操作

表格要求：

- 支持 checkbox 多选。
- 支持按文件名、大小、上传时间、本地状态排序。
- 行 key 使用稳定业务 key，不使用行号。
- 行 tooltip 展示文件名、大小、本地状态、上传时间、远端记录时间、本地修改时间和绝对路径。
- 异常记录大小显示破折号，上传时间显示健康状态文本。
- 行操作菜单包含：下载、上传、打开文件所在位置、查看详情、删除。

### 操作可用性

| 操作 | 可用条件 |
| --- | --- |
| 下载 | 健康记录，contentReady=true，uploadState=complete，本地保存路径有效 |
| 上传 | 健康记录，metadata 中的本地路径仍是普通文件 |
| 打开文件所在位置 | 本地路径有效；文件不存在时可提示 |
| 查看详情 | 任意记录，包括异常记录 |
| 删除 | 任意记录；删除远端内容，不删除本地文件 |

### 批量确认

上传确认应列出：

- 可上传项：本地文件存在，将覆盖远端记录和内容。
- 跳过项：异常记录、本地文件不存在、路径无效。

下载确认应列出：

- 可下载项：将下载到本地路径。
- 覆盖项：本地文件已存在，需二次危险确认。
- 跳过项：异常记录、未完成记录、路径无效。

删除确认应列出：

- 所有选中项。
- 明确说明“删除远端内容、Meta 和记录目录，不会删除本地文件”。
- 使用危险操作样式或等价警示。

### 详情视图

详情内容至少包含：

- 文件名。
- 文件大小。
- 远程更新时间 uploadedAtUtc 的本地时间。
- 远端记录时间 sourceLastWriteTimeUtc 的本地时间。
- 本地修改时间。
- 本地状态。
- 上传状态：uploadState 加“内容可用/不可用”。
- 系统绝对路径，异常记录则展示错误信息。
- 远端记录路径：filesPath/recordId。
- SHA-256，异常记录则展示记录 ID。

提供“复制全部”便于排查。

## 进程间提交和文件管理器右键入口

桌面应用中建议将传输队列做成单实例宿主：

1. 主程序或右键菜单命令收到上传/下载请求。
2. 如果传输宿主未运行，启动宿主进程或激活宿主窗口。
3. 通过 IPC 发送请求。
4. 宿主入队并显示队列窗口。
5. 队列无运行和等待任务且窗口隐藏后，宿主可自动退出。

Quattro 当前用命名管道：

- 启动宿主命令：--webdav-transfer-host。
- 右键上传命令：--upload-to-webdav selectedPath。
- 请求类型：上传、下载、显示队列。
- 请求体先写 32 位长度，再写二进制 payload。
- payload 包含 magic、version、kind 和路径或记录字段。
- 管道权限只允许 System 和当前所有者访问。
- 请求大小限制为 16 MB。

独立实现可用其它 IPC：

- 同进程：直接调用队列。
- Windows：Named Pipe、WM_COPYDATA、本地 RPC。
- macOS/Linux：Unix domain socket。
- 跨平台：本地 HTTP loopback，但必须限制只监听本机并加随机 token。

文件管理器右键入口：

- Windows 可注册到当前用户的 Software\Classes\*\shell\AppNameUploadToWebDav 或等价范围。
- 菜单名：上传到 WebDAV。
- 命令：app.exe --upload-to-webdav selectedPath。
- MultiSelectModel=Player 允许多选按多次或批量传递，具体行为取决于 Shell。
- 注册或反注册后通知 Shell 关联变化。

## 错误处理与日志

用户可见错误应说明失败对象和下一步：

- 配置缺失：提示配置 WebDAV 地址、用户名或密码。
- 网络失败：提示 WebDAV 请求失败，可稍后重试。
- 元数据缺失或无效：在列表中保留异常记录，允许删除或查看详情。
- 上传期间文件变化：提示重新上传。
- 下载校验失败：删除临时文件，不替换本地文件。
- 删除部分失败：保留失败项，显示最后错误。

日志记录建议：

- WebDAV 请求开始和完成，URL 可脱敏，至少不要记录密码。
- 刷新发现记录目录数、有效数、缺失 Meta 数、无效 Meta 数、读取失败数。
- 传输任务开始和结束，文件名、状态、错误。
- 缓存读写失败路径。
- IPC 请求收发和宿主启动/退出。

## 安全与可靠性要求

- 不在普通配置文件保存密码。
- 日志中不要记录密码、认证头或包含凭据的 URL。
- 下载始终写入临时文件并通过大小和 SHA-256 校验后再替换目标。
- 上传前后校验本地文件大小和最后修改时间，防止上传过程中内容变化。
- 删除远端目录前先删除 content 和 metadata.json，允许 404 以便恢复半失败状态。
- 刷新失败时不要清空表格；保留缓存和已有可用记录。
- 所有后台结果回到 UI 或 API 主线程前必须检查 generation 或 cancellation token，过期结果丢弃。
- UI 表格和 API 输出都以 record.id 为业务身份。
- 批量任务要支持停止；停止不是强杀线程，而是通过 token 或回调协作式取消。
- 对 WebDAV 服务端差异保持宽容：目录已存在、DELETE 404、PROPFIND 自身目录响应都要正常处理。

## 自动化验收建议

### 单元测试

- canonicalPath 和 recordId 对大小写、斜杠、相对路径的处理稳定。
- 下载目标路径校验拒绝相对路径、根目录、空路径、非法字符、单点或双点路径段。
- 元数据 JSON 写入和读取 round trip。
- 缺失、无效、读取失败 metadata 生成正确异常记录。
- 本地状态推导：本地不存在、远端较新、相同、本地较新、无法判断、未完成、Meta 异常。
- 状态排序升序和降序且同组稳定。
- 缓存按账号和远端目录隔离，Replace、Upsert、Remove 正确。
- 上传流程写 pending metadata，再写 content，最后写 complete metadata。
- 下载流程校验大小和 SHA-256，失败时不替换目标。
- 传输队列去重、并发上限、停止、清空终态、快照统计。

### 集成测试

- 使用本地 WebDAV 测试服务，验证 MKCOL、PROPFIND、PUT、GET、DELETE 全链路。
- 上传普通文件后刷新列表能看到同一记录。
- 远端手动删除 metadata.json 后刷新显示 Meta 缺失。
- 远端写入非法 JSON 后刷新显示 Meta 无效。
- 下载覆盖已有本地文件前必须出现确认。
- 批量删除中停止，未开始项恢复，已成功项从缓存移除。
- 右键上传入口能把文件加入同一传输队列。

### UI/可用性验收

- 打开文件管理时先显示缓存，再后台刷新。
- 刷新批次到达时表格不闪烁，选择、勾选和滚动位置尽量保持。
- 传输队列显示总进度、当前项进度和每行状态。
- 失败记录 tooltip 和详情能给出可排查信息。
- 空列表说明“远端没有文件记录”或“尚未刷新到记录”，并提供刷新入口。

## 独立实现的最小里程碑

1. 实现 WebDavClient：配置校验、MKCOL、PROPFIND、PUT、GET、DELETE。
2. 实现记录模型：recordId、远端目录结构、metadata schema、读取和写入。
3. 实现 listRecords：枚举记录目录、并发读取 metadata、生成异常记录。
4. 实现本地缓存：按账号和目录隔离，启动时可直接展示。
5. 实现 uploadFile：pending metadata 到 content 到 complete metadata。
6. 实现 downloadRecord：临时文件、大小和 SHA-256 校验、替换、恢复修改时间。
7. 实现 deleteRecord：content、metadata、目录三步删除。
8. 实现传输队列：入队、去重、并发、停止、快照。
9. 实现文件管理界面或 API：表格、刷新、批量确认、详情、操作菜单。
10. 可选实现 Shell 或文件管理器右键入口，复用传输队列。

完成以上 10 步后，即可脱离 Quattro 实现一套行为等价的 WebDAV 文件管理功能。
