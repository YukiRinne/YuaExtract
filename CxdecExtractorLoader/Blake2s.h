#pragma once

// BLAKE2s-256（RFC 7693）—— 自带一份实现，因为项目不引第三方库。
//
// 用途只有一个：算"资源名 -> 文件名哈希"，也就是
//     hash = BLAKE2s-256(key = FileNameHasherSalt（通常为空）, message = UTF16LE(名字 + seed))
// 这条口径已经拿真实游戏数据验证过（工具自己写的记录 2804/2804 全中），
// 所以这里的实现正确性直接决定"补充还原"能不能用 —— 必须过 RFC 向量 + 真实黄金样例。
//
// key 为空 = 普通 BLAKE2s；key 非空（1..32 字节）= 带密钥版本。

#include <cstddef>
#include <cstdint>
#include <string>

namespace Yua
{
    namespace Crypto
    {
        constexpr size_t kBlake2sDigestSize = 32u;
        constexpr size_t kBlake2sMaxKeySize = 32u;

        // 算 32 字节摘要。key 可以是 nullptr（长度必须为 0）。
        void Blake2s256(const void* key, size_t keyLength,
                        const void* data, size_t dataLength,
                        unsigned char out[kBlake2sDigestSize]);

        // 便捷版：直接给大写十六进制字符串（对撞时每算一次就要转一次，省一层）
        std::string Blake2s256Hex(const void* key, size_t keyLength,
                                  const void* data, size_t dataLength);
    }
}
