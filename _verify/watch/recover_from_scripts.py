# 从游戏脚本里挖候选名 + 离线算哈希 + 与磁盘上仍是 Hash 名的文件求交集（只读游戏目录）。
#
# 依据（均已实测验证）：
#   1) fileHash = BLAKE2s-256(UTF16LE(名字 + "xp3hnp"))      —— 与工具记录 2804/2804 一致
#   2) 脚本（PSB）里以 ASCII 存着语音引用，且**不带扩展名**（扩展名由引擎拼接）
#   3) 磁盘上的 64 位十六进制文件名就是待还原的 fileHash
#
# 输出：一份 HASH:name 对照文件（写到 _verify\watch\，**不碰游戏目录**：
#       运行时模块每次扫描都会整体重写 HashRestore_RecoveredNames.lst）。

import os
import re
import sys
import time
from hashlib import blake2s

SALT = "xp3hnp"
HEX64 = re.compile(r"^[0-9A-Fa-f]{64}$")
# 脚本里的引用形如 mor_006_0017，不带扩展名
TOKEN = re.compile(rb"[a-z][a-z0-9]{1,15}_\d{3}_\d{4}")
# 引擎会依次尝试的扩展名。
EXTENSIONS = [".ogg", ".opus", ".png", ".tlg", ".tjs", ".csv", ".ini", ".txt",
              ".mtn", ".tft", ".sli", ".mchx", ".ks", ".stage", ".psb", ".wav",
              ".mp3", ".dat", ".bin", ".json", ".toml"]
# 音频的循环信息 sidecar：主程序会在**完整逻辑名**后面再追加 .sli
# （文档原话：逻辑名 bgm01 参与计算的是 bgm01.opus，sidecar 是 bgm01.opus.sli）。
SIDECAR = ".sli"
# 这些包的内容没有"前缀_章节_序号"式的引用，跳过扫描（省时间）
SCAN_SKIP = {"voice", "voice2", "video", "Extractor_Log"}


def file_hash(name: str) -> str:
    h = blake2s(digest_size=32)
    h.update((name + SALT).encode("utf-16-le"))
    return h.digest().hex().upper()


def collect_targets(output_dir):
    """磁盘上仍是 Hash 名的文件名集合（就是待还原的 fileHash）。"""
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


def collect_candidates(output_dir):
    """从脚本/文本资源里挖出候选名（去掉扩展名的裸名）。"""
    tokens = set()
    scanned = 0
    scanned_bytes = 0
    for pkg in sorted(os.listdir(output_dir)):
        if pkg in SCAN_SKIP:
            continue
        pkg_dir = os.path.join(output_dir, pkg)
        if not os.path.isdir(pkg_dir):
            continue
        for root, _dirs, files in os.walk(pkg_dir):
            for f in files:
                path = os.path.join(root, f)
                try:
                    if os.path.getsize(path) > 64 * 1024 * 1024:
                        continue
                    with open(path, "rb") as fp:
                        data = fp.read()
                except OSError:
                    continue
                scanned += 1
                scanned_bytes += len(data)
                for m in TOKEN.findall(data):
                    tokens.add(m.decode("ascii"))
    return tokens, scanned, scanned_bytes


def main(output_dir, out_path):
    started = time.time()
    targets = collect_targets(output_dir)
    print(f"待还原的 Hash 名文件: {len(targets)}")

    tokens, scanned, scanned_bytes = collect_candidates(output_dir)
    print(f"扫描文件 {scanned} 个 / {scanned_bytes / 1024 / 1024:.1f} MB，"
          f"挖出候选裸名 {len(tokens)} 个（{time.time() - started:.1f}s）")

    if not tokens:
        print("没挖到候选名，放弃")
        return 1

    found = {}
    tried = 0
    for token in tokens:
        for ext in EXTENSIONS:
            # 同一个裸名可能同时存在多种扩展名的文件（例如 bgm35.opus 和 bgm35.mchx），
            # 所以命中后**不能 break**，要把所有扩展名都试完。
            for name in (token + ext, token + ext + SIDECAR):
                tried += 1
                digest = file_hash(name)
                if digest in targets:
                    found[digest] = name

    elapsed = time.time() - started
    print(f"\n试算 {tried} 次，用时 {elapsed:.1f}s")
    print(f"**命中 {len(found)} 个名字**")
    if targets:
        print(f"   占当前待还原的 {len(found) * 100.0 / len(targets):.2f}%")

    by_pkg = {}
    for digest, name in found.items():
        pkg = targets.get(digest, "?")
        by_pkg[pkg] = by_pkg.get(pkg, 0) + 1
    print("   命中分布：" + ", ".join(f"{k}={v}" for k, v in sorted(by_pkg.items(), key=lambda kv: -kv[1])[:10]))

    with open(out_path, "w", encoding="utf-8", newline="\r\n") as fp:
        for digest, name in sorted(found.items(), key=lambda kv: kv[1]):
            fp.write(f"{digest}:{name}\n")
    print(f"   已写出 {out_path}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("用法: recover_from_scripts.py <Extractor_Output> <输出lst>")
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2]))
