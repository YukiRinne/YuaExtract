// security-core-xp3-test
// 回归：CxdecRepacker 的 XP3 解析边界、多段重建与手写 inflate 替换后的有界解压。
// 合成包只在命令行给出的临时根下产生，不启动游戏、不写注册表。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <zlib.h>

#include "../../../CxdecRepacker/core/xp3.h"
#include "../../../CxdecRepacker/core/archive.h"
#include "../../../CxdecRepacker/core/drip.h"
#include "../../../CxdecRepacker/core/filter.h"
#include "../../../CxdecRepacker/core/mapping.h"
#include "../../../CxdecRepacker/core/pack.h"
#include "../../../CxdecRepacker/crypto/aead.h"
#include "../../../CxdecKeyStatic/Crypto/ZlibInflate.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* label) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", label);
    } else {
        std::printf("ok   %s\n", label);
    }
}

void Check(bool condition, const wchar_t* label) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %ls\n", label);
    } else {
        std::printf("ok   %ls\n", label);
    }
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void Put64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
void PutAt64(std::vector<uint8_t>& out, size_t offset, uint64_t v) {
    for (int i = 0; i < 8; ++i) out[offset + i] = static_cast<uint8_t>(v >> (8 * i));
}

std::vector<uint8_t> Deflate(const std::vector<uint8_t>& in) {
    uLongf bound = compressBound(static_cast<uLong>(in.size()));
    std::vector<uint8_t> out(bound);
    if (compress2(out.data(), &bound, in.data(), static_cast<uLong>(in.size()), 9) != Z_OK) return {};
    out.resize(bound);
    return out;
}

hxv4::DripProgram MakeDrip() {
    std::vector<uint32_t> holder(6, 0u);
    std::vector<uint32_t> context(1024, 0u);
    std::vector<std::vector<hxv4::DripRecord>> lanes;
    for (int i = 0; i < 128; ++i) {
        lanes.push_back({hxv4::DripRecord{0u, hxv4::kOpStop}});
    }
    return hxv4::DripProgram(holder, context, lanes);
}

// 手工拼一个最小 hxv4 包：40 字节头 + 占位数据 + N 个数据段 + 加密映射负载 + 索引。
// segmentsPlain 的每一段独立加过滤 + 压缩；多段版本与单段版本明文相同。
std::vector<uint8_t> BuildArchive(const std::vector<std::vector<uint8_t>>& segmentsPlain,
                                  const std::vector<uint8_t>& placeholder,
                                  const hxv4::Hxv4Keys& keys, const hxv4::DripProgram& drip) {
    std::vector<uint8_t> out;
    out.insert(out.end(), hxv4::kXp3Magic, hxv4::kXp3Magic + sizeof(hxv4::kXp3Magic));
    Put64(out, 0x17);
    Put32(out, 1);
    out.push_back(0x80);
    Put64(out, 0);
    Put64(out, 0);
    while (out.size() < 40u) out.push_back(0);
    out.insert(out.end(), placeholder.begin(), placeholder.end());

    std::vector<hxv4::Xp3Segment> segments;
    const uint64_t key = 0x1234567890ABCDEFull;
    uint8_t seed[48]{};
    if (!drip.build_filter_state(key, 0u, seed)) return {};
    uint64_t logical = 0;
    for (const auto& plain : segmentsPlain) {
        std::vector<uint8_t> chunk = plain;
        hxv4::FilterState(seed).apply(chunk.data(), chunk.size(), logical);
        logical += chunk.size();
        std::vector<uint8_t> comp = Deflate(chunk);
        if (comp.empty()) return {};
        hxv4::Xp3Segment seg;
        seg.flags = 1;
        seg.offset = out.size();
        seg.original_size = chunk.size();
        seg.archived_size = comp.size();
        segments.push_back(seg);
        out.insert(out.end(), comp.begin(), comp.end());
    }

    hxv4::MappingTable table;
    hxv4::MappingRecord ph;
    ph.domain_hash_bytes.fill(0x11);
    ph.file_hash.fill(0x22);
    ph.packed = 0;
    ph.key = 0;
    table.records.push_back(ph);
    hxv4::MappingRecord rec;
    rec.domain_hash_bytes.fill(0x33);
    rec.file_hash.fill(0x44);
    rec.packed = 0;
    rec.key = key;
    table.records.push_back(rec);

    const std::vector<uint8_t> tjs = hxv4::mapping_serialize(table);
    const std::vector<uint8_t> tjsZ = Deflate(tjs);
    std::vector<uint8_t> mappingPlain;
    Put32(mappingPlain, static_cast<uint32_t>(tjs.size()));
    mappingPlain.insert(mappingPlain.end(), tjsZ.begin(), tjsZ.end());

    const uint64_t payloadOffset = out.size();
    const std::vector<uint8_t> payload =
        hxv4::crypto::xchacha20poly1305_seal(keys.root_key, keys.nonce0,
                                             mappingPlain.data(), mappingPlain.size());
    out.insert(out.end(), payload.begin(), payload.end());

    hxv4::Hxv4Descriptor desc;
    desc.payload_offset = payloadOffset;
    desc.payload_size = static_cast<uint32_t>(payload.size());
    desc.flags = 0;

    hxv4::Xp3Entry placeholderEntry;
    placeholderEntry.info_flags = 0x80000000u;
    placeholderEntry.original_size = placeholder.size();
    placeholderEntry.archived_size = placeholder.size();
    placeholderEntry.name = u"p0";
    {
        // 占位图保留"陈旧 segm"语义：结构上仍需要一个 segm 子块（解析要求完整消费）。
        hxv4::Xp3Segment stale;
        stale.flags = 0;
        stale.offset = 40;
        stale.original_size = placeholder.size();
        stale.archived_size = placeholder.size();
        placeholderEntry.segments.push_back(stale);
    }
    hxv4::Xp3Entry fileEntry;
    fileEntry.info_flags = 0x80000000u;
    fileEntry.original_size = logical;
    fileEntry.archived_size = 0;
    for (const auto& seg : segments) fileEntry.archived_size += seg.archived_size;
    fileEntry.name = u"p1";
    fileEntry.segments = segments;

    const std::vector<uint8_t> tree = hxv4::build_index_tree(desc, {placeholderEntry, fileEntry});
    const std::vector<uint8_t> indexBlob = Deflate(tree);
    const uint64_t indexOffset = out.size();
    out.push_back(1);
    Put64(out, indexBlob.size());
    Put64(out, tree.size());
    out.insert(out.end(), indexBlob.begin(), indexBlob.end());
    PutAt64(out, 0x20, indexOffset);
    return out;
}

hxv4::Hxv4Keys MakeKeys() {
    hxv4::Hxv4Keys keys;
    for (size_t i = 0; i < keys.root_key.size(); ++i) keys.root_key[i] = static_cast<uint8_t>(i + 1);
    for (size_t i = 0; i < keys.nonce0.size(); ++i) keys.nonce0[i] = static_cast<uint8_t>(0x40 + i);
    for (size_t i = 0; i < keys.nonce1.size(); ++i) keys.nonce1[i] = static_cast<uint8_t>(0x80 + i);
    return keys;
}

// 只含索引的包（没有 Hxv4 负载），用于结构错误用例。
std::vector<uint8_t> MakeIndexOnlyArchive(const std::vector<uint8_t>& tree) {
    std::vector<uint8_t> out;
    out.insert(out.end(), hxv4::kXp3Magic, hxv4::kXp3Magic + sizeof(hxv4::kXp3Magic));
    Put64(out, 0x17);
    Put32(out, 1);
    out.push_back(0x80);
    Put64(out, 0);
    Put64(out, 0);
    while (out.size() < 40u) out.push_back(0);
    const std::vector<uint8_t> indexBlob = Deflate(tree);
    const uint64_t indexOffset = out.size();
    out.push_back(1);
    Put64(out, indexBlob.size());
    Put64(out, tree.size());
    out.insert(out.end(), indexBlob.begin(), indexBlob.end());
    PutAt64(out, 0x20, indexOffset);
    return out;
}

void AppendSubchunk(std::vector<uint8_t>& body, const char tag[4], const std::vector<uint8_t>& payload) {
    body.insert(body.end(), tag, tag + 4);
    Put64(body, payload.size());
    body.insert(body.end(), payload.begin(), payload.end());
}

std::vector<uint8_t> MakeFileBody(bool includeInfo, bool duplicateInfo, uint32_t segFlags,
                                  uint64_t segOriginal, uint64_t segArchived) {
    std::vector<uint8_t> body;
    std::vector<uint8_t> adlr;
    Put32(adlr, 1u);
    AppendSubchunk(body, "adlr", adlr);

    std::vector<uint8_t> segm;
    Put32(segm, segFlags);
    Put64(segm, 0);
    Put64(segm, segOriginal);
    Put64(segm, segArchived);
    AppendSubchunk(body, "segm", segm);

    if (includeInfo) {
        std::vector<uint8_t> info;
        Put32(info, 0x80000000u);
        Put64(info, segOriginal);
        Put64(info, segArchived);
        info.push_back(1);
        info.push_back(0);
        info.push_back('a');
        info.push_back(0);
        info.push_back(0);
        info.push_back(0);
        AppendSubchunk(body, "info", info);
        if (duplicateInfo) {
            AppendSubchunk(body, "info", info);
        }
    }
    return body;
}

std::vector<uint8_t> MakeFileTree(const std::vector<uint8_t>& fileBody, const std::vector<uint8_t>& trailing) {
    std::vector<uint8_t> tree;
    tree.insert(tree.end(), "File", "File" + 4);
    Put64(tree, fileBody.size());
    tree.insert(tree.end(), fileBody.begin(), fileBody.end());
    tree.insert(tree.end(), trailing.begin(), trailing.end());
    return tree;
}

// 两个 File 块：第一个是占位条目（整段校验豁免），第二个被测条目。
std::vector<uint8_t> MakeTwoFileTree(const std::vector<uint8_t>& placeholderBody,
                                     const std::vector<uint8_t>& fileBody) {
    std::vector<uint8_t> tree;
    tree.insert(tree.end(), "File", "File" + 4);
    Put64(tree, placeholderBody.size());
    tree.insert(tree.end(), placeholderBody.begin(), placeholderBody.end());
    tree.insert(tree.end(), "File", "File" + 4);
    Put64(tree, fileBody.size());
    tree.insert(tree.end(), fileBody.begin(), fileBody.end());
    return tree;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: security-core-xp3-test <scratch-root>\n");
        return 2;
    }
    const std::wstring root = argv[1];
    ::CreateDirectoryW(root.c_str(), nullptr);

    std::vector<uint8_t> placeholder = {0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    std::vector<uint8_t> wholePlain;
    for (int i = 0; i < 4096; ++i) wholePlain.push_back(static_cast<uint8_t>((i * 37) & 0xFF));
    std::vector<std::vector<uint8_t>> halves{std::vector<uint8_t>(wholePlain.begin(), wholePlain.begin() + 1000),
                                             std::vector<uint8_t>(wholePlain.begin() + 1000, wholePlain.end())};
    std::vector<std::vector<uint8_t>> one{wholePlain};

    const hxv4::DripProgram drip = MakeDrip();
    const hxv4::Hxv4Keys keys = MakeKeys();
    const std::vector<uint8_t> multi = BuildArchive(halves, placeholder, keys, drip);
    const std::vector<uint8_t> single = BuildArchive(one, placeholder, keys, drip);
    Check(!multi.empty() && !single.empty(), L"synthetic hxv4 archives built");

    hxv4::Xp3Archive parsed;
    Check(hxv4::parse_xp3(multi.data(), multi.size(), parsed), L"multisegment archive parses");
    Check(parsed.entries.size() == 2u && parsed.entries[1].segments.size() == 2u,
          L"two File chunks / two segments parsed");

    // ---- 多段 vs 等价单段：rebuild 结果逐字节一致 ----
    std::vector<uint8_t> rebuiltMulti;
    std::vector<uint8_t> rebuiltSingle;
    const bool okMulti = hxv4::rebuild_archive(multi.data(), multi.size(), keys, drip, rebuiltMulti);
    const bool okSingle = hxv4::rebuild_archive(single.data(), single.size(), keys, drip, rebuiltSingle);
    Check(okMulti && okSingle, L"rebuild_archive succeeds for both layouts");
    Check(!rebuiltMulti.empty() && rebuiltMulti == rebuiltSingle,
          L"multisegment rebuild equals equivalent single-segment");

    // ---- 畸形输入：不得崩溃/死循环 ----
    {
        std::vector<uint8_t> bad = multi;
        PutAt64(bad, 0x20, 0xFFFFFFFFFFFFFFF0ull);
        hxv4::Xp3Header header;
        std::vector<uint8_t> tree;
        Check(!hxv4::read_xp3(bad.data(), bad.size(), header, tree), L"huge index offset rejected");
    }
    {
        std::vector<uint8_t> bad = multi;
        const uint64_t indexOffset = [&]() { uint64_t v = 0; std::memcpy(&v, bad.data() + 0x20, 8); return v; }();
        PutAt64(bad, static_cast<size_t>(indexOffset + 1u), 0xFFFFFFFFFFFFFF00ull);
        hxv4::Xp3Header header;
        std::vector<uint8_t> tree;
        Check(!hxv4::read_xp3(bad.data(), bad.size(), header, tree), L"huge compressed size rejected");
    }
    {
        // 把索引树改成 File 块声明巨大 sub_size：压缩索引重新生成，parse 必须失败。
        std::vector<uint8_t> tree;
        tree.insert(tree.end(), "File", "File" + 4);
        Put64(tree, 0xFFFFFFFFFFFFFF00ull);
        tree.resize(12 + 64, 0);
        const std::vector<uint8_t> indexBlob = Deflate(tree);
        std::vector<uint8_t> bad;
        bad.insert(bad.end(), hxv4::kXp3Magic, hxv4::kXp3Magic + sizeof(hxv4::kXp3Magic));
        Put64(bad, 0x17);
        Put32(bad, 1);
        bad.push_back(0x80);
        Put64(bad, 0);
        Put64(bad, 0);
        while (bad.size() < 40u) bad.push_back(0);
        const uint64_t indexOffset = bad.size();
        bad.push_back(1);
        Put64(bad, indexBlob.size());
        Put64(bad, tree.size());
        bad.insert(bad.end(), indexBlob.begin(), indexBlob.end());
        PutAt64(bad, 0x20, indexOffset);
        hxv4::Xp3Archive archive;
        Check(!hxv4::parse_xp3(bad.data(), bad.size(), archive),
              L"File chunk size beyond index tree rejected");
    }
    {
        // info 名字长度巨大：sub_size=22 但 name_len=0xFFFF，必须失败。
        std::vector<uint8_t> body;
        std::vector<uint8_t> infoBody;
        Put32(infoBody, 0x80000000u);
        Put64(infoBody, 1);
        Put64(infoBody, 1);
        infoBody.push_back(0xFF);
        infoBody.push_back(0xFF);
        body.insert(body.end(), "info", "info" + 4);
        Put64(body, infoBody.size());
        body.insert(body.end(), infoBody.begin(), infoBody.end());
        std::vector<uint8_t> tree;
        tree.insert(tree.end(), "File", "File" + 4);
        Put64(tree, body.size());
        tree.insert(tree.end(), body.begin(), body.end());
        const std::vector<uint8_t> indexBlob = Deflate(tree);
        std::vector<uint8_t> bad;
        bad.insert(bad.end(), hxv4::kXp3Magic, hxv4::kXp3Magic + sizeof(hxv4::kXp3Magic));
        Put64(bad, 0x17);
        Put32(bad, 1);
        bad.push_back(0x80);
        Put64(bad, 0);
        Put64(bad, 0);
        while (bad.size() < 40u) bad.push_back(0);
        const uint64_t indexOffset = bad.size();
        bad.push_back(1);
        Put64(bad, indexBlob.size());
        Put64(bad, tree.size());
        bad.insert(bad.end(), indexBlob.begin(), indexBlob.end());
        PutAt64(bad, 0x20, indexOffset);
        hxv4::Xp3Archive archive;
        Check(!hxv4::parse_xp3(bad.data(), bad.size(), archive), L"oversized info name rejected");
    }

    // ---- 结构完整性：必需子块 / trailing / 未压缩 original==archived ----
    {
        const std::vector<uint8_t> body = MakeFileBody(false, false, 1u, 4u, 4u);
        const std::vector<uint8_t> arch = MakeIndexOnlyArchive(MakeFileTree(body, {}));
        hxv4::Xp3Archive parsedArchive;
        Check(!hxv4::parse_xp3(arch.data(), arch.size(), parsedArchive),
              L"missing info subchunk rejected");
    }
    {
        const std::vector<uint8_t> body = MakeFileBody(true, true, 1u, 4u, 4u);
        const std::vector<uint8_t> arch = MakeIndexOnlyArchive(MakeFileTree(body, {}));
        hxv4::Xp3Archive parsedArchive;
        Check(!hxv4::parse_xp3(arch.data(), arch.size(), parsedArchive),
              L"duplicate info subchunk rejected");
    }
    {
        const std::vector<uint8_t> placeholderBody = MakeFileBody(true, false, 0u, 1u, 1u);
        const std::vector<uint8_t> body = MakeFileBody(true, false, 0u, 4u, 5u);
        const std::vector<uint8_t> arch =
            MakeIndexOnlyArchive(MakeTwoFileTree(placeholderBody, body));
        hxv4::Xp3Archive parsedArchive;
        Check(!hxv4::parse_xp3(arch.data(), arch.size(), parsedArchive),
              L"uncompressed original/archived mismatch rejected");
    }
    {
        const std::vector<uint8_t> body = MakeFileBody(true, false, 1u, 4u, 4u);
        const std::vector<uint8_t> arch = MakeIndexOnlyArchive(MakeFileTree(body, {0, 0, 0, 0}));
        hxv4::Xp3Header header;
        std::vector<uint8_t> tree;
        Check(!hxv4::read_xp3(arch.data(), arch.size(), header, tree),
              L"index tree trailing bytes (<12) rejected");
    }

    // ---- ZlibInflate：有效/截断/坏校验/FDICT ----
    {
        const char* text = "hello zlib safety world";
        std::vector<uint8_t> raw(text, text + std::strlen(text));
        std::vector<uint8_t> compressed = Deflate(raw);
        const std::vector<uint8_t> good = Crypto::zlib_decompress(compressed.data(), compressed.size());
        Check(good == raw, L"zlib_decompress valid stream with adler check");

        const std::vector<uint8_t> truncated =
            Crypto::zlib_decompress(compressed.data(), compressed.size() - 5u);
        Check(truncated.empty(), L"zlib_decompress truncated returns empty");

        std::vector<uint8_t> bad = compressed;
        bad[bad.size() - 1] ^= 0xFF;
        Check(Crypto::zlib_decompress(bad.data(), bad.size()).empty(),
              L"zlib_decompress bad adler returns empty");

        uint8_t dictStream[8] = {0x78, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00};
        Check(Crypto::zlib_decompress(dictStream, sizeof(dictStream)).empty(),
              L"zlib_decompress FDICT stream fails explicitly");
    }
    {
        // 64 MiB cap：65 MiB 零流压缩后很小，inflate 到 cap 必须返回空而不是把内存吃光。
        std::vector<uint8_t> zeros(65ull * 1024ull * 1024ull, 0u);
        uLongf bound = compressBound(static_cast<uLong>(zeros.size()));
        std::vector<uint8_t> bomb(bound);
        const int rc = compress2(bomb.data(), &bound, zeros.data(), static_cast<uLong>(zeros.size()), 1);
        Check(rc == Z_OK && bound > 0, L"zlib cap bomb compressed");
        if (rc == Z_OK) {
            bomb.resize(bound);
            Check(Crypto::zlib_decompress(bomb.data(), bomb.size()).empty(),
                  L"zlib_decompress output cap enforced");
        }
    }

    std::printf("RESULT checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
