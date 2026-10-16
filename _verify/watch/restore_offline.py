# 离线批量还原：把 Extractor_Output 里仍是 Hash 名的文件，按映射复制成明文名，
# 输出到 Restored_Extractor_Output（源目录只读、不动）。
#
# 口径与产品里的"步骤3"一致：
#   - 映射来源：StringHashDumper_Output 下的 DirectoryHash.log / FileNameHash.log
#               （UTF-16，name##YSig##HASH）以及所有 *.lst（UTF-8，HASH:name）
#   - 目录哈希 16 位 → 目录名；文件哈希 64 位 → 文件名
#   - %EmptyString% 与 "/" 都表示根域（路径中直接省掉该层）
#   - 认不出来的保持 Hash 名原样复制
#   - 目标重名落 名字__dupN.后缀，绝不覆盖
#   - 跳过 ExtractLog / Extractor_Log
#   - 最后写 RestoreReport.txt（UTF-8 BOM）
#
# 因为运行时恢复模块可能正在就地改名源目录，主流程跑完会再补扫一遍，
# 把"枚举之后才被改名"的文件捞回来。

import os
import re
import shutil
import sys
import time
from collections import defaultdict

HEX16 = re.compile(r"^[0-9A-Fa-f]{16}$")
HEX64 = re.compile(r"^[0-9A-Fa-f]{64}$")
SKIP_DIRS = {"extractlog", "extractor_log"}
ROOT_NAMES = {"%EmptyString%", "/"}
BUFFER = 4 * 1024 * 1024


def long_path(path):
    r"""超过 240 字符时用 \\?\ 前缀，绕开 MAX_PATH（日语长名很容易超）。"""
    path = os.path.abspath(path)
    if len(path) > 240 and not path.startswith("\\\\?\\"):
        return "\\\\?\\" + path
    return path


def load_mappings(hash_dir):
    directories = {}
    files = {}
    sources = []

    def add(hash_value, name):
        hash_value = hash_value.strip().upper()
        name = name.strip().strip('"')
        if not name:
            return
        if name in ROOT_NAMES:
            name = "%EmptyString%"
        if HEX16.match(hash_value):
            directories.setdefault(hash_value, name)
        elif HEX64.match(hash_value):
            files.setdefault(hash_value, name)

    # 1) 运行时模块的日志：UTF-16，name##YSig##HASH
    for log_name in ("DirectoryHash.log", "FileNameHash.log"):
        path = os.path.join(hash_dir, log_name)
        if not os.path.isfile(path):
            continue
        count = 0
        with open(path, "r", encoding="utf-16", errors="replace") as fp:
            for line in fp:
                line = line.strip()
                if not line:
                    continue
                parts = line.split("##YSig##")
                if len(parts) != 2:
                    continue
                add(parts[1], parts[0])
                count += 1
        sources.append((log_name, count))

    # 2) 所有 .lst：UTF-8，HASH:name
    for entry in sorted(os.listdir(hash_dir)):
        if not entry.lower().endswith(".lst"):
            continue
        path = os.path.join(hash_dir, entry)
        if not os.path.isfile(path):
            continue
        count = 0
        with open(path, "r", encoding="utf-8-sig", errors="replace") as fp:
            for line in fp:
                line = line.strip()
                if not line:
                    continue
                split = line.find(":")
                if split <= 0:
                    continue
                add(line[:split], line[split + 1:])
                count += 1
        sources.append((entry, count))

    return directories, files, sources


def resolve(relative, directories, files):
    """把 <pkg>\\<dirHash>\\<fileHash> 解析成明文相对路径。"""
    parts = relative.split(os.sep)
    out = []
    missing_dir = 0
    missing_file = 0
    for index, part in enumerate(parts):
        last = index == len(parts) - 1
        if last and HEX64.match(part):
            key = part.upper()
            if key in files:
                out.append(files[key])
            else:
                out.append(part)
                missing_file += 1
        elif HEX16.match(part):
            key = part.upper()
            name = directories.get(key)
            if name is None:
                out.append(part)
                missing_dir += 1
            elif name != "%EmptyString%":
                out.append(name.replace("/", os.sep).replace("\\", os.sep))
        else:
            out.append(part)
    return os.path.join(*out) if out else relative, missing_dir, missing_file


def unique_target(path, used):
    if path.lower() not in used:
        used.add(path.lower())
        return path, False
    base, ext = os.path.splitext(path)
    index = 1
    while True:
        candidate = f"{base}__dup{index}{ext}"
        if candidate.lower() not in used:
            used.add(candidate.lower())
            return candidate, True
        index += 1


def main(source_dir, hash_dir, output_dir):
    started = time.time()
    directories, files, sources = load_mappings(hash_dir)
    print(f"映射：目录 {len(directories)} 条 / 文件 {len(files)} 条", flush=True)
    for name, count in sources:
        print(f"  来源 {name}: {count} 行", flush=True)
    if not directories and not files:
        print("没有可用映射，退出")
        return 1

    os.makedirs(long_path(output_dir), exist_ok=True)

    stats = {
        "total": 0, "hash_named": 0, "restored": 0, "missing_dir": 0,
        "missing_file": 0, "passthrough": 0, "dup": 0, "failed": 0, "bytes": 0,
    }
    per_top = defaultdict(lambda: [0, 0])   # 顶层目录 -> [总数, 已还原]
    used = set()
    failures = []
    details = []

    def walk():
        for root, dirs, names in os.walk(long_path(source_dir)):
            dirs[:] = [d for d in dirs if d.lower() not in SKIP_DIRS]
            for name in names:
                yield os.path.join(root, name)

    def handle(path, retry):
        relative = os.path.relpath(path, source_dir)
        top = relative.split(os.sep)[0]
        target_relative, missing_dir, missing_file = resolve(relative, directories, files)
        target = os.path.join(output_dir, target_relative)
        stats["total"] += 1
        per_top[top][0] += 1

        is_hash_named = bool(HEX64.match(os.path.basename(relative)))
        if is_hash_named:
            stats["hash_named"] += 1
            if missing_dir:
                stats["missing_dir"] += 1
            if missing_file:
                stats["missing_file"] += 1
            if not missing_dir and not missing_file:
                stats["restored"] += 1
                per_top[top][1] += 1
        else:
            stats["passthrough"] += 1

        try:
            final_target, dup = unique_target(target, used)
            if dup:
                stats["dup"] += 1
            os.makedirs(long_path(os.path.dirname(final_target)), exist_ok=True)
            with open(long_path(path), "rb") as src, open(long_path(final_target), "wb", BUFFER) as dst:
                shutil.copyfileobj(src, dst, BUFFER)
            stats["bytes"] += os.path.getsize(long_path(final_target))
        except OSError as exc:
            stats["failed"] += 1
            failures.append((relative, str(exc)))
            return
        if is_hash_named and (missing_dir or missing_file):
            details.append(f"{relative}\t{missing_dir}\t{missing_file}\t{target_relative}\t未完全还原")

    for index, path in enumerate(walk(), start=1):
        handle(path, False)
        if index % 5000 == 0:
            print(f"  {index} 个文件 / 已还原 {stats['restored']} / "
                  f"{time.time() - started:.0f}s", flush=True)

    print(f"主流程完成：{stats['total']} 个文件，已还原 {stats['restored']}，"
          f"失败 {stats['failed']}，{time.time() - started:.0f}s", flush=True)

    # 补扫：源目录正被运行时模块就地改名，凡是"枚举之后才被改名"的文件，
    # 第一轮就会整个漏掉（旧路径失效、新名字又没被枚举到）。
    # 所以这里重新枚举一次，只补目标不存在的，代价很小但能保证不漏。
    print("补扫：重新枚举源目录，补齐目标缺失的文件…", flush=True)
    filled = 0
    retried = 0
    for path in walk():
        relative = os.path.relpath(path, source_dir)
        target_relative, _md, _mf = resolve(relative, directories, files)
        target = os.path.join(output_dir, target_relative)
        if os.path.exists(long_path(target)):
            continue
        retried += 1
        try:
            final_target, dup = unique_target(target, used)
            if dup:
                stats["dup"] += 1
            os.makedirs(long_path(os.path.dirname(final_target)), exist_ok=True)
            with open(long_path(path), "rb") as src, open(long_path(final_target), "wb", BUFFER) as dst:
                shutil.copyfileobj(src, dst, BUFFER)
            stats["bytes"] += os.path.getsize(long_path(final_target))
            filled += 1
        except OSError as exc:
            stats["failed"] += 1
            failures.append((relative, str(exc)))
    print(f"补扫：检查 {retried} 个缺失目标，补齐 {filled} 个", flush=True)

    elapsed = time.time() - started
    rate = (stats["restored"] * 100.0 / stats["hash_named"]) if stats["hash_named"] else 0.0

    report = []
    report.append("YuaExtract 资源名还原报告（离线批量恢复）")
    report.append(f"生成时间：{time.strftime('%Y-%m-%d %H:%M:%S')}")
    report.append(f"解包目录：{source_dir}")
    report.append(f"映射目录：{hash_dir}")
    report.append(f"输出目录：{output_dir}")
    report.append("还原方式：复制（源解包目录保持不变）")
    report.append("")
    report.append("—— 统计 ——")
    report.append(f"总文件数            {stats['total']}")
    report.append(f"哈希名文件          {stats['hash_named']}")
    report.append(f"  已还原            {stats['restored']}")
    report.append(f"  缺目录 Hash       {stats['missing_dir']}")
    report.append(f"  缺文件 Hash       {stats['missing_file']}")
    report.append(f"非哈希名文件（原样）{stats['passthrough']}")
    report.append(f"目标重名改名        {stats['dup']}")
    report.append(f"失败                {stats['failed']}")
    report.append(f"已复制数据量        {stats['bytes'] / 1024 / 1024 / 1024:.2f} GB")
    report.append(f"还原成功率          {rate:.2f}%  （{stats['restored']} / {stats['hash_named']}）")
    report.append(f"耗时                {elapsed:.1f} 秒")
    report.append("")
    report.append("—— 映射来源 ——")
    for name, count in sources:
        report.append(f"  {name}\t{count}")
    report.append("")
    report.append("—— 顶层目录还原情况 ——")
    for top in sorted(per_top):
        done, total = per_top[top][1], per_top[top][0]
        report.append(f"  {top}：{done} / {total}")
    if failures:
        report.append("")
        report.append("—— 失败明细 ——")
        report.extend(f"  {rel}\t{msg}" for rel, msg in failures[:200])

    report_path = os.path.join(output_dir, "RestoreReport.txt")
    with open(long_path(report_path), "w", encoding="utf-8-sig", newline="\r\n") as fp:
        fp.write("\r\n".join(report) + "\r\n")

    print("")
    print(f"**已还原 {stats['restored']} / {stats['hash_named']} 个哈希名文件（{rate:.2f}%）**")
    print(f"仍保留 Hash 名：缺目录名 {stats['missing_dir']}，缺文件名 {stats['missing_file']}")
    print(f"失败 {stats['failed']}，重名改名 {stats['dup']}，复制 {stats['bytes'] / 1024 / 1024 / 1024:.2f} GB")
    print(f"报告：{report_path}")
    print(f"总耗时 {elapsed:.1f} 秒")
    return 0


if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("用法: restore_offline.py <Extractor_Output> <StringHashDumper_Output> <Restored_Extractor_Output>")
        sys.exit(2)
    sys.exit(main(sys.argv[1], sys.argv[2], sys.argv[3]))
