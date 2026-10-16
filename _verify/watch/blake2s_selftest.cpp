// BLAKE2s 自测：把 CxdecExtractorLoader/Blake2s.cpp 与 Python hashlib 生成的基准向量逐条对比。
// 这是临时验证工具，不属于产品，编译产物放 %TEMP%。

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Blake2s.h"
#include "vectors.h"

namespace
{
    std::vector<unsigned char> FromHex(const char* hex)
    {
        std::vector<unsigned char> out;
        if (hex == nullptr)
        {
            return out;
        }
        const size_t length = std::strlen(hex);
        for (size_t i = 0; i + 1 < length; i += 2)
        {
            auto nibble = [](char c) -> unsigned int
            {
                if (c >= '0' && c <= '9') return static_cast<unsigned int>(c - '0');
                if (c >= 'A' && c <= 'F') return static_cast<unsigned int>(c - 'A' + 10);
                if (c >= 'a' && c <= 'f') return static_cast<unsigned int>(c - 'a' + 10);
                return 0u;
            };
            out.push_back(static_cast<unsigned char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
        }
        return out;
    }

    std::string ToHex(const unsigned char* data, size_t length)
    {
        static const char kHex[] = "0123456789ABCDEF";
        std::string text;
        text.reserve(length * 2);
        for (size_t i = 0; i < length; ++i)
        {
            text.push_back(kHex[data[i] >> 4]);
            text.push_back(kHex[data[i] & 0x0F]);
        }
        return text;
    }
}

int main()
{
    int passed = 0;
    int failed = 0;

    const size_t count = sizeof(kTestVectors) / sizeof(kTestVectors[0]);
    for (size_t i = 0; i < count; ++i)
    {
        const TestVector& vector = kTestVectors[i];
        const std::vector<unsigned char> data = FromHex(vector.dataHex);
        const std::vector<unsigned char> key = FromHex(vector.keyHex);

        unsigned char out[Yua::Crypto::kBlake2sDigestSize];
        Yua::Crypto::Blake2s256(key.empty() ? nullptr : key.data(), key.size(),
                                data.empty() ? nullptr : data.data(), data.size(),
                                out);

        const std::string actual = ToHex(out, sizeof(out));
        if (actual == vector.expectedHex)
        {
            ++passed;
            std::printf("[OK]   %-28s %s\n", vector.name, actual.substr(0, 32).c_str());
        }
        else
        {
            ++failed;
            std::printf("[FAIL] %-28s\n       expect %s\n       actual %s\n",
                        vector.name, vector.expectedHex, actual.c_str());
        }
    }

    std::printf("\n共 %u 条：通过 %d / 失败 %d\n", static_cast<unsigned int>(count), passed, failed);
    return failed == 0 ? 0 : 1;
}
