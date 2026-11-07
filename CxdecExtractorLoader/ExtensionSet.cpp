#include "ExtensionSet.h"

// 扩展集内核（一期：同游戏闭环）。接口与口径说明见 ExtensionSet.h。
//
// 本文件的骨架按数据流分节：
//   1) 基础工具（编码、路径、文件、CRC32、SHA-256、zlib 包装）
//   2) 哈希（BLAKE2s-256 / SipHash-2-4）与名字归一化 —— 口径逐字对齐
//      CxdecRepacker\core\resource_hash.cpp 与 CxdecExtractorLoader\HashNameMiner.cpp
//   3) rules.int 容器编解码（照 Hxv4p 的惯例：magic/version/flags/chunk 表/CRC32/未知 chunk 跳过）
//   4) 采集 -> 归一化 -> **生成期自校验** -> 组装
//   5) 加载（§2.1 三分支）
//   6) 静态 Hash 表 / 干跑 / 扩展集库
//
// 两条贯穿全文的硬约束：
//   · 盐必须显式（Universal.log -> 静态派生 -> 报错中止），空串是合法值，靠 saltKnown 区分；
//   · **绝不自己造名字**：每条名字都要过「名字 + 盐 -> 重算哈希 == 采集到的哈希」，
//     不等的一律进 EXCEPTIONS 并计数。归一化口径写错时会在生成期大面积报例外，
//     而不是等到撞库命中 0 才发现。

#include "Blake2s.h"

#include "AtomicFile.h"
#include "PathSafety.h"
#include "crypto/siphash.h"
#include "zlib.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Yua
{
    namespace ExtensionSet
    {
        namespace
        {
            // =====================================================================
            // 1) 基础工具
            // =====================================================================

            constexpr const wchar_t* kPairSeparator = L"##YSig##";
            constexpr const wchar_t* kDirectoryHashLogName = L"DirectoryHash.log";
            constexpr const wchar_t* kFileNameHashLogName = L"FileNameHash.log";
            constexpr const wchar_t* kMinedNamesName = L"MinedNames.lst";
            constexpr const wchar_t* kRecoveredNamesName = L"HashRestore_RecoveredNames.lst";
            constexpr const wchar_t* kRootDirectoryName = L"%EmptyString%";

            constexpr unsigned long long kMaxReadBytes = 512ull * 1024ull * 1024ull;

            // 扩展集档案的硬上限（2026-11 安全轮）：
            //   · 输入档案（磁盘字节）64 MiB —— 正常远端档案约 0.25 MiB，留足余量；
            //   · 解压后 payload 64 MiB —— 压缩包不能靠"小输入 → 巨大输出"撑爆内存。
            // 任何超过上限的输入都明确报错，不尝试分配。
            constexpr size_t kMaxArchiveInputBytes = 64ull * 1024ull * 1024ull;
            constexpr size_t kMaxArchivePayloadBytes = 64ull * 1024ull * 1024ull;
            constexpr size_t kInflateChunkBytes = 64ull * 1024ull;
            // 64 MiB 输入理论上能塞 5m+ 个 12 字节表项。正常档案只有个位数 chunk，
            // 这里给一个远高于真实用量的结构上限，避免 ranges/sort 吃掉几十 MB。
            constexpr uint32_t kMaxChunkCount = 65536u;

            std::wstring CombinePath(const std::wstring& directory, const std::wstring& name)
            {
                if (directory.empty())
                {
                    return name;
                }
                if (name.empty())
                {
                    return directory;
                }
                if (directory.back() == L'\\' || directory.back() == L'/')
                {
                    return directory + name;
                }
                return directory + L'\\' + name;
            }

            std::wstring TrimTrailingSeparators(std::wstring path)
            {
                while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
                {
                    path.pop_back();
                }
                return path;
            }

            std::wstring TrimSpaces(std::wstring value)
            {
                while (!value.empty() && (value.front() == L' ' || value.front() == L'\t'))
                {
                    value.erase(value.begin());
                }
                while (!value.empty() && (value.back() == L' ' || value.back() == L'\t'))
                {
                    value.pop_back();
                }
                return value;
            }

            std::wstring LeafName(const std::wstring& path)
            {
                const size_t separator = path.find_last_of(L"\\/");
                return separator == std::wstring::npos ? path : path.substr(separator + 1u);
            }

            std::wstring ParentDirectory(const std::wstring& path)
            {
                const size_t separator = path.find_last_of(L"\\/");
                return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
            }

            bool DirectoryExists(const std::wstring& path)
            {
                if (path.empty())
                {
                    return false;
                }
                const DWORD attributes = ::GetFileAttributesW(path.c_str());
                return attributes != INVALID_FILE_ATTRIBUTES
                    && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            }

            bool FileExists(const std::wstring& path)
            {
                if (path.empty())
                {
                    return false;
                }
                const DWORD attributes = ::GetFileAttributesW(path.c_str());
                return attributes != INVALID_FILE_ATTRIBUTES
                    && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
            }

            // 逐级建目录。已存在不算错。
            //
            // createdDirectories 非空时，把**这次调用自己新建出来的**层级按 浅 -> 深 记进去：
            // 调用方失败时要靠它把库目录收回原样（独立验证 N-P1：失败后留下空目录，而提示
            // 写着"本地库未被改动"）。本来就存在的层级不算"自己建的"，不记。
            bool EnsureDirectoryTree(const std::wstring& path, DWORD& error,
                                     std::vector<std::wstring>* createdDirectories = nullptr)
            {
                error = 0;
                if (path.empty())
                {
                    return true;
                }
                if (DirectoryExists(path))
                {
                    return true;
                }

                std::wstring current;
                size_t index = 0;
                // 保留 "C:" 这种盘符前缀，别把它拆坏。
                if (path.size() >= 2u && path[1] == L':')
                {
                    current = path.substr(0, 2u);
                    index = 2u;
                }

                while (index <= path.size())
                {
                    const size_t separator = path.find_first_of(L"\\/", index);
                    const size_t end = separator == std::wstring::npos ? path.size() : separator;
                    const std::wstring component = path.substr(index, end - index);
                    if (!component.empty())
                    {
                        current = current.empty() ? component : current + L"\\" + component;
                        // 建之前先问一句"它在不在"：CreateDirectoryW 对 CON 这类保留设备名的
                        // 行为很别扭 —— 它**可能返回失败(183)却已经把目录建出来了**（设备名检查
                        // 在创建之后）。所以"这次建的"只能按**前后事实**判，不能信返回码。
                        const bool existedBefore = DirectoryExists(current);
                        if (!::CreateDirectoryW(current.c_str(), nullptr))
                        {
                            const DWORD createError = ::GetLastError();
                            if (createError != ERROR_ALREADY_EXISTS)
                            {
                                error = createError;
                                return false;
                            }
                            // ERROR_ALREADY_EXISTS **不能一律当成功**：CON / NUL 这类 Windows
                            // 保留设备名也会回这个码（名字被设备占着），可目录根本没建出来 ——
                            // 放过去的结果是"错误推迟到写文件那一步，还说得不准"（独立验证 P-5）。
                            // 先把它当候选码记着，等最后复核；复核过不去就把它报出去。
                            if (error == 0u)
                            {
                                error = createError;
                            }
                            if (!existedBefore && DirectoryExists(current) && createdDirectories != nullptr)
                            {
                                createdDirectories->push_back(current);
                            }
                        }
                        else if (createdDirectories != nullptr)
                        {
                            // 这一级是这次建出来的（上面那个失败分支不会走到这里）。
                            createdDirectories->push_back(current);
                        }
                    }
                    if (separator == std::wstring::npos)
                    {
                        break;
                    }
                    index = separator + 1u;
                }

                // 复核：目录真的在才算成功。
                if (DirectoryExists(path))
                {
                    error = 0u;   // 成功就别留一个非零码给调用方看
                    return true;
                }
                if (error == 0u)
                {
                    // 连候选码都没有（理论到不了）：再点一次属性，让它给出一个真实的码。
                    ::GetFileAttributesW(path.c_str());
                    error = ::GetLastError();
                }
                return false;
            }

            bool ReadAllBytes(const std::wstring& path, std::vector<unsigned char>& out, DWORD& error)
            {
                error = 0;
                out.clear();

                if (path.empty())
                {
                    error = ERROR_INVALID_PARAMETER;
                    return false;
                }

                // 不跟随重解析点（junction / symlink）：路径上任何一级已存在的是链接就拒绝。
                std::wstring offending;
                if (PathSafety::HasReparsePoint(path, &offending))
                {
                    error = ERROR_CANT_ACCESS_FILE;
                    return false;
                }

                // 共享读、不允许写：别的进程不能在我们读的同时改写这个文件。
                HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                            OPEN_EXISTING,
                                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                                            nullptr);
                if (file == INVALID_HANDLE_VALUE)
                {
                    error = ::GetLastError();
                    return false;
                }

                LARGE_INTEGER size{};
                if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0)
                {
                    error = ::GetLastError();
                    ::CloseHandle(file);
                    return false;
                }
                if (static_cast<unsigned long long>(size.QuadPart) > kMaxReadBytes
                    || !PathSafety::FitsSizeT(static_cast<uint64_t>(size.QuadPart)))
                {
                    error = ERROR_FILE_TOO_LARGE;
                    ::CloseHandle(file);
                    return false;
                }

                const size_t expected = static_cast<size_t>(size.QuadPart);
                try
                {
                    out.assign(expected, 0u);
                }
                catch (const std::bad_alloc&)
                {
                    error = ERROR_NOT_ENOUGH_MEMORY;
                    ::CloseHandle(file);
                    return false;
                }
                catch (const std::length_error&)
                {
                    error = ERROR_FILE_TOO_LARGE;
                    ::CloseHandle(file);
                    return false;
                }

                size_t offset = 0u;
                while (offset < expected)
                {
                    const DWORD chunk = static_cast<DWORD>(
                        std::min<size_t>(expected - offset, 1u << 20));
                    DWORD read = 0;
                    if (!::ReadFile(file, out.data() + offset, chunk, &read, nullptr))
                    {
                        error = ::GetLastError();
                        ::CloseHandle(file);
                        return false;
                    }
                    if (read == 0u)
                    {
                        // 读短 = 文件在读取过程中被截断，绝不当成功。
                        error = ERROR_HANDLE_EOF;
                        ::CloseHandle(file);
                        return false;
                    }
                    offset += read;
                }

                // 读完再核对一次大小：文件在读取期间变长/变短都不能算"完整读到"。
                LARGE_INTEGER finalSize{};
                if (!::GetFileSizeEx(file, &finalSize)
                    || finalSize.QuadPart != size.QuadPart)
                {
                    error = ERROR_FILE_INVALID;
                    ::CloseHandle(file);
                    return false;
                }
                ::CloseHandle(file);
                return true;
            }

            bool WriteAllBytes(const std::wstring& path, const void* data, size_t size, DWORD& error)
            {
                error = 0;
                const std::wstring directory = ParentDirectory(path);
                if (!directory.empty() && !EnsureDirectoryTree(directory, error))
                {
                    return false;
                }

                HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE)
                {
                    error = ::GetLastError();
                    return false;
                }

                const unsigned char* cursor = static_cast<const unsigned char*>(data);
                size_t offset = 0u;
                while (offset < size)
                {
                    const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - offset, 1u << 20));
                    DWORD written = 0;
                    if (!::WriteFile(file, cursor + offset, chunk, &written, nullptr) || written != chunk)
                    {
                        error = ::GetLastError();
                        ::CloseHandle(file);
                        return false;
                    }
                    offset += written;
                }
                ::CloseHandle(file);
                return true;
            }

            std::string WideToUtf8(const std::wstring& text)
            {
                if (text.empty())
                {
                    return std::string();
                }
                const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
                                                       static_cast<int>(text.size()), nullptr, 0,
                                                       nullptr, nullptr);
                if (need <= 0)
                {
                    return std::string();
                }
                std::string utf8(static_cast<size_t>(need), '\0');
                ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                      &utf8[0], need, nullptr, nullptr);
                return utf8;
            }

            std::wstring Utf8ToWide(const char* data, size_t size)
            {
                if (size == 0u)
                {
                    return std::wstring();
                }
                const int need = ::MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size),
                                                       nullptr, 0);
                if (need <= 0)
                {
                    return std::wstring();
                }
                std::wstring wide(static_cast<size_t>(need), L'\0');
                ::MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size), &wide[0], need);
                return wide;
            }

            std::wstring Utf8ToWide(const std::string& text)
            {
                return Utf8ToWide(text.data(), text.size());
            }

            // 映射表既有 UTF-16（*.log）也有 UTF-8（*.lst），手工放进去的对照文件还可能没 BOM，
            // 所以无 BOM 时按字节形态猜一次 —— 与 HashNameRestorer::ReadTextFileAuto 同口径。
            std::wstring DecodeTextAuto(const std::vector<unsigned char>& bytes)
            {
                if (bytes.empty())
                {
                    return std::wstring();
                }

                size_t offset = 0u;
                bool utf16 = false;
                if (bytes.size() >= 2u && bytes[0] == 0xFFu && bytes[1] == 0xFEu)
                {
                    utf16 = true;
                    offset = 2u;
                }
                else if (bytes.size() >= 2u && bytes[0] == 0xFEu && bytes[1] == 0xFFu)
                {
                    // UTF-16BE：项目里没见过，但认出来别当乱码
                    std::wstring out;
                    out.reserve(bytes.size() / 2u);
                    for (size_t i = 2u; i + 1u < bytes.size(); i += 2u)
                    {
                        out.push_back(static_cast<wchar_t>(
                            (static_cast<unsigned int>(bytes[i]) << 8) | bytes[i + 1u]));
                    }
                    return out;
                }
                else if (bytes.size() >= 3u && bytes[0] == 0xEFu && bytes[1] == 0xBBu && bytes[2] == 0xBFu)
                {
                    offset = 3u;
                }
                else if (bytes.size() >= 2u && bytes[0] != 0u && bytes[1] == 0u)
                {
                    utf16 = true;
                }

                if (utf16)
                {
                    std::wstring out;
                    out.reserve((bytes.size() - offset) / 2u);
                    for (size_t i = offset; i + 1u < bytes.size(); i += 2u)
                    {
                        out.push_back(static_cast<wchar_t>(
                            static_cast<unsigned int>(bytes[i])
                            | (static_cast<unsigned int>(bytes[i + 1u]) << 8)));
                    }
                    return out;
                }

                return Utf8ToWide(reinterpret_cast<const char*>(bytes.data() + offset),
                                  bytes.size() - offset);
            }

            bool ReadTextFileAuto(const std::wstring& path, std::wstring& out, DWORD& error)
            {
                std::vector<unsigned char> bytes;
                if (!ReadAllBytes(path, bytes, error))
                {
                    return false;
                }
                out = DecodeTextAuto(bytes);
                return true;
            }

            // 逐行切分（同时接受 \r\n 与 \n），跳过空行。
            template <typename Visitor>
            void ForEachLine(const std::wstring& content, Visitor visitor)
            {
                size_t start = 0u;
                while (start < content.length())
                {
                    size_t end = content.find_first_of(L"\r\n", start);
                    if (end == std::wstring::npos)
                    {
                        end = content.length();
                    }
                    if (end > start)
                    {
                        visitor(content.substr(start, end - start));
                    }
                    start = end;
                    while (start < content.length()
                           && (content[start] == L'\r' || content[start] == L'\n'))
                    {
                        ++start;
                    }
                }
            }

            uint32_t Crc32(const unsigned char* data, size_t size)
            {
                static uint32_t table[256];
                static bool ready = false;
                if (!ready)
                {
                    for (uint32_t i = 0u; i < 256u; ++i)
                    {
                        uint32_t c = i;
                        for (int k = 0; k < 8; ++k)
                        {
                            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                        }
                        table[i] = c;
                    }
                    ready = true;
                }
                uint32_t crc = 0xFFFFFFFFu;
                for (size_t i = 0u; i < size; ++i)
                {
                    crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
                }
                return crc ^ 0xFFFFFFFFu;
            }

            // ---------- SHA-256（自带一份，项目不引第三方库；Blake2s 也是同样理由自带的） ----------

            const uint32_t kSha256K[64] = {
                0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u, 0x3956C25Bu, 0x59F111F1u,
                0x923F82A4u, 0xAB1C5ED5u, 0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
                0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u, 0xE49B69C1u, 0xEFBE4786u,
                0x0FC19DC6u, 0x240CA1CCu, 0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
                0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u, 0xC6E00BF3u, 0xD5A79147u,
                0x06CA6351u, 0x14292967u, 0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
                0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u, 0xA2BFE8A1u, 0xA81A664Bu,
                0xC24B8B70u, 0xC76C51A3u, 0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
                0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u, 0x391C0CB3u, 0x4ED8AA4Au,
                0x5B9CCA4Fu, 0x682E6FF3u, 0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
                0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u
            };

            inline uint32_t Rotr32(uint32_t value, int bits)
            {
                return (value >> bits) | (value << (32 - bits));
            }

            void Sha256Transform(uint32_t state[8], const unsigned char block[64])
            {
                uint32_t w[64];
                for (int i = 0; i < 16; ++i)
                {
                    w[i] = (static_cast<uint32_t>(block[i * 4]) << 24)
                         | (static_cast<uint32_t>(block[i * 4 + 1]) << 16)
                         | (static_cast<uint32_t>(block[i * 4 + 2]) << 8)
                         | static_cast<uint32_t>(block[i * 4 + 3]);
                }
                for (int i = 16; i < 64; ++i)
                {
                    const uint32_t s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
                    const uint32_t s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
                    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
                }

                uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
                uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
                for (int i = 0; i < 64; ++i)
                {
                    const uint32_t s1 = Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25);
                    const uint32_t ch = (e & f) ^ ((~e) & g);
                    const uint32_t temp1 = h + s1 + ch + kSha256K[i] + w[i];
                    const uint32_t s0 = Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22);
                    const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                    const uint32_t temp2 = s0 + maj;
                    h = g; g = f; f = e; e = d + temp1;
                    d = c; c = b; b = a; a = temp1 + temp2;
                }
                state[0] += a; state[1] += b; state[2] += c; state[3] += d;
                state[4] += e; state[5] += f; state[6] += g; state[7] += h;
            }

            void Sha256Digest(const void* data, size_t size, unsigned char out[32])
            {
                uint32_t state[8] = {
                    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
                };

                const unsigned char* bytes = static_cast<const unsigned char*>(data);
                size_t offset = 0u;
                while (size - offset >= 64u)
                {
                    Sha256Transform(state, bytes + offset);
                    offset += 64u;
                }

                unsigned char tail[128];
                std::memset(tail, 0, sizeof(tail));
                const size_t rest = size - offset;
                if (rest > 0u)
                {
                    std::memcpy(tail, bytes + offset, rest);
                }
                tail[rest] = 0x80u;
                const size_t padded = (rest + 1u + 8u <= 64u) ? 64u : 128u;
                const unsigned long long bits = static_cast<unsigned long long>(size) * 8ull;
                for (int i = 0; i < 8; ++i)
                {
                    tail[padded - 1u - static_cast<size_t>(i)] =
                        static_cast<unsigned char>((bits >> (8 * i)) & 0xFFu);
                }
                Sha256Transform(state, tail);
                if (padded == 128u)
                {
                    Sha256Transform(state, tail + 64);
                }

                for (int i = 0; i < 8; ++i)
                {
                    out[i * 4] = static_cast<unsigned char>((state[i] >> 24) & 0xFFu);
                    out[i * 4 + 1] = static_cast<unsigned char>((state[i] >> 16) & 0xFFu);
                    out[i * 4 + 2] = static_cast<unsigned char>((state[i] >> 8) & 0xFFu);
                    out[i * 4 + 3] = static_cast<unsigned char>(state[i] & 0xFFu);
                }
            }

            std::wstring HexUpper(const unsigned char* data, size_t size)
            {
                static const wchar_t* digits = L"0123456789ABCDEF";
                std::wstring text;
                text.reserve(size * 2u);
                for (size_t i = 0u; i < size; ++i)
                {
                    text.push_back(digits[(data[i] >> 4) & 0x0Fu]);
                    text.push_back(digits[data[i] & 0x0Fu]);
                }
                return text;
            }

            // ---------- 小端读写 ----------

            void AppendU16(std::vector<unsigned char>& out, uint16_t value)
            {
                out.push_back(static_cast<unsigned char>(value & 0xFFu));
                out.push_back(static_cast<unsigned char>((value >> 8) & 0xFFu));
            }

            void AppendU32(std::vector<unsigned char>& out, uint32_t value)
            {
                for (int i = 0; i < 4; ++i)
                {
                    out.push_back(static_cast<unsigned char>((value >> (8 * i)) & 0xFFu));
                }
            }

            uint16_t ReadU16(const unsigned char* data)
            {
                return static_cast<uint16_t>(static_cast<uint16_t>(data[0])
                                             | (static_cast<uint16_t>(data[1]) << 8));
            }

            uint32_t ReadU32(const unsigned char* data)
            {
                uint32_t value = 0u;
                for (int i = 0; i < 4; ++i)
                {
                    value |= static_cast<uint32_t>(data[i]) << (8 * i);
                }
                return value;
            }

            std::wstring FormatUInt(unsigned long long value)
            {
                wchar_t buffer[32]{};
                ::swprintf_s(buffer, L"%llu", value);
                return buffer;
            }

            bool ParseUInt(const std::wstring& text, unsigned long long& value)
            {
                if (text.empty())
                {
                    return false;
                }
                for (wchar_t ch : text)
                {
                    if (ch < L'0' || ch > L'9')
                    {
                        return false;
                    }
                }
                value = ::_wcstoui64(text.c_str(), nullptr, 10);
                return true;
            }

            // ---------- key=value 文本（META / IDENTITY / STATS chunk 用） ----------

            std::wstring EscapeValue(const std::wstring& value)
            {
                std::wstring out;
                out.reserve(value.size() + 8u);
                for (wchar_t ch : value)
                {
                    switch (ch)
                    {
                        case L'\\': out += L"\\\\"; break;
                        case L'\n': out += L"\\n"; break;
                        case L'\r': out += L"\\r"; break;
                        default: out.push_back(ch); break;
                    }
                }
                return out;
            }

            std::wstring UnescapeValue(const std::wstring& value)
            {
                std::wstring out;
                out.reserve(value.size());
                for (size_t i = 0u; i < value.size(); ++i)
                {
                    if (value[i] != L'\\' || i + 1u >= value.size())
                    {
                        out.push_back(value[i]);
                        continue;
                    }
                    const wchar_t next = value[i + 1u];
                    if (next == L'n')
                    {
                        out.push_back(L'\n');
                        ++i;
                    }
                    else if (next == L'r')
                    {
                        out.push_back(L'\r');
                        ++i;
                    }
                    else if (next == L'\\')
                    {
                        out.push_back(L'\\');
                        ++i;
                    }
                    else
                    {
                        out.push_back(value[i]);
                    }
                }
                return out;
            }

            void AppendKeyValue(std::wstring& text, const wchar_t* key, const std::wstring& value)
            {
                text += key;
                text.push_back(L'=');
                text += EscapeValue(value);
                text.push_back(L'\n');
            }

            void AppendKeyValue(std::wstring& text, const wchar_t* key, unsigned long long value)
            {
                AppendKeyValue(text, key, FormatUInt(value));
            }

            void AppendKeyValueBool(std::wstring& text, const wchar_t* key, bool value)
            {
                AppendKeyValue(text, key, std::wstring(value ? L"1" : L"0"));
            }

            // 逐行解析 key=value；重复键的回调次数与出现次数一致（collectionSource 靠这个累积）。
            template <typename Visitor>
            void ForEachKeyValue(const std::wstring& text, Visitor visitor)
            {
                ForEachLine(text,
                            [&](const std::wstring& line)
                            {
                                const size_t split = line.find(L'=');
                                if (split == std::wstring::npos)
                                {
                                    return;
                                }
                                visitor(TrimSpaces(line.substr(0u, split)),
                                        UnescapeValue(line.substr(split + 1u)));
                            });
            }

            std::wstring GetKeyValue(const std::wstring& text, const wchar_t* key)
            {
                std::wstring found;
                ForEachKeyValue(text,
                                [&](const std::wstring& name, const std::wstring& value)
                                {
                                    if (found.empty() && name == key)
                                    {
                                        found = value;
                                    }
                                });
                return found;
            }

            // =====================================================================
            // 2) 哈希与归一化
            // =====================================================================

            // UTF-16LE(名字 ‖ 盐)。wchar_t 在 Windows 上就是 2 字节，所以整段 wstring
            // 的内存布局直接就是 UTF-16LE —— 与 HashNameMiner::ComputeFileHash 同一手法。
            std::vector<unsigned char> Utf16LeBytes(const std::wstring& text)
            {
                std::vector<unsigned char> bytes;
                bytes.reserve(text.size() * 2u);
                for (wchar_t ch : text)
                {
                    const unsigned int unit = static_cast<unsigned int>(static_cast<uint16_t>(ch));
                    bytes.push_back(static_cast<unsigned char>(unit & 0xFFu));
                    bytes.push_back(static_cast<unsigned char>((unit >> 8) & 0xFFu));
                }
                return bytes;
            }

            uint64_t ByteSwap64(uint64_t value)
            {
                return ((value & 0x00000000000000FFull) << 56)
                     | ((value & 0x000000000000FF00ull) << 40)
                     | ((value & 0x0000000000FF0000ull) << 24)
                     | ((value & 0x00000000FF000000ull) << 8)
                     | ((value & 0x000000FF00000000ull) >> 8)
                     | ((value & 0x0000FF0000000000ull) >> 24)
                     | ((value & 0x00FF000000000000ull) >> 40)
                     | ((value & 0xFF00000000000000ull) >> 56);
            }

            bool SameHash(const std::wstring& left, const std::wstring& right)
            {
                return _wcsicmp(left.c_str(), right.c_str()) == 0;
            }

            // =====================================================================
            // 3) 容器
            // =====================================================================

            struct ChunkEntry
            {
                uint16_t id = 0;
                uint32_t offset = 0;
                uint32_t size = 0;
            };

            void AppendChunk(std::vector<unsigned char>& payload, std::vector<ChunkEntry>& table,
                             uint16_t id, const std::vector<unsigned char>& body)
            {
                ChunkEntry entry;
                entry.id = id;
                entry.offset = static_cast<uint32_t>(payload.size());
                entry.size = static_cast<uint32_t>(body.size());
                table.push_back(entry);
                payload.insert(payload.end(), body.begin(), body.end());
            }

            void AppendChunkText(std::vector<unsigned char>& payload, std::vector<ChunkEntry>& table,
                                 uint16_t id, const std::wstring& text)
            {
                const std::string utf8 = WideToUtf8(text);
                AppendChunk(payload, table, id,
                            std::vector<unsigned char>(utf8.begin(), utf8.end()));
            }

            std::wstring DecodeChunkText(const unsigned char* data, size_t size)
            {
                return Utf8ToWide(reinterpret_cast<const char*>(data), size);
            }

            std::wstring JoinLines(const std::vector<std::wstring>& lines)
            {
                std::wstring text;
                for (const std::wstring& line : lines)
                {
                    text += line;
                    text.push_back(L'\n');
                }
                return text;
            }

            std::vector<std::wstring> SplitLines(const std::wstring& text)
            {
                std::vector<std::wstring> lines;
                ForEachLine(text, [&](const std::wstring& line) { lines.push_back(line); });
                return lines;
            }

            // =====================================================================
            // 4) 采集与归一化
            // =====================================================================

            struct Candidate
            {
                std::wstring rawName;
                std::wstring hash;   // 大写 hex
                bool isDirectory = false;
            };

            // 名字 -> 哈希 的采集结果，按维度分开。
            struct Collected
            {
                std::vector<Candidate> directories;
                std::vector<Candidate> files;
                SourceCounts counts;
                std::vector<std::wstring> sourceLines;
            };

            bool IsCandidateTable(const std::wstring& fileName)
            {
                const std::wstring lower = ToLowerString(fileName);
                return lower.rfind(L"dirs_", 0u) == 0u || lower.rfind(L"files_", 0u) == 0u;
            }

            // `名字##YSig##HASH`（运行时映射日志，UTF-16）。
            unsigned int LoadSignatureLog(const std::wstring& path, std::vector<Candidate>& directories,
                                          std::vector<Candidate>& files, std::wstring& error)
            {
                error.clear();
                std::wstring content;
                DWORD readError = 0;
                if (!ReadTextFileAuto(path, content, readError))
                {
                    error = L"读不到映射日志：" + path;
                    return 0u;
                }

                unsigned int loaded = 0u;
                const std::wstring separator = kPairSeparator;
                ForEachLine(content,
                            [&](const std::wstring& line)
                            {
                                const size_t split = line.find(separator);
                                if (split == std::wstring::npos)
                                {
                                    return;
                                }
                                const std::wstring name = line.substr(0u, split);
                                const std::wstring hash = ToUpperString(
                                    TrimSpaces(line.substr(split + separator.length())));
                                Candidate candidate;
                                candidate.rawName = name;
                                candidate.hash = hash;
                                if (IsHexLength(hash, 16u))
                                {
                                    candidate.isDirectory = true;
                                    directories.push_back(candidate);
                                    ++loaded;
                                }
                                else if (IsHexLength(hash, 64u))
                                {
                                    files.push_back(candidate);
                                    ++loaded;
                                }
                            });
                return loaded;
            }

            // `HASH:name`（撞库成果 / 挖掘成果 / 手工对照文件，UTF-8）。
            unsigned int LoadRecoveredList(const std::wstring& path, std::vector<Candidate>& directories,
                                           std::vector<Candidate>& files, std::wstring& error)
            {
                error.clear();
                std::wstring content;
                DWORD readError = 0;
                if (!ReadTextFileAuto(path, content, readError))
                {
                    error = L"读不到对照文件：" + path;
                    return 0u;
                }

                unsigned int loaded = 0u;
                ForEachLine(content,
                            [&](const std::wstring& line)
                            {
                                const size_t split = line.find(L':');
                                if (split == std::wstring::npos || split == 0u)
                                {
                                    return;
                                }
                                const std::wstring hash = ToUpperString(line.substr(0u, split));
                                const std::wstring name = line.substr(split + 1u);
                                Candidate candidate;
                                candidate.rawName = name;
                                candidate.hash = hash;
                                if (IsHexLength(hash, 16u))
                                {
                                    candidate.isDirectory = true;
                                    directories.push_back(candidate);
                                    ++loaded;
                                }
                                else if (IsHexLength(hash, 64u))
                                {
                                    files.push_back(candidate);
                                    ++loaded;
                                }
                            });
                return loaded;
            }

            // 采集顺序 = 优先级：运行时采集（*.log）-> 挖掘成果 -> 撞库成果 -> 同目录其它 *.lst（按名排序）。
            // 同哈希多名字时先到的赢（§4「运行时采集优先，不覆盖已有」）。
            bool CollectSources(const std::wstring& hashDirectory, Collected& collected, std::wstring& error)
            {
                error.clear();
                if (!DirectoryExists(hashDirectory))
                {
                    error = L"映射目录不存在：" + hashDirectory
                          + L"（请先执行步骤2 采集，或把对照 *.lst 放进这个目录）";
                    return false;
                }
                std::wstring offending;
                if (PathSafety::HasReparsePoint(hashDirectory, &offending))
                {
                    error = L"映射目录本身是/含有重解析点，拒绝跟随：" + offending;
                    return false;
                }

                const std::wstring directoryLog = CombinePath(hashDirectory, kDirectoryHashLogName);
                const std::wstring fileNameLog = CombinePath(hashDirectory, kFileNameHashLogName);

                std::wstring sourceError;
                const auto failIfReadFailed = [&](bool hadFile) -> bool
                {
                    if (hadFile && !sourceError.empty())
                    {
                        error = sourceError;
                        return true;
                    }
                    return false;
                };
                if (FileExists(directoryLog))
                {
                    collected.counts.directoryLog =
                        LoadSignatureLog(directoryLog, collected.directories, collected.files, sourceError);
                    if (failIfReadFailed(true))
                    {
                        return false;
                    }
                    collected.sourceLines.push_back(kDirectoryHashLogName + std::wstring(L"（")
                                                   + FormatUInt(collected.counts.directoryLog) + L" 条）");
                }

                if (FileExists(fileNameLog))
                {
                    std::vector<Candidate> ignoredDirectories;
                    collected.counts.fileNameLog = LoadSignatureLog(fileNameLog, ignoredDirectories,
                                                                    collected.files, sourceError);
                    if (failIfReadFailed(true))
                    {
                        return false;
                    }
                    // 文件日志里出现的 16 位哈希不是文件映射，丢弃但如实报数。
                    collected.sourceLines.push_back(kFileNameHashLogName + std::wstring(L"（")
                                                   + FormatUInt(collected.counts.fileNameLog) + L" 条）");
                }

                const std::wstring minedPath = CombinePath(hashDirectory, kMinedNamesName);
                if (FileExists(minedPath))
                {
                    collected.counts.minedNames =
                        LoadRecoveredList(minedPath, collected.directories, collected.files, sourceError);
                    if (failIfReadFailed(true))
                    {
                        return false;
                    }
                    collected.sourceLines.push_back(kMinedNamesName + std::wstring(L"（")
                                                   + FormatUInt(collected.counts.minedNames) + L" 条）");
                }

                const std::wstring recoveredPath = CombinePath(hashDirectory, kRecoveredNamesName);
                if (FileExists(recoveredPath))
                {
                    collected.counts.recoveredNames =
                        LoadRecoveredList(recoveredPath, collected.directories, collected.files, sourceError);
                    if (failIfReadFailed(true))
                    {
                        return false;
                    }
                    collected.sourceLines.push_back(kRecoveredNamesName + std::wstring(L"（")
                                                   + FormatUInt(collected.counts.recoveredNames) + L" 条）");
                }

                // 同目录其它 *.lst（既有契约：用户手工放进来的对照文件）。
                std::vector<std::wstring> otherLists;
                {
                    WIN32_FIND_DATAW data{};
                    HANDLE find = ::FindFirstFileW(CombinePath(hashDirectory, L"*").c_str(), &data);
                    if (find != INVALID_HANDLE_VALUE)
                    {
                        do
                        {
                            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                            {
                                continue;
                            }
                            const std::wstring name = data.cFileName;
                            const std::wstring lower = ToLowerString(name);
                            if (lower.size() <= 4u
                                || lower.compare(lower.size() - 4u, 4u, L".lst") != 0)
                            {
                                continue;
                            }
                            if (IsCandidateTable(name)
                                || _wcsicmp(name.c_str(), kMinedNamesName) == 0
                                || _wcsicmp(name.c_str(), kRecoveredNamesName) == 0)
                            {
                                continue;
                            }
                            otherLists.push_back(name);
                        } while (::FindNextFileW(find, &data));
                        ::FindClose(find);
                    }
                }
                std::sort(otherLists.begin(), otherLists.end(),
                          [](const std::wstring& left, const std::wstring& right)
                          { return _wcsicmp(left.c_str(), right.c_str()) < 0; });

                for (const std::wstring& name : otherLists)
                {
                    std::vector<Candidate> extraDirectories;
                    std::vector<Candidate> extraFiles;
                    const unsigned int loaded =
                        LoadRecoveredList(CombinePath(hashDirectory, name), extraDirectories, extraFiles,
                                          sourceError);
                    if (!sourceError.empty())
                    {
                        error = sourceError;
                        return false;
                    }
                    collected.directories.insert(collected.directories.end(), extraDirectories.begin(),
                                                 extraDirectories.end());
                    collected.files.insert(collected.files.end(), extraFiles.begin(), extraFiles.end());
                    collected.counts.otherLists += loaded;
                    collected.sourceLines.push_back(name + std::wstring(L"（") + FormatUInt(loaded) + L" 条）");
                }

                return true;
            }

            // 目录名：'\' -> '/'、小写、补结尾斜杠、根域记 "/"。
            // 幂等（先削掉所有结尾分隔符再补一个），所以 "system" 与 "system/" 都折成 "system/"。
            // 目录字符串使用小写形式并保留结尾斜杠。
            std::wstring NormalizeDirectoryImpl(const std::wstring& raw)
            {
                std::wstring value = TrimSpaces(raw);
                if (value.empty() || value == L"/" || value == kRootDirectoryName)
                {
                    return std::wstring(L"/");
                }

                std::wstring normalized;
                normalized.reserve(value.size() + 1u);
                for (wchar_t ch : value)
                {
                    if (ch == L'\\' || ch == L'/')
                    {
                        normalized.push_back(L'/');
                    }
                    else
                    {
                        normalized.push_back(static_cast<wchar_t>(::towlower(ch)));
                    }
                }
                while (!normalized.empty() && normalized.back() == L'/')
                {
                    normalized.pop_back();
                }
                if (normalized.empty())
                {
                    return std::wstring(L"/");
                }
                normalized.push_back(L'/');
                return normalized;
            }

            // 文件名：只取裸名（哈希不含目录），**不改大小写**（file_hash 区分大小写）。
            std::wstring NormalizeFileImpl(const std::wstring& raw)
            {
                std::wstring value = TrimSpaces(raw);
                if (value.empty())
                {
                    return value;
                }
                std::wstring unified;
                unified.reserve(value.size());
                for (wchar_t ch : value)
                {
                    unified.push_back(ch == L'/' ? L'\\' : ch);
                }
                return LeafName(unified);
            }

            std::wstring DomainHashFor(const std::wstring& normalizedDirectory, const std::wstring& salt)
            {
                // 根域 = 只哈希盐本身（与 resource_hash.cpp 的 is_root 分支一致）。
                const bool isRoot = normalizedDirectory.empty() || normalizedDirectory == L"/";
                std::wstring message = isRoot ? std::wstring() : normalizedDirectory;
                message += salt;

                const std::vector<unsigned char> bytes = Utf16LeBytes(message);
                const unsigned char zeroKey[16] = {};
                const uint64_t raw = hxv4::crypto::siphash24(
                    bytes.empty() ? nullptr : bytes.data(), bytes.size(), zeroKey);

                wchar_t buffer[32]{};
                ::swprintf_s(buffer, L"%016llX", static_cast<unsigned long long>(ByteSwap64(raw)));
                return buffer;
            }

            std::wstring FileHashFor(const std::wstring& bareFileName, const std::wstring& salt)
            {
                std::wstring message = bareFileName;
                message += salt;
                // 与 HashNameMiner::ComputeFileHash（空 key 分支）逐字一致。
                // Blake2s256Hex 回的是大写 hex 的 ASCII，逐字符加宽即可。
                const std::string hex = Yua::Crypto::Blake2s256Hex(
                    nullptr, 0u, message.data(), message.size() * sizeof(wchar_t));
                return std::wstring(hex.begin(), hex.end());
            }

            // 名字 + 盐 -> 重算哈希，与采集到的哈希比对。**这是整个设计的核心保险。**
            bool VerifyCandidate(const Candidate& candidate, const std::wstring& salt,
                                 std::wstring& normalizedName)
            {
                normalizedName = candidate.isDirectory ? NormalizeDirectoryImpl(candidate.rawName)
                                                       : NormalizeFileImpl(candidate.rawName);
                if (normalizedName.empty() || (candidate.isDirectory == false && normalizedName.empty()))
                {
                    return false;
                }
                const std::wstring recomputed = candidate.isDirectory
                    ? DomainHashFor(normalizedName, salt)
                    : FileHashFor(normalizedName, salt);
                return SameHash(recomputed, candidate.hash);
            }

            // =====================================================================
            // 5) 文件名工具（干跑用，口径对齐 HashNameRestorer）
            // =====================================================================

            std::wstring StripExtension(const std::wstring& leaf)
            {
                const size_t dot = leaf.find_last_of(L'.');
                if (dot == std::wstring::npos || dot == 0u)
                {
                    return leaf;
                }
                return leaf.substr(0u, dot);
            }

            bool IsHashStyleLeaf(const std::wstring& leaf)
            {
                return IsHexLength(ToUpperString(StripExtension(leaf)), 64u);
            }

            bool IsSkippedLogComponent(const std::wstring& name)
            {
                return _wcsicmp(name.c_str(), L"ExtractLog") == 0
                    || _wcsicmp(name.c_str(), L"Extractor_Log") == 0;
            }

            std::vector<std::wstring> SplitRelativePath(const std::wstring& relative)
            {
                std::vector<std::wstring> parts;
                std::wstring current;
                for (wchar_t ch : relative)
                {
                    if (ch == L'\\' || ch == L'/')
                    {
                        if (!current.empty())
                        {
                            parts.push_back(current);
                            current.clear();
                        }
                    }
                    else
                    {
                        current.push_back(ch);
                    }
                }
                if (!current.empty())
                {
                    parts.push_back(current);
                }
                return parts;
            }

            struct ScannedFile
            {
                std::wstring fullPath;
                std::wstring relativePath;
            };

            // 递归枚举（只用于 DryRun 的只读扫描）：**不 follow 重解析点**，不把
            // "列不出来"当成功。返回 false 时 error 点名不可列的子目录。
            bool CollectScannedFiles(const std::wstring& root, const std::wstring& current,
                                     std::vector<ScannedFile>& files, unsigned int& skippedLogs,
                                     unsigned int& skippedReparsePoints, std::wstring& error)
            {
                WIN32_FIND_DATAW data{};
                HANDLE find = ::FindFirstFileW(CombinePath(current, L"*").c_str(), &data);
                if (find == INVALID_HANDLE_VALUE)
                {
                    const DWORD findError = ::GetLastError();
                    if (findError == ERROR_FILE_NOT_FOUND)
                    {
                        return true;   // 空目录
                    }
                    error = L"无法列出目录：" + current + L"（Win32 错误码 "
                          + FormatUInt(findError) + L"）";
                    return false;
                }
                bool ok = true;
                for (;;)
                {
                    const std::wstring name = data.cFileName;
                    bool skip = (name == L"." || name == L"..");
                    if (!skip && IsSkippedLogComponent(name))
                    {
                        // 工具自己的日志目录不是游戏资源，与还原口径一致地跳过。
                        ++skippedLogs;
                        skip = true;
                    }
                    if (!skip)
                    {
                        const std::wstring full = CombinePath(current, name);
                        if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                        {
                            // junction / symlink：不跟进去，也不当成普通文件读。
                            ++skippedReparsePoints;
                        }
                        else if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                        {
                            if (!CollectScannedFiles(root, full, files, skippedLogs,
                                                     skippedReparsePoints, error))
                            {
                                ok = false;
                                break;
                            }
                        }
                        else
                        {
                            ScannedFile file;
                            file.fullPath = full;
                            file.relativePath = full.substr(root.length());
                            while (!file.relativePath.empty()
                                   && (file.relativePath.front() == L'\\'
                                       || file.relativePath.front() == L'/'))
                            {
                                file.relativePath.erase(file.relativePath.begin());
                            }
                            files.push_back(file);
                        }
                    }

                    if (!::FindNextFileW(find, &data))
                    {
                        const DWORD nextError = ::GetLastError();
                        if (nextError != ERROR_NO_MORE_FILES && nextError != 0u)
                        {
                            error = L"枚举目录时出错：" + current + L"（Win32 错误码 "
                                  + FormatUInt(nextError) + L"）";
                            ok = false;
                        }
                        break;
                    }
                }
                ::FindClose(find);
                return ok;
            }

            // =====================================================================
            // 6) 报告文本
            // =====================================================================

            std::wstring SaltOriginText(SaltOrigin origin)
            {
                switch (origin)
                {
                    case SaltOrigin::UniversalLog: return L"Log\\Universal.log 的 Hash Seed";
                    case SaltOrigin::StaticKeyJson: return L"静态派生（ExtractKey_Output\\Static\\*.json 的 hash_domain）";
                    case SaltOrigin::Provided: return L"调用方直接提供";
                    default: return L"无";
                }
            }

            std::wstring IdentityMatchText(IdentityMatch match)
            {
                switch (match)
                {
                    case IdentityMatch::ExeMatch: return L"exe 摘要命中";
                    case IdentityMatch::SaltMatch: return L"摘要不中、盐一致";
                    // 这条路上没有盐可比，**不能**说"盐一致"（独立验证 P-2）。
                    case IdentityMatch::Unverified: return L"未校验（用户强制放行）";
                    default: return L"拒绝";
                }
            }

            std::wstring LoadFailureText(LoadFailure failure)
            {
                switch (failure)
                {
                    case LoadFailure::Structure: return L"档案结构缺失或损坏";
                    case LoadFailure::SaltMismatch: return L"扩展集属于别的游戏（盐不同）";
                    case LoadFailure::Unverified: return L"未能校验归属";
                    case LoadFailure::BadArgument: return L"调用参数不完整";
                    default: return L"无";
                }
            }

            // zlib 的状态码是有符号的负数（Z_DATA_ERROR = -3 等）。直接按无符号打印会变成
            // 18446744073709551613 这种垃圾数字（独立验证 P-4），所以映射成人话 + 有符号值。
            std::wstring ZlibStatusText(int status)
            {
                const wchar_t* name = L"未知";
                switch (status)
                {
                    case Z_OK: name = L"Z_OK"; break;
                    case Z_STREAM_END: name = L"Z_STREAM_END"; break;
                    case Z_NEED_DICT: name = L"Z_NEED_DICT"; break;
                    case Z_ERRNO: name = L"Z_ERRNO"; break;
                    case Z_STREAM_ERROR: name = L"Z_STREAM_ERROR"; break;
                    case Z_DATA_ERROR: name = L"数据损坏(Z_DATA_ERROR)"; break;
                    case Z_MEM_ERROR: name = L"内存不足(Z_MEM_ERROR)"; break;
                    case Z_BUF_ERROR: name = L"输出缓冲不足(Z_BUF_ERROR)"; break;
                    case Z_VERSION_ERROR: name = L"zlib 版本不符(Z_VERSION_ERROR)"; break;
                    default: break;
                }
                wchar_t buffer[64]{};
                ::swprintf_s(buffer, L"%d", status);
                return std::wstring(name) + L"，status=" + buffer;
            }
        }

        // =====================================================================
        // 归一化 / 哈希（对外）
        // =====================================================================

        std::wstring NormalizeDirectoryName(const std::wstring& raw)
        {
            return NormalizeDirectoryImpl(raw);
        }

        std::wstring NormalizeFileName(const std::wstring& raw)
        {
            return NormalizeFileImpl(raw);
        }

        std::wstring FileHashHex(const std::wstring& fileName, const std::wstring& salt)
        {
            return FileHashFor(fileName, salt);
        }

        std::wstring DomainHashHex(const std::wstring& directory, const std::wstring& salt)
        {
            return DomainHashFor(directory, salt);
        }

        bool IsHexLength(const std::wstring& value, size_t length)
        {
            if (value.length() != length)
            {
                return false;
            }
            for (wchar_t ch : value)
            {
                if (!::iswxdigit(ch))
                {
                    return false;
                }
            }
            return true;
        }

        std::wstring ToUpperString(const std::wstring& value)
        {
            std::wstring out = value;
            std::transform(out.begin(), out.end(), out.begin(),
                           [](wchar_t ch) { return static_cast<wchar_t>(::towupper(ch)); });
            return out;
        }

        std::wstring ToLowerString(const std::wstring& value)
        {
            std::wstring out = value;
            std::transform(out.begin(), out.end(), out.begin(),
                           [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });
            return out;
        }

        std::wstring MakeSafePathComponent(const std::wstring& raw)
        {
            std::wstring out;
            out.reserve(raw.size());
            for (wchar_t ch : raw)
            {
                if (ch < 0x20 || ch == L'<' || ch == L'>' || ch == L':' || ch == L'"'
                    || ch == L'/' || ch == L'\\' || ch == L'|' || ch == L'?' || ch == L'*')
                {
                    continue;
                }
                out.push_back(ch);
            }
            while (!out.empty() && (out.back() == L' ' || out.back() == L'.'))
            {
                out.pop_back();
            }
            while (!out.empty() && (out.front() == L' ' || out.front() == L'.'))
            {
                out.erase(out.begin());
            }
            return out;
        }

        std::wstring LibrarySlotKey(const std::wstring& publisher, const std::wstring& title)
        {
            // 全工程唯一的"物理槽位"口径（2026-11 安全轮）：
            //   MakeSafePathComponent（实际落盘组件）+ casefold（Windows 大小写不敏感）+ '\\'。
            // Install 的 upsert、EnumerateLibrary 的目录去重、Online::Classify 的 sameSlot
            // 必须全部用这一份，否则"Game:A" 与 "GameA" 这种 sanitize 后相同的名字会被
            // 判成 New，然后无确认地覆盖同一物理目录。
            const std::wstring safePublisher = MakeSafePathComponent(publisher);
            const std::wstring safeTitle = MakeSafePathComponent(title);
            if (safePublisher.empty() || safeTitle.empty())
            {
                return std::wstring();
            }
            return ToLowerString(safePublisher) + L"\\" + ToLowerString(safeTitle);
        }

        bool ValidateMeta(const Meta& meta, std::wstring& error)
        {
            error.clear();
            if (MakeSafePathComponent(meta.publisher).empty())
            {
                error = L"会社名为空（或全是 Windows 目录名非法字符），无法作为扩展集库的第一级目录。";
                return false;
            }
            if (MakeSafePathComponent(meta.title).empty())
            {
                error = L"作品名为空（或全是 Windows 目录名非法字符），无法作为扩展集库的第二级目录。";
                return false;
            }
            return true;
        }

        // =====================================================================
        // 摘要
        // =====================================================================

        std::wstring Sha256HexBytes(const void* data, size_t size)
        {
            unsigned char digest[32] = {};
            Sha256Digest(data, size, digest);
            return HexUpper(digest, sizeof(digest));
        }

        std::wstring Sha256HexFile(const std::wstring& path)
        {
            HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return std::wstring();
            }

            // 流式摘要，避免把大 exe 整个读进内存。
            uint32_t state[8] = {
                0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
                0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u
            };
            unsigned char buffer[64];
            size_t buffered = 0u;
            unsigned long long total = 0ull;
            std::vector<unsigned char> chunk(1u << 20);

            for (;;)
            {
                DWORD read = 0;
                if (!::ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr))
                {
                    ::CloseHandle(file);
                    return std::wstring();
                }
                if (read == 0u)
                {
                    break;
                }
                total += read;
                size_t offset = 0u;
                while (offset < read)
                {
                    const size_t take = std::min<size_t>(64u - buffered, read - offset);
                    std::memcpy(buffer + buffered, chunk.data() + offset, take);
                    buffered += take;
                    offset += take;
                    if (buffered == 64u)
                    {
                        Sha256Transform(state, buffer);
                        buffered = 0u;
                    }
                }
            }
            ::CloseHandle(file);

            unsigned char tail[128];
            std::memset(tail, 0, sizeof(tail));
            if (buffered > 0u)
            {
                std::memcpy(tail, buffer, buffered);
            }
            tail[buffered] = 0x80u;
            const size_t padded = (buffered + 1u + 8u <= 64u) ? 64u : 128u;
            const unsigned long long bits = total * 8ull;
            for (int i = 0; i < 8; ++i)
            {
                tail[padded - 1u - static_cast<size_t>(i)] =
                    static_cast<unsigned char>((bits >> (8 * i)) & 0xFFu);
            }
            Sha256Transform(state, tail);
            if (padded == 128u)
            {
                Sha256Transform(state, tail + 64);
            }

            unsigned char digest[32];
            for (int i = 0; i < 8; ++i)
            {
                digest[i * 4] = static_cast<unsigned char>((state[i] >> 24) & 0xFFu);
                digest[i * 4 + 1] = static_cast<unsigned char>((state[i] >> 16) & 0xFFu);
                digest[i * 4 + 2] = static_cast<unsigned char>((state[i] >> 8) & 0xFFu);
                digest[i * 4 + 3] = static_cast<unsigned char>(state[i] & 0xFFu);
            }
            return HexUpper(digest, sizeof(digest));
        }

        std::wstring Sha256HexText(const std::wstring& text)
        {
            const std::string utf8 = WideToUtf8(text);
            return Sha256HexBytes(utf8.data(), utf8.size());
        }

        // =====================================================================
        // 盐
        // =====================================================================

        namespace
        {
            // 读 Log\Universal.log 里的 `Hash Seed:`。
            // **与 Yua::Miner::ReadSalt 的唯一差别**：那边把空值当"不可用"，这里按设计稿
            // 把空串当合法盐（known=true）。判据是"这一行在不在"，不是"值空不空"。
            bool ReadSeedFromUniversalLog(const std::wstring& logDirectory, std::wstring& seed,
                                          std::wstring& sourcePath, std::wstring& reason)
            {
                reason.clear();
                seed.clear();

                std::wstring path = logDirectory;
                if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
                {
                    path += L'\\';
                }
                path += L"Universal.log";
                sourcePath = path;

                std::wstring content;
                DWORD readError = 0;
                if (!ReadTextFileAuto(path, content, readError))
                {
                    reason = L"没有读到 " + path + L"（该文件由步骤2 的收集模块写出）";
                    return false;
                }

                bool found = false;
                ForEachLine(content,
                            [&](const std::wstring& rawLine)
                            {
                                if (found)
                                {
                                    return;
                                }
                                const size_t position = rawLine.find(L"Hash Seed:");
                                if (position == std::wstring::npos)
                                {
                                    return;
                                }
                                seed = TrimSpaces(rawLine.substr(position + 10u));
                                found = true;
                            });

                if (!found)
                {
                    reason = L"读到了 " + path + L"，但里面没有 Hash Seed: 这一行";
                    return false;
                }
                return true;
            }

            // 静态派生：ExtractKey_Output\Static\*.json 里的 `"hash_domain": "..."`。
            // 只做最小的字符串提取（不引 JSON 库），够用即可；解析不出来就算没找到。
            bool ExtractHashDomainFromJson(const std::wstring& path, std::wstring& value, bool& found)
            {
                found = false;
                value.clear();

                std::wstring content;
                DWORD readError = 0;
                if (!ReadTextFileAuto(path, content, readError))
                {
                    return false;
                }

                const std::wstring key = L"\"hash_domain\"";
                size_t position = content.find(key);
                if (position == std::wstring::npos)
                {
                    return true;
                }
                position += key.size();
                const size_t colon = content.find(L':', position);
                if (colon == std::wstring::npos)
                {
                    return true;
                }
                const size_t open = content.find(L'"', colon);
                if (open == std::wstring::npos)
                {
                    return true;
                }
                size_t close = open + 1u;
                std::wstring raw;
                while (close < content.size() && content[close] != L'"')
                {
                    if (content[close] == L'\\' && close + 1u < content.size())
                    {
                        const wchar_t next = content[close + 1u];
                        if (next == L'n') { raw.push_back(L'\n'); }
                        else if (next == L'r') { raw.push_back(L'\r'); }
                        else if (next == L't') { raw.push_back(L'\t'); }
                        else if (next == L'"') { raw.push_back(L'"'); }
                        else if (next == L'\\') { raw.push_back(L'\\'); }
                        else if (next == L'u' && close + 5u < content.size())
                        {
                            // \uXXXX：只在 BMP 内解码，够覆盖盐这种短串
                            unsigned int code = 0u;
                            bool valid = true;
                            for (size_t i = 0u; i < 4u; ++i)
                            {
                                const wchar_t digit = content[close + 2u + i];
                                unsigned int nibble = 0u;
                                if (digit >= L'0' && digit <= L'9') { nibble = static_cast<unsigned int>(digit - L'0'); }
                                else if (digit >= L'a' && digit <= L'f') { nibble = static_cast<unsigned int>(digit - L'a') + 10u; }
                                else if (digit >= L'A' && digit <= L'F') { nibble = static_cast<unsigned int>(digit - L'A') + 10u; }
                                else { valid = false; break; }
                                code = (code << 4) | nibble;
                            }
                            if (!valid)
                            {
                                return true;
                            }
                            raw.push_back(static_cast<wchar_t>(code));
                            close += 4u;
                        }
                        else
                        {
                            raw.push_back(next);
                        }
                        close += 2u;
                        continue;
                    }
                    raw.push_back(content[close]);
                    ++close;
                }
                if (close >= content.size())
                {
                    return true;
                }
                value = raw;
                found = true;
                return true;
            }

            bool ReadHashDomainFromStaticDirectory(const std::wstring& staticKeyDirectory,
                                                   std::wstring& value, std::wstring& sourcePath,
                                                   std::wstring& reason)
            {
                reason.clear();
                value.clear();
                sourcePath.clear();

                if (staticKeyDirectory.empty() || !DirectoryExists(staticKeyDirectory))
                {
                    reason = L"静态派生目录不存在：" + staticKeyDirectory;
                    return false;
                }

                std::vector<std::wstring> jsonFiles;
                {
                    WIN32_FIND_DATAW data{};
                    HANDLE find = ::FindFirstFileW(CombinePath(staticKeyDirectory, L"*.json").c_str(), &data);
                    if (find != INVALID_HANDLE_VALUE)
                    {
                        do
                        {
                            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                            {
                                jsonFiles.push_back(data.cFileName);
                            }
                        } while (::FindNextFileW(find, &data));
                        ::FindClose(find);
                    }
                }
                std::sort(jsonFiles.begin(), jsonFiles.end(),
                          [](const std::wstring& left, const std::wstring& right)
                          { return _wcsicmp(left.c_str(), right.c_str()) < 0; });

                for (const std::wstring& name : jsonFiles)
                {
                    const std::wstring path = CombinePath(staticKeyDirectory, name);
                    std::wstring candidate;
                    bool found = false;
                    if (!ExtractHashDomainFromJson(path, candidate, found))
                    {
                        continue;
                    }
                    if (found)
                    {
                        value = candidate;
                        sourcePath = path;
                        return true;
                    }
                }

                reason = L"静态派生目录里没有带 hash_domain 的 json：" + staticKeyDirectory;
                return false;
            }
        }

        SaltResult ResolveSalt(const std::wstring& logDirectory, const std::wstring& staticKeyDirectory)
        {
            SaltResult result;

            std::wstring reason;
            std::wstring source;
            std::wstring seed;
            if (!logDirectory.empty()
                && ReadSeedFromUniversalLog(logDirectory, seed, source, reason))
            {
                result.available = true;
                result.known = true;
                result.salt = seed;
                result.origin = SaltOrigin::UniversalLog;
                result.source = source;
                return result;
            }

            std::wstring staticValue;
            std::wstring staticSource;
            std::wstring staticReason;
            if (ReadHashDomainFromStaticDirectory(staticKeyDirectory, staticValue, staticSource,
                                                  staticReason))
            {
                result.available = true;
                result.known = true;
                result.salt = staticValue;
                result.origin = SaltOrigin::StaticKeyJson;
                result.source = staticSource;
                return result;
            }

            result.available = false;
            result.known = false;
            result.origin = SaltOrigin::None;
            result.message = L"拿不到该游戏的盐（mediaName），无法生成扩展集："
                           + (reason.empty() ? std::wstring(L"没有读 Universal.log") : reason)
                           + L"；静态派生也没成：" + staticReason
                           + L"。请先执行一次步骤2（收集模块会写 Hash Seed），"
                             L"或先跑一次静态 Key 提取。";
            return result;
        }

        // =====================================================================
        // 容器编解码
        // =====================================================================

        std::vector<unsigned char> EncodeArchive(const Archive& archive, std::wstring& error)
        {
            error.clear();
            try
            {
            std::vector<unsigned char> payload;
            std::vector<ChunkEntry> table;

            // META
            {
                std::wstring text;
                AppendKeyValue(text, L"publisher", archive.meta.publisher);
                AppendKeyValue(text, L"title", archive.meta.title);
                AppendKeyValue(text, L"contributor", archive.meta.contributor);
                AppendKeyValue(text, L"version", archive.meta.version);
                AppendKeyValue(text, L"date", archive.meta.date);
                AppendKeyValue(text, L"comment", archive.meta.comment);
                AppendKeyValue(text, L"generatedAt", archive.meta.generatedAt);
                AppendKeyValue(text, L"toolVersion",
                               archive.meta.toolVersion.empty() ? std::wstring(kToolVersion)
                                                                : archive.meta.toolVersion);
                AppendChunkText(payload, table, kChunkMeta, text);
            }

            // IDENTITY
            {
                std::wstring text;
                AppendKeyValue(text, L"target.name", archive.identity.targetExe.name);
                AppendKeyValue(text, L"target.size", archive.identity.targetExe.size);
                AppendKeyValue(text, L"target.sha256", archive.identity.targetExe.sha256);
                AppendKeyValue(text, L"sibling.name", archive.identity.siblingExe.name);
                AppendKeyValue(text, L"sibling.size", archive.identity.siblingExe.size);
                AppendKeyValue(text, L"sibling.sha256", archive.identity.siblingExe.sha256);
                AppendKeyValueBool(text, L"saltKnown", archive.identity.saltKnown);
                AppendKeyValue(text, L"salt", archive.identity.salt);
                AppendKeyValue(text, L"sourceDirectory", archive.identity.sourceDirectory);
                for (const std::wstring& item : archive.identity.collectionSources)
                {
                    AppendKeyValue(text, L"collectionSource", item);
                }
                AppendChunkText(payload, table, kChunkIdentity, text);
            }

            // DIRS / FILES / EXCEPTIONS
            AppendChunkText(payload, table, kChunkDirs, JoinLines(archive.directories));
            AppendChunkText(payload, table, kChunkFiles, JoinLines(archive.files));
            AppendChunkText(payload, table, kChunkExceptions, JoinLines(archive.exceptions));

            // STATS（只存计数：生成耗时属于报告，否则档案无法逐字节复现）
            {
                std::wstring text;
                AppendKeyValue(text, L"sources.directoryLog", archive.stats.sources.directoryLog);
                AppendKeyValue(text, L"sources.fileNameLog", archive.stats.sources.fileNameLog);
                AppendKeyValue(text, L"sources.minedNames", archive.stats.sources.minedNames);
                AppendKeyValue(text, L"sources.recoveredNames", archive.stats.sources.recoveredNames);
                AppendKeyValue(text, L"sources.otherLists", archive.stats.sources.otherLists);
                AppendKeyValue(text, L"directories", archive.stats.directories);
                AppendKeyValue(text, L"files", archive.stats.files);
                AppendKeyValue(text, L"exceptions", archive.stats.exceptions);
                AppendKeyValue(text, L"duplicates", archive.stats.duplicates);
                AppendChunkText(payload, table, kChunkStats, text);
            }

            // RULES（预留，二期 L1 模式层；有内容才写，保持默认产物最小）
            if (!archive.rules.empty())
            {
                AppendChunk(payload, table, kChunkRules, archive.rules);
            }

            if (payload.size() > kMaxArchivePayloadBytes)
            {
                error = L"扩展集内容超过上限（" + FormatUInt(payload.size())
                      + L" 字节，上限 " + FormatUInt(kMaxArchivePayloadBytes)
                      + L" 字节），拒绝生成。";
                return std::vector<unsigned char>();
            }

            std::vector<unsigned char> stored = payload;
            uint32_t flags = 0u;
            if (!payload.empty())
            {
                uLongf bound = ::compressBound(static_cast<uLong>(payload.size()));
                std::vector<unsigned char> compressed(static_cast<size_t>(bound));
                const int status = ::compress2(compressed.data(), &bound, payload.data(),
                                               static_cast<uLong>(payload.size()), Z_DEFAULT_COMPRESSION);
                if (status != Z_OK)
                {
                    error = L"zlib 压缩失败（" + ZlibStatusText(status) + L"）";
                    return std::vector<unsigned char>();
                }
                compressed.resize(static_cast<size_t>(bound));
                // 压不小就不压：flags 如实反映 payload 形态，读侧照 flags 走。
                if (compressed.size() < payload.size())
                {
                    stored.swap(compressed);
                    flags |= kFlagZlib;
                }
            }

            std::vector<unsigned char> out;
            out.reserve(kHeaderSize + table.size() * 12u + stored.size());
            AppendU32(out, kMagic);
            AppendU16(out, kFormatVersion);
            AppendU16(out, kHeaderSize);
            AppendU32(out, flags);
            AppendU32(out, payload.empty() ? 0u : Crc32(payload.data(), payload.size()));
            AppendU32(out, static_cast<uint32_t>(table.size()));

            for (const ChunkEntry& entry : table)
            {
                AppendU16(out, entry.id);
                AppendU16(out, 0u);
                AppendU32(out, entry.offset);
                AppendU32(out, entry.size);
            }

            out.insert(out.end(), stored.begin(), stored.end());
            return out;
            }
            catch (const std::bad_alloc&)
            {
                error = L"内存不足：扩展集内容太大，无法编码。";
                return std::vector<unsigned char>();
            }
            catch (const std::length_error&)
            {
                error = L"扩展集内容超过可处理长度，拒绝编码。";
                return std::vector<unsigned char>();
            }
        }

        bool DecodeArchive(const std::vector<unsigned char>& blob, Archive& out, std::wstring& error)
        {
            error.clear();
            out = Archive();
            try
            {

            if (blob.size() < kHeaderSize)
            {
                error = L"档案被截断：只有 " + FormatUInt(blob.size()) + L" 字节，连 20 字节头部都不够。";
                return false;
            }
            if (blob.size() > kMaxArchiveInputBytes)
            {
                error = L"档案超过上限：文件有 " + FormatUInt(blob.size()) + L" 字节，本工具只接受 "
                      + FormatUInt(kMaxArchiveInputBytes) + L" 字节以内的扩展集档案。";
                return false;
            }

            const uint32_t magic = ReadU32(blob.data());
            if (magic != kMagic)
            {
                error = L"magic 不对：期望 'YXS1'（0x31535859），实际 0x"
                      + ToUpperString(FormatUInt(magic)) + L"。这不是扩展集档案。";
                return false;
            }

            const uint16_t version = ReadU16(blob.data() + 4u);
            if (version != kFormatVersion)
            {
                if (version > kFormatVersion)
                {
                    error = L"档案格式版本过高：文件是 " + FormatUInt(version) + L"，本工具只认 "
                          + FormatUInt(kFormatVersion) + L"。请升级 YuaExtract。";
                }
                else
                {
                    error = L"档案格式版本不支持：" + FormatUInt(version);
                }
                return false;
            }

            const uint16_t headerSize = ReadU16(blob.data() + 6u);
            const uint32_t flags = ReadU32(blob.data() + 8u);
            const uint32_t crc = ReadU32(blob.data() + 12u);
            const uint32_t chunkCount = ReadU32(blob.data() + 16u);

            if ((flags & ~kFlagZlib) != 0u)
            {
                error = L"头部 flags 不合法：0x" + ToUpperString(FormatUInt(flags))
                      + L"，本格式只认 bit0（zlib）。";
                return false;
            }
            if (chunkCount > kMaxChunkCount)
            {
                error = L"chunk 表项过多：chunkCount=" + FormatUInt(chunkCount)
                      + L"，上限 " + FormatUInt(kMaxChunkCount)
                      + L"（正常档案只有个位数 chunk）。";
                return false;
            }

            if (headerSize < kHeaderSize || static_cast<size_t>(headerSize) > blob.size())
            {
                error = L"头部长度不合法：headerSize=" + FormatUInt(headerSize)
                      + L"，文件长度 " + FormatUInt(blob.size()) + L"。";
                return false;
            }

            const unsigned long long tableBytes =
                12ull * static_cast<unsigned long long>(chunkCount);
            if (tableBytes + headerSize > blob.size())
            {
                error = L"chunk 表越界：chunkCount=" + FormatUInt(chunkCount)
                      + L" 需要 " + FormatUInt(tableBytes) + L" 字节，但文件只剩 "
                      + FormatUInt(blob.size() - headerSize) + L" 字节。档案被截断或已损坏。";
                return false;
            }

            const size_t storedSize = blob.size() - headerSize - static_cast<size_t>(tableBytes);
            const unsigned char* stored = blob.data() + headerSize + tableBytes;

            std::vector<unsigned char> payload;
            if ((flags & kFlagZlib) != 0u)
            {
                // **有界流式 inflate**（2026-11 安全轮）：
                // 旧实现按 storedSize * 1024 先 resize 一个可能高达 2 GB 的缓冲区，
                // 一个几十字节的畸形输入就能让 UI 线程崩掉。现在固定 64 KiB 输出块渐进追加，
                // 每次追加前检查 64 MiB 上限；必须真正走到 Z_STREAM_END，且不允许尾随字节。
                z_stream stream{};
                const int initStatus = ::inflateInit(&stream);
                if (initStatus != Z_OK)
                {
                    error = L"档案结构损坏：初始化 zlib 解压失败（"
                          + ZlibStatusText(initStatus) + L"）。";
                    return false;
                }

                // inflateEnd 必须无条件执行：chunk 分配 / payload.insert 等任何异常
                // 都要经过这里，否则 zlib 内部状态会泄漏。
                struct InflateGuard
                {
                    z_stream* state = nullptr;
                    bool active = false;
                    ~InflateGuard()
                    {
                        End();
                    }
                    void End()
                    {
                        if (active && state != nullptr)
                        {
                            ::inflateEnd(state);
                            active = false;
                        }
                    }
                };
                InflateGuard inflateGuard{&stream, true};

                stream.next_in = const_cast<Bytef*>(stored);
                stream.avail_in = static_cast<uInt>(storedSize);

                std::vector<unsigned char> chunk(kInflateChunkBytes);
                bool finished = false;
                int status = Z_OK;
                while (!finished)
                {
                    const uLong inputBefore = stream.total_in;
                    stream.next_out = chunk.data();
                    stream.avail_out = static_cast<uInt>(chunk.size());
                    status = ::inflate(&stream, Z_NO_FLUSH);
                    const size_t produced = chunk.size() - stream.avail_out;

                    if (produced > 0u)
                    {
                        if (payload.size() > kMaxArchivePayloadBytes - produced)
                        {
                            inflateGuard.End();
                            error = L"档案解压后超过上限：payload 会超过 "
                                  + FormatUInt(kMaxArchivePayloadBytes)
                                  + L" 字节，拒绝继续解压（疑似压缩炸弹或损坏档案）。";
                            return false;
                        }
                        payload.insert(payload.end(), chunk.begin(),
                                       chunk.begin() + static_cast<std::ptrdiff_t>(produced));
                    }

                    if (status == Z_STREAM_END)
                    {
                        if (stream.avail_in != 0u)
                        {
                            inflateGuard.End();
                            error = L"档案结构损坏：zlib 流结束后还有 "
                                  + FormatUInt(stream.avail_in) + L" 字节尾随数据。";
                            return false;
                        }
                        finished = true;
                        break;
                    }
                    if (status == Z_NEED_DICT)
                    {
                        inflateGuard.End();
                        error = L"档案结构损坏：payload 需要 zlib 预置字典，本格式不支持。";
                        return false;
                    }
                    if (status != Z_OK && status != Z_BUF_ERROR)
                    {
                        inflateGuard.End();
                        error = L"档案结构损坏：payload 解压失败（"
                              + ZlibStatusText(status) + L"）。";
                        return false;
                    }
                    if (produced == 0u && stream.avail_in == 0u)
                    {
                        inflateGuard.End();
                        error = L"档案结构损坏：payload 解压中途结束（截断）。";
                        return false;
                    }
                    if (status == Z_BUF_ERROR && produced == 0u)
                    {
                        inflateGuard.End();
                        error = L"档案结构损坏：payload 解压没有进展（截断或数据非法）。";
                        return false;
                    }
                    if (status == Z_OK && produced == 0u && stream.total_in == inputBefore)
                    {
                        inflateGuard.End();
                        error = L"档案结构损坏：payload 解压没有进展（异常数据）。";
                        return false;
                    }
                }
                inflateGuard.End();
            }
            else
            {
                if (storedSize > kMaxArchivePayloadBytes)
                {
                    error = L"档案 payload 超过上限：未压缩 payload " + FormatUInt(storedSize)
                          + L" 字节，上限 " + FormatUInt(kMaxArchivePayloadBytes) + L" 字节。";
                    return false;
                }
                payload.assign(stored, stored + storedSize);
            }

            // CRC32 **始终**校验，包含空 payload（空 payload 的 CRC32 是 0）。
            // 旧实现只在 non-empty 时比，等于放过了 crc 被改成任意值的空档案。
            if (Crc32(payload.empty() ? nullptr : payload.data(), payload.size()) != crc)
            {
                error = L"CRC32 校验失败：档案内容与头部记录不一致（已损坏或被改动）。";
                return false;
            }

            bool hasIdentity = false;
            bool hasMeta = false;

            // 先整体验证 chunk 表：越界 / 重叠 / 已知 chunk 重复 / 非零 flags 都拒绝，
            // 再进入解析。绝不能一边解析一边发现"这个 chunk 和上一个叠在一起"。
            std::vector<std::pair<uint32_t, uint32_t>> ranges; // [begin, end)
            std::set<uint16_t> seenKnown;
            for (uint32_t index = 0u; index < chunkCount; ++index)
            {
                const unsigned char* entry = blob.data() + headerSize + static_cast<size_t>(index) * 12u;
                const uint16_t id = ReadU16(entry);
                const uint16_t entryFlags = ReadU16(entry + 2u);
                const uint32_t offset = ReadU32(entry + 4u);
                const uint32_t size = ReadU32(entry + 8u);

                if (entryFlags != 0u)
                {
                    error = L"chunk 表不合法：id=0x" + ToUpperString(FormatUInt(id))
                          + L" 的 flags=" + FormatUInt(entryFlags) + L"，本格式要求为 0。";
                    return false;
                }
                if (static_cast<size_t>(offset) > payload.size()
                    || static_cast<size_t>(size) > payload.size() - static_cast<size_t>(offset))
                {
                    error = L"chunk 越界：id=0x" + ToUpperString(FormatUInt(id))
                          + L" offset=" + FormatUInt(offset) + L" size=" + FormatUInt(size)
                          + L"，payload 只有 " + FormatUInt(payload.size()) + L" 字节。";
                    return false;
                }
                const bool known = id == kChunkMeta || id == kChunkIdentity || id == kChunkDirs
                    || id == kChunkFiles || id == kChunkExceptions || id == kChunkRules
                    || id == kChunkStats;
                if (known)
                {
                    if (!seenKnown.insert(id).second)
                    {
                        error = L"chunk 表不合法：已知 chunk 0x" + ToUpperString(FormatUInt(id))
                              + L" 出现了不止一次。";
                        return false;
                    }
                }
                if (size > 0u)
                {
                    ranges.push_back(std::make_pair(offset, offset + size));
                }
            }
            std::sort(ranges.begin(), ranges.end());
            for (size_t i = 1u; i < ranges.size(); ++i)
            {
                if (ranges[i].first < ranges[i - 1u].second)
                {
                    error = L"chunk 表不合法：chunk 内容范围重叠（offset "
                          + FormatUInt(ranges[i].first) + L" < 前一个结束 "
                          + FormatUInt(ranges[i - 1u].second) + L"）。";
                    return false;
                }
            }

            for (uint32_t index = 0u; index < chunkCount; ++index)
            {
                const unsigned char* entry = blob.data() + headerSize + static_cast<size_t>(index) * 12u;
                const uint16_t id = ReadU16(entry);
                const uint32_t offset = ReadU32(entry + 4u);
                const uint32_t size = ReadU32(entry + 8u);

                // 注意别写成 offset + size > payload.size()：32 位下 size_t 就是 32 位，
                // 两者相加会回绕，畸形档案能骗过这个判断（Hxv4p 里踩过同一个坑）。
                if (static_cast<size_t>(offset) > payload.size()
                    || static_cast<size_t>(size) > payload.size() - static_cast<size_t>(offset))
                {
                    error = L"chunk 越界：id=0x" + ToUpperString(FormatUInt(id))
                          + L" offset=" + FormatUInt(offset) + L" size=" + FormatUInt(size)
                          + L"，payload 只有 " + FormatUInt(payload.size()) + L" 字节。";
                    return false;
                }

                const unsigned char* body = payload.data() + offset;
                const size_t length = static_cast<size_t>(size);

                switch (id)
                {
                    case kChunkMeta:
                    {
                        const std::wstring text = DecodeChunkText(body, length);
                        out.meta.publisher = GetKeyValue(text, L"publisher");
                        out.meta.title = GetKeyValue(text, L"title");
                        out.meta.contributor = GetKeyValue(text, L"contributor");
                        out.meta.version = GetKeyValue(text, L"version");
                        out.meta.date = GetKeyValue(text, L"date");
                        out.meta.comment = GetKeyValue(text, L"comment");
                        out.meta.generatedAt = GetKeyValue(text, L"generatedAt");
                        out.meta.toolVersion = GetKeyValue(text, L"toolVersion");
                        hasMeta = true;
                        break;
                    }
                    case kChunkIdentity:
                    {
                        const std::wstring text = DecodeChunkText(body, length);
                        out.identity.targetExe.name = GetKeyValue(text, L"target.name");
                        out.identity.targetExe.sha256 = GetKeyValue(text, L"target.sha256");
                        out.identity.siblingExe.name = GetKeyValue(text, L"sibling.name");
                        out.identity.siblingExe.sha256 = GetKeyValue(text, L"sibling.sha256");
                        out.identity.salt = GetKeyValue(text, L"salt");
                        out.identity.sourceDirectory = GetKeyValue(text, L"sourceDirectory");
                        out.identity.collectionSources.clear();
                        ForEachKeyValue(text,
                                        [&](const std::wstring& name, const std::wstring& value)
                                        {
                                            if (name == L"collectionSource")
                                            {
                                                out.identity.collectionSources.push_back(value);
                                            }
                                        });

                        unsigned long long targetSize = 0ull;
                        if (ParseUInt(GetKeyValue(text, L"target.size"), targetSize))
                        {
                            out.identity.targetExe.size = targetSize;
                        }
                        unsigned long long siblingSize = 0ull;
                        if (ParseUInt(GetKeyValue(text, L"sibling.size"), siblingSize))
                        {
                            out.identity.siblingExe.size = siblingSize;
                        }

                        const std::wstring knownText = GetKeyValue(text, L"saltKnown");
                        out.identity.saltKnown = (knownText == L"1");
                        hasIdentity = true;
                        break;
                    }
                    case kChunkDirs:
                    {
                        out.directories = SplitLines(DecodeChunkText(body, length));
                        break;
                    }
                    case kChunkFiles:
                    {
                        out.files = SplitLines(DecodeChunkText(body, length));
                        break;
                    }
                    case kChunkExceptions:
                    {
                        out.exceptions = SplitLines(DecodeChunkText(body, length));
                        break;
                    }
                    case kChunkStats:
                    {
                        const std::wstring text = DecodeChunkText(body, length);
                        auto take = [&](const wchar_t* key, unsigned int& target)
                        {
                            unsigned long long value = 0ull;
                            if (ParseUInt(GetKeyValue(text, key), value))
                            {
                                target = static_cast<unsigned int>(value);
                            }
                        };
                        take(L"sources.directoryLog", out.stats.sources.directoryLog);
                        take(L"sources.fileNameLog", out.stats.sources.fileNameLog);
                        take(L"sources.minedNames", out.stats.sources.minedNames);
                        take(L"sources.recoveredNames", out.stats.sources.recoveredNames);
                        take(L"sources.otherLists", out.stats.sources.otherLists);
                        take(L"directories", out.stats.directories);
                        take(L"files", out.stats.files);
                        take(L"exceptions", out.stats.exceptions);
                        take(L"duplicates", out.stats.duplicates);
                        break;
                    }
                    case kChunkRules:
                    {
                        out.rules.assign(body, body + length);
                        break;
                    }
                    default:
                        break;  // 未知 chunk 按格式要求跳过（前向兼容）
                }
            }

            if (!hasMeta)
            {
                error = L"档案结构缺失（META）：档案缺少 META chunk（0x0001），"
                        L"这不是一份完整的扩展集档案。";
                return false;
            }
            if (!hasIdentity)
            {
                // 这是**档案结构缺失**，不是"别的游戏"。两者必须能分开：
                // 前者该换/重生成档案，后者该换游戏或做静态 Key 提取（独立验证 P-3）。
                error = L"档案结构缺失（IDENTITY）：档案缺少 IDENTITY chunk（0x0002），"
                        L"没有盐与目标 exe 摘要，无法确认它属于哪部游戏，拒绝加载。";
                return false;
            }
            if (!out.identity.saltKnown)
            {
                error = L"档案结构缺失（IDENTITY 无盐）：档案的 IDENTITY 里 saltKnown=false，"
                        L"没有记录盐，档案不可用"
                        L"（绝不能拿默认盐去凑，那会让整包哈希全错且不报错）。";
                return false;
            }

            std::wstring lookupError;
            if (!RebuildLookup(out, lookupError))
            {
                error = L"展开档案名字层失败：" + lookupError;
                return false;
            }
            return true;
            }
            catch (const std::bad_alloc&)
            {
                out = Archive();
                error = L"内存不足：档案内容太大或结构异常，解码被中止。";
                return false;
            }
            catch (const std::length_error&)
            {
                out = Archive();
                error = L"档案内容超过可处理长度，解码被中止。";
                return false;
            }
        }

        bool RebuildLookup(Archive& archive, std::wstring& error)
        {
            error.clear();
            archive.directoryNames.clear();
            archive.fileNames.clear();
            archive.lookupReady = false;

            if (!archive.identity.saltKnown)
            {
                error = L"档案没有盐（saltKnown=false），无法由名字重算哈希。";
                return false;
            }

            for (const std::wstring& name : archive.directories)
            {
                const std::wstring normalized = NormalizeDirectoryImpl(name);
                const std::wstring hash = DomainHashFor(normalized, archive.identity.salt);
                archive.directoryNames.emplace(ToUpperString(hash), normalized);
            }
            for (const std::wstring& name : archive.files)
            {
                const std::wstring normalized = NormalizeFileImpl(name);
                if (normalized.empty())
                {
                    continue;
                }
                const std::wstring hash = FileHashFor(normalized, archive.identity.salt);
                archive.fileNames.emplace(ToUpperString(hash), normalized);
            }
            archive.lookupReady = true;
            return true;
        }

        bool WriteArchiveFile(const std::wstring& path, const Archive& archive, std::wstring& error)
        {
            error.clear();
            const std::vector<unsigned char> blob = EncodeArchive(archive, error);
            if (blob.empty())
            {
                if (error.empty())
                {
                    error = L"编码扩展集档案失败（结果为空）。";
                }
                return false;
            }

            DWORD writeError = 0;
            (void)writeError;
            std::wstring atomicError;
            if (!Common::AtomicFile::WriteFileAtomic(path, blob, atomicError))
            {
                error = L"写档案失败：" + path + (atomicError.empty() ? std::wstring()
                                                                      : (L"\r\n" + atomicError));
                return false;
            }
            return true;
        }

        bool ReadArchiveFile(const std::wstring& path, Archive& out, std::wstring& error)
        {
            error.clear();
            std::vector<unsigned char> blob;
            DWORD readError = 0;
            if (!ReadAllBytes(path, blob, readError))
            {
                error = L"读不到档案：" + path + L"（Win32 错误码 " + FormatUInt(readError) + L"）";
                return false;
            }
            return DecodeArchive(blob, out, error);
        }

        // =====================================================================
        // 生成
        // =====================================================================

        namespace
        {
            // 由目标 exe 路径填 ExeDigest。
            ExeDigest MakeExeDigest(const std::wstring& path)
            {
                ExeDigest digest;
                if (path.empty() || !FileExists(path))
                {
                    return digest;
                }
                digest.name = LeafName(path);
                WIN32_FILE_ATTRIBUTE_DATA attributes{};
                if (::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes))
                {
                    LARGE_INTEGER size{};
                    size.LowPart = attributes.nFileSizeLow;
                    size.HighPart = static_cast<LONG>(attributes.nFileSizeHigh);
                    digest.size = static_cast<unsigned long long>(size.QuadPart);
                }
                digest.sha256 = Sha256HexFile(path);
                return digest;
            }

            // _crack 兄弟 exe：设计稿 §2.1 —— 步骤1 的补丁流程会把目标改成 <原名>_crack.exe，
            // 查找时目标与兄弟两个摘要都试。两个方向都算（有 _crack 就去掉，没有就加上）。
            std::wstring SiblingExePath(const std::wstring& targetExe)
            {
                const std::wstring directory = ParentDirectory(targetExe);
                const std::wstring name = LeafName(targetExe);
                if (name.empty())
                {
                    return std::wstring();
                }

                const std::wstring lower = ToLowerString(name);
                const size_t position = lower.find(L"_crack");
                std::wstring siblingName;
                if (position != std::wstring::npos)
                {
                    siblingName = name.substr(0u, position) + name.substr(position + 6u);
                }
                else
                {
                    const size_t dot = name.find_last_of(L'.');
                    if (dot == std::wstring::npos)
                    {
                        siblingName = name + L"_crack";
                    }
                    else
                    {
                        siblingName = name.substr(0u, dot) + L"_crack" + name.substr(dot);
                    }
                }

                if (siblingName.empty() || _wcsicmp(siblingName.c_str(), name.c_str()) == 0)
                {
                    return std::wstring();
                }
                return CombinePath(directory, siblingName);
            }

            // 把 sourceDirectory 记成相对 workspaceRoot 的相对路径（记不进就原样留着）。
            std::wstring MakeRelativeSource(const std::wstring& workspaceRoot,
                                            const std::wstring& sourceDirectory)
            {
                if (workspaceRoot.empty() || sourceDirectory.empty())
                {
                    return sourceDirectory;
                }
                const std::wstring root = TrimTrailingSeparators(workspaceRoot);
                if (sourceDirectory.size() > root.size() + 1u
                    && _wcsnicmp(sourceDirectory.c_str(), root.c_str(), root.size()) == 0
                    && (sourceDirectory[root.size()] == L'\\' || sourceDirectory[root.size()] == L'/'))
                {
                    return sourceDirectory.substr(root.size() + 1u);
                }
                return sourceDirectory;
            }

            // 名字列表排序：不区分大小写，同名再按原串兜底 —— 顺序必须稳定，否则档案不可复现。
            void SortNames(std::vector<std::wstring>& names)
            {
                std::sort(names.begin(), names.end(),
                          [](const std::wstring& left, const std::wstring& right)
                          {
                              const int compare = _wcsicmp(left.c_str(), right.c_str());
                              if (compare != 0)
                              {
                                  return compare < 0;
                              }
                              return left < right;
                          });
            }
        }

        GenerateResult BuildArchive(const GenerateOptions& options)
        {
            GenerateResult result;
            const DWORD started = ::GetTickCount();
            auto finish = [&]() -> GenerateResult
            {
                result.elapsedMillis = static_cast<unsigned long long>(::GetTickCount() - started);
                return result;
            };
            try
            {
            // 1) 定盐：Universal.log -> 静态派生 -> 明确报错中止（绝不静默）
            result.salt = ResolveSalt(options.logDirectory, options.staticKeyDirectory);
            if (!result.salt.available)
            {
                result.error = result.salt.message;
                return finish();
            }

            // 2) 采集
            Collected collected;
            if (!CollectSources(options.hashDirectory, collected, result.error))
            {
                return finish();
            }

            if (collected.directories.empty() && collected.files.empty())
            {
                result.error = L"没有从 " + options.hashDirectory
                             + L" 采集到任何名字映射（DirectoryHash.log / FileNameHash.log / "
                               L"MinedNames.lst / HashRestore_RecoveredNames.lst / 其它 *.lst 都是空的）。"
                               L"请先执行步骤2 采集或步骤3 还原，再生成扩展集。";
                return finish();
            }

            if (options.cancelRequested && options.cancelRequested())
            {
                result.error = L"用户中止";
                return finish();
            }

            // 3) 归一化 + **生成期自校验**
            result.sources = collected.counts;
            result.collectedDirectories = static_cast<unsigned int>(collected.directories.size());
            result.collectedFiles = static_cast<unsigned int>(collected.files.size());

            std::map<std::wstring, std::wstring> directoryByHash;   // 大写 hash -> 规范化名字
            std::map<std::wstring, std::wstring> fileByHash;
            std::set<std::wstring> directoryNames;
            std::set<std::wstring> fileNames;
            std::map<std::wstring, std::wstring> exceptions;        // 大写 hash -> 原样名字
            unsigned int acceptedDirectories = 0u;
            unsigned int acceptedFiles = 0u;

            const size_t totalCandidates = collected.directories.size() + collected.files.size();
            size_t processed = 0u;
            unsigned int lastPercent = 0xFFFFFFFFu;

            auto reportProgress = [&](const wchar_t* stage)
            {
                if (!options.progress || totalCandidates == 0u)
                {
                    return;
                }
                const unsigned int percent = static_cast<unsigned int>(
                    (static_cast<unsigned long long>(processed) * 100ull) / totalCandidates);
                if (percent != lastPercent)
                {
                    lastPercent = percent;
                    options.progress(percent, stage);
                }
            };

            auto processCandidate = [&](const Candidate& candidate)
            {
                ++processed;
                const std::wstring hash = ToUpperString(candidate.hash);
                std::wstring normalized;
                if (!VerifyCandidate(candidate, result.salt.salt, normalized))
                {
                    // 重算不回去 —— 绝不写进名字层，原样进 EXCEPTIONS 并计数（§3.2）。
                    exceptions.emplace(hash, candidate.rawName);
                    return;
                }

                if (candidate.isDirectory)
                {
                    ++acceptedDirectories;
                    directoryByHash.emplace(hash, normalized);
                    directoryNames.insert(normalized);
                }
                else
                {
                    ++acceptedFiles;
                    fileByHash.emplace(hash, normalized);
                    fileNames.insert(normalized);
                }
            };

            for (const Candidate& candidate : collected.directories)
            {
                if (options.cancelRequested && options.cancelRequested())
                {
                    result.error = L"用户中止";
                    return finish();
                }
                processCandidate(candidate);
                reportProgress(L"归一化 + 自校验（目录）");
            }
            for (const Candidate& candidate : collected.files)
            {
                if (options.cancelRequested && options.cancelRequested())
                {
                    result.error = L"用户中止";
                    return finish();
                }
                processCandidate(candidate);
                reportProgress(L"归一化 + 自校验（文件）");
            }

            result.normalizedDirectories = static_cast<unsigned int>(directoryByHash.size());
            result.normalizedFiles = static_cast<unsigned int>(fileByHash.size());
            result.verifiedDirectories = acceptedDirectories;
            result.verifiedFiles = acceptedFiles;
            result.exceptionCount = static_cast<unsigned int>(exceptions.size());
            result.duplicateEntries =
                (acceptedDirectories - static_cast<unsigned int>(directoryByHash.size()))
                + (acceptedFiles - static_cast<unsigned int>(fileByHash.size()));

            if (result.exceptionCount > 0u)
            {
                result.warnings.push_back(
                    L"有 " + FormatUInt(result.exceptionCount)
                    + L" 条名字重算哈希与采集到的不一致，已原样放进 EXCEPTIONS（不写进名字层）。"
                      L"数量很大时通常意味着归一化口径或盐不对。");
            }

            // 4) 组装
            Archive archive;
            archive.meta = options.meta;
            if (archive.meta.toolVersion.empty())
            {
                archive.meta.toolVersion = kToolVersion;
            }
            if (options.stampGeneratedAt && archive.meta.generatedAt.empty())
            {
                SYSTEMTIME now{};
                ::GetLocalTime(&now);
                wchar_t buffer[64]{};
                ::swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth,
                             now.wDay, now.wHour, now.wMinute, now.wSecond);
                archive.meta.generatedAt = buffer;
            }
            if (archive.meta.title.empty())
            {
                // 默认 = 目标 exe 名去扩展名（§8.2），只在这里兜底，不覆盖用户填的值。
                const std::wstring leaf = LeafName(options.targetExe);
                const size_t dot = leaf.find_last_of(L'.');
                archive.meta.title = (dot == std::wstring::npos || dot == 0u) ? leaf
                                                                             : leaf.substr(0u, dot);
            }

            archive.identity.salt = result.salt.salt;
            archive.identity.saltKnown = true;
            archive.identity.sourceDirectory =
                MakeRelativeSource(options.workspaceRoot, options.sourceDirectory);
            archive.identity.collectionSources = collected.sourceLines;
            archive.identity.targetExe = MakeExeDigest(options.targetExe);
            if (archive.identity.targetExe.Present())
            {
                const std::wstring sibling = SiblingExePath(options.targetExe);
                if (!sibling.empty())
                {
                    archive.identity.siblingExe = MakeExeDigest(sibling);
                }
            }
            else if (!options.targetExe.empty())
            {
                result.warnings.push_back(L"读不到目标 exe：" + options.targetExe
                                          + L"，档案里不会记 exe 摘要（加载时只能靠盐校验归属）。");
            }

            for (const auto& entry : directoryByHash)
            {
                archive.directories.push_back(entry.second);
            }
            for (const auto& entry : fileByHash)
            {
                archive.files.push_back(entry.second);
            }
            SortNames(archive.directories);
            SortNames(archive.files);
            for (const auto& entry : exceptions)
            {
                archive.exceptions.push_back(entry.first + L":" + entry.second);
            }
            archive.stats.sources = collected.counts;
            archive.stats.directories = static_cast<unsigned int>(archive.directories.size());
            archive.stats.files = static_cast<unsigned int>(archive.files.size());
            archive.stats.exceptions = result.exceptionCount;
            archive.stats.duplicates = result.duplicateEntries;

            std::wstring lookupError;
            if (!RebuildLookup(archive, lookupError))
            {
                result.error = lookupError;
                return finish();
            }

            result.archive = archive;
            result.ok = true;
            return finish();
            }
            catch (const std::bad_alloc&)
            {
                result.ok = false;
                result.error = L"内存不足：生成扩展集被中止。";
                return finish();
            }
            catch (const std::length_error&)
            {
                result.ok = false;
                result.error = L"采集内容超过可处理长度，生成被中止。";
                return finish();
            }
        }

        namespace
        {
            std::wstring BuildInfoText(const Archive& archive, const std::wstring& archivePath)
            {
                std::wstring text;
                text += L"YuaExtract 扩展集信息（只读副本，仅供人看）\r\n";
                text += L"工具一律不读本文件；加载只认 rules.int 里的 META chunk。\r\n";
                text += L"\r\n";
                text += L"会社：" + archive.meta.publisher + L"\r\n";
                text += L"作品：" + archive.meta.title + L"\r\n";
                text += L"贡献者：" + archive.meta.contributor + L"\r\n";
                text += L"版本：" + archive.meta.version + L"\r\n";
                text += L"日期：" + archive.meta.date + L"\r\n";
                text += L"留言：" + archive.meta.comment + L"\r\n";
                text += L"生成时间：" + archive.meta.generatedAt + L"\r\n";
                text += L"工具版本：" + archive.meta.toolVersion + L"\r\n";
                text += L"\r\n";
                text += L"盐：" + archive.identity.salt + L"（saltKnown="
                      + std::wstring(archive.identity.saltKnown ? L"1" : L"0") + L"）\r\n";
                text += L"盐摘要（sha256）：" + Sha256HexText(archive.identity.salt) + L"\r\n";
                text += L"目标 exe：" + archive.identity.targetExe.name + L"（"
                      + FormatUInt(archive.identity.targetExe.size) + L" 字节）\r\n";
                text += L"目标 exe 摘要：" + archive.identity.targetExe.sha256 + L"\r\n";
                text += L"兄弟 exe：" + archive.identity.siblingExe.name + L"\r\n";
                text += L"兄弟 exe 摘要：" + archive.identity.siblingExe.sha256 + L"\r\n";
                text += L"来源目录：" + archive.identity.sourceDirectory + L"\r\n";
                text += L"\r\n";
                text += L"目录条数：" + FormatUInt(archive.stats.directories) + L"\r\n";
                text += L"文件条数：" + FormatUInt(archive.stats.files) + L"\r\n";
                text += L"例外条数：" + FormatUInt(archive.stats.exceptions) + L"\r\n";
                text += L"去重条数：" + FormatUInt(archive.stats.duplicates) + L"\r\n";
                text += L"档案路径：" + archivePath + L"\r\n";
                text += L"\r\n";
                text += L"采集来源：\r\n";
                for (const std::wstring& item : archive.identity.collectionSources)
                {
                    text += L"  " + item + L"\r\n";
                }
                return text;
            }
        }

        GenerateResult GenerateArchive(const GenerateOptions& options, const std::wstring& archivePath)
        {
            GenerateResult result = BuildArchive(options);
            if (!result.ok)
            {
                return result;
            }
            if (archivePath.empty())
            {
                result.ok = false;
                result.error = L"没有给出档案落盘路径。";
                return result;
            }
            try
            {

            std::wstring error;
            const std::vector<unsigned char> blob = EncodeArchive(result.archive, error);
            if (blob.empty())
            {
                result.ok = false;
                result.error = error.empty() ? std::wstring(L"编码扩展集档案失败。") : error;
                return result;
            }

            const std::wstring outputDirectory = ParentDirectory(archivePath);
            DWORD directoryError = 0u;
            if (!outputDirectory.empty()
                && !EnsureDirectoryTree(outputDirectory, directoryError))
            {
                result.ok = false;
                result.error = L"建不出档案输出目录：" + outputDirectory + L"（Win32 错误码 "
                             + FormatUInt(directoryError) + L"）";
                return result;
            }
            std::wstring atomicError;
            if (!Common::AtomicFile::WriteFileAtomic(archivePath, blob, atomicError))
            {
                result.ok = false;
                result.error = L"写档案失败：" + archivePath
                             + (atomicError.empty() ? std::wstring() : (L"\r\n" + atomicError));
                return result;
            }

            result.archivePath = archivePath;
            result.archiveBytes = static_cast<unsigned long long>(blob.size());
            result.archiveSha256 = Sha256HexBytes(blob.data(), blob.size());

            // §4 步骤5：同目录另写 info.txt（meta 的只读副本，仅供人看）。
            const std::wstring infoPath = CombinePath(outputDirectory, kInfoFileName);
            const std::string infoUtf8 = WideToUtf8(BuildInfoText(result.archive, archivePath));
            std::vector<unsigned char> infoBytes;
            infoBytes.reserve(infoUtf8.size() + 3u);
            infoBytes.push_back(0xEFu);
            infoBytes.push_back(0xBBu);
            infoBytes.push_back(0xBFu);
            infoBytes.insert(infoBytes.end(), infoUtf8.begin(), infoUtf8.end());
            if (Common::AtomicFile::WriteFileAtomic(infoPath, infoBytes, atomicError))
            {
                result.infoPath = infoPath;
            }
            else
            {
                result.warnings.push_back(L"档案已写出，但 info.txt 没写成：" + infoPath
                                          + (atomicError.empty() ? std::wstring()
                                                                 : (L"\r\n" + atomicError)));
            }
            return result;
            }
            catch (const std::bad_alloc&)
            {
                result.ok = false;
                result.error = L"内存不足：写出扩展集档案被中止。";
                return result;
            }
            catch (const std::length_error&)
            {
                result.ok = false;
                result.error = L"扩展集内容超过可处理长度，写出被中止。";
                return result;
            }
        }

        // =====================================================================
        // 加载（§2.1）
        // =====================================================================

        LoadOptions ResolveLoadOptions(const std::wstring& targetExe, const std::wstring& logDirectory,
                                       const std::wstring& staticKeyDirectory)
        {
            LoadOptions options;
            options.targetExe = targetExe;
            const SaltResult salt = ResolveSalt(logDirectory, staticKeyDirectory);
            options.currentSalt = salt.salt;
            options.currentSaltKnown = salt.available;
            return options;
        }

        LoadResult LoadArchiveFile(const std::wstring& archivePath, const LoadOptions& options)
        {
            LoadResult result;
            result.archivePath = archivePath;
            try
            {

            std::vector<unsigned char> blob;
            DWORD readError = 0;
            if (!ReadAllBytes(archivePath, blob, readError))
            {
                result.error = L"读不到档案：" + archivePath + L"（Win32 错误码 "
                             + FormatUInt(readError) + L"）";
                result.failure = LoadFailure::Structure;
                return result;
            }
            result.archiveBytes = static_cast<unsigned long long>(blob.size());
            result.archiveSha256 = Sha256HexBytes(blob.data(), blob.size());

            if (!DecodeArchive(blob, result.archive, result.error))
            {
                // 解码失败一律是**结构**问题（magic/版本/截断/CRC/缺 chunk），
                // 绝不是"这是别的游戏的档案"——那个结论只能由盐比对得出。
                result.match = IdentityMatch::Rejected;
                result.failure = LoadFailure::Structure;
                return result;
            }

            const Archive& archive = result.archive;
            const std::wstring owner = archive.meta.publisher + L"/" + archive.meta.title;

            // 分支 1：exe 摘要命中（最强信号）。目标 exe 与 _crack 兄弟 exe 两个都试。
            bool exeMatched = false;
            if (!options.targetExe.empty())
            {
                const std::wstring targetDigest = Sha256HexFile(options.targetExe);
                if (!targetDigest.empty())
                {
                    if (!archive.identity.targetExe.sha256.empty()
                        && SameHash(targetDigest, archive.identity.targetExe.sha256))
                    {
                        exeMatched = true;
                    }
                    else if (!archive.identity.siblingExe.sha256.empty()
                             && SameHash(targetDigest, archive.identity.siblingExe.sha256))
                    {
                        exeMatched = true;
                        result.matchNote = L"当前 exe 命中的是档案里记录的 _crack 兄弟 exe 摘要。";
                    }
                }
            }

            if (exeMatched)
            {
                result.ok = true;
                result.match = IdentityMatch::ExeMatch;
                if (result.matchNote.empty())
                {
                    result.matchNote = L"来源：扩展集 " + owner + L"（exe 摘要命中）";
                }
                return result;
            }

            // 分支 2/3：看盐。
            if (options.currentSaltKnown)
            {
                if (!SameHash(options.currentSalt, archive.identity.salt)
                    && options.currentSalt != archive.identity.salt)
                {
                    result.ok = false;
                    result.match = IdentityMatch::Rejected;
                    result.failure = LoadFailure::SaltMismatch;
                    result.error = L"该扩展集属于 " + owner + L"，与当前游戏的盐不同，拒绝加载。"
                                   L"（哈希不可跨作移植：盐变了档案里一条都不会中。）";
                    return result;
                }
                result.ok = true;
                result.match = IdentityMatch::SaltMatch;
                result.matchNote = L"来源：扩展集 " + owner
                                 + L"（盐一致、exe 不同：可能打过补丁或换过版本）";
                return result;
            }

            // 既没命中摘要，又拿不到当前游戏的盐：绝不能装作没事。
            if (options.allowUnverified)
            {
                result.ok = true;
                // **不能**置成 SaltMatch：这条路上根本没有盐可比，说"盐一致"是假话（P-2）。
                result.match = IdentityMatch::Unverified;
                result.failure = LoadFailure::None;
                result.matchNote = L"来源：扩展集 " + owner
                                 + L"（未能校验归属：exe 摘要不中，且拿不到当前游戏的盐，"
                                   L"无法做任何盐比对；本条是用户强制放行，未做归属校验）";
                return result;
            }

            result.ok = false;
            result.match = IdentityMatch::Rejected;
            result.failure = LoadFailure::Unverified;
            result.error = L"无法校验扩展集归属，拒绝加载：档案属于 " + owner
                         + L"，但当前目标 exe 的摘要与档案记录不符，"
                           L"而且没有可用的当前游戏盐（先跑一次步骤2 或静态 Key 提取即可拿到）。"
                           L"确需强制加载请显式打开 allowUnverified。";
            return result;
            }
            catch (const std::bad_alloc&)
            {
                result.ok = false;
                result.match = IdentityMatch::Rejected;
                result.failure = LoadFailure::Structure;
                result.error = L"内存不足：读取/解码扩展集档案被中止。";
                return result;
            }
            catch (const std::length_error&)
            {
                result.ok = false;
                result.match = IdentityMatch::Rejected;
                result.failure = LoadFailure::Structure;
                result.error = L"扩展集档案超过可处理长度，加载被中止。";
                return result;
            }
        }

        LoadResult LoadArchiveFromLibrary(const std::wstring& libraryRoot, const std::wstring& publisher,
                                          const std::wstring& title, const LoadOptions& options)
        {
            LoadResult result;
            const std::wstring safePublisher = MakeSafePathComponent(publisher);
            const std::wstring safeTitle = MakeSafePathComponent(title);
            if (libraryRoot.empty() || safePublisher.empty() || safeTitle.empty())
            {
                result.error = L"扩展集库路径 / 会社 / 作品名不完整，无法定位档案。";
                result.failure = LoadFailure::BadArgument;
                return result;
            }
            const std::wstring path =
                CombinePath(CombinePath(CombinePath(libraryRoot, safePublisher), safeTitle),
                            kArchiveFileName);
            return LoadArchiveFile(path, options);
        }

        // =====================================================================
        // 静态 Hash 表
        // =====================================================================

        StaticHashResult WriteStaticHashMap(const Archive& archive, const std::wstring& outputDirectory)
        {
            StaticHashResult result;
            try
            {
            if (outputDirectory.empty())
            {
                result.error = L"没有给出 StaticHash_Output 目录。";
                return result;
            }

            Archive local;
            const Archive* source = &archive;
            if (!archive.lookupReady)
            {
                local = archive;
                std::wstring lookupError;
                if (!RebuildLookup(local, lookupError))
                {
                    result.error = lookupError;
                    return result;
                }
                source = &local;
            }

            if (source->fileNames.empty() && source->directoryNames.empty())
            {
                result.error = L"档案里没有任何名字（DIRS/FILES 都是空的），生成的静态 Hash 表会是空的，"
                               L"按设计明确报错而不是写一个空表出去。";
                return result;
            }

            // 行序照 HashRestore_RecoveredNames.lst：16 位目录在前 / 64 位文件在后，
            // 同档按名字不区分大小写排序，再按整行兜底 —— 顺序必须稳定。
            struct Line
            {
                std::wstring hash;
                std::wstring name;
                std::wstring text;
                int rank = 0;
            };
            std::vector<Line> lines;
            lines.reserve(source->directoryNames.size() + source->fileNames.size());
            for (const auto& entry : source->directoryNames)
            {
                Line line;
                line.hash = entry.first;
                line.name = entry.second;
                line.text = entry.first + L":" + entry.second;
                line.rank = 0;
                lines.push_back(line);
            }
            for (const auto& entry : source->fileNames)
            {
                Line line;
                line.hash = entry.first;
                line.name = entry.second;
                line.text = entry.first + L":" + entry.second;
                line.rank = 1;
                lines.push_back(line);
            }
            std::sort(lines.begin(), lines.end(),
                      [](const Line& left, const Line& right)
                      {
                          if (left.rank != right.rank)
                          {
                              return left.rank < right.rank;
                          }
                          const int compare = _wcsicmp(left.name.c_str(), right.name.c_str());
                          if (compare != 0)
                          {
                              return compare < 0;
                          }
                          return _wcsicmp(left.text.c_str(), right.text.c_str()) < 0;
                      });

            std::wstring text;
            text.reserve(lines.size() * 96u);
            for (const Line& line : lines)
            {
                text += line.text;
                text += L"\r\n";
            }

            DWORD error = 0;
            if (!EnsureDirectoryTree(outputDirectory, error))
            {
                result.error = L"建不出 StaticHash_Output 目录：" + outputDirectory + L"（Win32 错误码 "
                             + FormatUInt(error) + L"）";
                return result;
            }

            // UTF-8 **无 BOM**：与 HashRestore_RecoveredNames.lst 一致，步骤3 直接读得回来。
            const std::string utf8 = WideToUtf8(text);
            const std::wstring path = CombinePath(outputDirectory, kStaticHashFileName);
            if (!WriteAllBytes(path, utf8.data(), utf8.size(), error))
            {
                result.error = L"写 " + path + L" 失败（Win32 错误码 " + FormatUInt(error) + L"）";
                return result;
            }

            result.ok = true;
            result.path = path;
            result.directoryEntries = static_cast<unsigned int>(source->directoryNames.size());
            result.fileEntries = static_cast<unsigned int>(source->fileNames.size());
            result.bytes = static_cast<unsigned long long>(utf8.size());
            return result;
            }
            catch (const std::bad_alloc&)
            {
                result.ok = false;
                result.error = L"内存不足：生成静态 Hash 表被中止。";
                return result;
            }
            catch (const std::length_error&)
            {
                result.ok = false;
                result.error = L"静态 Hash 表内容超过可处理长度，生成被中止。";
                return result;
            }
        }

        // =====================================================================
        // 干跑（只读推演）
        // =====================================================================

        DryRunResult DryRun(const Archive& archive, const DryRunOptions& options)
        {
            DryRunResult result;
            try
            {

            if (options.sourceDirectory.empty() || !DirectoryExists(options.sourceDirectory))
            {
                result.error = L"测试来源目录不存在：" + options.sourceDirectory;
                return result;
            }
            std::wstring offending;
            if (PathSafety::HasReparsePoint(options.sourceDirectory, &offending))
            {
                result.error = L"测试来源目录本身是/含有重解析点，拒绝跟随：" + offending;
                return result;
            }

            Archive local;
            const Archive* source = &archive;
            if (!archive.lookupReady)
            {
                local = archive;
                std::wstring lookupError;
                if (!RebuildLookup(local, lookupError))
                {
                    result.error = lookupError;
                    return result;
                }
                source = &local;
            }

            std::vector<ScannedFile> files;
            unsigned int skippedLogs = 0u;
            unsigned int skippedReparse = 0u;
            std::wstring scanError;
            if (!CollectScannedFiles(options.sourceDirectory, options.sourceDirectory, files,
                                     skippedLogs, skippedReparse, scanError))
            {
                // 不可列的子目录**不能**被当成"全部扫完了"：直接失败并把目录名带出去。
                result.error = scanError;
                return result;
            }
            result.skippedReparsePoints = skippedReparse;
            result.totalFiles = static_cast<unsigned int>(files.size());

            std::set<std::wstring> hitDirectories;
            std::set<std::wstring> hitFiles;
            std::set<std::wstring> missDirectories;
            std::set<std::wstring> missFiles;

            const size_t total = files.size();
            size_t processed = 0u;
            unsigned int lastPercent = 0xFFFFFFFFu;

            for (const ScannedFile& file : files)
            {
                if (options.cancelRequested && options.cancelRequested())
                {
                    result.error = L"用户中止";
                    return result;
                }
                ++processed;

                const std::vector<std::wstring> parts = SplitRelativePath(file.relativePath);
                const bool hashNamed = parts.size() >= 2u
                                    && IsHexLength(ToUpperString(parts[parts.size() - 2u]), 16u)
                                    && IsHashStyleLeaf(parts.back());

                if (!hashNamed)
                {
                    ++result.passthroughFiles;
                }
                else
                {
                    ++result.hashNamedFiles;
                    const std::wstring directoryHash = ToUpperString(parts[parts.size() - 2u]);
                    const std::wstring lastPart = parts.back();

                    // 与 HashNameRestorer::ResolveHashName 同口径：
                    // 先整段（含后缀）查，不中再剥掉后缀用主干查。
                    std::wstring fileHash = ToUpperString(StripExtension(lastPart));
                    bool fileResolved = false;
                    if (source->fileNames.find(ToUpperString(lastPart)) != source->fileNames.end())
                    {
                        fileHash = ToUpperString(lastPart);
                        fileResolved = true;
                    }
                    else
                    {
                        const size_t dot = lastPart.find_last_of(L'.');
                        if (dot != std::wstring::npos && dot > 0u)
                        {
                            const auto stem = source->fileNames.find(fileHash);
                            if (stem != source->fileNames.end() && !stem->second.empty())
                            {
                                fileResolved = true;
                            }
                        }
                    }

                    const bool directoryResolved =
                        source->directoryNames.find(directoryHash) != source->directoryNames.end();

                    if (directoryResolved)
                    {
                        hitDirectories.insert(directoryHash);
                    }
                    else
                    {
                        ++result.missingDirectory;
                        missDirectories.insert(directoryHash);
                    }
                    if (fileResolved)
                    {
                        hitFiles.insert(fileHash);
                    }
                    else
                    {
                        ++result.missingFileName;
                        missFiles.insert(fileHash);
                    }

                    if (directoryResolved && fileResolved)
                    {
                        ++result.fullyResolvedFiles;
                    }
                    else
                    {
                        DryRunMiss miss;
                        miss.relativePath = file.relativePath;
                        miss.directoryHash = directoryHash;
                        miss.fileHash = fileHash;
                        miss.directoryResolved = directoryResolved;
                        miss.fileResolved = fileResolved;
                        result.misses.push_back(miss);
                    }
                }

                if (options.progress && total > 0u)
                {
                    const unsigned int percent = static_cast<unsigned int>(
                        (static_cast<unsigned long long>(processed) * 100ull) / total);
                    if (percent != lastPercent)
                    {
                        lastPercent = percent;
                        options.progress(percent, file.relativePath);
                    }
                }
            }

            result.resolvedDirectories = static_cast<unsigned int>(hitDirectories.size());
            result.resolvedFiles = static_cast<unsigned int>(hitFiles.size());
            result.missingDirectoryHashes.assign(missDirectories.begin(), missDirectories.end());
            result.missingFileHashes.assign(missFiles.begin(), missFiles.end());
            result.successRate = result.hashNamedFiles > 0u
                ? static_cast<unsigned int>(
                      (static_cast<unsigned long long>(result.fullyResolvedFiles) * 100ull)
                      / result.hashNamedFiles)
                : 0u;
            result.ok = true;
            return result;
            }
            catch (const std::bad_alloc&)
            {
                result.ok = false;
                result.error = L"内存不足：读取测试目录时被中止。";
                return result;
            }
            catch (const std::length_error&)
            {
                result.ok = false;
                result.error = L"测试目录内容超过可处理长度，扫描被中止。";
                return result;
            }
        }

        // =====================================================================
        // 扩展集库
        // =====================================================================

        namespace
        {
            // manifest 的一行：会社 / 作品 / exe摘要 / 兄弟exe摘要 / 盐摘要 / 档案摘要 /
            //                  条数 / 生成时间 / 安装时间 / 来源
            struct ManifestRow
            {
                std::wstring publisher;
                std::wstring title;
                std::wstring exeDigest;
                std::wstring siblingDigest;
                std::wstring saltDigest;
                std::wstring archiveDigest;
                std::wstring entryCount;
                std::wstring generatedAt;
                std::wstring installedAt;
                std::wstring source;
            };

            std::wstring SanitizeField(const std::wstring& value)
            {
                std::wstring out;
                out.reserve(value.size());
                for (wchar_t ch : value)
                {
                    if (ch == L'\t' || ch == L'\r' || ch == L'\n')
                    {
                        out.push_back(L' ');
                    }
                    else
                    {
                        out.push_back(ch);
                    }
                }
                return out;
            }

            std::wstring ManifestLine(const ManifestRow& row)
            {
                std::wstring line;
                const std::wstring* fields[10] = {
                    &row.publisher, &row.title, &row.exeDigest, &row.siblingDigest, &row.saltDigest,
                    &row.archiveDigest, &row.entryCount, &row.generatedAt, &row.installedAt, &row.source
                };
                for (int i = 0; i < 10; ++i)
                {
                    if (i > 0)
                    {
                        line.push_back(L'\t');
                    }
                    line += SanitizeField(*fields[i]);
                }
                return line;
            }

            // 解析出来的一行还原成写盘用的行：安装时 upsert 清单要**保住别人写的行**
            //（整份重写清单是既有行为，那就得把读回来的行原样写回去才有意义）。
            ManifestRow RowFromEntry(const LibraryEntry& entry)
            {
                ManifestRow row;
                row.publisher = entry.publisher;
                row.title = entry.title;
                row.exeDigest = entry.exeDigest;
                row.siblingDigest = entry.siblingDigest;
                row.saltDigest = entry.saltDigest;
                row.archiveDigest = entry.archiveDigest;
                row.entryCount = FormatUInt(static_cast<unsigned long long>(entry.entryCount));
                row.generatedAt = entry.generatedAt;
                row.installedAt = entry.installedAt;
                row.source = entry.source;
                return row;
            }

            // 一行解析的结果，分三档。
            // 为什么要分档（2026-10-15 定）：本地库清单是"用户已经装好的东西的索引"，
            // 一行被改坏不该让整个扩展集页突然空白；而"字段里明明写了值、值却是坏的"说明
            // 这一份文本不可信，必须整份拒掉（false = 整份不可用，见 ExtensionSet.h）。
            enum class ManifestLineStatus
            {
                Ok = 0,       // 这一行能用
                Skipped = 1,  // 这一行不合规，跳过它、其余照收（原因写进 message，绝不静默）
                Failed = 2    // 这一行坏到不能用，整份文本作废（原因写进 message）
            };

            // 清单 / 索引的一行 -> LibraryEntry。**全工程唯一的 TSV 解析实现**：
            // ExtensionSet::ParseManifestText 只经这一条路，本地库清单（EnumerateLibrary）
            // 与远端索引（Online::FetchIndex）共用它 —— 两处口径一分叉就会出现
            // "本地能读、远端读不了"这种怪事（ExtensionSet.h 里已写明）。
            //
            // lineNumber 只进 message：报错要点名是第几行，否则用户拿着几十行的索引无从下手。
            // 没有一行是"悄悄没有的"：能收的进 entry，不能收的一定给出 message。
            // entry.archivePath 留空：解析器不知道库根，由调用方补。
            ManifestLineStatus ParseManifestLine(const std::wstring& line, size_t lineNumber,
                                                LibraryEntry& entry, std::wstring& message)
            {
                // 按 \t 切；第 10 列里再冒出 \t 就是"多列"，靠下面的总数判定拦掉。
                std::vector<std::wstring> fields;
                size_t start = 0u;
                for (;;)
                {
                    const size_t tab = line.find(L'\t', start);
                    if (tab == std::wstring::npos)
                    {
                        fields.push_back(line.substr(start));
                        break;
                    }
                    fields.push_back(line.substr(start, tab - start));
                    start = tab + 1u;
                }

                const std::wstring where =
                    L"第 " + FormatUInt(static_cast<unsigned long long>(lineNumber)) + L" 行：";
                if (fields.size() != 10u)
                {
                    // 旧实现在这里也是"不要这一行"（列数不够就丢，多出来的被吸进最后一列），
                    // 所以按跳过处理，只是这次把行号与原因报出去。
                    message = where + L"列数不对（应有 10 列，实际 "
                            + FormatUInt(static_cast<unsigned long long>(fields.size()))
                            + L" 列）。清单与索引都是 TSV，列之间必须用制表符分隔。";
                    return ManifestLineStatus::Skipped;
                }

                LibraryEntry parsed;
                parsed.publisher = fields[0];
                parsed.title = fields[1];
                if (parsed.publisher.empty() || parsed.title.empty())
                {
                    message = where + L"会社或作品是空的。每一条都必须写清是哪部作品的扩展集。";
                    return ManifestLineStatus::Skipped;
                }
                if (MakeSafePathComponent(parsed.publisher).empty()
                    || MakeSafePathComponent(parsed.title).empty())
                {
                    message = where + L"会社或作品名全是 Windows 目录非法字符，做不出库目录名。";
                    return ManifestLineStatus::Skipped;
                }

                // 四个摘要**都允许为空**：
                //   · 目标 exe 摘要 / 兄弟 exe 摘要：GenerateArchive 允许"没采集到目标 exe"，
                //     那种档案装进库时，InstallToLibrary 真的会往清单写一列空摘要。把它当坏行
                //     的后果是"用户装过这种扩展集之后，扩展集页整页空白" —— 相对旧行为的回归。
                //   · 盐摘要 / 档案摘要：同理，空 = "未知"，不是损坏。
                // 空的后果由消费方承担：Classify 里摘要为空就证明不了 Same；在线那侧另有
                // 更严的要求（索引是信任根，两个摘要少一个都不行，见 Online::FetchIndex）。
                // 但**写了就得写对**：非空却不是 64 位十六进制，说明这份文本不可信 → 整份作废。
                parsed.exeDigest = fields[2];
                if (!parsed.exeDigest.empty() && !IsHexLength(parsed.exeDigest, 64u))
                {
                    message = where + L"目标 exe 摘要既不是空，也不是 64 位十六进制（实际 \""
                            + parsed.exeDigest + L"\"）。";
                    return ManifestLineStatus::Failed;
                }

                parsed.siblingDigest = fields[3];
                if (!parsed.siblingDigest.empty() && !IsHexLength(parsed.siblingDigest, 64u))
                {
                    message = where + L"兄弟 exe 摘要既不是空，也不是 64 位十六进制（实际 \""
                            + parsed.siblingDigest + L"\"）。";
                    return ManifestLineStatus::Failed;
                }

                parsed.saltDigest = fields[4];
                if (!parsed.saltDigest.empty() && !IsHexLength(parsed.saltDigest, 64u))
                {
                    message = where + L"盐摘要既不是空，也不是 64 位十六进制（实际 \""
                            + parsed.saltDigest + L"\"）。";
                    return ManifestLineStatus::Failed;
                }

                parsed.archiveDigest = fields[5];
                if (!parsed.archiveDigest.empty() && !IsHexLength(parsed.archiveDigest, 64u))
                {
                    message = where + L"档案摘要既不是空，也不是 64 位十六进制（实际 \""
                            + parsed.archiveDigest + L"\"）。";
                    return ManifestLineStatus::Failed;
                }

                // 条数同理：空 = 不知道（按 0 记），写了但不是整数 = 文本不可信。
                unsigned long long count = 0ull;
                if (!fields[6].empty() && (!ParseUInt(fields[6], count) || count > 0xFFFFFFFFull))
                {
                    message = where + L"条数既不是空，也不是合法整数（实际 \"" + fields[6] + L"\"）。";
                    return ManifestLineStatus::Failed;
                }
                parsed.entryCount = static_cast<unsigned int>(count);

                parsed.generatedAt = fields[7];
                parsed.installedAt = fields[8];
                parsed.source = fields[9];

                entry = parsed;
                return ManifestLineStatus::Ok;
            }

            std::string BuildManifestBytes(const std::vector<ManifestRow>& rows)
            {
                std::wstring text;
                text += L"# 会社\t作品\t目标exe摘要\t兄弟exe摘要\t盐摘要\t档案摘要\t条数\t生成时间\t安装时间\t来源\r\n";
                for (const ManifestRow& row : rows)
                {
                    text += ManifestLine(row);
                    text += L"\r\n";
                }
                return WideToUtf8(text);
            }

            bool WriteManifest(const std::wstring& path, const std::vector<ManifestRow>& rows,
                               std::wstring& error)
            {
                error.clear();
                const std::string utf8 = BuildManifestBytes(rows);
                if (!Common::AtomicFile::WriteFileAtomic(
                        path, std::vector<unsigned char>(utf8.begin(), utf8.end()), error))
                {
                    error = L"写扩展集库清单失败：" + path + L"\r\n" + error;
                    return false;
                }
                return true;
            }

            // 逐行切分并**给每行编号**（空行也数）。
            // 不复用 ForEachLine：它跳过空行，用它数出来的行号跟编辑器里看到的对不上，
            // 而报错恰恰要点名"第几行"。
            template <typename Visitor>
            void ForEachNumberedLine(const std::wstring& content, Visitor visitor)
            {
                size_t start = 0u;
                size_t number = 0u;
                while (start <= content.length())
                {
                    ++number;
                    const size_t end = content.find_first_of(L"\r\n", start);
                    const size_t stop = end == std::wstring::npos ? content.length() : end;
                    visitor(content.substr(start, stop - start), number);
                    if (end == std::wstring::npos)
                    {
                        break;
                    }
                    start = end;
                    while (start < content.length()
                           && (content[start] == L'\r' || content[start] == L'\n'))
                    {
                        ++start;
                    }
                }
            }

            // 读一份清单 / 索引文件 -> 文本 -> **唯一的解析实现**（ParseManifestText）。
            // 文件不存在不算错（库刚建好、仓库刚开张都还没有清单），按空清单返回。
            // 编码认既有口径（ReadTextFileAuto：带 BOM 的 UTF-8 / UTF-16、无 BOM 的 UTF-16 / UTF-8）。
            //
            // 成功返回时 error 里**可能仍有内容**：那是"清单里有几行不合规、已跳过"的报告
            // （点名行号），不是失败。调用方按自己的口径处置：EnumerateLibrary 把它挂进
            // LibraryCatalog，不静默吞掉。
            bool ReadManifestFile(const std::wstring& path, std::vector<LibraryEntry>& out,
                                  std::wstring& error)
            {
                out.clear();
                error.clear();

                // 不能只靠 FileExists：access denied / 同名目录都会返回 false，把它当
                // "清单不存在"会静默重建空清单、丢光旧条目。只把真正的 FILE_NOT_FOUND /
                // PATH_NOT_FOUND 当"还没有清单"，其余一律失败。
                const DWORD attributes = ::GetFileAttributesW(path.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES)
                {
                    const DWORD attributeError = ::GetLastError();
                    if (attributeError == ERROR_FILE_NOT_FOUND
                        || attributeError == ERROR_PATH_NOT_FOUND)
                    {
                        return true;   // 清单还不存在就当空的（与参数仓库 Manifest::load 一致）
                    }
                    error = L"读不到扩展集库清单：" + path + L"（Win32 错误码 "
                          + FormatUInt(attributeError) + L"）—— 不能当成空清单处理。";
                    return false;
                }
                if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                {
                    error = L"扩展集库清单位置是一个目录：" + path;
                    return false;
                }
                if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                {
                    error = L"扩展集库清单是重解析点/链接文件，拒绝读取：" + path;
                    return false;
                }
                std::wstring offending;
                if (PathSafety::HasReparsePoint(path, &offending))
                {
                    error = L"扩展集库清单路径上有重解析点，拒绝读取：" + offending;
                    return false;
                }

                std::wstring content;
                DWORD readError = 0;
                if (!ReadTextFileAuto(path, content, readError))
                {
                    error = L"读不到扩展集库清单：" + path + L"（Win32 错误码 "
                          + FormatUInt(readError) + L"）";
                    return false;
                }

                std::wstring parseReport;
                if (!ParseManifestText(WideToUtf8(content), out, parseReport))
                {
                    // 报错要带上是**哪一个文件**的哪一行，否则"第 3 行"没有指向。
                    error = L"扩展集库清单格式不对：" + path + L"\r\n" + parseReport;
                    return false;
                }

                error = parseReport.empty()
                    ? std::wstring()
                    : (L"扩展集库清单有不合规的行（已跳过）：" + path + L"\r\n" + parseReport);
                return true;
            }

            std::wstring InstalledPath(const std::wstring& libraryRoot, const std::wstring& publisher,
                                       const std::wstring& title)
            {
                return CombinePath(CombinePath(CombinePath(libraryRoot, publisher), title),
                                   kArchiveFileName);
            }
        }

        // 语义（与 ExtensionSet.h 的说明配套，2026-10-15 补充）：
        //   · 返回 false  = **整份文本不可用**：某一行"写了值但值是坏的"（摘要不是 64 位 hex、
        //                   条数不是整数）。这时 out 已清空，调用方不得使用。
        //   · 返回 true   = 剩下的行有效，out 可用；error 里可能有"跳过了哪几行"的报告
        //                   （列数不对、会社/作品为空 —— 旧 ReadManifest 就是跳过它们，
        //                   只是不吭声）。报告点名行号，**没有任何一行是悄悄消失的**。
        bool ParseManifestText(const std::string& utf8, std::vector<LibraryEntry>& out,
                               std::wstring& error)
        {
            out.clear();
            error.clear();

            // 远端拿回来的字节很可能带 UTF-8 BOM（编辑器"另存为 UTF-8"默认就带），
            // 留着的话第一行的第一列会多出三个看不见的字节，表头那句 '#' 注释也认不出来。
            size_t offset = 0u;
            if (utf8.size() >= 3u && static_cast<unsigned char>(utf8[0]) == 0xEFu
                && static_cast<unsigned char>(utf8[1]) == 0xBBu
                && static_cast<unsigned char>(utf8[2]) == 0xBFu)
            {
                offset = 3u;
            }
            const std::wstring text = Utf8ToWide(utf8.data() + offset, utf8.size() - offset);

            // 不合规的行（列数不对 / 会社作品为空）跳过后放这里，最后一次性报出去（点名行号）。
            std::vector<std::wstring> skipped;
            bool failed = false;
            std::wstring failure;

            ForEachNumberedLine(text,
                                [&](const std::wstring& line, size_t lineNumber)
                                {
                                    // '#' 开头的整行是注释（WriteManifest 写的表头就是这种），空行跳过。
                                    if (failed || line.empty() || line[0] == L'#')
                                    {
                                        return;
                                    }
                                    LibraryEntry entry;
                                    std::wstring message;
                                    const ManifestLineStatus status =
                                        ParseManifestLine(line, lineNumber, entry, message);
                                    if (status == ManifestLineStatus::Ok)
                                    {
                                        out.push_back(entry);
                                        return;
                                    }
                                    if (status == ManifestLineStatus::Skipped)
                                    {
                                        skipped.push_back(message);
                                        return;
                                    }
                                    // 第一处硬错就够了：整份文本已经不可信，后面不用再看。
                                    failed = true;
                                    failure = message;
                                });

            if (failed)
            {
                // 半份结果不许被当成有效清单用（头文件里已写明）：整份不算数。
                out.clear();
                error = failure;
                return false;
            }

            if (!skipped.empty())
            {
                // **不静默**：跳过了几行、第几行、什么原因，全写进 error 带出去。
                // 仍然返回 true —— 剩下的行是有效的，调用方该照收：本地库清单里有一行被改坏，
                // 不该让整个扩展集页空白（旧 ReadManifest 也是"跳过坏行、其余照收"，只是它
                // 一声不吭，这里补上那份报告）。
                // 要不要把"跳过"升级成硬错，由调用方按自己的口径定：在线索引要求整份干净
                //（§14.3），见 Online::FetchIndex。
                std::wstring report = L"有 " + FormatUInt(skipped.size()) + L" 行不合规，已跳过；"
                                      L"其余 " + FormatUInt(out.size()) + L" 行照收：\r\n";
                for (const std::wstring& item : skipped)
                {
                    report += L"  " + item + L"\r\n";
                }
                error = report;
            }
            return true;
        }

        LibraryCatalog EnumerateLibrary(const std::wstring& libraryRoot)
        {
            LibraryCatalog catalog;
            catalog.root = libraryRoot;

            if (libraryRoot.empty())
            {
                catalog.error = L"没有给出扩展集库根目录。";
                return catalog;
            }
            if (!DirectoryExists(libraryRoot))
            {
                // 库还不存在不算错：UI 显示"库是空的"即可。
                catalog.ok = true;
                catalog.manifestPath = CombinePath(libraryRoot, kManifestFileName);
                return catalog;
            }

            catalog.manifestPath = CombinePath(libraryRoot, kManifestFileName);
            std::vector<LibraryEntry> entries;
            // 清单的解析走 ParseManifestText —— 与在线索引（Online::FetchIndex）是同一份实现。
            // 返回 true 时 catalog.error 里**可能**躺着"哪几行不合规、已跳过"的报告：
            // 那是警告不是失败，ok 仍为 true —— 库照样列出来，报告由 LibraryCatalog 带出去，
            // 不静默吞掉（界面要不要显示由界面决定）。
            if (!ReadManifestFile(catalog.manifestPath, entries, catalog.error))
            {
                return catalog;
            }

            std::set<std::wstring> known;   // "会社\作品"（都取 sanitize 后的形态，见下）
            for (const LibraryEntry& parsed : entries)
            {
                LibraryEntry entry = parsed;
                // 库内档案路径要用**解析器不知道的**库根拼，而且会社 / 作品**必须先 sanitize**：
                // 清单里存的是原始名（展示保真，不改），库目录却是 InstallToLibrary 用
                // MakeSafePathComponent 建的 —— 不 sanitize 的话，名字里带 '/' 或 '\' 时
                // 这里会拼出一个根本不存在的路径（2026-10-15 修）。
                const std::wstring safePublisher = MakeSafePathComponent(parsed.publisher);
                const std::wstring safeTitle = MakeSafePathComponent(parsed.title);
                if (safePublisher.empty() || safeTitle.empty())
                {
                    // 解析器已经把这行当"不合规"跳过了，这里是兜底：绝不拿空目录名去拼路径。
                    catalog.error += L"会社 / 作品名为「" + parsed.publisher + L" / " + parsed.title
                                   + L"」的那一条做不出库目录名，已跳过。\r\n";
                    continue;
                }
                const std::wstring slotKey = LibrarySlotKey(parsed.publisher, parsed.title);
                if (slotKey.empty())
                {
                    catalog.error += L"会社 / 作品名为「" + parsed.publisher + L" / " + parsed.title
                                   + L"」的那一条做不出库目录名，已跳过。\r\n";
                    continue;
                }
                entry.archivePath = InstalledPath(libraryRoot, safePublisher, safeTitle);
                catalog.entries.push_back(entry);
                // 去重键必须与 Install upsert / Online::Classify 同一口径（sanitize + casefold），
                // 否则 sanitize 后同名的两份会被列成两条。
                known.insert(slotKey);
            }

            // 清单里没有、但目录里真有 rules.int 的，也列出来（否则 UI 会把它们藏起来）。
            {
                WIN32_FIND_DATAW publisherData{};
                HANDLE publisherFind =
                    ::FindFirstFileW(CombinePath(libraryRoot, L"*").c_str(), &publisherData);
                if (publisherFind != INVALID_HANDLE_VALUE)
                {
                    do
                    {
                        const std::wstring publisher = publisherData.cFileName;
                        if (publisher == L"." || publisher == L".."
                            || (publisherData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
                            || (publisherData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                        {
                            continue;   // 目录扫描不 follow 重解析点
                        }
                        const std::wstring publisherPath = CombinePath(libraryRoot, publisher);
                        WIN32_FIND_DATAW titleData{};
                        HANDLE titleFind =
                            ::FindFirstFileW(CombinePath(publisherPath, L"*").c_str(), &titleData);
                        if (titleFind == INVALID_HANDLE_VALUE)
                        {
                            continue;
                        }
                        do
                        {
                            const std::wstring title = titleData.cFileName;
                            if (title == L"." || title == L".."
                                || (titleData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
                                || (titleData.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                            {
                                continue;
                            }
                            const std::wstring slotKey = LibrarySlotKey(publisher, title);
                            if (slotKey.empty() || known.find(slotKey) != known.end())
                            {
                                continue;
                            }
                            const std::wstring path =
                                InstalledPath(libraryRoot, publisher, title);
                            if (!FileExists(path) || PathSafety::HasReparsePoint(path))
                            {
                                continue;
                            }
                            LibraryEntry entry;
                            entry.publisher = publisher;
                            entry.title = title;
                            entry.archivePath = path;
                            entry.source = L"（清单里没有，按目录补列）";
                            catalog.entries.push_back(entry);
                            known.insert(slotKey);
                        } while (::FindNextFileW(titleFind, &titleData));
                        ::FindClose(titleFind);
                    } while (::FindNextFileW(publisherFind, &publisherData));
                    ::FindClose(publisherFind);
                }
            }

            for (const LibraryEntry& entry : catalog.entries)
            {
                catalog.titles[entry.publisher].push_back(entry.title);
            }
            for (auto& item : catalog.titles)
            {
                std::sort(item.second.begin(), item.second.end(),
                          [](const std::wstring& left, const std::wstring& right)
                          { return _wcsicmp(left.c_str(), right.c_str()) < 0; });
                item.second.erase(std::unique(item.second.begin(), item.second.end()), item.second.end());
                catalog.publishers.push_back(item.first);
            }
            std::sort(catalog.publishers.begin(), catalog.publishers.end(),
                      [](const std::wstring& left, const std::wstring& right)
                      { return _wcsicmp(left.c_str(), right.c_str()) < 0; });

            catalog.ok = true;
            return catalog;
        }

        namespace
        {
            // 库级写事务的命名互斥体：同名（同一规范化库根）跨进程串行。
            // 超时有界，不把 UI 线程无限卡住；WAIT_ABANDONED 表示上一个持有者异常退出，
            // 我们接管所有权，并按普通成功处理（事务本身已由 STAGED/ROLLBACK 兜底）。
            class LibraryWriteLock
            {
            public:
                LibraryWriteLock() = default;
                LibraryWriteLock(const LibraryWriteLock&) = delete;
                LibraryWriteLock& operator=(const LibraryWriteLock&) = delete;

                ~LibraryWriteLock()
                {
                    if (handle_ != nullptr)
                    {
                        if (locked_)
                        {
                            ::ReleaseMutex(handle_);
                        }
                        ::CloseHandle(handle_);
                    }
                }

                bool Acquire(const std::wstring& normalizedRoot, std::wstring& error)
                {
                    error.clear();
                    // 互斥体名用**本地 digest**，不把用户路径直接写进内核对象名（路径里可能有
                    // 反斜杠、非 ASCII 或超长）。Local\ 前缀 = 本机会话内可见。
                    const std::wstring digest = Sha256HexText(ToLowerString(normalizedRoot));
                    const std::wstring name =
                        L"Local\\YuaExtract.ExtensionLibrary." + digest.substr(0u, 40u);
                    handle_ = ::CreateMutexW(nullptr, FALSE, name.c_str());
                    if (handle_ == nullptr)
                    {
                        error = L"创建库写锁失败（Win32 错误码 "
                              + FormatUInt(::GetLastError()) + L"）。";
                        return false;
                    }
                    const DWORD wait = ::WaitForSingleObject(handle_, 30000u);
                    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED)
                    {
                        locked_ = true;
                        return true;
                    }
                    error = (wait == WAIT_TIMEOUT)
                        ? L"另一个安装操作正在写同一个扩展集库，等待 30 秒仍未完成，已放弃。"
                        : L"等待库写锁失败（Win32 错误码 " + FormatUInt(::GetLastError()) + L"）。";
                    return false;
                }

            private:
                HANDLE handle_ = nullptr;
                bool locked_ = false;
            };

            // 只收回**本次调用自己新建**的目录（浅 -> 深记录，深 -> 浅删除）。
            // 只删空目录：里面若有用户数据/别的进程写入，RemoveDirectoryW 会失败，这里就停手，
            // 绝不做递归删除。
            std::wstring RemoveCreatedDirectories(const std::vector<std::wstring>& created)
            {
                std::wstring blocked;
                for (auto it = created.rbegin(); it != created.rend(); ++it)
                {
                    if (it->empty() || it->size() <= 3u)
                    {
                        continue;
                    }
                    if (!::RemoveDirectoryW(it->c_str()))
                    {
                        blocked = *it;
                        break;
                    }
                }
                return blocked;
            }

            bool ValidateInstallSlot(const std::wstring& normalizedRoot,
                                     const std::wstring& safePublisher,
                                     const std::wstring& safeTitle,
                                     std::wstring& targetDirectory, std::wstring& error)
            {
                std::wstring why;
                const std::wstring relative = safePublisher + L"\\" + safeTitle;
                if (!PathSafety::ValidateRelativeName(relative, PathSafety::Kind::Directory, &why))
                {
                    error = L"会社 / 作品名不安全，拒绝落库：" + why;
                    return false;
                }
                if (!PathSafety::TryResolveUnderRoot(normalizedRoot, relative,
                                                    PathSafety::Kind::Directory,
                                                    targetDirectory, &why))
                {
                    error = L"会社 / 作品名越出扩展集库根或路径不安全：" + why;
                    return false;
                }
                return true;
            }
        }

        bool InstallToLibrary(const std::wstring& libraryRoot, const std::wstring& archivePath,
                              std::wstring& error, const std::wstring& sourceLabel)
        {
            error.clear();
            if (libraryRoot.empty())
            {
                error = L"没有给出扩展集库根目录。";
                return false;
            }
            if (archivePath.empty())
            {
                error = L"没有给出要安装的档案路径。";
                return false;
            }

            // 路径接口：**只读一次**完整字节，之后全走字节接口。
            std::vector<unsigned char> blob;
            DWORD readError = 0;
            if (!ReadAllBytes(archivePath, blob, readError))
            {
                error = L"读不到要安装的档案：" + archivePath + L"（Win32 错误码 "
                      + FormatUInt(readError) + L"）";
                return false;
            }
            return InstallBytesToLibrary(libraryRoot, blob, error,
                                         sourceLabel.empty() ? archivePath : sourceLabel);
        }

        bool InstallBytesToLibrary(const std::wstring& libraryRoot,
                                   const std::vector<unsigned char>& blob,
                                   std::wstring& error,
                                   const std::wstring& sourceLabel,
                                   const LibraryInstallPolicy& policy)
        {
            error.clear();
            error.reserve(4096u);
            // 事务所需状态全部放在 try 外：catch 也要能在锁仍持有、createdDirectories 仍在
            // 的情况下尽力回滚。lock 在函数末尾析构（catch 之后），不会提前放锁。
            std::vector<Common::AtomicFile::StagedFile> pending;
            std::vector<std::wstring> createdDirectories;
            LibraryWriteLock lock;
            // 三个目标文件全部 Commit 成功后置 true：此后任何清理异常都**绝不能**回滚
            // （某些 backup 可能已经被 Finalize 删掉，再 Rollback 会误伤/误报）。
            bool transactionCommitted = false;

            const auto cancellationRequested = [&]() -> bool
            {
                if (!policy.cancelRequested)
                {
                    return false;
                }
                try
                {
                    return policy.cancelRequested();
                }
                catch (...)
                {
                    return true;
                }
            };

            const auto discardAll = [&]()
            {
                for (Common::AtomicFile::StagedFile& item : pending)
                {
                    Common::AtomicFile::Discard(item);
                }
            };
            // count = 要回滚的 pending 前缀长度（包含"失败的那一项"）。
            // **每一项独立 try**：某项 Rollback 构造错误串时再 bad_alloc，也只标记失败，
            // 继续恢复下一项；绝不因恢复前/恢复中分配失败而跳过剩余已更新项。
            const auto rollbackPrefix = [&](size_t count, std::wstring& report) -> bool
            {
                bool allOk = true;
                for (size_t i = count; i > 0u; --i)
                {
                    bool itemOk = false;
                    try
                    {
                        std::wstring rollbackError;
                        itemOk = Common::AtomicFile::Rollback(pending[i - 1u], rollbackError);
                        if (!itemOk)
                        {
                            try
                            {
                                report += L"  · " + rollbackError + L"\r\n";
                            }
                            catch (...)
                            {
                            }
                        }
                    }
                    catch (...)
                    {
                        itemOk = false;
                        try
                        {
                            report += L"  · 回滚某一项时再次发生异常；该项 backup/temp 已保留。\r\n";
                        }
                        catch (...)
                        {
                        }
                    }
                    if (!itemOk)
                    {
                        allOk = false;
                    }
                }
                return allOk;
            };
            const auto recoverAfterException = [&](std::wstring& report) -> bool
            {
                // bad_alloc 可能正发生在 Commit 之后（backup 已产生 / 目标已更新）。
                // 这里尽力回滚全部 pending（含失败项）；每项独立处理，异常不外抛。
                try
                {
                    return rollbackPrefix(pending.size(), report);
                }
                catch (...)
                {
                    try
                    {
                        report += L"  · 回滚过程中再次发生异常，状态不明。\r\n";
                    }
                    catch (...)
                    {
                    }
                    return false;
                }
            };
            const auto abortCancelled = [&](size_t committedCount) -> bool
            {
                std::wstring rollbackReport;
                bool rollbackOk = true;
                if (committedCount > 0u)
                {
                    rollbackOk = rollbackPrefix(committedCount, rollbackReport);
                }
                discardAll();
                const std::wstring blocked = RemoveCreatedDirectories(createdDirectories);
                error = L"扩展集安装已取消。";
                if (rollbackOk)
                {
                    error += L"\r\n本地库未改动。";
                }
                else
                {
                    error += L"\r\n取消后的回滚没有全部成功，旧数据仍在备份里（请勿删除）：\r\n"
                             + rollbackReport;
                }
                if (!blocked.empty())
                {
                    error += L"\r\n注意：本次新建的目录没能全部收回：" + blocked;
                }
                return false;
            };
            try
            {
            if (libraryRoot.empty())
            {
                error = L"没有给出扩展集库根目录。";
                return false;
            }

            const std::wstring normalizedRoot = PathSafety::NormalizeFullPath(libraryRoot);
            if (normalizedRoot.empty())
            {
                error = L"扩展集库根路径无效：" + libraryRoot;
                return false;
            }
            if (FileExists(normalizedRoot))
            {
                error = L"扩展集库根是一个文件，不是目录：" + normalizedRoot;
                return false;
            }

            if (!lock.Acquire(normalizedRoot, error))
            {
                return false;
            }
            if (cancellationRequested())
            {
                error = L"扩展集安装已取消，尚未改动本地库。";
                return false;
            }

            // ---------- 1) 所有"读 + 解码 + 编码 + 路径检查"都在任何目标覆盖之前完成 ----------
            Archive archive;
            if (!DecodeArchive(blob, archive, error))
            {
                return false;
            }

            if (!ValidateMeta(archive.meta, error))
            {
                return false;
            }

            const std::wstring publisher = MakeSafePathComponent(archive.meta.publisher);
            const std::wstring title = MakeSafePathComponent(archive.meta.title);
            const std::wstring slotKey = LibrarySlotKey(archive.meta.publisher, archive.meta.title);
            if (publisher.empty() || title.empty() || slotKey.empty())
            {
                error = L"会社 / 作品名做不出安全的库目录名，拒绝安装。";
                return false;
            }

            const std::wstring manifestPath = CombinePath(normalizedRoot, kManifestFileName);

            // 根路径与清单路径都不能是重解析点；库根不存在时检查已存在的祖先。
            std::wstring offending;
            if (PathSafety::HasReparsePoint(normalizedRoot, &offending))
            {
                error = L"拒绝安装：扩展集库路径上有重解析点：" + offending;
                return false;
            }
            if (FileExists(manifestPath) && PathSafety::HasReparsePoint(manifestPath, &offending))
            {
                error = L"拒绝安装：库清单是重解析点/链接文件：" + offending;
                return false;
            }

            // **清单读失败绝不能当成"不存在/空清单"**：那会静默丢掉所有旧条目。
            // 同理，清单里**有跳过的坏行**（ReadManifestFile 返回 true 但带报告）时也拒绝安装：
            // 安装会整份重写清单，那些"跳过的原始行"会被永久写丢。宁可拒绝并点名行号，
            // 也不静默破坏用户已有数据。
            std::vector<LibraryEntry> existingEntries;
            {
                std::wstring manifestReport;
                if (!ReadManifestFile(manifestPath, existingEntries, manifestReport))
                {
                    error = manifestReport;
                    return false;
                }
                if (!manifestReport.empty())
                {
                    error = L"扩展集库清单里有不合规的行，安装会整份重写并丢失这些原始行，"
                            L"已拒绝安装（本地一个字节都没改）。请先修好清单再装：\r\n"
                          + manifestReport;
                    return false;
                }
            }

            // Protection must use the list read under the library write lock. The UI-side
            // snapshot may already be stale by the time the download finishes.
            const unsigned long long incomingEntryCount =
                static_cast<unsigned long long>(archive.stats.directories) + archive.stats.files;
            if (policy.protectMoreComplete && !policy.forceReplaceMoreComplete)
            {
                for (const LibraryEntry& existing : existingEntries)
                {
                    const bool sameExe = !existing.exeDigest.empty()
                        && !archive.identity.targetExe.sha256.empty()
                        && SameHash(existing.exeDigest, archive.identity.targetExe.sha256);
                    const bool sameSlot =
                        LibrarySlotKey(existing.publisher, existing.title) == slotKey;
                    if ((sameExe || sameSlot) &&
                        static_cast<unsigned long long>(existing.entryCount) > incomingEntryCount)
                    {
                        error = L"安装已拒绝：本地同槽位或同目标程序的档案包含更多条目（本地 "
                                + FormatUInt(existing.entryCount) + L"，远端 "
                                + FormatUInt(incomingEntryCount)
                                + L"），且没有明确要求覆盖。\r\n\r\n本地库未改动。";
                        return false;
                    }
                }
            }

            std::wstring why;
            std::wstring targetDirectory;
            if (!ValidateInstallSlot(normalizedRoot, publisher, title, targetDirectory, why))
            {
                error = why;
                return false;
            }

            const std::wstring installedArchive = CombinePath(targetDirectory, kArchiveFileName);
            const std::wstring installedInfo = CombinePath(targetDirectory, kInfoFileName);
            if (!PathSafety::IsUnderRoot(normalizedRoot, targetDirectory, true)
                || !PathSafety::IsUnderRoot(normalizedRoot, installedArchive, true)
                || !PathSafety::IsUnderRoot(normalizedRoot, installedInfo, true)
                || !PathSafety::IsUnderRoot(normalizedRoot, manifestPath, true))
            {
                error = L"安装目标越出扩展集库根，拒绝。";
                return false;
            }
            if (PathSafety::HasReparsePoint(installedArchive, &offending)
                || PathSafety::HasReparsePoint(installedInfo, &offending)
                || PathSafety::HasReparsePoint(targetDirectory, &offending))
            {
                error = L"拒绝安装：库内目标路径上有重解析点：" + offending;
                return false;
            }

            const std::wstring archiveDigest = Sha256HexBytes(blob.data(), blob.size());
            const std::wstring saltDigest = Sha256HexText(archive.identity.salt);

            if (cancellationRequested())
            {
                error = L"扩展集安装已取消，尚未改动本地库。";
                return false;
            }

            SYSTEMTIME now{};
            ::GetLocalTime(&now);
            wchar_t installedAt[64]{};
            ::swprintf_s(installedAt, L"%04u-%02u-%02u %02u:%02u:%02u", now.wYear, now.wMonth,
                         now.wDay, now.wHour, now.wMinute, now.wSecond);

            // upsert：**统一用 LibrarySlotKey**（sanitize + casefold）判同一物理槽位，
            // 同一 exe 摘要的旧条目也替换；显示名仍保留清单里的原始名。
            std::vector<ManifestRow> rows;
            rows.reserve(existingEntries.size() + 1u);
            for (const LibraryEntry& existing : existingEntries)
            {
                const bool sameExe = !existing.exeDigest.empty()
                    && !archive.identity.targetExe.sha256.empty()
                    && SameHash(existing.exeDigest, archive.identity.targetExe.sha256);
                const bool sameSlot =
                    LibrarySlotKey(existing.publisher, existing.title) == slotKey;
                if (sameExe || sameSlot)
                {
                    continue;   // 被本次安装取代
                }
                rows.push_back(RowFromEntry(existing));
            }

            ManifestRow row;
            row.publisher = archive.meta.publisher;
            row.title = archive.meta.title;
            row.exeDigest = archive.identity.targetExe.sha256;
            row.siblingDigest = archive.identity.siblingExe.sha256;
            row.saltDigest = saltDigest;
            row.archiveDigest = archiveDigest;
            row.entryCount = FormatUInt(static_cast<unsigned long long>(archive.stats.directories)
                                        + archive.stats.files);
            row.generatedAt = archive.meta.generatedAt;
            row.installedAt = installedAt;
            row.source = sourceLabel;
            rows.push_back(row);

            const std::string manifestUtf8 = BuildManifestBytes(rows);
            const std::string infoUtf8 = WideToUtf8(BuildInfoText(archive, installedArchive));
            std::vector<unsigned char> infoBytes;
            infoBytes.reserve(infoUtf8.size() + 3u);
            infoBytes.push_back(0xEFu);
            infoBytes.push_back(0xBBu);
            infoBytes.push_back(0xBFu);
            infoBytes.insert(infoBytes.end(), infoUtf8.begin(), infoUtf8.end());
            const std::vector<unsigned char> manifestBytes(manifestUtf8.begin(), manifestUtf8.end());

            // ---------- 2) 建目录（只记本次新建的，失败时只收空目录） ----------
            DWORD createError = 0u;
            if (!EnsureDirectoryTree(normalizedRoot, createError, &createdDirectories))
            {
                const std::wstring blocked = RemoveCreatedDirectories(createdDirectories);
                error = L"建不出扩展集库根：" + normalizedRoot + L"（Win32 错误码 "
                      + FormatUInt(createError) + L"）";
                if (!blocked.empty())
                {
                    error += L"\r\n注意：本次新建的目录没能全部收回：" + blocked;
                }
                else
                {
                    error += L"\r\n\r\n本次没有改动本地库。";
                }
                return false;
            }
            if (!EnsureDirectoryTree(targetDirectory, createError, &createdDirectories))
            {
                const std::wstring blocked = RemoveCreatedDirectories(createdDirectories);
                error = L"建不出库目录：" + targetDirectory + L"（Win32 错误码 "
                      + FormatUInt(createError) + L"）";
                if (createError == ERROR_ALREADY_EXISTS || createError == ERROR_DIRECTORY)
                {
                    error += L"\r\n这个名字不能当目录用：可能占用了 Windows 保留设备名"
                             L"（CON / PRN / AUX / NUL / COM1-9 / LPT1-9），或位置上有同名文件挡着。";
                }
                if (!blocked.empty())
                {
                    error += L"\r\n注意：本次新建的目录没能全部收回：" + blocked;
                }
                else
                {
                    error += L"\r\n\r\n本次没有改动本地库。";
                }
                return false;
            }

            // 建目录后再查一次重解析点（防止检查与创建之间被换成 junction）。
            if (PathSafety::HasReparsePoint(targetDirectory, &offending))
            {
                const std::wstring blocked = RemoveCreatedDirectories(createdDirectories);
                error = L"拒绝安装：目标目录在创建后变成/含有重解析点：" + offending;
                if (!blocked.empty())
                {
                    error += L"\r\n注意：本次新建的目录没能全部收回：" + blocked;
                }
                else
                {
                    error += L"\r\n\r\n本次没有改动本地库。";
                }
                return false;
            }

            // ---------- 3) 同卷唯一暂存（rules / info / manifest） ----------
            pending.resize(3u);

            if (!Common::AtomicFile::StageBytes(installedArchive, blob,
                                                pending[0], error))
            {
                discardAll();
                const std::wstring blocked = RemoveCreatedDirectories(createdDirectories);
                error = L"暂存 rules.int 失败：" + error;
                error += blocked.empty() ? L"\r\n\r\n本次没有改动本地库。"
                                         : (L"\r\n注意：本次新建的目录没能全部收回：" + blocked);
                return false;
            }
            if (!Common::AtomicFile::StageBytes(installedInfo, infoBytes,
                                                pending[1], error))
            {
                discardAll();
                const std::wstring blocked = RemoveCreatedDirectories(createdDirectories);
                error = L"暂存 info.txt 失败：" + error;
                error += blocked.empty() ? L"\r\n\r\n本次没有改动本地库。"
                                         : (L"\r\n注意：本次新建的目录没能全部收回：" + blocked);
                return false;
            }
            if (!Common::AtomicFile::StageBytes(manifestPath, manifestBytes,
                                                pending[2], error))
            {
                discardAll();
                const std::wstring blocked = RemoveCreatedDirectories(createdDirectories);
                error = L"暂存库清单失败：" + error;
                error += blocked.empty() ? L"\r\n\r\n本次没有改动本地库。"
                                         : (L"\r\n注意：本次新建的目录没能全部收回：" + blocked);
                return false;
            }

            if (cancellationRequested())
            {
                return abortCancelled(0u);
            }

            // ---------- 4) 依次提交；任一步失败按相反顺序回滚（**含失败的那一项**） ----------
            for (size_t i = 0u; i < pending.size(); ++i)
            {
                if (cancellationRequested())
                {
                    return abortCancelled(i);
                }
                std::wstring commitError;
                commitError.reserve(1024u);
                if (!Common::AtomicFile::Commit(pending[i], commitError))
                {
                    std::wstring rollbackReport;
                    rollbackReport.reserve(2048u);
                    const bool rollbackOk = rollbackPrefix(i + 1u, rollbackReport);
                    discardAll();
                    const std::wstring blocked = RemoveCreatedDirectories(createdDirectories);
                    error = L"提交库文件失败（" + std::wstring(i == 0u ? L"rules.int"
                                  : (i == 1u ? L"info.txt" : L"manifest.txt"))
                          + L"）：" + commitError;
                    if (rollbackOk && rollbackReport.empty())
                    {
                        error += L"\r\n已回滚本次已提交的文件（含失败项的可能半状态）。";
                        error += blocked.empty()
                            ? L"\r\n\r\n本次没有改动本地库。"
                            : (L"\r\n注意：本次新建的目录没能全部收回：" + blocked);
                    }
                    else
                    {
                        error += L"\r\n回滚没有全部成功，旧数据仍在备份里（**请勿删除**）：\r\n"
                               + rollbackReport;
                        if (!blocked.empty())
                        {
                            error += L"注意：本次新建的目录没能全部收回：" + blocked + L"\r\n";
                        }
                    }
                    return false;
                }
            }

            // ---------- 5) 成功收尾：只删本次自己的 backup / 残留 temp ----------
            // 到这里三个目标文件都已 Commit 成功。之后**任何**清理异常都只做 best-effort，
            // 绝不回滚（backup 可能已被删，再回滚会误伤/误报）。
            transactionCommitted = true;
            try
            {
                std::wstring cleanupNotes;
                bool cleanupException = false;
                for (Common::AtomicFile::StagedFile& item : pending)
                {
                    try
                    {
                        std::wstring finalizeError;
                        if (!Common::AtomicFile::Finalize(item, finalizeError))
                        {
                            try
                            {
                                cleanupNotes += finalizeError;
                            }
                            catch (...)
                            {
                            }
                        }
                    }
                    catch (...)
                    {
                        // 保留该 item 的 backup/temp 路径；数据已成功提交。
                        cleanupException = true;
                    }
                }
                try
                {
                    if (cleanupException)
                    {
                        error = L"扩展集已安装成功，但收尾清理时发生异常；"
                                L"如目录中出现 .yua-bak / .yua-tmp，可手动删除。\r\n";
                    }
                    if (!cleanupNotes.empty())
                    {
                        error += cleanupNotes;
                    }
                }
                catch (...)
                {
                }
            }
            catch (...)
            {
                try
                {
                    error = L"扩展集已安装成功，但清理 backup/temp 时发生异常；"
                            L"如目录中出现 .yua-bak / .yua-tmp，可手动删除。";
                }
                catch (...)
                {
                }
            }
            return true;
            }
            catch (const std::bad_alloc&)
            {
                if (transactionCommitted)
                {
                    try
                    {
                        error = L"扩展集已安装成功；内存不足发生在收尾清理阶段，"
                                L"如目录中出现 .yua-bak / .yua-tmp，可手动删除。";
                    }
                    catch (...)
                    {
                    }
                    return true;
                }
                std::wstring rollbackReport;
                const bool rollbackOk = recoverAfterException(rollbackReport);
                try
                {
                    discardAll();
                }
                catch (...)
                {
                }
                std::wstring blocked;
                try
                {
                    blocked = RemoveCreatedDirectories(createdDirectories);
                }
                catch (...)
                {
                    blocked = L"（收目录时再次异常）";
                }
                try
                {
                    error = L"内存不足：安装扩展集被中止。";
                    if (rollbackOk && rollbackReport.empty() && blocked.empty())
                    {
                        error += L"\r\n已回滚本次事务，本次没有改动本地库。";
                    }
                    else
                    {
                        error += L"\r\n安装事务未能完整回滚，本地库状态可能不一致；"
                                 L"旧数据（如有）仍在 .yua-bak 备份里，**请勿删除**：\r\n"
                               + rollbackReport;
                        if (!blocked.empty())
                        {
                            error += L"本次新建目录未能全部收回：" + blocked + L"\r\n";
                        }
                    }
                }
                catch (...)
                {
                    try
                    {
                        error = L"内存不足：安装被中止；回滚报告构造失败，"
                                L"请保留 .yua-bak / .yua-tmp 并检查本地库。";
                    }
                    catch (...)
                    {
                    }
                }
                return false;
            }
            catch (const std::length_error&)
            {
                if (transactionCommitted)
                {
                    try
                    {
                        error = L"扩展集已安装成功；长度异常发生在收尾清理阶段，"
                                L"如目录中出现 .yua-bak / .yua-tmp，可手动删除。";
                    }
                    catch (...)
                    {
                    }
                    return true;
                }
                std::wstring rollbackReport;
                const bool rollbackOk = recoverAfterException(rollbackReport);
                try
                {
                    discardAll();
                }
                catch (...)
                {
                }
                std::wstring blocked;
                try
                {
                    blocked = RemoveCreatedDirectories(createdDirectories);
                }
                catch (...)
                {
                    blocked = L"（收目录时再次异常）";
                }
                try
                {
                    error = L"扩展集内容超过可处理长度，安装被中止。";
                    if (rollbackOk && rollbackReport.empty() && blocked.empty())
                    {
                        error += L"\r\n已回滚本次事务，本次没有改动本地库。";
                    }
                    else
                    {
                        error += L"\r\n安装事务未能完整回滚，本地库状态可能不一致；"
                                 L"旧数据（如有）仍在 .yua-bak 备份里，**请勿删除**：\r\n"
                               + rollbackReport;
                        if (!blocked.empty())
                        {
                            error += L"本次新建目录未能全部收回：" + blocked + L"\r\n";
                        }
                    }
                }
                catch (...)
                {
                    try
                    {
                        error = L"长度异常：安装被中止；回滚报告构造失败，"
                                L"请保留 .yua-bak / .yua-tmp 并检查本地库。";
                    }
                    catch (...)
                    {
                    }
                }
                return false;
            }
        }
        // =====================================================================
        // 默认路径
        // =====================================================================

        std::wstring DefaultDraftDirectory(const std::wstring& workspaceRoot)
        {
            return workspaceRoot.empty() ? std::wstring(kDraftFolderName)
                                         : CombinePath(workspaceRoot, kDraftFolderName);
        }

        std::wstring DefaultArchivePath(const std::wstring& workspaceRoot)
        {
            return CombinePath(DefaultDraftDirectory(workspaceRoot), kArchiveFileName);
        }

        std::wstring DefaultLibraryRoot(const std::wstring& toolDirectory)
        {
            return toolDirectory.empty() ? std::wstring(kLibraryFolderName)
                                         : CombinePath(toolDirectory, kLibraryFolderName);
        }

        std::wstring DefaultStaticHashDirectory(const std::wstring& workspaceRoot)
        {
            return workspaceRoot.empty() ? std::wstring(kStaticHashFolderName)
                                         : CombinePath(workspaceRoot, kStaticHashFolderName);
        }

        std::wstring DefaultStaticKeyDirectory(const std::wstring& workspaceRoot)
        {
            if (workspaceRoot.empty())
            {
                return std::wstring(L"ExtractKey_Output\\Static");
            }
            return CombinePath(CombinePath(workspaceRoot, L"ExtractKey_Output"), L"Static");
        }

        // =====================================================================
        // 报告文本
        // =====================================================================

        std::wstring DescribeSalt(const SaltResult& salt)
        {
            if (!salt.available)
            {
                return L"盐：不可用 —— " + salt.message;
            }
            std::wstring text = L"盐：";
            text += salt.salt.empty() ? std::wstring(L"（空串，合法值）") : salt.salt;
            text += L"　来源：" + SaltOriginText(salt.origin);
            if (!salt.source.empty())
            {
                text += L"（" + salt.source + L"）";
            }
            return text;
        }

        std::wstring DescribeGenerateResult(const GenerateResult& result)
        {
            if (!result.ok)
            {
                return L"生成扩展集失败：" + result.error;
            }

            std::wstring text;
            text += L"扩展集生成完成：" + result.archive.meta.publisher + L" / "
                  + result.archive.meta.title + L"\r\n";
            text += L"各来源条数：目录日志 " + FormatUInt(result.sources.directoryLog)
                  + L" / 文件日志 " + FormatUInt(result.sources.fileNameLog)
                  + L" / 挖掘成果 " + FormatUInt(result.sources.minedNames)
                  + L" / 撞库成果 " + FormatUInt(result.sources.recoveredNames)
                  + L" / 其它 lst " + FormatUInt(result.sources.otherLists) + L"\r\n";
            text += L"去重后条数：目录 " + FormatUInt(result.archive.stats.directories)
                  + L" / 文件 " + FormatUInt(result.archive.stats.files) + L"\r\n";
            text += L"自校验例外：" + FormatUInt(result.exceptionCount)
                  + L"（重算哈希与采集到的不一致，已进 EXCEPTIONS）\r\n";
            text += L"去重丢弃：" + FormatUInt(result.duplicateEntries) + L"\r\n";
            text += DescribeSalt(result.salt) + L"\r\n";
            if (!result.archivePath.empty())
            {
                text += L"档案：" + result.archivePath + L"（"
                      + FormatUInt(result.archiveBytes) + L" 字节，sha256 "
                      + result.archiveSha256.substr(0u, 16u) + L"…）\r\n";
            }
            text += L"耗时：" + FormatUInt(result.elapsedMillis) + L" ms";
            for (const std::wstring& warning : result.warnings)
            {
                text += L"\r\n警告：" + warning;
            }
            return text;
        }

        std::wstring DescribeLoadResult(const LoadResult& result)
        {
            if (!result.ok)
            {
                // 失败性质写在最前面，UI 与日志一眼能分清"档案坏了"和"这是别的游戏的档案"（P-3）。
                return L"扩展集加载被拒绝（" + LoadFailureText(result.failure) + L"）：" + result.error;
            }
            std::wstring text = L"扩展集加载成功：" + result.archive.meta.publisher + L" / "
                              + result.archive.meta.title + L"　"
                              + IdentityMatchText(result.match);
            if (!result.matchNote.empty())
            {
                text += L"\r\n" + result.matchNote;
            }
            text += L"\r\n目录条数 " + FormatUInt(result.archive.stats.directories)
                  + L" / 文件条数 " + FormatUInt(result.archive.stats.files)
                  + L" / 例外 " + FormatUInt(result.archive.stats.exceptions);
            return text;
        }

        std::wstring DescribeDryRunResult(const DryRunResult& result, const LoadResult* load)
        {
            if (!result.ok)
            {
                return L"干跑失败：" + result.error;
            }
            std::wstring text;
            if (load != nullptr)
            {
                text += DescribeLoadResult(*load) + L"\r\n";
            }
            text += L"测试来源目录文件总数 " + FormatUInt(result.totalFiles)
                  + L"（哈希名 " + FormatUInt(result.hashNamedFiles)
                  + L" / 非哈希名 " + FormatUInt(result.passthroughFiles) + L"）\r\n";
            text += L"可解目录 " + FormatUInt(result.resolvedDirectories)
                  + L" / 可解文件 " + FormatUInt(result.resolvedFiles)
                  + L" / 整路径可还原 " + FormatUInt(result.fullyResolvedFiles) + L"\r\n";
            text += L"缺目录 Hash " + FormatUInt(result.missingDirectory)
                  + L" / 缺文件 Hash " + FormatUInt(result.missingFileName) + L"\r\n";
            if (result.skippedReparsePoints > 0u)
            {
                text += L"扫描时跳过重解析点（junction / symlink，不跟随）"
                      + FormatUInt(result.skippedReparsePoints) + L" 个\r\n";
            }
            text += L"成功率 " + FormatUInt(result.successRate) + L"%（"
                  + FormatUInt(result.fullyResolvedFiles) + L" / "
                  + FormatUInt(result.hashNamedFiles) + L"）";
            if (!result.misses.empty())
            {
                text += L"\r\n未命中明细 " + FormatUInt(result.misses.size()) + L" 条（前 20 条）：";
                const size_t limit = std::min<size_t>(result.misses.size(), 20u);
                for (size_t index = 0u; index < limit; ++index)
                {
                    text += L"\r\n  " + result.misses[index].relativePath;
                }
            }
            return text;
        }
    }
}
