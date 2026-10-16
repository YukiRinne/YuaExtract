#!/usr/bin/env python3
# 像素级比对（验证脚手架专用，task-7）：页签切换"幽灵残留"回归断言的核心。
#
# 为什么不用 PowerShell 自己比：1150×795 ≈ 91 万像素，逐像素在 PS 里跑要几十秒且吃内存；
# 这里用自带 Python 的 Pillow，一次 C 级运算搞定，还能顺手输出热区与差异图。
#
# 用法：
#   python image-diff.py diff --a <基线.png> --b <对照.png> --json <out.json>
#        [--tolerance 8] [--grid 16] [--top 6] [--heatmap <out.png>]
#   python image-diff.py ghostify --base <干净图.png> --overlay <旧内容.png> --out <out.png>
#        [--region x,y,w,h] [--alpha 1.0]        # 人为把旧内容叠上去，用于自检"幽灵必须炸"
#
# 判据口径（写进断言详情，便于解释阈值）：
#   * 单像素差异 = max(|ΔR|,|ΔG|,|ΔB|)；> tolerance 才算"不同像素"。
#     tolerance=8 用来吸收 PNG 编解码/取整噪声，同时幽灵（旧控件文字/边框）的差值远大于 8。
#   * 差异比例 = 不同像素数 ÷ 总像素数。
#   * 热区 = 把图切成 grid×grid 块，按块内不同像素数排序；每块再收紧成"块内不同像素的精确包围盒"。
#
# 退出码：0 = 比对完成（是否有差异由调用方判）；3 = 尺寸不一致（几何不可比）；2 = 读图失败。

import argparse
import json
import os
import sys

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

try:
    from PIL import Image, ImageChops
except Exception as error:      # noqa: BLE001
    print(f"需要 Pillow：{error}", file=sys.stderr)
    sys.exit(2)


def load_rgb(path):
    image = Image.open(path)
    return image.convert("RGB"), image


def diff_command(args) -> int:
    if not os.path.exists(args.a) or not os.path.exists(args.b):
        print("读图失败：文件不存在", file=sys.stderr)
        return 2
    image_a, raw_a = load_rgb(args.a)
    image_b, raw_b = load_rgb(args.b)

    result = {
        "a": os.path.abspath(args.a),
        "b": os.path.abspath(args.b),
        "sizeA": list(image_a.size),
        "sizeB": list(image_b.size),
        "tolerance": args.tolerance,
        "sameSize": image_a.size == image_b.size,
    }
    if image_a.size != image_b.size:
        result["note"] = "尺寸不一致：几何不可比，按失败处理（抓图前必须把客户区调成同一尺寸）"
        if args.json:
            with open(args.json, "w", encoding="utf-8", newline="\n") as fp:
                json.dump(result, fp, ensure_ascii=False, indent=2)
        print(json.dumps(result, ensure_ascii=False))
        return 3

    width, height = image_a.size
    diff = ImageChops.difference(image_a, image_b)
    # 单通道最大差：把 R/G/B 三通道拆开取逐像素最大值
    r, g, b = diff.split()
    max_diff = ImageChops.lighter(ImageChops.lighter(r, g), b)
    histogram = max_diff.histogram()
    total = width * height
    diff_pixels = sum(histogram[args.tolerance + 1:])
    result.update({
        "totalPixels": total,
        "diffPixels": diff_pixels,
        "ratio": (diff_pixels / total) if total else 0.0,
        "ratioPercent": (diff_pixels * 100.0 / total) if total else 0.0,
        "maxChannelDiff": max(index for index, count in enumerate(histogram) if count > 0),
    })

    # 精确包围盒（所有不同像素）
    mask = max_diff.point(lambda value: 255 if value > args.tolerance else 0)
    bbox = mask.getbbox()
    result["bbox"] = list(bbox) if bbox else None

    # 热区：grid×grid 分块排序 + 块内精确包围盒
    grid = max(2, args.grid)
    block_w = max(1, width // grid)
    block_h = max(1, height // grid)
    blocks = []
    for row in range(grid):
        for col in range(grid):
            left = col * block_w
            top = row * block_h
            right = width if col == grid - 1 else left + block_w
            bottom = height if row == grid - 1 else top + block_h
            crop = mask.crop((left, top, right, bottom))
            count = sum(crop.histogram()[128:])
            if count <= 0:
                continue
            inner = crop.getbbox()
            blocks.append({
                "block": [left, top, right, bottom],
                "diffPixels": count,
                "bbox": [left + inner[0], top + inner[1], left + inner[2], top + inner[3]] if inner else [left, top, right, bottom],
            })
    blocks.sort(key=lambda item: item["diffPixels"], reverse=True)
    result["grid"] = {"cols": grid, "rows": grid, "blockWidth": block_w, "blockHeight": block_h}
    result["hotspots"] = blocks[:max(1, args.top)]
    result["hotspotCount"] = len(blocks)

    if args.heatmap:
        heat = max_diff.point(lambda value: min(255, value * 4))
        heat.save(args.heatmap)
        result["heatmap"] = os.path.abspath(args.heatmap)

    if args.json:
        with open(args.json, "w", encoding="utf-8", newline="\n") as fp:
            json.dump(result, fp, ensure_ascii=False, indent=2)

    print(json.dumps({key: result[key] for key in ("sameSize", "diffPixels", "totalPixels", "ratio", "ratioPercent", "bbox")}, ensure_ascii=False))
    return 0


def ghostify_command(args) -> int:
    if not os.path.exists(args.base) or not os.path.exists(args.overlay):
        print("读图失败：文件不存在", file=sys.stderr)
        return 2
    base = Image.open(args.base).convert("RGB")
    overlay = Image.open(args.overlay).convert("RGB")
    if overlay.size != base.size:
        overlay = overlay.resize(base.size)

    if args.region:
        parts = [int(value) for value in args.region.split(",")]
        if len(parts) != 4:
            print("--region 需要 x,y,w,h", file=sys.stderr)
            return 2
        box = (parts[0], parts[1], parts[0] + parts[2], parts[1] + parts[3])
    else:
        # 默认：把叠加图的**左上 60% × 55%** 贴过去（页签内容区，正是幽灵出现的地方）
        box = (0, 0, int(base.size[0] * 0.6), int(base.size[1] * 0.55))

    patch = overlay.crop(box)
    if args.alpha >= 1.0:
        base.paste(patch, (box[0], box[1]))
    else:
        base.paste(Image.blend(base.crop(box), patch, args.alpha), (box[0], box[1]))
    base.save(args.out)
    print(f"已把 {os.path.basename(args.overlay)} 的 {box} 叠到 {os.path.basename(args.base)} -> {args.out}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    diff = sub.add_parser("diff")
    diff.add_argument("--a", required=True)
    diff.add_argument("--b", required=True)
    diff.add_argument("--json", default="")
    diff.add_argument("--tolerance", type=int, default=8)
    diff.add_argument("--grid", type=int, default=16)
    diff.add_argument("--top", type=int, default=6)
    diff.add_argument("--heatmap", default="")
    diff.set_defaults(func=diff_command)

    ghost = sub.add_parser("ghostify")
    ghost.add_argument("--base", required=True)
    ghost.add_argument("--overlay", required=True)
    ghost.add_argument("--out", required=True)
    ghost.add_argument("--region", default="")
    ghost.add_argument("--alpha", type=float, default=1.0)
    ghost.set_defaults(func=ghostify_command)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
