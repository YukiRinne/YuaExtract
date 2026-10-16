# 语音名离线批量恢复可行性测试 v2：全笛卡尔积扫描（只读，不写任何文件）。
#
# v1 的教训：语音序号是按剧本行号稀疏编号的，不能"连续未命中就停"。
# v2 直接枚举 前缀 × 章节 × 序号 的完整组合，成本可接受（百万级 BLAKE2s ≈ 数秒）。
#
# 已验证的哈希口径：fileHash = BLAKE2s-256(UTF16LE(名字 + "xp3hnp"))
# 语音包的目录哈希恒为根域 94D4A97C61498621，所以只枚举文件名即可。

import os
import re
import sys
import time
from hashlib import blake2s

SALT = "xp3hnp"
HEX64 = re.compile(r"^[0-9A-Fa-f]{64}$")
NAME_PATTERN = re.compile(r"^(?P<prefix>.+?)_(?P<chapter>\d{3})_(?P<serial>\d{4})(?P<ext>\.\w+)$")


def file_hash(name: str) -> str:
    h = blake2s(digest_size=32)
    h.update((name + SALT).encode("utf-16-le"))
    return h.digest().hex().upper()


def main(output_dir, chapter_lo=1, chapter_hi=60, serial_hi=1200):
    targets = set()
    restored = []
    for pkg in ("voice", "voice2"):
        pkg_dir = os.path.join(output_dir, pkg)
        if not os.path.isdir(pkg_dir):
            continue
        for root, _dirs, files in os.walk(pkg_dir):
            for f in files:
                if HEX64.match(f):
                    targets.add(f.upper())
                else:
                    restored.append(f)

    prefixes = set()
    for name in restored:
        m = NAME_PATTERN.match(name)
        if m:
            prefixes.add(m.group("prefix"))

    print(f"待恢复 {len(targets)} 个 / 已还原 {len(restored)} 个 / 学到前缀 {len(prefixes)} 个")
    print(f"扫描范围：前缀 {len(prefixes)} × 章节 {chapter_lo:03d}..{chapter_hi:03d} × 序号 0001..{serial_hi:04d}")

    started = time.time()
    found = {}
    tried = 0
    for prefix in sorted(prefixes):
        for chapter in range(chapter_lo, chapter_hi + 1):
            for serial in range(1, serial_hi + 1):
                name = f"{prefix}_{chapter:03d}_{serial:04d}.ogg"
                tried += 1
                digest = file_hash(name)
                if digest in targets:
                    found[digest] = name
        done = time.time() - started
        print(f"  {prefix:<12} 累计命中 {len(found):<6} 已试 {tried:<9} 用时 {done:.1f}s")
        if done > 240:
            print("  （已超过 240 秒，提前收尾）")
            break

    print(f"\n枚举候选 {tried} 个，用时 {time.time() - started:.1f}s")
    print(f"命中     {len(found)} 个")
    if targets:
        print(f"覆盖率   {len(found) * 100.0 / len(targets):.2f}%（占当前待恢复）")
        whole = len(found) + len(restored)
        print(f"合计可知名 {whole} / {whole + (len(targets) - len(found))}")
    print("命中样本：" + ", ".join(list(found.values())[:6]))

    # 落盘到独立的对照文件，**不碰游戏目录**：
    # 运行时模块在每次扫描后会整体重写 HashRestore_RecoveredNames.lst，
    # 会话进行中往那里写会被覆盖掉。这份文件等会话结束后再合并（或当作补充 lst 用）。
    if len(sys.argv) > 5:
        out_path = sys.argv[5]
        with open(out_path, "w", encoding="utf-8", newline="\r\n") as fp:
            for digest, name in sorted(found.items(), key=lambda kv: kv[1]):
                fp.write(f"{digest}:{name}\n")
        print(f"已写出 {out_path}（{len(found)} 条 HASH:name）")
    return 0


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    lo = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    hi = int(sys.argv[3]) if len(sys.argv) > 3 else 60
    shi = int(sys.argv[4]) if len(sys.argv) > 4 else 1200
    sys.exit(main(out, lo, hi, shi))
