#!/usr/bin/env python3
# 扩展集夹具侧的名字 -> 哈希计算器（验证脚手架专用，不属于产品）。
#
# 口径与产品**完全一致**（CxdecRepacker/core/resource_hash.cpp）：
#     fileHash   = BLAKE2s-256(UTF16LE(裸名 + 盐))                        32 字节 -> 64 位大写 hex
#     domainHash = bswap64( SipHash-2-4(UTF16LE(目录 + 盐), key = 16×0) )  8 字节 -> 16 位大写 hex
#     root       = 目录为空串（产品把空串与 "/" 都当根），即只哈希盐
# 目录名按产品的归一化口径处理：'\' -> '/'、转小写、补结尾 '/'、根记 "/"。
#
# 为什么要单独写一个 Python 计算器，而不是让 PowerShell 自己算：
#   夹具的哈希必须与产品对得上，自己实现等于"自己给自己判卷"。
#   另外还有一重锚点：build-hashprobe.ps1 会编出 HashProbe.exe，
#   它**直接调用产品自己的 resource_hash.cpp** 算同一批名字；
#   make-extension-fixture.ps1 会拿两者的结果逐条对账，不一致就抛错（--crosscheck）。
#
# 用法：
#   python hash-extension.py --selftest
#   python hash-extension.py --spec <spec.tsv> --out <out.tsv> --salt xp3hnp [--no-normalize]
#   python hash-extension.py --spec a.tsv --out a.tsv --salt s --crosscheck <probe.tsv>
#
# spec 每行（UTF-8，制表符分隔，# 开头是注释）：
#   FILE<TAB>base.png                  裸文件名，不加盐以外的任何处理
#   DIR<TAB>characters/yuki            目录；默认按归一化口径算哈希
#   DIR<TAB>Characters\Yuki<TAB>raw    第三个字段 raw = **按原样算哈希**（不归一化）
#   ROOT<TAB><空>                      根域；归一化成 "/"，哈希 = 只哈希盐
#   DIR<TAB>characters/yuki<TAB>norm   显式归一化（与默认相同）
#
# out 每行：KIND<TAB>原始输入<TAB>归一化后<TAB>HASH

import argparse
import sys
from hashlib import blake2s

# 控制台默认代码页会把中文打成乱码（GBK 控制台下实测），显式钉成 UTF-8。
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

DEFAULT_SEED = "xp3hnp"

# RFC 7693：BLAKE2s-256 空消息
RFC7693_BLAKE2S_EMPTY = "69217a3079908094e11121d042354a7c1f55b6482ca1a51e1b250dfd1ed0eef9"
# SipHash-2-4 官方参考向量：key = 000102...0f，msg = 00 01 ... 0e（15 字节）
SIPHASH_VECTORS = {
    0: 0x726FDB47DD0E0E31,
    1: 0x74F839C593DC67FD,
    8: 0x93F5F5799A932462,
    15: 0xA129CA6149BE45E5,
}

MASK64 = (1 << 64) - 1


def _rotl(v: int, c: int) -> int:
    return ((v << c) | (v >> (64 - c))) & MASK64


def siphash24(data: bytes, key: bytes) -> int:
    """SipHash-2-4（2 轮压缩 / 4 轮终结），与 CxdecRepacker/crypto/siphash.cpp 同构。"""
    if len(key) != 16:
        raise ValueError("SipHash 的 key 必须 16 字节")
    k0 = int.from_bytes(key[0:8], "little")
    k1 = int.from_bytes(key[8:16], "little")

    v0 = 0x736F6D6570736575 ^ k0
    v1 = 0x646F72616E646F6D ^ k1
    v2 = 0x6C7967656E657261 ^ k0
    v3 = 0x7465646279746573 ^ k1

    def sipround():
        nonlocal v0, v1, v2, v3
        v0 = (v0 + v1) & MASK64
        v1 = _rotl(v1, 13) ^ v0
        v0 = _rotl(v0, 32)
        v2 = (v2 + v3) & MASK64
        v3 = _rotl(v3, 16) ^ v2
        v0 = (v0 + v3) & MASK64
        v3 = _rotl(v3, 21) ^ v0
        v2 = (v2 + v1) & MASK64
        v1 = _rotl(v1, 17) ^ v2
        v2 = _rotl(v2, 32)

    full = len(data) & ~7
    for offset in range(0, full, 8):
        m = int.from_bytes(data[offset:offset + 8], "little")
        v3 ^= m
        sipround()
        sipround()
        v0 ^= m

    b = (len(data) & 0xFF) << 56
    tail = data[full:]
    for index, byte in enumerate(tail):
        b |= byte << (8 * index)

    v3 ^= b
    sipround()
    sipround()
    v0 ^= b
    v2 ^= 0xFF
    for _ in range(4):
        sipround()
    return v0 ^ v1 ^ v2 ^ v3


def bswap64(value: int) -> int:
    return int.from_bytes(value.to_bytes(8, "big"), "little")


def normalize_dir(name: str) -> str:
    """目录归一化：'\\' -> '/'、转小写、补结尾 '/'；根（空串或 '/' 或 %EmptyString%）记 '/'。"""
    text = (name or "").strip()
    if text in ("", "/", "\\", "%EmptyString%"):
        return "/"
    text = text.replace("\\", "/")
    while "//" in text:
        text = text.replace("//", "/")
    text = text.strip("/").lower()
    if not text:
        return "/"
    return text + "/"


def file_hash(name: str, seed: str) -> str:
    h = blake2s(digest_size=32)
    h.update((name + seed).encode("utf-16-le"))
    return h.digest().hex().upper()


def domain_hash(path: str, seed: str) -> str:
    """path 必须是**已归一化**的目录（根为 '/' 或空串）。"""
    root = path in ("", "/")
    message = ("" if root else path) + seed
    value = bswap64(siphash24(message.encode("utf-16-le"), b"\x00" * 16))
    return value.to_bytes(8, "big").hex().upper()


def selftest() -> int:
    ok = True

    empty = blake2s(digest_size=32).hexdigest().upper()
    same = empty == RFC7693_BLAKE2S_EMPTY.upper()
    print(f"blake2s-256(empty) = {empty}  -> {'OK' if same else 'MISMATCH'}")
    ok = ok and same

    key = bytes(range(16))
    for length, expected in sorted(SIPHASH_VECTORS.items()):
        actual = siphash24(bytes(range(length)), key)
        same = actual == expected
        print(f"siphash24(len={length:2d}) = 0x{actual:016X}  期望 0x{expected:016X}  -> {'OK' if same else 'MISMATCH'}")
        ok = ok and same

    # 归一化单测（E-9 的脚手架侧口径）
    cases = [
        ("Characters\\Yuki", "characters/yuki/"),
        ("characters/yuki", "characters/yuki/"),
        ("characters/yuki/", "characters/yuki/"),
        ("", "/"),
        ("/", "/"),
        ("\\", "/"),
        ("%EmptyString%", "/"),
        ("UI", "ui/"),
        ("a//b", "a/b/"),
    ]
    for raw, expected in cases:
        actual = normalize_dir(raw)
        same = actual == expected
        print(f"normalize_dir({raw!r}) = {actual!r}  期望 {expected!r}  -> {'OK' if same else 'MISMATCH'}")
        ok = ok and same

    print(f"样例：base.png -> {file_hash('base.png', DEFAULT_SEED)}")
    print(f"样例：characters/yuki/ -> {domain_hash('characters/yuki/', DEFAULT_SEED)}")
    print(f"样例：根 '/' -> {domain_hash('/', DEFAULT_SEED)}")
    return 0 if ok else 1


def parse_spec(path: str):
    items = []
    with open(path, "r", encoding="utf-8") as fp:
        for number, line in enumerate(fp, start=1):
            line = line.rstrip("\r\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            kind = parts[0].strip().upper()
            raw = parts[1] if len(parts) > 1 else ""
            mode = (parts[2].strip().lower() if len(parts) > 2 and parts[2].strip() else "")
            if kind not in ("FILE", "DIR", "ROOT"):
                raise SystemExit(f"{path}:{number} 未知 KIND={kind!r}（只认 FILE/DIR/ROOT）")
            if mode not in ("", "norm", "raw"):
                raise SystemExit(f"{path}:{number} 未知第三列={mode!r}（只认 norm/raw）")
            if kind == "FILE" and mode == "raw":
                raise SystemExit(f"{path}:{number} FILE 不支持 raw（裸名没有归一化）")
            items.append((kind, raw, mode))
    return items


def compute(items, seed: str, no_normalize: bool):
    rows = []
    for kind, raw, mode in items:
        if kind == "FILE":
            rows.append(("FILE", raw, raw, file_hash(raw, seed)))
            continue
        if kind == "ROOT":
            rows.append(("ROOT", raw, "/", domain_hash("/", seed)))
            continue
        use_raw = no_normalize or mode == "raw"
        normalized = raw if use_raw else normalize_dir(raw)
        rows.append(("DIR", raw, normalized, domain_hash(normalized, seed)))
    return rows


def emit_probe_spec(rows, path: str) -> None:
    """写出给 HashProbe.exe 用的 spec：**哈希的输入必须是最终要哈希的那个串**。
    坑：HashProbe 只负责哈希，不负责归一化 —— 若把未归一化的目录名喂给它，
    它算的是 'characters/yuki' 而不是 'characters/yuki/'，对账就会全红。"""
    with open(path, "w", encoding="utf-8", newline="\n") as fp:
        for kind, _raw, normalized, _digest in rows:
            if kind == "ROOT":
                fp.write("ROOT\t\n")
            else:
                fp.write(f"{kind}\t{normalized}\n")


def crosscheck(rows, probe_path: str) -> int:
    """把 Python 的结果与 HashProbe.exe（产品自己的 resource_hash.cpp）逐条对账。"""
    probe = {}
    with open(probe_path, "r", encoding="utf-8") as fp:
        for line in fp:
            line = line.rstrip("\r\n")
            if not line.strip():
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                continue
            # HashProbe 输出 3 列（KIND/输入/哈希），本脚本输出 4 列（多一列归一化结果）——
            # 统一取**最后一列**当哈希，两种格式都能对账。
            probe[(parts[0].strip().upper(), parts[1].strip())] = parts[-1].strip().upper()

    bad = []
    for kind, raw, normalized, digest in rows:
        key = ("ROOT", "") if kind == "ROOT" else (kind, normalized)
        if key not in probe:
            bad.append(f"{kind} {raw!r}（哈希输入 {normalized!r}）：HashProbe 没有输出这一条")
            continue
        if probe[key] != digest.upper():
            bad.append(f"{kind} {raw!r}（哈希输入 {normalized!r}）：Python {digest} != 产品 {probe[key]}")
    if bad:
        print("交叉对账失败（夹具哈希与产品不一致，这样的夹具会冤枉实现）：")
        for item in bad[:20]:
            print("    " + item)
        return 1
    print(f"交叉对账通过：{len(rows)} 条与 HashProbe（产品 resource_hash.cpp）逐条一致")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--spec")
    parser.add_argument("--out")
    parser.add_argument("--salt", default=DEFAULT_SEED)
    parser.add_argument("--no-normalize", action="store_true")
    parser.add_argument("--crosscheck", default="")
    parser.add_argument("--emit-probe-spec", dest="emit_probe_spec", default="")
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()

    if args.selftest:
        return selftest()
    if not args.spec or not args.out:
        parser.error("需要 --spec 与 --out（或 --selftest）")

    items = parse_spec(args.spec)
    rows = compute(items, args.salt, args.no_normalize)

    with open(args.out, "w", encoding="utf-8", newline="\n") as fp:
        for kind, raw, normalized, digest in rows:
            fp.write(f"{kind}\t{raw}\t{normalized}\t{digest}\n")
    print(f"已写入 {len(rows)} 条 -> {args.out}（盐 {args.salt!r}）")

    if args.emit_probe_spec:
        emit_probe_spec(rows, args.emit_probe_spec)
        print(f"已写出 HashProbe 用 spec（哈希输入 = 归一化后的串）-> {args.emit_probe_spec}")

    if args.crosscheck:
        return crosscheck(rows, args.crosscheck)
    return 0


if __name__ == "__main__":
    sys.exit(main())
