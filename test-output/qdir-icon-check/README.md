# Q-Dir 图标诊断

## 范围

- 本机只读样本：`C:\Soft\Q-Dir.exe`，文件版本 `8.7.1.0`。
- SHA256：`55ef3f7944c5742fa4d5cc6e9aaedaddebe2f64908eb2baf70b71a7c41fbf6dc`。
- 实际取图全部调用生产 `IconResolverService`；PE 枚举只用于核对资源目录。
- 不启动 Q-Dir 或 Quattro，不创建窗口，不注入输入，不读取/改写正式配置，不清除生产缓存。
- Link 请求未指定自定义图标；无 PIDL、有 PIDL、ShellParseName、FilePath 和显式 IconLocation 分别对比。
- Disabled 仅绕过 Quattro 磁盘缓存，不代表清除 Windows 自身的 Shell 缓存。
- 图像为 resolver 实际 PNG 输出的拼图，不是 HWND 截图，不代表当前用户界面或所有 Q-Dir 版本已验收。

## 结果

1. **正常 Link/Shell 的图标身份正确**：红色网格和黑色 Q 完整。10 个尺寸（16/20/24/32/40/48/64/96/128/256）下，Link、带 PIDL 的 Link、ShellParseName 三者 PNG 像素相同。
2. **best-resource 选错图标组**：16px 结果等同 ID329（剪贴板）；20px 至 256px 的所测尺寸均等同 ID204（蓝色小房子）。这些是不同的图案，不是应用标识的高清版本。该文件有 30 个图标组，最大资源尺寸 32px；首组 ID128 为 32px、4bit，ID204 为 32px、32bit。跨组按色深/尺寸评分错误地把画质优先于身份。
3. **显式选择首组仍存在透明度错误**：32px 的 Shell 输出与 ID128 直接输出，1024 个像素的 RGB 全部相同，但 162 个黑色不透明像素在直接提取结果中变成透明。`CaptureIcon` 对无 alpha 的图标只给非黑色像素补 alpha，导致黑色 Q 丢失。不能把“固定取首组”当完整修复。
4. **缓存 alpha 不一致**：7 个尺寸回读后原始像素不同，alpha 不变；64px 有 496 个像素改变，RGB 单通道最大差 26。回读 RGB 与“写入前 RGB 再乘一次 alpha”的误差不超过 0.5，符合重复预乘。源码 `SavePngIcon` 将服务像素按 BGRA 写出，而读入转为 PBGRA。图标身份不变，但半透明边缘会变暗；不是房子图标的成因。

## 测试结果的边界

- 编译成功，见 `build.log`。
- 120 次主要尺寸/路径请求均成功输出 PNG。
- 额外 30 个内部资源组枚举中，ID233、ID362 无有效像素导出，因此诊断进程退出码为 1、`qdir_icon_probe_errors=2`；没有将其伪报为全通过。拼图对这两项明确标注，未使用遗留 PNG。
- 原先要求 Q-Dir 必须走 `file-resource-best-resource` 的断言不合理；当前测试允许 Shell 或 best-resource 并检查彩色像素，也不能证明身份正确，因为蓝色房子同样可以通过颜色判定。
- 没有运行完整单测或 GUI/DPI 截图验收；本次未修改生产逻辑、缓存规则或现有单测。

## 建议修复顺序

1. 保留 Shell 优先；尊重明确指定的图标位置。需要资源回退时先确定应用图标组，再在该组内按尺寸/色深挑选，不能跨所有功能图标排名。不为 Q-Dir 硬编码 ID128。
2. 在公共 CaptureIcon 中正确结合传统图标的掩码恢复 alpha，保留不透明黑色；统一服务像素为预乘格式。
3. 修复 PNG 编解码的 BGRA/PBGRA 一致性，并在修复后升级缓存命名空间，使旧错误缓存自然失效。
4. 使用自建多图标组 fixture 增加身份回归测试：低位深主图标 + 高位深功能图标；另测不透明黑色、半透明边缘和 PNG 往返。Q-Dir 本机样本只作为补充，不能作为唯一环境依赖。

## 证据

- `comparison.png`：五种尺寸的四路实际输出对照。
- `resource-atlas.png`：30 个图标组的公共 resolver 输出。
- `comparison.json`：PE 目录、SHA256、逐像素一致性及 alpha 指标。
- `images/resolver-results.tsv`：完整路径、尺寸、来源和导出状态。
- `images/cache-roundtrip.tsv`、`images/*.bgra`：缓存前后原始像素。
- `probe.log`、`probe-exit-code.txt`：进程结果。
- 诊断源码：`tests/scripts/qdir-icon-probe.cpp`、`tests/scripts/qdir-icon-report.py`。

诊断程序使用已有 QuattroTests 的生产 obj/lib 链接，仅替换测试入口；响应文件为本目录 `link.rsp`，未修改正式构建目标或产物。

报告生成命令：

```powershell
python tests/scripts/qdir-icon-report.py 'C:\Soft\Q-Dir.exe' 'test-output/qdir-icon-check'
```
