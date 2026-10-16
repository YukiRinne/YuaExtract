# 离线哈希校验：拿工具自己写下的"hash -> 明文名"当标准答案，验证我们能否离线复算。
#
# 依据（都来自实测日志）：
#   Log\Universal.log : Hash Seed = xp3hnp，两个 HasherSalt 均为空
#   Log\HashRestore.log : RESTORED | source="voice\94D4...\<fileHash>" ... fileName="mor_006_0017.ogg"
#
# 若 BLAKE2s-256(UTF16LE(名字 + "xp3hnp")) 能复算出同一个 fileHash，
# 那"不启动游戏、离线生成整份映射表"这条路就是成立的。

import re
import sys
from hashlib import blake2s

SALT = "xp3hnp"


def file_hash(name: str) -> str:
    h = blake2s(digest_size=32)
    h.update((name + SALT).encode("utf-16-le"))
    return h.digest().hex().upper()


def main(log_path: str):
    pattern = re.compile(
        r'RESTORED.*?fileHash=([0-9A-F]{64}).*?fileName="([^"]+)"'
    )
    pairs = []
    seen = set()
    with open(log_path, "r", encoding="utf-8", errors="replace") as fp:
        for line in fp:
            m = pattern.search(line)
            if not m:
                continue
            fh, name = m.group(1), m.group(2)
            if fh in seen:
                continue
            seen.add(fh)
            pairs.append((fh, name))

    if not pairs:
        print("没从 HashRestore.log 里解析到 RESTORED 记录")
        return 1

    ok = 0
    bad = []
    for fh, name in pairs:
        if file_hash(name) == fh:
            ok += 1
        else:
            bad.append((name, fh, file_hash(name)))

    total = len(pairs)
    print(f"样本条数        : {total}")
    print(f"离线复算命中    : {ok}")
    print(f"不命中          : {total - ok}")
    if total:
        print(f"命中率          : {ok * 100.0 / total:.2f}%")

    if bad:
        print("\n前 10 条不命中的：")
        for name, expect, got in bad[:10]:
            print(f"  {name}\n    工具记录 = {expect}\n    离线复算 = {got}")
    else:
        print("\n全部命中 —— 离线生成映射表这条路成立。")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "HashRestore.log"))
