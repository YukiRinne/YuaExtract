# 用"已知真名"反推候选提取规则：看看哪种规则能覆盖最多的真名，同时产生最少的噪声。
# 已知真名来自两份真实数据：游戏跑流程收集的映射 + 之前离线挖到的对照表。

import os
import re
import sys
from collections import Counter

GAME = r"E:\GAME\GALGAME\LimeLight Lemonade Jam\ライムライト・レモネードジャム\ライムライト・レモネードジャム"
WATCH = r"E:\Harness&Agent Project\YuaToolkit插件\YuaExtract\_verify\watch"
HEX64 = re.compile(r"^[0-9A-Fa-f]{64}$")

JP = re.compile(r"[\u3040-\u30ff\u4e00-\u9fff]")
STRUCT = re.compile(r"[_0-9.\-]")


def load_known_names():
    names = set()

    # 游戏收集的映射（UTF-16，名字##YSig##哈希）
    path = os.path.join(GAME, "StringHashDumper_Output", "FileNameHash.log")
    if os.path.exists(path):
        with open(path, "r", encoding="utf-16", errors="replace") as fp:
            for line in fp:
                if "##YSig##" in line:
                    names.add(line.split("##YSig##", 1)[0].strip())

    # 离线对照表（UTF-8，哈希:名字）
    path = os.path.join(WATCH, "offline_recovered.lst")
    if os.path.exists(path):
        with open(path, "r", encoding="utf-8") as fp:
            for line in fp:
                if ":" in line:
                    names.add(line.split(":", 1)[1].strip())

    return {n for n in names if n}


def split_ext(name):
    dot = name.rfind(".")
    if dot > 0:
        return name[:dot], name[dot:]
    return name, ""


def main():
    names = load_known_names()
    print("已知真名: %d 个" % len(names))

    stem_only, with_ext = set(), set()
    for n in names:
        stem, ext = split_ext(n)
        with_ext.add(n)
        if stem:
            stem_only.add(stem)

    # 真名里有多少是"裸名 + 扩展名"形态（脚本里通常只写裸名）
    print("去掉扩展名后的裸名: %d 个" % len(stem_only))

    # 看真名的形态分布，决定提取规则
    def shape(name):
        has_digit = any(c.isdigit() for c in name)
        has_us = "_" in name
        has_dot = "." in name
        has_dash = "-" in name
        has_jp = bool(JP.search(name))
        ascii_only = name.isascii()
        return has_digit, has_us, has_dot, has_dash, has_jp, ascii_only

    stats = Counter()
    for n in stem_only:
        d, u, dot, dash, jp, a = shape(n)
        stats["总裸名"] += 1
        if d: stats["含数字"] += 1
        if u: stats["含下划线"] += 1
        if dot: stats["含点"] += 1
        if dash: stats["含横线"] += 1
        if jp: stats["含日文"] += 1
        if a: stats["纯ASCII"] += 1
        if d or u or dot or dash: stats["含结构字符(数字/下划线/点/横线)"] += 1
        if not (d or u or dot or dash): stats["无结构字符"] += 1
        if not (d or u or dot or dash) and len(n) <= 8: stats["无结构字符且<=8字符"] += 1
        if not (d or u or dot or dash) and len(n) > 8: stats["无结构字符且>8字符"] += 1

    for key in ("总裸名", "含数字", "含下划线", "含点", "含横线", "含日文", "纯ASCII",
                "含结构字符(数字/下划线/点/横线)", "无结构字符", "无结构字符且<=8字符", "无结构字符且>8字符"):
        if stats[key]:
            print("  %-32s %6d  (%.1f%%)" % (key, stats[key], stats[key] * 100.0 / stats["总裸名"]))

    print()
    print("结论：若规则取「含数字/下划线/点/横线 或 短于9字符的日文名」，")
    print("      能覆盖 %d / %d = %.2f%% 的真名裸名。" % (
        stats["含结构字符(数字/下划线/点/横线)"] + stats["无结构字符且<=8字符"],
        stats["总裸名"],
        (stats["含结构字符(数字/下划线/点/横线)"] + stats["无结构字符且<=8字符"]) * 100.0 / stats["总裸名"]))
    print("      只取「含结构字符」能覆盖 %.2f%%。" % (
        stats["含结构字符(数字/下划线/点/横线)"] * 100.0 / stats["总裸名"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
