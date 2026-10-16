# 单文件对照：用参考实现的规则数一张 PNG 能产出多少候选，用来定位 C++ 版候选爆炸的原因。

import os
import re
import sys

CANDIDATE = re.compile(r"[\w\u3040-\u30ff\u4e00-\u9fff][\w\u3040-\u30ff\u4e00-\u9fff.\-]{1,47}")


def main(path: str) -> int:
    with open(path, "rb") as fp:
        raw = fp.read()

    text = raw.decode("utf-8", errors="ignore")

    tokens = []
    for match in CANDIDATE.finditer(text):
        token = match.group(0).strip(".-")
        if 2 <= len(token) <= 48:
            tokens.append(token)

    unique = set(tokens)
    print("文件      : %s" % os.path.basename(path))
    print("原始字节  : %d" % len(raw))
    print("解码字符  : %d  (存活率 %.1f%%)" % (len(text), len(text) * 100.0 / max(1, len(raw))))
    print("候选(含重): %d" % len(tokens))
    print("候选(去重): %d" % len(unique))
    print("前 10 个  : %s" % ", ".join(list(unique)[:10]))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
