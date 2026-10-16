# 模板枚举恢复：只对"规则命名"的资源有效（ev###a.pimg、sd###a##.png 这种）。
#
# 与"从脚本挖候选"的区别：这里不猜名字来源，而是从**已还原的名字**里学出模板，
# 把数字段/末位字母全枚举一遍。成本可控（百万级哈希 ≈ 数秒），且只可能命中真名。
#
# 只处理 ASCII 且有数字段的模板；日文名（街_道a_春 之类）模板枚举无能为力。

import os
import re
import sys
import time
from collections import defaultdict
from hashlib import blake2s

SALT = "xp3hnp"
HEX64 = re.compile(r"^[0-9A-Fa-f]{64}$")
DIGITS = re.compile(r"\d+")
# 每个模板最多展开多少候选，防止某个模板意外爆炸
TEMPLATE_CAP = 4_000_000


def file_hash(name: str) -> str:
    h = blake2s(digest_size=32)
    h.update((name + SALT).encode("utf-16-le"))
    return h.digest().hex().upper()


def collect(output_dir):
    """返回 {hash: 包名} 和 {包名: [已还原明文名]}。"""
    targets = {}
    restored = defaultdict(list)
    for pkg in sorted(os.listdir(output_dir)):
        pkg_dir = os.path.join(output_dir, pkg)
        if not os.path.isdir(pkg_dir):
            continue
        for root, _dirs, files in os.walk(pkg_dir):
            for f in files:
                if HEX64.match(f):
                    targets[f.upper()] = pkg
                else:
                    restored[pkg].append(f)
    return targets, restored


def build_templates(names):
    """ev102a.pimg → ('ev', 3, '.pimg')；末位字母作为可枚举字段。"""
    templates = set()
    for name in names:
        if not name.isascii() or len(name) > 32 or "." not in name:
            continue
        stem, ext = os.path.splitext(name)
        if not DIGITS.search(stem):
            continue
        # 数字段 → 可枚举；末位若是小写字母 → 也可枚举
        pieces = []
        position = 0
        for match in DIGITS.finditer(stem):
            if match.start() > position:
                pieces.append(("lit", stem[position:match.start()]))
            pieces.append(("num", len(match.group(0))))
            position = match.end()
        tail = stem[position:]
        has_letter = False
        if tail and tail[-1].islower() and tail[-1].isalpha():
            tail, letter = tail[:-1], tail[-1]
            has_letter = True
        if tail:
            pieces.append(("lit", tail))
        templates.add((tuple(pieces), ext, has_letter))
    return templates


def enumerate_template(pieces, ext, has_letter, cap=TEMPLATE_CAP):
    results = [""]
    for kind, value in pieces:
        if kind == "lit":
            results = [r + value for r in results]
        else:
            limit = 10 ** value
            if len(results) * limit > cap:
                limit = max(1, cap // max(1, len(results)))
            results = [f"{r}{i:0{value}d}" for r in results for i in range(limit)]
            if len(results) > cap:
                results = results[:cap]
    if has_letter:
        letters = "abcdefghijklmnopqrstuvwxyz"
        if len(results) * 26 > cap:
            letters = letters[: max(1, cap // max(1, len(results)))]
        results = [r + c for r in results for c in letters]
    return [r + ext for r in results]


def main(output_dir, out_path, min_coverage=0.0):
    started = time.time()
    targets, restored = collect(output_dir)
    print(f"待还原 {len(targets)} 个")

    found = {}
    tried = 0
    for pkg in sorted(restored):
        names = restored[pkg]
        total = len(names) + sum(1 for h, p in targets.items() if p == pkg)
        coverage = len(names) / total if total else 1.0
        if coverage >= 0.6:
            continue
        templates = build_templates(names)
        if not templates:
            continue
        pkg_found = 0
        pkg_tried = 0
        for pieces, ext, has_letter in sorted(templates, key=lambda t: -len(t[0])):
            # 模板字段太多就别展开了（容易爆炸且多半不是同一族）
            if len(pieces) > 4:
                continue
            for name in enumerate_template(pieces, ext, has_letter):
                pkg_tried += 1
                tried += 1
                digest = file_hash(name)
                if digest in targets:
                    found[digest] = name
                    pkg_found += 1
        print(f"  {pkg:10} 覆盖率 {coverage * 100:5.1f}%  模板 {len(templates):3}  "
              f"试算 {pkg_tried:9}  命中 {pkg_found:6}  ({time.time() - started:.0f}s)", flush=True)

    by_pkg = defaultdict(int)
    for digest in found:
        by_pkg[targets.get(digest, "?")] += 1
    print(f"\n合计试算 {tried} 次，用时 {time.time() - started:.0f}s，命中 {len(found)} 个")
    for pkg, count in sorted(by_pkg.items(), key=lambda kv: -kv[1]):
        print(f"    {pkg}: +{count}")

    with open(out_path, "w", encoding="utf-8", newline="\r\n") as fp:
        for digest, name in sorted(found.items(), key=lambda kv: kv[1]):
            fp.write(f"{digest}:{name}\n")
    print(f"已写出 {out_path}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("用法: recover_templates.py <Extractor_Output> <输出lst>")
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
