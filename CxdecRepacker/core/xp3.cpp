#include "core/xp3.h"

#include <zlib.h>

#include <cstring>

namespace hxv4 {
namespace {

inline uint32_t load32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline uint64_t load64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    return v;
}

inline void put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

inline void put64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void chunk(std::vector<uint8_t>& out, const char tag[4], const std::vector<uint8_t>& body) {
    out.insert(out.end(), tag, tag + 4);
    put64(out, body.size());
    out.insert(out.end(), body.begin(), body.end());
}

// 单个段解压后的上限（防畸形 original_size 申请几十 GB）。
constexpr uint64_t kMaxSegmentOriginalSize = 512ull * 1024 * 1024;
// 一个 File 块里最多接受多少段。
constexpr size_t kMaxSegmentsPerEntry = 65536;

bool parse_file_chunk(const uint8_t* body, size_t size, Xp3Entry& entry) {
    size_t q = 0;
    bool seenAdlr = false;
    bool seenSegm = false;
    bool seenInfo = false;
    while (q + 12 <= size) {
        const uint8_t* p = body + q;
        const uint64_t sub_size = load64(p + 4);
        const size_t sub_body = q + 12;
        // 先做减法边界：sub_body + sub_size 是 64 位加法，窄化/回绕都会让越界数据过关。
        if (sub_size > size - sub_body) return false;

        if (std::memcmp(p, "adlr", 4) == 0) {
            if (seenAdlr) return false;  // 重复的已知子块：结构不可信
            seenAdlr = true;
            if (sub_size < 4) return false;
            entry.adler = load32(body + sub_body);
        } else if (std::memcmp(p, "segm", 4) == 0) {
            if (seenSegm) return false;
            seenSegm = true;
            if (sub_size % 28 != 0) return false;
            const uint64_t count = sub_size / 28;
            if (count == 0 || count > kMaxSegmentsPerEntry ||
                entry.segments.size() > kMaxSegmentsPerEntry - static_cast<size_t>(count)) {
                return false;
            }
            for (uint64_t k = 0; k < count; ++k) {
                const uint8_t* s = body + sub_body + static_cast<size_t>(k) * 28;
                Xp3Segment seg;
                seg.flags = load32(s);
                seg.offset = load64(s + 4);
                seg.original_size = load64(s + 12);
                seg.archived_size = load64(s + 20);
                entry.segments.push_back(seg);
            }
        } else if (std::memcmp(p, "info", 4) == 0) {
            if (seenInfo) return false;
            seenInfo = true;
            if (sub_size < 22) return false;
            const uint8_t* b = body + sub_body;
            entry.info_flags = load32(b);
            entry.original_size = load64(b + 4);
            entry.archived_size = load64(b + 12);
            const uint16_t name_len = static_cast<uint16_t>(b[20] | (b[21] << 8));
            // 22 + name_len*2 一定在 size_t 内（name_len 是 16 位），用减法避免任何提前溢出。
            if (static_cast<uint64_t>(name_len) * 2u > sub_size - 22u) return false;
            entry.name.resize(name_len);
            for (uint16_t i = 0; i < name_len; ++i) {
                entry.name[i] = static_cast<char16_t>(b[22 + i * 2] | (b[23 + i * 2] << 8));
            }
        }
        q = sub_body + static_cast<size_t>(sub_size);
    }
    // 结构必须被完整消费：尾部不足 12 字节也是畸形，不能当"没有更多块"放行。
    if (q != size) return false;
    return seenAdlr && seenSegm && seenInfo;
}

}  // namespace

bool read_xp3(const uint8_t* data, size_t len, Xp3Header& header,
              std::vector<uint8_t>& index_tree) {
    if (len < 40) return false;
    if (std::memcmp(data, kXp3Magic, sizeof(kXp3Magic)) != 0) return false;

    header.decoy_index_offset = load64(data + 11);
    header.index_offset = load64(data + 32);
    if (header.index_offset > len || len - header.index_offset < 17) return false;

    const uint8_t* p = data + header.index_offset;
    if (p[0] != 1) return false;  // 目前见到的样本索引都走 zlib 分支

    const uint64_t comp_size = load64(p + 1);
    const uint64_t orig_size = load64(p + 9);
    // 减法边界：index_offset + 17 + comp_size 直接相加，任何一项接近 UINT64_MAX 都会回绕。
    if (comp_size > len - header.index_offset - 17) return false;
    if (comp_size > static_cast<uint64_t>(static_cast<uLong>(-1))) return false;

    // orig_size 是文件里的 64 位字段，直接拿去分配内存前必须先挡一道：
    // 下面的 size_t / uLongf 都是 32 位，装不下就会截断，畸形文件能让我们
    // 去申请几个 GB。实测索引树一条记录约 100 字节，64 MB 够几百万条了。
    constexpr uint64_t kMaxIndexTreeSize = 64ull * 1024 * 1024;
    if (orig_size > kMaxIndexTreeSize) return false;
    if (orig_size > static_cast<uint64_t>(static_cast<uLongf>(-1))) return false;

    index_tree.assign(static_cast<size_t>(orig_size), 0);
    uLongf dest_len = static_cast<uLongf>(orig_size);
    const int rc =
        uncompress(index_tree.data(), &dest_len, p + 17, static_cast<uLong>(comp_size));
    if (rc != Z_OK || dest_len != orig_size) return false;

    header.file_chunk_count = 0;
    size_t q = 0;
    while (q + 12 <= index_tree.size()) {
        const uint64_t size = load64(index_tree.data() + q + 4);
        if (std::memcmp(index_tree.data() + q, "File", 4) == 0) ++header.file_chunk_count;
        // 和下面那两处遍历一样，先挡住上界再前进：size 是 64 位字段，窄化到
        // 32 位的 size_t 会回绕，q 就再也前进不了、循环出不去
        if (size > index_tree.size() - q - 12) return false;
        q += 12 + static_cast<size_t>(size);
    }
    // 索引 chunk 树必须被完整消费；尾部不足 12 字节是畸形。
    if (q != index_tree.size()) return false;
    return true;
}

bool find_hxv4(const std::vector<uint8_t>& index_tree, Hxv4Descriptor& out) {
    size_t q = 0;
    while (q + 12 <= index_tree.size()) {
        const uint64_t size = load64(index_tree.data() + q + 4);
        const size_t body = q + 12;
        if (size > index_tree.size() - body) return false;

        if (std::memcmp(index_tree.data() + q, "Hxv4", 4) == 0) {
            if (size < 14) return false;
            const uint8_t* b = index_tree.data() + body;
            out.payload_offset = load64(b);
            out.payload_size = load32(b + 8);
            out.flags = static_cast<uint16_t>(b[12] | (b[13] << 8));
            return true;
        }
        q = body + static_cast<size_t>(size);
    }
    return false;
}

bool parse_xp3(const uint8_t* data, size_t len, Xp3Archive& out) {
    out = Xp3Archive{};
    if (!read_xp3(data, len, out.header, out.index_tree)) return false;

    out.has_hxv4 = find_hxv4(out.index_tree, out.hxv4);
    if (out.has_hxv4) {
        // 映射负载的偏移/长度也是文件里的 64 位字段；在解析阶段就卡进文件范围，
        // read_hxv4_mapping 里的 payload_off + payload_len 才不会先回绕再越界。
        if (out.hxv4.payload_offset > len) return false;
        if (out.hxv4.payload_size > len - out.hxv4.payload_offset) return false;
        if (out.hxv4.payload_size <= 16u) return false;
    }

    const uint8_t* p = data + out.header.index_offset;
    out.index_compressed_size = load64(p + 1);
    out.index_original_size = load64(p + 9);

    size_t q = 0;
    while (q + 12 <= out.index_tree.size()) {
        const uint64_t size = load64(out.index_tree.data() + q + 4);
        const size_t body = q + 12;
        if (size > out.index_tree.size() - body) return false;

        if (std::memcmp(out.index_tree.data() + q, "File", 4) == 0) {
            Xp3Entry entry;
            if (!parse_file_chunk(out.index_tree.data() + body, static_cast<size_t>(size), entry)) {
                return false;
            }

            // 段表必须真的落在文件里；声明的 original/archived 也要和段的累计一致。
            // 条目 0 是已知的占位图：原包那条 segm 是陈旧值（指向 PNG 中段，且可能
            // original != archived），复刻时要原样写回，所以只对 i>0 做严格校验。
            const bool placeholderEntry = out.entries.empty();
            uint64_t sumOriginal = 0;
            uint64_t sumArchived = 0;
            for (const Xp3Segment& seg : entry.segments) {
                if (seg.offset > len) return false;
                if (seg.archived_size > len - seg.offset) return false;
                if (seg.original_size > kMaxSegmentOriginalSize) return false;
                if (seg.archived_size > static_cast<uint64_t>(static_cast<uLong>(-1))) return false;
                if (seg.original_size > static_cast<uint64_t>(static_cast<uLongf>(-1))) return false;
                if (!placeholderEntry) {
                    // 目前只确认 bit0 = compressed；其它位没有可靠样本，宁可拒绝。
                    if ((seg.flags & ~1u) != 0u) return false;
                    if (!seg.is_compressed() && seg.original_size != seg.archived_size) return false;
                }
                if (sumOriginal > UINT64_MAX - seg.original_size) return false;
                if (sumArchived > UINT64_MAX - seg.archived_size) return false;
                sumOriginal += seg.original_size;
                sumArchived += seg.archived_size;
            }
            if (!placeholderEntry && !entry.segments.empty()) {
                if (sumOriginal != entry.original_size) return false;
                if (sumArchived != entry.archived_size) return false;
            }
            out.entries.push_back(std::move(entry));
        }
        q = body + static_cast<size_t>(size);
    }
    if (q != out.index_tree.size()) return false;
    return true;
}

std::vector<uint8_t> build_index_tree(const Hxv4Descriptor& hxv4,
                                      const std::vector<Xp3Entry>& entries) {
    std::vector<uint8_t> tree;

    // Hxv4 描述符（14 字节）
    std::vector<uint8_t> hx;
    put64(hx, hxv4.payload_offset);
    put32(hx, hxv4.payload_size);
    hx.push_back(static_cast<uint8_t>(hxv4.flags));
    hx.push_back(static_cast<uint8_t>(hxv4.flags >> 8));
    chunk(tree, "Hxv4", hx);

    for (const Xp3Entry& entry : entries) {
        std::vector<uint8_t> body;

        std::vector<uint8_t> adlr;
        put32(adlr, entry.adler);
        chunk(body, "adlr", adlr);

        std::vector<uint8_t> segm;
        for (const Xp3Segment& seg : entry.segments) {
            put32(segm, seg.flags);
            put64(segm, seg.offset);
            put64(segm, seg.original_size);
            put64(segm, seg.archived_size);
        }
        chunk(body, "segm", segm);

        std::vector<uint8_t> info;
        put32(info, entry.info_flags);
        put64(info, entry.original_size);
        put64(info, entry.archived_size);
        const uint16_t name_len = static_cast<uint16_t>(entry.name.size());
        info.push_back(static_cast<uint8_t>(name_len));
        info.push_back(static_cast<uint8_t>(name_len >> 8));
        for (const char16_t c : entry.name) {
            info.push_back(static_cast<uint8_t>(c));
            info.push_back(static_cast<uint8_t>(c >> 8));
        }
        info.push_back(0);
        info.push_back(0);
        chunk(body, "info", info);

        chunk(tree, "File", body);
    }
    return tree;
}

}  // namespace hxv4
