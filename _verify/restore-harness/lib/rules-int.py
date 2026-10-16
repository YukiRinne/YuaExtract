#!/usr/bin/env python3
# rules.int（扩展集档案）的**独立**读取器 / 破坏器 —— 验证脚手架专用，不属于产品。
#
# 为什么脚手架要自己解一遍档案：
#   1) 断言不能只信产品报告，也不能只信展开出来的 StaticHash.lst ——
#      DIRS / FILES / EXCEPTIONS / IDENTITY / STATS 这些 chunk 的内容必须能独立读到；
#   2) E-4「坏档案必须被拒」需要**故意做坏**的档案（CRC 错 / 截断 / 版本过高 / 缺 IDENTITY）；
#   3) E-7 幂等要逐 chunk 比对，E-8 体积要真实测量。
#
# 容器口径来自 docs/扩展集设计.md §3（**规格**，不是实现的复述）：
#   header 20B: magic 'YXS1' u32 | version u16 | headerSize u16 | flags u32 | crc32 u32 | chunkCount u32
#   chunk 表:  每项 12B: id u16 | flags u16 | offset u32 | size u32   （offset 相对 payload 起点）
#   payload:   起点 = headerSize + 12*chunkCount；flags bit0 = payload 经 zlib 压缩
#   crc32:     **未压缩** payload 的 CRC-32
#   chunk id:  META=1 IDENTITY=2 DIRS=3 FILES=4 EXCEPTIONS=5 RULES=6 STATS=7
#   未知 chunk 必须能跳过（前向兼容）—— dump 会如实列出未知 id。
#
# 用法：
#   python rules-int.py dump   --in <rules.int> [--json <out.json>] [--print]
#   python rules-int.py mutate --in <good rules.int> --outdir <dir> [--report <out.json>]
#
# dump 退出码：0 = 结构自洽（magic/版本/尺寸/CRC/偏移都在范围内）；3 = 结构有问题（并打印原因）。
# 这是"独立验证"的一部分：产品说档案好，这里要能自己判。

import argparse
import hashlib
import json
import os
import struct
import sys
import zlib

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

MAGIC = b"YXS1"
HEADER_SIZE = 20
CHUNK_TABLE_ENTRY = 12
CHUNK_NAMES = {
    0x0001: "META",
    0x0002: "IDENTITY",
    0x0003: "DIRS",
    0x0004: "FILES",
    0x0005: "EXCEPTIONS",
    0x0006: "RULES",
    0x0007: "STATS",
}
NAME_TO_ID = {name: cid for cid, name in CHUNK_NAMES.items()}
MAX_ARCHIVE_BYTES = 64 * 1024 * 1024   # 拒绝把畸形头当"几十 GB 的档案"来读


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest().upper()


def parse(path: str):
    with open(path, "rb") as fp:
        raw = fp.read()
    problems = []
    info = {
        "path": os.path.abspath(path),
        "fileSize": len(raw),
        "sha256": sha256_hex(raw),
        "problems": problems,
        "chunks": [],
    }
    if len(raw) < HEADER_SIZE:
        problems.append(f"文件只有 {len(raw)} 字节，连 {HEADER_SIZE} 字节的头都不够")
        return info, raw, None

    magic, version, header_size, flags, crc32_stored, chunk_count = struct.unpack_from("<4sHHIII", raw, 0)
    info.update({
        "magic": magic.decode("latin-1"),
        "version": version,
        "headerSize": header_size,
        "flags": flags,
        "compressed": bool(flags & 1),
        "crc32Stored": f"{crc32_stored:08X}",
        "chunkCount": chunk_count,
    })
    if magic != MAGIC:
        problems.append(f"magic 不是 'YXS1'（实际 {magic!r}）")
    if header_size != HEADER_SIZE:
        problems.append(f"headerSize 不是 {HEADER_SIZE}（实际 {header_size}）")
    if chunk_count > 4096:
        problems.append(f"chunkCount 大得离谱（{chunk_count}）")

    table_end = header_size + CHUNK_TABLE_ENTRY * chunk_count
    if table_end > len(raw):
        problems.append(f"chunk 表越界：需要 {table_end} 字节，文件只有 {len(raw)} 字节")
        return info, raw, None

    table = []
    for index in range(chunk_count):
        cid, cflags, offset, size = struct.unpack_from("<HHII", raw, header_size + index * CHUNK_TABLE_ENTRY)
        table.append({"id": cid, "name": CHUNK_NAMES.get(cid, f"UNKNOWN_0x{cid:04X}"), "flags": cflags,
                      "offset": offset, "size": size})

    payload_stored = raw[table_end:]
    payload = payload_stored
    if flags & 1:
        try:
            payload = zlib.decompress(payload_stored)
        except Exception as error:      # noqa: BLE001 - 报错信息要原样留证
            problems.append(f"payload 声称是 zlib 压缩，但解压失败：{error}")
            payload = b""
    info["payloadStoredSize"] = len(payload_stored)
    info["payloadSize"] = len(payload)

    crc_actual = zlib.crc32(payload) & 0xFFFFFFFF
    info["crc32Computed"] = f"{crc_actual:08X}"
    info["crcOk"] = (crc_actual == crc32_stored)
    if not info["crcOk"]:
        problems.append(f"CRC32 不符：头里 {crc32_stored:08X}，实算 {crc_actual:08X}")

    for entry in table:
        offset, size = entry["offset"], entry["size"]
        if offset + size > len(payload):
            problems.append(f"chunk {entry['name']} 越界：offset {offset} + size {size} > payload {len(payload)}")
            entry["data"] = b""
            info["chunks"].append(entry)
            continue
        entry["data"] = payload[offset:offset + size]
        info["chunks"].append(entry)

    return info, raw, payload


def _decode_text(data: bytes) -> str:
    if data.startswith(b"\xef\xbb\xbf"):
        data = data[3:]
    return data.decode("utf-8", errors="replace")


def _lines(text: str):
    return [line for line in text.replace("\r\n", "\n").split("\n") if line != ""]


def enrich(info):
    """把 DIRS/FILES/EXCEPTIONS/META/IDENTITY/STATS 解析成便于断言的形式。"""
    info["parsed"] = {}
    for entry in info["chunks"]:
        data = entry.get("data", b"")
        entry["sha256"] = sha256_hex(data)
        if entry["name"] == "DIRS":
            names = _lines(_decode_text(data))
            entry["count"] = len(names)
            info["parsed"]["dirs"] = names
        elif entry["name"] == "FILES":
            names = _lines(_decode_text(data))
            entry["count"] = len(names)
            info["parsed"]["files"] = names
        elif entry["name"] == "EXCEPTIONS":
            names = _lines(_decode_text(data))
            entry["count"] = len(names)
            info["parsed"]["exceptions"] = names
        elif entry["name"] == "META":
            text = _decode_text(data)
            entry["text"] = text
            info["parsed"]["meta"] = dict(
                line.split("=", 1) for line in _lines(text) if "=" in line
            )
        elif entry["name"] == "IDENTITY":
            text = _decode_text(data)
            entry["text"] = text
            identity = dict(line.split("=", 1) for line in _lines(text) if "=" in line)
            info["parsed"]["identity"] = identity
            for key in ("salt", "hash_domain", "mediaName", "media_name"):
                if key in identity:
                    info["parsed"]["salt"] = identity[key]
                    break
        elif entry["name"] == "STATS":
            text = _decode_text(data)
            entry["text"] = text
            info["parsed"]["stats"] = dict(
                line.split("=", 1) for line in _lines(text) if "=" in line
            )
    return info


def command_dump(args) -> int:
    info, _, _ = parse(args.input)
    enrich(info)
    problems = info["problems"]

    if args.print or not args.json:
        print(f"档案：{info['path']}")
        print(f"  大小 {info['fileSize']} 字节  sha256 {info['sha256'][:16]}  magic {info.get('magic')!r} "
              f"version {info.get('version')} flags {info.get('flags')} chunkCount {info.get('chunkCount')}")
        print(f"  payload 存储 {info.get('payloadStoredSize')} / 解压后 {info.get('payloadSize')}  "
              f"CRC {info.get('crc32Stored')}（实算 {info.get('crc32Computed')}，{'一致' if info.get('crcOk') else '不一致'}）")
        for entry in info["chunks"]:
            extra = ""
            if "count" in entry:
                extra = f"  条数 {entry['count']}"
            print(f"  chunk {entry['name']:<12} id=0x{entry['id']:04X} flags={entry['flags']} "
                  f"offset={entry['offset']} size={entry['size']}{extra}")
        parsed = info["parsed"]
        for key in ("dirs", "files", "exceptions"):
            if key in parsed:
                print(f"  {key}: {len(parsed[key])} 条")
        if "identity" in parsed:
            print(f"  IDENTITY: {json.dumps(parsed['identity'], ensure_ascii=False)}")
        if "meta" in parsed:
            print(f"  META: {json.dumps(parsed['meta'], ensure_ascii=False)}")
        if problems:
            print("  结构问题：")
            for item in problems:
                print("    - " + item)
        else:
            print("  结构自洽（magic/尺寸/CRC/偏移全部在范围内）")

    if args.json:
        payload = dict(info)
        for entry in payload["chunks"]:
            entry.pop("data", None)
        with open(args.json, "w", encoding="utf-8", newline="\n") as fp:
            json.dump(payload, fp, ensure_ascii=False, indent=2)

    return 3 if problems else 0


def build_archive(version: int, chunks, crc_override=None) -> bytes:
    """按规格重新拼一份档案（payload 不压缩；offset 连续）。chunks = [(id, bytes)]"""
    table = b""
    payload = b""
    for cid, data in chunks:
        table += struct.pack("<HHII", cid, 0, len(payload), len(data))
        payload += data
    crc = zlib.crc32(payload) & 0xFFFFFFFF if crc_override is None else crc_override
    header = struct.pack("<4sHHIII", MAGIC, version, HEADER_SIZE, 0, crc, len(chunks))
    return header + table + payload


def command_mutate(args) -> int:
    info, raw, payload = parse(args.input)
    if info["problems"]:
        print("源档案本身就有结构问题，拒绝拿它做坏档案：")
        for item in info["problems"]:
            print("    - " + item)
        return 3

    os.makedirs(args.outdir, exist_ok=True)
    report = {"source": info["path"], "sourceSha256": info["sha256"], "mutants": {}}

    # 1) CRC 错：payload 原样，只把头里的 CRC32 改坏（结构上其余字段都合法）
    crc_bad = int(info["crc32Stored"], 16) ^ 0xFFFFFFFF
    header = bytearray(raw[:HEADER_SIZE])
    struct.pack_into("<I", header, 12, crc_bad)
    crc_path = os.path.join(args.outdir, "crc-error.rules.int")
    with open(crc_path, "wb") as fp:
        fp.write(bytes(header) + raw[HEADER_SIZE:])
    report["mutants"]["crc-error"] = {
        "path": crc_path, "expect": "CRC 校验失败",
        "detail": f"头里 CRC32 改成 {crc_bad:08X}（正确值 {info['crc32Stored']}），payload 未动",
    }

    # 2) 截断：只留前 60%（头和 chunk 表还在，payload 缺一截）
    truncated_path = os.path.join(args.outdir, "truncated.rules.int")
    keep = max(HEADER_SIZE, int(len(raw) * 0.6))
    with open(truncated_path, "wb") as fp:
        fp.write(raw[:keep])
    report["mutants"]["truncated"] = {
        "path": truncated_path, "expect": "结构不完整 / 尺寸不符",
        "detail": f"{len(raw)} 字节截成 {keep} 字节",
    }

    # 3) 版本过高：version 改成 0xFFFF，其余（含 CRC）全部合法
    header = bytearray(raw[:HEADER_SIZE])
    struct.pack_into("<H", header, 4, 0xFFFF)
    version_path = os.path.join(args.outdir, "version-high.rules.int")
    with open(version_path, "wb") as fp:
        fp.write(bytes(header) + raw[HEADER_SIZE:])
    report["mutants"]["version-high"] = {
        "path": version_path, "expect": "版本过高 / 不支持",
        "detail": "version 改成 0xFFFF，CRC 与 chunk 表保持合法（专测版本判定，而不是被 CRC 顺带拦住）",
    }

    # 4) 缺 IDENTITY：从 chunk 表里摘掉，其余 chunk 原样保留，重算表与 CRC
    kept = [(entry["id"], entry["data"]) for entry in info["chunks"] if entry["name"] != "IDENTITY"]
    if len(kept) == len(info["chunks"]):
        print("源档案里本来就没有 IDENTITY chunk —— 夹具或实现有问题，先查这个")
        return 3
    missing_path = os.path.join(args.outdir, "missing-identity.rules.int")
    rebuilt = build_archive(info["version"], kept)
    with open(missing_path, "wb") as fp:
        fp.write(rebuilt)
    report["mutants"]["missing-identity"] = {
        "path": missing_path, "expect": "缺 IDENTITY / 没有归属信息",
        "detail": f"chunk 数 {len(info['chunks'])} -> {len(kept)}，表与 CRC 已按规格重算（payload 不压缩）",
    }

    # 5) 空名字层：保留 META/IDENTITY/STATS，去掉 DIRS/FILES/EXCEPTIONS/RULES。
    #    结构完全合法，但一个名字都没有 —— 生成静态 Hash 表时必须**明确报错、不写空表**
    #    （lead 同步的 task-1 事实 #5）。
    keep_ids = {NAME_TO_ID["META"], NAME_TO_ID["IDENTITY"], NAME_TO_ID["STATS"]}
    stripped = [(entry["id"], entry["data"]) for entry in info["chunks"] if entry["id"] in keep_ids]
    if len(stripped) == len(info["chunks"]):
        print("源档案本来就没有 DIRS/FILES —— 夹具不对，先查这个")
        return 3
    empty_path = os.path.join(args.outdir, "empty-names.rules.int")
    with open(empty_path, "wb") as fp:
        fp.write(build_archive(info["version"], stripped))
    report["mutants"]["empty-names"] = {
        "path": empty_path, "expect": "明确报错：没有名字，不写空表",
        "detail": f"chunk 数 {len(info['chunks'])} -> {len(stripped)}（只留 META/IDENTITY/STATS），表与 CRC 已重算",
    }

    if args.report:
        with open(args.report, "w", encoding="utf-8", newline="\n") as fp:
            json.dump(report, fp, ensure_ascii=False, indent=2)
    print(f"已生成 {len(report['mutants'])} 份坏档案 -> {args.outdir}")
    for name, entry in report["mutants"].items():
        print(f"  {name:<18} {os.path.basename(entry['path'])}  期望被拒：{entry['expect']}")
    return 0


def command_selftest(args) -> int:
    """自检：按 §3 规格自己造档案 -> dump 校验 -> 破坏 -> 校验破坏结果符合预期。
    这一层很重要：如果读取器本身读错了，E-7/E-8/E-9 会拿错数据去判产品（假失败/假通过）。"""
    import tempfile

    ok = True

    def check(label: str, condition: bool, detail: str = "") -> None:
        nonlocal ok
        ok = ok and condition
        print(f"  [{'OK' if condition else 'FAIL'}] {label}{('  ' + detail) if detail else ''}")

    work = tempfile.mkdtemp(prefix="yua-rules-selftest-")
    meta = "publisher=YuaHarness\ntitle=otomeki_ckr\nversion=v1\n".encode("utf-8")
    identity = "exe=otomeki_ckr.exe\nexe_sha256=" + ("A" * 64) + "\nsalt=xp3hnp\nsaltKnown=true\n".encode("utf-8").decode("utf-8")
    identity_bytes = identity.encode("utf-8")
    dirs = "characters/yuki/\nui/\n/".encode("utf-8")
    files = "base.png\nstand_a.png\n".encode("utf-8")
    exceptions = "DEADBEEFDEADBEEF:weird.bin".encode("utf-8")
    stats = "dirs=3\nfiles=2\nexceptions=1\n".encode("utf-8")
    unknown = b"\x01\x02\x03\x04"

    chunks = [(1, meta), (2, identity_bytes), (3, dirs), (4, files), (5, exceptions), (7, stats), (0x00FF, unknown)]
    plain_path = os.path.join(work, "plain.rules.int")
    with open(plain_path, "wb") as fp:
        fp.write(build_archive(1, chunks))

    # 压缩变体（flags bit0 = 1）
    table = b""
    payload = b""
    for cid, data in chunks:
        table += struct.pack("<HHII", cid, 0, len(payload), len(data))
        payload += data
    compressed = zlib.compress(payload)
    header = struct.pack("<4sHHIII", MAGIC, 1, HEADER_SIZE, 1, zlib.crc32(payload) & 0xFFFFFFFF, len(chunks))
    compressed_path = os.path.join(work, "compressed.rules.int")
    with open(compressed_path, "wb") as fp:
        fp.write(header + table + compressed)

    for path, label in ((plain_path, "未压缩"), (compressed_path, "zlib 压缩")):
        info, _, _ = parse(path)
        enrich(info)
        check(f"{label}：结构自洽（无 problems）", not info["problems"], "；".join(info["problems"]))
        check(f"{label}：CRC 一致", bool(info.get("crcOk")))
        check(f"{label}：DIRS 3 条", info["parsed"].get("dirs") == ["characters/yuki/", "ui/", "/"])
        check(f"{label}：FILES 2 条", info["parsed"].get("files") == ["base.png", "stand_a.png"])
        check(f"{label}：EXCEPTIONS 1 条", len(info["parsed"].get("exceptions", [])) == 1)
        check(f"{label}：META 解析出 publisher", info["parsed"].get("meta", {}).get("publisher") == "YuaHarness")
        check(f"{label}：IDENTITY 解析出盐", info["parsed"].get("salt") == "xp3hnp")
        check(f"{label}：未知 chunk 被跳过（前向兼容）",
              any(c["name"].startswith("UNKNOWN_") for c in info["chunks"]))

    # 破坏器：四种坏档案必须真的"坏"在预期的地方
    outdir = os.path.join(work, "bad")
    report_path = os.path.join(work, "bad.json")

    class _Args:
        pass

    mutate_args = _Args()
    mutate_args.input = plain_path
    mutate_args.outdir = outdir
    mutate_args.report = report_path
    code = command_mutate(mutate_args)
    check("mutate 退出码 0", code == 0, f"code={code}")

    crc_info, _, _ = parse(os.path.join(outdir, "crc-error.rules.int"))
    check("crc-error：CRC 校验失败被检出", not crc_info["crcOk"] and any("CRC" in p for p in crc_info["problems"]))

    trunc_info, _, _ = parse(os.path.join(outdir, "truncated.rules.int"))
    check("truncated：结构问题被检出", bool(trunc_info["problems"]), "；".join(trunc_info["problems"]))

    ver_info, _, _ = parse(os.path.join(outdir, "version-high.rules.int"))
    check("version-high：版本字段 = 65535 且结构其余部分合法", ver_info.get("version") == 0xFFFF and not ver_info["problems"])

    miss_info, _, _ = parse(os.path.join(outdir, "missing-identity.rules.int"))
    enrich(miss_info)
    check("missing-identity：IDENTITY 真的没了且结构合法",
          not any(c["name"] == "IDENTITY" for c in miss_info["chunks"]) and not miss_info["problems"]
          and miss_info["parsed"].get("dirs") == ["characters/yuki/", "ui/", "/"])

    empty_info, _, _ = parse(os.path.join(outdir, "empty-names.rules.int"))
    enrich(empty_info)
    check("empty-names：结构合法但没有任何名字",
          not empty_info["problems"]
          and not any(c["name"] in ("DIRS", "FILES", "EXCEPTIONS") for c in empty_info["chunks"])
          and any(c["name"] == "IDENTITY" for c in empty_info["chunks"]))

    print(f"自检工作目录：{work}")
    print("结论：" + ("全部通过" if ok else "存在失败项"))
    return 0 if ok else 1



def command_make(args) -> int:
    """用**本文件里那份构造器**（build_archive，规格来自 §3）造一份结构合法的最小档案。

    为什么要有这条命令：自检（self-test-extension.ps1）要造一个"合成世界"来证明断言会炸，
    而 E-18/E-19 的断言会**独立解析仓库里的档案**（sha256 / IDENTITY / STATS），
    所以那份档案必须真的是合法档案，不能是随手写的字节。
    注意它**不是给验收夹具用的**：E-18/E-19 的正式夹具一律用产品生成的档案
    （make-online-repo.ps1 的 -OldArchive/-OtherArchive），这一条只服务于断言自身的鉴别力自检。
    """
    directory = os.path.dirname(os.path.abspath(args.out))
    if directory:
        os.makedirs(directory, exist_ok=True)
    meta = ("publisher=" + args.publisher + "\ntitle=" + args.title + "\ncontributor=selftest\n"
            "version=v1\ndate=\ncomment=" + args.comment + "\ngeneratedAt=\ntoolVersion=selftest\n")
    identity = ("target.name=" + args.title + ".exe\ntarget.size=1000\ntarget.sha256=" + args.exe_sha + "\n"
                "sibling.name=\nsibling.size=\nsibling.sha256=\n"
                "saltKnown=true\nsalt=" + args.salt + "\nsourceDirectory=\n")
    dirs = args.dir or []
    files = args.file or []
    dirs_bytes = ("\n".join(dirs) + "\n").encode("utf-8") if dirs else b""
    files_bytes = ("\n".join(files) + "\n").encode("utf-8") if files else b""
    stats = ("sources.directoryLog=1\nsources.fileNameLog=1\nsources.minedNames=0\n"
             "sources.recoveredNames=0\nsources.otherLists=0\n"
             "directories=" + str(len(dirs)) + "\nfiles=" + str(len(files)) + "\n"
             "exceptions=0\nduplicates=0\n")
    chunks = [
        (NAME_TO_ID["META"], meta.encode("utf-8")),
        (NAME_TO_ID["IDENTITY"], identity.encode("utf-8")),
        (NAME_TO_ID["DIRS"], dirs_bytes),
        (NAME_TO_ID["FILES"], files_bytes),
        (NAME_TO_ID["EXCEPTIONS"], b""),
        (NAME_TO_ID["STATS"], stats.encode("utf-8")),
    ]
    blob = build_archive(1, chunks)
    with open(args.out, "wb") as fp:
        fp.write(blob)
    print("已写出合成档案：" + args.out + "（" + str(len(blob)) + " 字节，会社 " + args.publisher +
          " / 作品 " + args.title + " / 目录 " + str(len(dirs)) + " 条 / 文件 " + str(len(files)) + " 条）")
    return 0

def main() -> int:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    dump = sub.add_parser("dump")
    dump.add_argument("--in", dest="input", required=True)
    dump.add_argument("--json", default="")
    dump.add_argument("--print", action="store_true")
    dump.set_defaults(func=command_dump)

    mutate = sub.add_parser("mutate")
    mutate.add_argument("--in", dest="input", required=True)
    mutate.add_argument("--outdir", required=True)
    mutate.add_argument("--report", default="")
    mutate.set_defaults(func=command_mutate)

    selftest = sub.add_parser("selftest")
    selftest.set_defaults(func=command_selftest)

    # 合成一份结构合法的最小档案（**只给"断言鉴别力自检"用**，见 command_make 的说明）
    make = sub.add_parser("make")
    make.add_argument("--out", required=True)
    make.add_argument("--publisher", required=True)
    make.add_argument("--title", required=True)
    make.add_argument("--salt", default="xp3hnp")
    make.add_argument("--exe-sha", dest="exe_sha", default="A" * 64)
    make.add_argument("--comment", default="")
    make.add_argument("--dir", action="append", default=[])
    make.add_argument("--file", action="append", default=[])
    make.set_defaults(func=command_make)

    args = parser.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())


