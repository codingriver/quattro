# Quattro

Windows 10 原生桌面快速启动器。当前工程使用 C++20 + Win32 + Direct2D + DirectWrite，目标是保持轻量、便携和低运行时依赖。

## 当前开发状态

- 已完成 Win32 应用入口、单实例、窗口唤醒。
- 已完成 `conf.ini` 读取与窗口状态保存；缺失时使用代码内置默认配置。
- 已完成 Direct2D/DirectWrite 主窗口绘制，包括标题栏、分组栏、标签栏、列表布局和平铺布局。
- 已完成主窗口布局优化：分组位于顶部横向导航，标签页位于左侧导航，内容区提供清晰空状态。
- 已完成主题 XML 的基础颜色解析、回退主题和运行期主题切换。
- 已完成启动项执行入口，支持普通打开、URL 归一化、管理员运行和自定义目录打开命令。
- 已集成 SQLite amalgamation，静态编译进主程序。
- 已完成启动项新增、编辑、删除的原生窗口。
- 已完成启动项新增/编辑窗口的紧凑表单重排。
- 已完成托盘图标，支持显示/隐藏、退出和新增启动项入口。
- 已完成启动项图标的内存提取和绘制。
- 已完成分组、标签新增/编辑/删除。
- 已完成本地搜索窗口，支持按名称、路径、参数、备注匹配并运行。
- 已完成搜索结果右键菜单，支持运行、打开所在目录和复制路径。
- 已完成主窗口/搜索热键注册入口，读取 `nMainHotKey`、`nSearchHotKey`。
- 已完成右键菜单增强：运行、编辑、删除、打开所在目录、复制路径。
- 已完成拖拽导入、剪贴板导入和基础设置窗口。
- 已完成 OLE 拖拽导入，支持文件列表、Unicode 文本/URL 和 Shell IDList，保留 `WM_DROPFILES` 降级路径。
- 已完成自动停靠贴边隐藏、鼠标靠近恢复、失焦隐藏、延迟停靠和全屏前台窗口避让。
- 已扩展设置窗口，覆盖常用显示、窗口行为、运行统计、热键和打开目录命令。
- 已完成标签内排序：按位置、运行次数、名称；支持标签级列表/平铺布局和图标尺寸切换。
- 已完成首字母式轻量排序键：英文按首字母，数字按数字组，中文和其他字符稳定排序。
- 已完成特殊标签：`全部` 汇总当前分组启动项，`待办` 汇总含待办标记的启动项。
- 已完成文件图标缓存：URL 图标从 `icons/url` 读取，程序/文件/文件夹图标运行时缓存到 `icons/cache`。
- 已完成设置页热键录入和清除。
- 已完成启动项移动到标签、复制到标签，以及分组、标签、启动项上移/下移。
- 已完成开机自启动同步、启动项热键直接运行、内部复制/剪切/粘贴、图标缓存清理、帮助/关于/检查更新入口、日志、启动报告、验证脚本和打包脚本。
- 已完成配置化帮助/更新/FAQ/赞助链接、启动失败修复入口、单项图标刷新、系统位置启动项类型。
- 已完成 Shell/PIDL 深度兼容：`Links.Pidl` 读写、PIDL 校验、虚拟 Shell 对象打开、路径失效兜底和打开所在位置。
- 已完成单 exe 发布模式：默认资源内嵌到程序，首次运行时自动释放缺失的主题、URL 图标和文档；配置和数据库在运行时按需生成。
- 已完成轻量单元测试目标，覆盖配置、工具函数、Storage CRUD、PIDL BLOB 回读和 Launcher 目标识别。
- 已完成兼容测试报告，记录系统、DPI、显示器、权限、拖拽支持、冒烟和打包结果。

## 构建、测试与打包

要求：

- Windows 10
- Visual Studio 2022，安装 C++ 桌面开发工作负载
- CMake 3.20+
- PowerShell 5+

### 普通构建

首次配置 x64 构建目录：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
```

编译 Release：

```powershell
cmake --build build --config Release -- /m
```

常用产物：

```text
build/Release/Quattro.exe
build/Release/QuattroTests.exe
build/Release/QuattroDbProbe.exe
build/Release/QuattroDbSeed.exe
```

如果使用平台专用构建目录，项目约定为：

```powershell
cmake -S . -B build-x64 -G "Visual Studio 17 2022" -A x64
cmake --build build-x64 --config Release -- /m

cmake -S . -B build-x86 -G "Visual Studio 17 2022" -A Win32
cmake --build build-x86 --config Release -- /m
```

构建时会自动生成主题资源和内嵌默认资源。默认资源来自 `README.md`、`theme/`、`icons/url/`、`docs/`。`conf.ini` 是运行时本地配置文件，缺失时使用代码默认值并在保存设置时生成；`db/link.db` 缺失时会自动创建表结构和默认分组/标签。

### 测试

运行完整验证：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run-tests.ps1 -Package
```

`run-tests.ps1` 会执行：

- 配置并编译 `build/Release`
- 运行 `QuattroTests.exe` 单元测试
- 运行 UI 冒烟测试
- 运行菜单测试
- 运行滚动测试
- 运行弹窗显示测试
- 扫描已移除或敏感实现词
- 当传入 `-Package` 时，额外调用打包脚本生成发布包

测试日志和兼容报告输出到：

```text
build/Release/logs/
build/Release/logs/compatibility-report.txt
```

只运行单元测试时，可以先构建，再直接执行：

```powershell
.\build\Release\QuattroTests.exe
```

### 冒烟测试

只跑主窗口和托盘行为冒烟测试：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run-ui-smoke.ps1 -ExePath .\build\Release\Quattro.exe
```

如果省略 `-ExePath`，脚本默认使用 `build/<Configuration>/Quattro.exe`：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run-ui-smoke.ps1 -Configuration Release
```

独立 UI 测试脚本：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\run-menu-tests.ps1 -ExePath .\build\Release\Quattro.exe -ProbePath .\build\Release\QuattroDbProbe.exe
powershell -ExecutionPolicy Bypass -File .\tools\run-scroll-tests.ps1 -ExePath .\build\Release\Quattro.exe -SeedPath .\build\Release\QuattroDbSeed.exe
powershell -ExecutionPolicy Bypass -File .\tools\run-dialog-display-tests.ps1 -ExePath .\build\Release\Quattro.exe
```

这些脚本会启动真实 `Quattro.exe`，通过 Win32 消息和窗口枚举验证基础交互。运行前建议关闭已运行的 Quattro 实例，避免单实例唤醒影响测试。

### 打包

一键打包，默认生成 64 位单 exe 并运行单元测试：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build.ps1
```

生成 32 位和 64 位单 exe：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build.ps1 --all
```

只打 32 位：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build.ps1 -Platform x86
```

清理后重新打包：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build.ps1 -Clean
```

生成传统目录包：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build.ps1 -FullPackage
```

跳过打包前单元测试：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build.ps1 -SkipTests
```

查看帮助：

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\build.ps1 --help
```

脚本每次执行会先清空 `dist` 产出目录，完成后在控制台列出本次生成的 `artifacts` 绝对路径。

默认产物：

```text
dist/Quattro.exe
dist/Quattro.zip
```

`--all` 会同时生成 `dist/x86/Quattro.exe`、`dist/x64/Quattro.exe` 和对应 zip；`-FullPackage` 会额外生成传统目录包。

默认发布包可以只分发 `dist/Quattro.exe`。程序首次运行会在可写目录释放缺失资源；如果 exe 同级目录不可写，会使用当前用户本地数据目录。

打包脚本会删除旧 `dist`。如果 `dist/Quattro.exe` 正在运行，Windows 会锁定文件导致覆盖失败；先退出 Quattro 后再打包即可。

## SQLite

SQLite 使用官方 amalgamation 静态编译到 `Quattro.exe`，无需额外携带 `sqlite3.dll`。业务数据只使用运行时生成的 `db/link.db`；图标缓存使用 `icons/cache/*.png` 文件，不使用单独的图标数据库。

## 下一步

- 外部文件索引集成已延期，详见需求 TODO 文档。
- 增加更多实机兼容测试样本。

