"""Read-only PE audit and comparison of the public-resolver probe's output.

Usage: python tests/scripts/qdir-icon-report.py EXE OUTPUT_DIRECTORY
Requires Pillow and pefile. Does not execute EXE or acquire icons itself.
"""
import csv
import hashlib
import json
import struct
import sys
from pathlib import Path

import pefile
from PIL import Image, ImageDraw, ImageFont

exe, root = Path(sys.argv[1]), Path(sys.argv[2])
images = root / "images"
sizes = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]
pe = pefile.PE(str(exe), fast_load=False)
version = pe.VS_FIXEDFILEINFO[0]
version_text = ".".join(str(i) for i in (version.FileVersionMS >> 16, version.FileVersionMS & 65535,
                                        version.FileVersionLS >> 16, version.FileVersionLS & 65535))
groups = []
for kind in pe.DIRECTORY_ENTRY_RESOURCE.entries:
    if kind.id != 14:
        continue
    for group in kind.directory.entries:
        item = group.directory.entries[0].data.struct
        data = pe.get_data(item.OffsetToData, item.Size)
        entries = []
        for i in range(struct.unpack_from("<H", data, 4)[0]):
            w, h, colors, reserved, planes, depth, length, resource = struct.unpack_from("<BBBBHHIH", data, 6 + i * 14)
            entries.append(dict(width=w or 256, height=h or 256, depth=depth, bytes=length, resource_id=resource))
        groups.append(dict(group_id=group.id, entries=entries))

def load(name, size):
    return Image.open(images / f"{name}-{size}.png").convert("RGBA")

def same(a, b, size):
    return load(a, size).tobytes() == load(b, size).tobytes()

comparisons = []
for size in sizes:
    fresh = (images / f"cache-refresh-{size}.bgra").read_bytes()
    cached = (images / f"cache-read-{size}.bgra").read_bytes()
    rgb_differences = [abs(fresh[i] - cached[i]) for i in range(len(fresh)) if i % 4 != 3]
    # WIC PBGRA decoding multiplies stored PNG channels by alpha.
    premultiply_error = max(abs(cached[i] - fresh[i] * fresh[(i // 4) * 4 + 3] / 255)
                            for i in range(len(fresh)) if i % 4 != 3)
    comparisons.append(dict(
        size=size, link_equals_pidl=same("link", "link-pidl", size),
        link_equals_shell=same("link", "shell", size),
        first_equals_id128=same("resource0", "resource-128", size),
        best_matches=[i for i in (128, 204, 329, 330, 332) if same("file-best", f"resource-{i}", size)],
        cache_rgb_max_delta=max(rgb_differences),
        cache_alpha_unchanged=fresh[3::4] == cached[3::4],
        cache_changed_pixels=sum(fresh[i:i+4] != cached[i:i+4] for i in range(0, len(fresh), 4)),
        cache_vs_extra_premultiply_max_error=round(premultiply_error, 4)))

with (images / "resolver-results.tsv").open() as file:
    records = list(csv.DictReader(file, delimiter="\t"))
shell32 = list(load("link", 32).get_flattened_data())
resource32 = list(load("resource-128", 32).get_flattened_data())
alpha_audit = dict(
    same_rgb_pixels=sum(a[:3] == b[:3] for a, b in zip(shell32, resource32)), total_pixels=1024,
    opaque_black_lost=sum(a == (0, 0, 0, 255) and b[3] == 0 for a, b in zip(shell32, resource32)))
audit = dict(exe=str(exe), file_version=version_text, sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
             first_resource_alpha_audit_32px=alpha_audit,
             groups=groups, comparisons=comparisons,
             unsuccessful_exports=[r for r in records if r["saved"] != "1"])
(root / "comparison.json").write_text(json.dumps(audit, ensure_ascii=False, indent=2), encoding="utf-8")

font_path = "C:/Windows/Fonts/msyh.ttc"
font = ImageFont.truetype(font_path, 18)
small = ImageFont.truetype(font_path, 14)
title = ImageFont.truetype(font_path, 24)
columns = [("link", "正常 Link / Shell", "红色网格 + 黑色 Q：完整"),
           ("resource-128", "显式首组 · ID 128", "组正确，但黑色部分丢失"),
           ("file-best", "当前 best-resource", "16px: ID329；其余: ID204"),
           ("cache-read", "隔离缓存回读", "身份不变；另有边缘像素差异")]
canvas = Image.new("RGB", (1080, 850), "#f3f5f8")
draw = ImageDraw.Draw(canvas)
draw.text((25, 15), f"Q-Dir 图标实测对比 · 文件版本 {version_text}", fill="#182536", font=title)
draw.text((25, 52), "公共解析服务实际输出；无应用启动 / 无界面模拟。原图最近邻放大，仅方便检查像素。", fill="#4a5667", font=small)
for column, (_, label, subtitle) in enumerate(columns):
    x = 130 + column * 235
    draw.text((x, 88), label, fill="#182536", font=font)
    draw.text((x, 118), subtitle, fill="#4a5667", font=small)
for row, size in enumerate([16, 20, 24, 32, 64]):
    y = 155 + row * 132
    draw.text((18, y + 42), f"{size}px", fill="#182536", font=font)
    for column, (name, _, _) in enumerate(columns):
        x = 130 + column * 235
        tile = Image.new("RGBA", (205, 120), "white")
        icon = load(name, size)
        factor = 3 if size <= 32 else 1
        icon = icon.resize((size * factor, size * factor), Image.Resampling.NEAREST)
        tile.alpha_composite(icon, ((205 - icon.width) // 2, (120 - icon.height) // 2))
        canvas.paste(tile.convert("RGB"), (x, y))
        draw.rectangle((x, y, x + 204, y + 119), outline="#d5dce5")
canvas.save(root / "comparison.png")

atlas = Image.new("RGB", (900, 660), "#f3f5f8")
draw = ImageDraw.Draw(atlas)
draw.text((20, 10), "本机 EXE 的 30 个图标组 · 公共 resolver 显式 ID 获取", fill="#182536", font=title)
failed = {r["case"] for r in audit["unsuccessful_exports"]}
for index, group in enumerate(groups):
    x, y = (index % 6) * 150, 65 + (index // 6) * 118
    label = f"resource-{group['group_id']}"
    draw.text((x + 15, y), f"ID {group['group_id']}", fill="#182536", font=font)
    if label in failed:
        draw.text((x + 5, y + 40), "本次无有效导出", fill="#a33a30", font=small)
    else:
        icon = load(label, 32).resize((64, 64), Image.Resampling.NEAREST)
        tile = Image.new("RGBA", (64, 64), "white")
        tile.alpha_composite(icon)
        atlas.paste(tile.convert("RGB"), (x + 35, y + 30))
atlas.save(root / "resource-atlas.png")
print(json.dumps(comparisons, ensure_ascii=False, indent=2))
print("unsuccessful_exports:", len(audit["unsuccessful_exports"]))
