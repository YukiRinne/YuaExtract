#!/usr/bin/env python3
# 夹具侧的名字→哈希计算器（验证脚手架专用，不属于产品）。
#
# 口径与产品/参考实现一致（_verify/watch/recover_from_scripts.py）：
#     fileHash = BLAKE2s-256(key 为空, UTF16LE(名字 + seed))      小写 hex → 这里输出大写
#     seed 默认 "xp3hnp"（游戏 Log\Universal.log 里的 Hash Seed）
#
# 特意用 Python 的 hashlib.blake2s 来算，而不是在 PowerShell 里自己实现一遍 ——
# 夹具的哈希必须和产品对得上，自己实现等于"自己给自己判卷"。
#
# 用法：
#   python hash-names.py --in <名字清单 UTF-8，一行一个> --out <输出 name<TAB>HASH>
#   python hash-names.py --selftest
#
# 输入输出都走文件（不走 stdout），避免命令行/控制台的编码把中文名字弄坏。

import argparse
import sys
from hashlib import blake2s

DEFAULT_SEED = "xp3hnp"
RFC7693_BLAKE2S_EMPTY = "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9"


def file_hash(name: str, seed: str) -> str:
    h = blake2s(digest_size=32)
    h.update((name + seed).encode("utf-16-le"))
    return h.digest().hex().upper()


def selftest() -> int:
    # 先用 RFC 7693 的空消息向量把 hashlib 的 blake2s 钉住，
    # 再给出本夹具实际用到的两个样例（便于人工核对）。
    empty = blake2s(digest_size=32).hexdigest()
    ok = empty == RFC7693_BLAKE2S_EMPTY
    print(f"blake2s-256(empty) = {empty}")
    print(f"RFC 7693 向量      = {RFC7693_BLAKE2S_EMPTY}  -> {'OK' if ok else 'MISMATCH'}")
    for name in ("anj_000_0001.ogg", "bgm_010_0003.bin", "nosuch_999_9999"):
        print(f"  {name} -> {file_hash(name, DEFAULT_SEED)}")
    return 0 if ok else 1


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--in", dest="in_path")
    parser.add_argument("--out", dest="out_path")
    parser.add_argument("--seed", default=DEFAULT_SEED)
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.in_path or not args.out_path:
        parser.error("需要 --in 与 --out（或 --selftest）")

    with open(args.in_path, "r", encoding="utf-8") as fp:
        names = [line.rstrip("\r\n") for line in fp]
    names = [name for name in names if name]

    with open(args.out_path, "w", encoding="utf-8", newline="\n") as fp:
        for name in names:
            fp.write(f"{name}\t{file_hash(name, args.seed)}\n")
    print(f"已写入 {len(names)} 条 -> {args.out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
