# 语音名离线批量恢复的可行性测试（只读，不改任何文件）。
#
# 已验证：fileHash = BLAKE2s-256(UTF16LE(名字 + "xp3hnp"))，与工具记录 2804/2804 一致。
# 语音文件的目录哈希都是根域 94D4A97C61498621，所以只需要枚举"文件名"。
#
# 本脚本：
#   1. 从已还原的明文名里学出命名规律（前缀 / 章节 / 序号 / 后缀）
#   2. 按 (前缀, 章节) 组合自适应地把序号从 0001 往上枚举，遇到长串未命中就停
#   3. 算出哈希，与磁盘上"仍是 Hash 名"的文件集合求交集 —— 命中的就是白捡回来的名字
#   4. 只报告统计，不写任何文件（要不要真写进去由人决定）

import os
import re
import sys
from collections import Counter, defaultdict
from hashlib import blake2s

SALT = "xp3hnp"
HEX64 = re.compile(r"^[0-9A-Fa-f]{64}$")
NAME_PATTERN = re.compile(r"^(?P<prefix>.+?)_(?P<chapter>\d{3})_(?P<serial>\d{4})(?P<ext>\.\w+)$")


def file_hash(name: str) -> str:
    h = blake2s(digest_size=32)
    h.update((name + SALT).encode("utf-16-le"))
    return h.digest().hex().upper()


def main(output_dir: str, max_serial: int = 2000, miss_limit: int = 120):
    targets = set()        # 仍是 Hash 名的文件名（= 待恢复的文件哈希）
    restored = []          # 已还原的明文名
    dir_hashes = Counter()  # 语音包下出现过的目录哈希

    for pkg in ("voice", "voice2"):
        pkg_dir = os.path.join(output_dir, pkg)
        if not os.path.isdir(pkg_dir):
            continue
        for root, _dirs, files in os.walk(pkg_dir):
            rel = os.path.relpath(root, pkg_dir)
            parts = rel.split(os.sep)
            if parts and re.fullmatch(r"[0-9A-Fa-f]{16}", parts[0]):
                dir_hashes[parts[0].upper()] += 1
            for f in files:
                if HEX64.match(f):
                    targets.add(f.upper())
                else:
                    restored.append(f)

    print(f"待恢复（仍是 Hash 名）: {len(targets)}")
    print(f"已还原（学规律用）    : {len(restored)}")
    print(f"语音包目录哈希分布    : {dict(dir_hashes)}")

    if not restored:
        print("没有已还原样本，无法学规律")
        return 1

    prefix_ext = Counter()
    prefix_chapters = defaultdict(set)
    for name in restored:
        m = NAME_PATTERN.match(name)
        if not m:
            prefix_ext[("(不匹配规律)", os.path.splitext(name)[1])] += 1
            continue
        prefix_ext[(m.group("prefix"), m.group("ext"))] += 1
        prefix_chapters[m.group("prefix")].add(m.group("chapter"))

    print(f"\n命名规律：前缀 {len(prefix_chapters)} 个，后缀分布 {dict(Counter(e for _, e in prefix_ext))}")
    top = prefix_ext.most_common(8)
    print("前缀样本：" + ", ".join(f"{p}{e}×{c}" for (p, e), c in top))

    found = {}
    tried = 0
    for prefix, chapters in prefix_chapters.items():
        for chapter in sorted(chapters):
            misses = 0
            serial = 1
            while serial <= max_serial and misses < miss_limit:
                name = f"{prefix}_{chapter}_{serial:04d}.ogg"
                tried += 1
                digest = file_hash(name)
                if digest in targets:
                    found[digest] = name
                    misses = 0
                else:
                    misses += 1
                serial += 1

    print(f"\n枚举候选            : {tried}")
    print(f"命中（可白捡回来）  : {len(found)}")
    if targets:
        print(f"占待恢复比例        : {len(found) * 100.0 / len(targets):.2f}%")

    if found:
        sample = list(found.values())[:5]
        print("命中样本            : " + ", ".join(sample))

    misses_left = len(targets) - len(found)
    print(f"\n仍然缺名            : {misses_left}")
    if misses_left:
        print("说明：缺的部分需要更多前缀/章节被发现，或改用脚本推导候选名。")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("用法: probe_voice_recovery.py <Extractor_Output 目录>")
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
