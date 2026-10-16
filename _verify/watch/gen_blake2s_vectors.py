# 生成 BLAKE2s 测试向量：以 Python hashlib.blake2s 为基准（它的口径已经拿真实
# 工具数据验证过 2804/2804），输出一个 C++ 头文件给自测程序用。

import hashlib
import os
import sys

OUT_DIR = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ.get("TEMP", "."), "blake2s-test")


def digest(data: bytes, key: bytes = b"") -> str:
    return hashlib.blake2s(data, key=key).hexdigest().upper()


def main() -> int:
    os.makedirs(OUT_DIR, exist_ok=True)
    vectors = []

    def add(name, data, key=b""):
        vectors.append((name, data.hex().upper(), key.hex().upper(), digest(data, key)))

    add("empty", b"")
    add("abc", b"abc")
    add("64-zero-bytes", b"\x00" * 64)
    add("65-zero-bytes", b"\x00" * 65)
    add("127-bytes-seq", bytes(range(127)))
    add("128-ff-bytes", b"\xff" * 128)
    add("keyed-32byte-key-abc", b"abc", bytes(range(32)))
    add("keyed-key-data", b"data", b"key")
    add("keyed-empty-data", b"", bytes(range(32)))
    add("golden-anj_000_0001", ("anj_000_0001.ogg" + "xp3hnp").encode("utf-16-le"))
    add("golden-anj_000_0002-sli", ("anj_000_0002.ogg.sli" + "xp3hnp").encode("utf-16-le"))
    long_name = "\u8857_\u9053a_\u6625.png" * 6
    add("long-utf16-multiblock", (long_name + "xp3hnp").encode("utf-16-le"))
    add("exactly-64-utf16", ("\u3042" * 29).encode("utf-16-le"))

    path = os.path.join(OUT_DIR, "vectors.h")
    with open(path, "w", encoding="utf-8") as fp:
        fp.write("// 由 Python hashlib.blake2s 生成 —— 基准值，不要手改\n")
        fp.write("struct TestVector { const char* name; const char* dataHex; const char* keyHex; const char* expectedHex; };\n")
        fp.write("static const TestVector kTestVectors[] = {\n")
        for name, data_hex, key_hex, expected in vectors:
            fp.write('    { "%s", "%s", "%s", "%s" },\n' % (name, data_hex, key_hex, expected))
        fp.write("};\n")

    print("生成 %d 条向量 -> %s" % (len(vectors), path))
    for name, _d, _k, expected in vectors:
        print("  %-28s %s" % (name, expected[:32] + "..."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
