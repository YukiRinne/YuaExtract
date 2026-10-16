# 离线批量恢复 v2：面向"非语音资源"（evimage / bgimage / image / data / uipsd …）
#
# v1 的教训：只挖了 ASCII 的语音模式（前缀_3位_4位），所以图片类几乎一个没恢复。
# 实际情况（已实测）：
#   - 脚本里资源名以 **UTF-8** 存着，日文名也是 UTF-8（不是 UTF-16）
#   - 名字**不带扩展名**，扩展名由引擎拼接
#   - scn 包里未还原的脚本在子目录里，必须递归扫
#
# 两条候选来源：
#   A. 从脚本（scn/data/uipsd/image…）里挖所有"像资源名"的 UTF-8 串
#   B. 从已还原的名字里学模板，做定向枚举（如 ev###<字母>、sd###<字母>##）
# 命中判定依然是哈希对撞，不会误判。

import os
import re
import sys
import time
from collections import defaultdict
from hashlib import blake2s

SALT = "xp3hnp"
HEX64 = re.compile(r"^[0-9A-Fa-f]{64}$")
HEX16 = re.compile(r"^[0-9A-Fa-f]{16}$")
# 资源名候选：\w 在 str 模式下含 Unicode 字母（日文假名/汉字都算），
# 所以对话里的「。、！？」会把句子切断，长句再被长度上限滤掉。
CANDIDATE = re.compile(r"[\w\u3040-\u30ff\u4e00-\u9fff][\w\u3040-\u30ff\u4e00-\u9fff.\-]{1,47}")

# 扫描范围：脚本/配置所在的包（图片音频包是纯二进制，扫了也没名字）
SCAN_PACKAGES = ("scn", "data", "uipsd", "image", "bgimage", "evimage", "evimage2", "fgimage")
# 引擎会拼的扩展名（取自已还原名字里实际出现过的）
EXTENSIONS = (".png", ".tlg", ".pimg", ".pbd", ".asd", ".stand", ".jpg", ".webp",
              ".opus", ".ogg", ".mchx", ".wav", ".mp3",
              ".tjs", ".ini", ".csv", ".tft", ".ks", ".scn", ".mtn", ".toml",
              ".txt", ".dat", ".psb", ".json")
SIDECAR_EXTS = (".opus", ".ogg", ".mchx", ".wav", ".mp3")   # 这些后面还可能跟 .sli


def file_hash(name: str) -> str:
    h = blake2s(digest_size=32)
    h.update((name + SALT).encode("utf-16-le"))
    return h.digest().hex().upper()


def collect_targets(output_dir):
    targets = {}
    for pkg in sorted(os.listdir(output_dir)):
        pkg_dir = os.path.join(output_dir, pkg)
        if not os.path.isdir(pkg_dir):
            continue
        for root, _dirs, files in os.walk(pkg_dir):
            for f in files:
                if HEX64.match(f):
                    targets[f.upper()] = pkg
    return targets


def collect_restored(output_dir):
    """已还原的明文名（用来学扩展名和模板）。"""
    names = defaultdict(list)
    for pkg in sorted(os.listdir(output_dir)):
        pkg_dir = os.path.join(output_dir, pkg)
        if not os.path.isdir(pkg_dir):
            continue
        for root, _dirs, files in os.walk(pkg_dir):
            for f in files:
                if not HEX64.match(f):
                    names[pkg].append(f)
    return names


def mine_script_candidates(output_dir):
    """A. 递归扫脚本包，挖 UTF-8 名字串。"""
    candidates = set()
    scanned = 0
    total_bytes = 0
    for pkg in SCAN_PACKAGES:
        pkg_dir = os.path.join(output_dir, pkg)
        if not os.path.isdir(pkg_dir):
            continue
        for root, _dirs, files in os.walk(pkg_dir):
            for f in files:
                path = os.path.join(root, f)
                try:
                    size = os.path.getsize(path)
                    if size > 32 * 1024 * 1024:
                        continue
                    with open(path, "rb") as fp:
                        raw = fp.read()
                except OSError:
                    continue
                scanned += 1
                total_bytes += len(raw)
                text = raw.decode("utf-8", errors="ignore")
                for match in CANDIDATE.finditer(text):
                    token = match.group(0).strip(".-")
                    if 2 <= len(token) <= 48:
                        candidates.add(token)
    print(f"  A. 扫描 {scanned} 个文件 / {total_bytes / 1024 / 1024:.0f} MB，"
          f"挖出候选 {len(candidates)} 个（{scanned and 0:.0f}）", flush=True)
    return candidates


DIGITS = re.compile(r"\d+")


def templates_from(names):
    """B. 从已还原名字里学模板：数字段 → 可枚举，单字母 → a..z。"""
    templates = set()
    for name in names:
        if not name.isascii() or len(name) > 24 or "." not in name:
            continue
        stem, ext = os.path.splitext(name)
        if not DIGITS.search(stem):
            continue
        fields = []
        position = 0
        for match in DIGITS.finditer(stem):
            if match.start() > position:
                fields.append(("lit", stem[position:match.start()]))
            fields.append(("num", len(match.group(0))))
            position = match.end()
        if position < len(stem):
            fields.append(("lit", stem[position:]))
        templates.add((tuple(fields), ext))
    return templates


def enumerate_template(fields, ext, cap=400000):
    """把模板展开成候选名；数字段按位宽全枚举，字面量里的单字母也展开。"""
    results = [""]
    for kind, value in fields:
        if kind == "lit":
            # 字面量里的字母不做展开（太容易爆），原样保留
            results = [r + value for r in results]
        else:
            width = value
            limit = 10 ** width
            if len(results) * limit > cap:
                limit = max(1, cap // max(1, len(results)))
            results = [f"{r}{i:0{width}d}" for r in results for i in range(limit)]
            if len(results) > cap:
                results = results[:cap]
    return [r + ext for r in results]


def letter_variants(stem, ext, cap=60000):
    """把名字里最后一个单独的小写字母位展开成 a..z（ev201a → ev201b…）。"""
    for index in range(len(stem) - 1, -1, -1):
        char = stem[index]
        if char.isalpha() and char.islower():
            if index > 0 and stem[index - 1].isdigit():
                prefix, suffix = stem[:index], stem[index + 1:]
                return [f"{prefix}{c}{suffix}{ext}" for c in "abcdefghijklmnopqrstuvwxyz"][:cap]
    return []


def main(output_dir, out_path):
    started = time.time()
    targets = collect_targets(output_dir)
    restored = collect_restored(output_dir)
    print(f"待还原 {len(targets)} 个；已还原样本 " +
          ", ".join(f"{k}={len(v)}" for k, v in sorted(restored.items())), flush=True)

    observed_ext = defaultdict(int)
    for names in restored.values():
        for name in names:
            observed_ext[os.path.splitext(name)[1].lower()] += 1
    print("  已出现的扩展名: " + ", ".join(f"{k}×{v}" for k, v in
          sorted(observed_ext.items(), key=lambda kv: -kv[1])[:14]), flush=True)

    candidates = mine_script_candidates(output_dir)

    # B1. 模板枚举（数字段全枚举）
    templates = templates_from([n for names in restored.values() for n in names])
    print(f"  B1. 学到 {len(templates)} 个模板", flush=True)
    for fields, ext in sorted(templates, key=lambda t: -len(t[0]))[:40]:
        for name in enumerate_template(fields, ext):
            candidates.add(name)

    # B2. 末位字母展开（ev201a → ev201b…）
    letters = set()
    for names in restored.values():
        for name in names:
            stem, ext = os.path.splitext(name)
            letters.update(letter_variants(stem, ext))
    print(f"  B2. 末位字母展开 {len(letters)} 个", flush=True)
    candidates.update(letters)

    candidates = [c for c in candidates if "." in c or True]
    print(f"候选总数 {len(candidates)}，开始对撞…", flush=True)

    found = {}
    tried = 0
    checkpoints = {i for i in range(1, 21)}
    for index, name in enumerate(candidates, start=1):
        variants = [name]
        stem, ext = os.path.splitext(name)
        if ext:
            variants.append(name)
        for e in EXTENSIONS:
            variants.append(name + e)
            if e in SIDECAR_EXTS:
                variants.append(name + e + ".sli")
        for variant in dict.fromkeys(variants):
            tried += 1
            digest = file_hash(variant)
            if digest in targets:
                found[digest] = variant
        if index % max(1, len(candidates) // 10) == 0:
            print(f"    {index}/{len(candidates)}  命中 {len(found)}  {time.time() - started:.0f}s",
                  flush=True)

    by_pkg = defaultdict(int)
    for digest in found:
        by_pkg[targets.get(digest, "?")] += 1

    print(f"\n试算 {tried} 次，用时 {time.time() - started:.0f}s")
    print(f"**命中 {len(found)} 个**")
    for pkg, count in sorted(by_pkg.items(), key=lambda kv: -kv[1]):
        print(f"    {pkg}: {count}")

    with open(out_path, "w", encoding="utf-8", newline="\r\n") as fp:
        for digest, name in sorted(found.items(), key=lambda kv: kv[1]):
            fp.write(f"{digest}:{name}\n")
    print(f"已写出 {out_path}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("用法: recover_broad.py <Extractor_Output> <输出lst>")
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
