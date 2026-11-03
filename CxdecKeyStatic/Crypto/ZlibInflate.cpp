#include "ZlibInflate.h"

#include <zlib.h>

#include <algorithm>
#include <climits>

namespace Crypto {

namespace {
    // 输出上限：正常引导 DLL 只有几 MB；64 MiB 已经远超任何真实样本。
    // 畸形流靠这个 cap 在内存吃光之前失败，而不是靠"看起来跑完了"。
    constexpr size_t kMaxOutputBytes = 64ull * 1024ull * 1024ull;
    constexpr size_t kOutChunk = 64ull * 1024ull;
}

// 仓库自带 zlib 的有界流式解压（与 CxdecExtractorLoader 用的同一份 vendored 源码）。
// 口径：
//   - 必须真的走到 Z_STREAM_END（zlib 在此时才校验 Adler-32）；
//   - 需要预设字典（FDICT）就明确失败，不支持；
//   - 输入耗尽但流没结束、或输出超过 cap，都返回空 vector；
//   - inflateEnd 由 RAII guard 保证，reserve/insert 抛异常也不会泄漏。
std::vector<uint8_t> zlib_decompress(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) return {};

    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) return {};

    struct InflateGuard {
        z_stream* state;
        ~InflateGuard() { if (state != nullptr) inflateEnd(state); }
    } guard{&stream};

    try {
        std::vector<uint8_t> out;
        // 预估只作提示：不能做 len*4 这种可溢出乘法；cap 本身是 64 MiB。
        const size_t hint = len > kMaxOutputBytes / 4u ? kMaxOutputBytes : len * 4u;
        out.reserve(std::min(hint, kMaxOutputBytes));

        uint8_t chunk[kOutChunk];
        size_t in_pos = 0;
        int rc = Z_OK;
        bool failed = false;

        for (;;) {
            if (stream.avail_in == 0 && in_pos < len) {
                const size_t remaining = len - in_pos;
                const uInt feed = static_cast<uInt>(std::min<size_t>(remaining, UINT_MAX));
                stream.next_in = const_cast<Bytef*>(data + in_pos);
                stream.avail_in = feed;
                in_pos += feed;
            }

            stream.next_out = chunk;
            stream.avail_out = static_cast<uInt>(sizeof(chunk));
            rc = inflate(&stream, Z_NO_FLUSH);
            const size_t produced = sizeof(chunk) - stream.avail_out;

            if (produced > 0) {
                if (out.size() > kMaxOutputBytes - produced) {
                    failed = true;
                    break;
                }
                out.insert(out.end(), chunk, chunk + produced);
            }

            if (rc == Z_STREAM_END) break;
            if (rc == Z_NEED_DICT) {
                // 流头声明了预设字典；本工具没有字典可用，不能猜。
                failed = true;
                break;
            }
            if (rc != Z_OK && rc != Z_BUF_ERROR) {
                failed = true;
                break;
            }
            if (rc == Z_BUF_ERROR && produced == 0) {
                // 没有任何进展：不是正常的"还要更多输入"，再循环就是死转。
                failed = true;
                break;
            }
            if (in_pos >= len && stream.avail_in == 0) {
                // 输入已经耗尽还没到 Z_STREAM_END：截断流。
                failed = true;
                break;
            }
        }

        if (failed) return {};
        return out;
    } catch (...) {
        // reserve/insert 抛异常（内存不足等）：按失败返回空；guard 仍会 inflateEnd。
        return {};
    }
}

} // namespace Crypto
