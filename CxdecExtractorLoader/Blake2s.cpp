#include "Blake2s.h"

#include <cstring>

// BLAKE2s-256，照 RFC 7693 实现。结构刻意贴着规范走（IV / 轮常量表 / G 函数 /
// 压缩函数分开写），这样对照规范复核时不用在优化过的代码里找对应关系 ——
// 这段代码的正确性直接决定"补充还原"能不能用，可读性比微优化重要。
//
// 性能取舍：候选对撞要算百万次哈希，所以 Blake2s256Hex 里避免多余分配；
// 但单条消息都很短（几十字节），寄存器压力小，不值得为此把结构揉成一团。

namespace
{
    // SHA-256 的 IV 前 8 个字
    constexpr uint32_t kIV[8] = {
        0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
        0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
    };

    // 轮常量表的 10 个排列
    constexpr uint8_t kSigma[10][16] = {
        {  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15 },
        { 14, 10,  4,  8,  9, 15, 13,  6,  1, 12,  0,  2, 11,  7,  5,  3 },
        { 11,  8, 12,  0,  5,  2, 15, 13, 10, 14,  3,  6,  7,  1,  9,  4 },
        {  7,  9,  3,  1, 13, 12, 11, 14,  2,  6,  5, 10,  4,  0, 15,  8 },
        {  9,  0,  5,  7,  2,  4, 10, 15, 14,  1, 11, 12,  6,  8,  3, 13 },
        {  2, 12,  6, 10,  0, 11,  8,  3,  4, 13,  7,  5, 15, 14,  1,  9 },
        { 12,  5,  1, 15, 14, 13,  4, 10,  0,  7,  6,  3,  9,  2,  8, 11 },
        { 13, 11,  7, 14, 12,  1,  3,  9,  5,  0, 15,  4,  8,  6,  2, 10 },
        {  6, 15, 14,  9, 11,  3,  0,  8, 12,  2, 13,  7,  1,  4, 10,  5 },
        { 10,  2,  8,  4,  7,  6,  1,  5, 15, 11,  9, 14,  3, 12, 13,  0 }
    };

    inline uint32_t RotateRight(uint32_t value, unsigned int count)
    {
        return (value >> count) | (value << (32u - count));
    }

    // RFC 7693 里的 G 函数：旋转量固定 16 / 12 / 8 / 7
    inline void Mix(uint32_t* v, int a, int b, int c, int d, uint32_t x, uint32_t y)
    {
        v[a] = v[a] + v[b] + x;
        v[d] = RotateRight(v[d] ^ v[a], 16);
        v[c] = v[c] + v[d];
        v[b] = RotateRight(v[b] ^ v[c], 12);
        v[a] = v[a] + v[b] + y;
        v[d] = RotateRight(v[d] ^ v[a], 8);
        v[c] = v[c] + v[d];
        v[b] = RotateRight(v[b] ^ v[c], 7);
    }

    void Compress(uint32_t h[8], const uint8_t block[64], uint64_t counter, bool last)
    {
        uint32_t m[16];
        for (int i = 0; i < 16; ++i)
        {
            // BLAKE2 是小端序读取
            m[i] = static_cast<uint32_t>(block[i * 4 + 0])
                 | (static_cast<uint32_t>(block[i * 4 + 1]) << 8)
                 | (static_cast<uint32_t>(block[i * 4 + 2]) << 16)
                 | (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
        }

        uint32_t v[16];
        for (int i = 0; i < 8; ++i)
        {
            v[i] = h[i];
            v[i + 8] = kIV[i];
        }

        v[12] ^= static_cast<uint32_t>(counter & 0xFFFFFFFFull);
        v[13] ^= static_cast<uint32_t>(counter >> 32);
        if (last)
        {
            // 最后一块把 f0 全部置 1（规范里是 v[14] = ~v[14]）
            v[14] = ~v[14];
        }

        for (int round = 0; round < 10; ++round)
        {
            const uint8_t* s = kSigma[round];
            Mix(v, 0, 4,  8, 12, m[s[0]],  m[s[1]]);
            Mix(v, 1, 5,  9, 13, m[s[2]],  m[s[3]]);
            Mix(v, 2, 6, 10, 14, m[s[4]],  m[s[5]]);
            Mix(v, 3, 7, 11, 15, m[s[6]],  m[s[7]]);
            Mix(v, 0, 5, 10, 15, m[s[8]],  m[s[9]]);
            Mix(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
            Mix(v, 2, 7,  8, 13, m[s[12]], m[s[13]]);
            Mix(v, 3, 4,  9, 14, m[s[14]], m[s[15]]);
        }

        for (int i = 0; i < 8; ++i)
        {
            h[i] ^= v[i] ^ v[i + 8];
        }
    }
}

namespace Yua
{
    namespace Crypto
    {
        void Blake2s256(const void* key, size_t keyLength,
                        const void* data, size_t dataLength,
                        unsigned char out[kBlake2sDigestSize])
        {
            uint32_t h[8];
            for (int i = 0; i < 8; ++i)
            {
                h[i] = kIV[i];
            }

            // 参数块：digest 32 字节、fanout=1、depth=1、key 长度
            h[0] ^= 0x01010000u
                  ^ (static_cast<uint32_t>(keyLength) << 8)
                  ^ static_cast<uint32_t>(kBlake2sDigestSize);

            uint8_t buffer[64];
            std::memset(buffer, 0, sizeof(buffer));
            size_t buffered = 0;
            uint64_t counter = 0;      // 已计入 t 的字节数（含密钥块）

            const uint8_t* cursor = static_cast<const uint8_t*>(data);
            size_t remaining = dataLength;

            // 带密钥时，密钥自身先当作一个整块（右侧补零）——这是 BLAKE2 的规定
            if (keyLength > 0)
            {
                std::memcpy(buffer, key, keyLength);
                buffered = 64;
                counter = 64;
            }

            while (remaining > 0)
            {
                if (buffered == 64)
                {
                    // 后面还有数据，所以这一块不是最后一块
                    Compress(h, buffer, counter, false);
                    buffered = 0;
                }

                size_t take = 64 - buffered;
                if (take > remaining)
                {
                    take = remaining;
                }
                std::memcpy(buffer + buffered, cursor, take);
                buffered += take;
                cursor += take;
                remaining -= take;
                counter += take;
            }

            // 收尾：空输入也要压一个全零块（counter 为 0）；密钥块的场景在这里正好收尾
            if (buffered < 64)
            {
                std::memset(buffer + buffered, 0, 64 - buffered);
            }
            Compress(h, buffer, counter, true);

            for (int i = 0; i < 8; ++i)
            {
                out[i * 4 + 0] = static_cast<unsigned char>(h[i] & 0xFFu);
                out[i * 4 + 1] = static_cast<unsigned char>((h[i] >> 8) & 0xFFu);
                out[i * 4 + 2] = static_cast<unsigned char>((h[i] >> 16) & 0xFFu);
                out[i * 4 + 3] = static_cast<unsigned char>((h[i] >> 24) & 0xFFu);
            }
        }

        std::string Blake2s256Hex(const void* key, size_t keyLength,
                                  const void* data, size_t dataLength)
        {
            unsigned char digest[kBlake2sDigestSize];
            Blake2s256(key, keyLength, data, dataLength, digest);

            static const char kHex[] = "0123456789ABCDEF";
            std::string text;
            text.resize(kBlake2sDigestSize * 2u);
            for (size_t i = 0; i < kBlake2sDigestSize; ++i)
            {
                text[i * 2u] = kHex[digest[i] >> 4];
                text[i * 2u + 1u] = kHex[digest[i] & 0x0Fu];
            }
            return text;
        }
    }
}
