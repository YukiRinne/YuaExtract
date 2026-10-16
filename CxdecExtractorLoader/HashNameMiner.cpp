#include "HashNameMiner.h"

#include "Blake2s.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

// 「补充还原」的实现：从游戏自己的资源里挖候选名，用哈希验证后产出"哈希 -> 真名"。
// 见 HashNameMiner.h 里对"为什么它写不错名字"的说明。
//
// 这份实现对照的参考版是 _verify/watch/recover_from_scripts.py（Python，已用真实数据
// 验过 52,906 命中），口径必须一致：同样的候选字符集、同样的长度上限、同样的扩展名表、
// 同样"命中后不 break"。差异会导致命中数对不上。

namespace
{
    constexpr size_t kMaxScannedFileBytes = 32u * 1024u * 1024u;   // 单文件超过就不扫
    constexpr size_t kMaxCandidateLength = 48u;                    // 名字长度上限
    constexpr size_t kMaxRunLength = 200u;                         // 先抓整段再按长度过滤

    // 引擎会拼的扩展名（取自已还原名字里实际出现过的）
    const wchar_t* const kExtensions[] = {
        L".ogg", L".opus", L".png", L".tlg", L".pimg", L".pbd", L".jpg", L".asd",
        L".stand", L".mchx", L".wav", L".mp3", L".tjs", L".ini", L".csv", L".tft",
        L".ks", L".scn", L".mtn", L".toml", L".txt", L".dat", L".psb", L".json"
    };

    // 音频类名字后面还可能跟一个 .sli（循环信息 sidecar）
    const wchar_t* const kSidecarExtensions[] = { L".opus", L".ogg", L".mchx", L".wav", L".mp3" };

    bool IsAudioExtension(const std::wstring& extension)
    {
        for (const wchar_t* item : kSidecarExtensions)
        {
            if (extension == item)
            {
                return true;
            }
        }
        return false;
    }

    bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right)
    {
        if (left.size() != right.size())
        {
            return false;
        }
        for (size_t i = 0; i < left.size(); ++i)
        {
            if (::towlower(left[i]) != ::towlower(right[i]))
            {
                return false;
            }
        }
        return true;
    }

    // 候选字符集：ASCII 字母数字下划线 + 日文假名 + CJK + 全角区 + '.' '-'
    // 刻意贴近 Python 参考实现里 \w 的行为（那个 \w 在 Unicode 模式下也吃日文），
    // 否则挖出的候选会比参考版少。
    bool IsCandidateChar(wchar_t value)
    {
        if (value < 0x80)
        {
            return (value >= L'0' && value <= L'9')
                || (value >= L'A' && value <= L'Z')
                || (value >= L'a' && value <= L'z')
                || value == L'_' || value == L'.' || value == L'-';
        }

        const unsigned int code = static_cast<unsigned int>(value);
        return (code >= 0x3040u && code <= 0x30FFu)      // 平假名 / 片假名
            || (code >= 0x4E00u && code <= 0x9FFFu)      // CJK 统一汉字
            || (code >= 0x3400u && code <= 0x4DBFu)      // CJK 扩展 A
            || (code >= 0xFF00u && code <= 0xFFEFu)      // 全角/半角形式
            || (code >= 0x3005u && code <= 0x3007u);     // 々〆〇
    }

    std::string ReadWholeFile(const std::wstring& path, bool& ok)
    {
        ok = false;
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return std::string();
        }

        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0
            || static_cast<unsigned long long>(size.QuadPart) > kMaxScannedFileBytes)
        {
            ::CloseHandle(file);
            return std::string();
        }

        std::string content;
        content.resize(static_cast<size_t>(size.QuadPart));
        size_t offset = 0;
        while (offset < content.size())
        {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(content.size() - offset, 1u << 20));
            DWORD read = 0;
            if (!::ReadFile(file, &content[offset], chunk, &read, nullptr) || read == 0)
            {
                break;
            }
            offset += read;
        }
        ::CloseHandle(file);
        content.resize(offset);
        ok = true;
        return content;
    }

    // UTF-8 -> UTF-16，非法字节**丢掉后继续**（对齐 Python 的 errors='ignore'：
    // 那边丢字节不会切断字符串，所以这里也不能把非法字节当分隔符，否则候选会变少）。
    void DecodeUtf8(const std::string& bytes, std::wstring& out)
    {
        out.clear();
        out.reserve(bytes.size());

        const unsigned char* cursor = reinterpret_cast<const unsigned char*>(bytes.data());
        const unsigned char* end = cursor + bytes.size();

        while (cursor < end)
        {
            const unsigned char lead = *cursor;
            size_t length = 0;
            unsigned int code = 0;

            if (lead < 0x80u)
            {
                length = 1;
                code = lead;
            }
            else if ((lead & 0xE0u) == 0xC0u) { length = 2; code = lead & 0x1Fu; }
            else if ((lead & 0xF0u) == 0xE0u) { length = 3; code = lead & 0x0Fu; }
            else if ((lead & 0xF8u) == 0xF0u) { length = 4; code = lead & 0x07u; }
            else { ++cursor; continue; }   // 非法起始字节：丢掉

            if (cursor + length > end)
            {
                break;
            }

            bool valid = true;
            for (size_t i = 1; i < length; ++i)
            {
                if ((cursor[i] & 0xC0u) != 0x80u)
                {
                    valid = false;
                    break;
                }
                code = (code << 6) | (cursor[i] & 0x3Fu);
            }
            if (valid)
            {
                // 严格校验：overlong 编码、代理区码位、超出 U+10FFFF 一律判非法。
                // 这一步不能省 —— 二进制文件里有大量 0xC0/0xC1 后跟续字节的 overlong 序列，
                // 放过它们等于把噪声"解码"成合法 ASCII 字符，候选集会瞬间涨到几百万，
                // 把真正的脚本名挤掉（第一版就是这么失败的：扫了 82 个文件候选就顶到 400 万）。
                if ((length == 2 && code < 0x80u)
                    || (length == 3 && code < 0x800u)
                    || (length == 4 && code < 0x10000u)
                    || code > 0x10FFFFu
                    || (code >= 0xD800u && code <= 0xDFFFu))
                {
                    valid = false;
                }
            }

            if (!valid)
            {
                ++cursor;
                continue;
            }

            cursor += length;

            if (code <= 0xFFFFu)
            {
                out.push_back(static_cast<wchar_t>(code));
            }
            else if (code <= 0x10FFFFu)
            {
                // 代理对
                const unsigned int adjusted = code - 0x10000u;
                out.push_back(static_cast<wchar_t>(0xD800u + (adjusted >> 10)));
                out.push_back(static_cast<wchar_t>(0xDC00u + (adjusted & 0x3FFu)));
            }
            // 超出范围的码位直接丢掉
        }
    }

    void ExtractCandidates(const std::wstring& text, std::set<std::wstring>& out, size_t limit, bool& limitHit)
    {
        std::wstring run;
        run.reserve(kMaxRunLength);

        auto flush = [&]()
        {
            // 去掉首尾的 '.' 与 '-'（和参考实现一致）
            size_t begin = 0;
            size_t end = run.size();
            while (begin < end && (run[begin] == L'.' || run[begin] == L'-')) { ++begin; }
            while (end > begin && (run[end - 1] == L'.' || run[end - 1] == L'-')) { --end; }

            const size_t length = end - begin;
            if (length < 2u || length > kMaxCandidateLength || out.size() >= limit)
            {
                run.clear();
                return;
            }

            // 高精度门槛：必须含"结构字符"（数字 / 下划线 / 点 / 横线）。
            // 依据是用已知真名反推出来的：把游戏收集到的映射与离线对照表合起来取裸名，
            // 63359 个真名里 99.5% 都含结构字符，只有 313 个不含。
            // 而脚本正文里的日文对话碎片几乎不含这些字符 —— 这一条能把候选从几百万
            // 压到可处理的量级，同时几乎不丢真名。
            bool structural = false;
            for (size_t i = begin; i < end; ++i)
            {
                const wchar_t value = run[i];
                if (value == L'_' || value == L'.' || value == L'-'
                    || (value >= L'0' && value <= L'9'))
                {
                    structural = true;
                    break;
                }
            }

            if (structural)
            {
                out.insert(run.substr(begin, length));
            }
            run.clear();
        };

        for (wchar_t value : text)
        {
            if (IsCandidateChar(value))
            {
                run.push_back(value);
                if (run.size() > kMaxRunLength)
                {
                    // 超长段直接丢掉（参考实现也是先抓长段、再按长度过滤掉）
                    run.clear();
                }
            }
            else
            {
                if (!run.empty())
                {
                    flush();
                    if (out.size() >= limit)
                    {
                        limitHit = true;
                        return;
                    }
                }
            }
        }
        if (!run.empty())
        {
            flush();
        }
        if (out.size() >= limit)
        {
            limitHit = true;
        }
    }

    // 递归收集文件：跳过 excludeNames 里的目录名
    void CollectFiles(const std::wstring& directory, const std::vector<std::wstring>& excludeNames,
                      std::vector<std::wstring>& files)
    {
        WIN32_FIND_DATAW data{};
        const std::wstring pattern = directory + L"\\*";
        HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            return;
        }

        do
        {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }

            const std::wstring full = directory + L"\\" + name;
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                bool excluded = false;
                for (const std::wstring& item : excludeNames)
                {
                    if (EqualsIgnoreCase(item, name))
                    {
                        excluded = true;
                        break;
                    }
                }
                if (!excluded)
                {
                    CollectFiles(full, excludeNames, files);
                }
            }
            else
            {
                files.push_back(full);
            }
        } while (::FindNextFileW(find, &data));

        ::FindClose(find);
    }

    // 文件名哈希：BLAKE2s-256(key = FileNameHasherSalt, message = UTF16LE(名字 + seed))
    std::string ComputeFileHash(const std::string& keyBytes, const std::wstring& name,
                                const std::wstring& seed)
    {
        std::wstring message = name;
        message += seed;

        return Yua::Crypto::Blake2s256Hex(
            keyBytes.empty() ? nullptr : keyBytes.data(), keyBytes.size(),
            message.data(), message.size() * sizeof(wchar_t));
    }

    std::wstring Utf8ToWide(const std::string& bytes)
    {
        std::wstring text;
        DecodeUtf8(bytes, text);
        return text;
    }

    // ---- 文本判定 ----
    //
    // 这一步是必须的，不是优化：PNG/OGG 这类二进制被"解码"后会产出海量垃圾候选
    // （实测一张 2.6 MB 的 PNG 能产出 11 万个去重候选，而一个 4.4 MB 的脚本只有 4 千个），
    // 候选上限会被图片包瞬间打满，真正的脚本名反而扫不到。
    //
    // 判据用"有效字节占比"而不是"能不能解码"：真脚本里也混着二进制段（实测存活率 92%），
    // 而图片只有 53%，用 80% 当门槛能把两者分开。

    constexpr size_t kSampleBytes = 8192u;

    bool IsUtf16Text(const std::string& raw, bool& littleEndian)
    {
        if (raw.size() >= 2u)
        {
            const unsigned char first = static_cast<unsigned char>(raw[0]);
            const unsigned char second = static_cast<unsigned char>(raw[1]);
            if (first == 0xFFu && second == 0xFEu) { littleEndian = true; return true; }
            if (first == 0xFEu && second == 0xFFu) { littleEndian = false; return true; }
        }

        const size_t sample = std::min(raw.size(), kSampleBytes);
        if (sample < 8u)
        {
            return false;
        }
        size_t zeros = 0;
        for (size_t i = 0; i < sample; ++i)
        {
            if (raw[i] == '\0')
            {
                ++zeros;
            }
        }
        // 文本里的 ASCII 若按 UTF-16 存，NUL 会占到接近一半
        if (zeros * 100u / sample < 25u)
        {
            return false;
        }

        size_t zerosAtEven = 0;
        size_t zerosAtOdd = 0;
        for (size_t i = 0; i + 1 < sample; i += 2)
        {
            if (raw[i] == '\0') { ++zerosAtEven; }
            if (raw[i + 1] == '\0') { ++zerosAtOdd; }
        }
        littleEndian = zerosAtOdd >= zerosAtEven;   // 小端：高字节在前的是 NUL
        return true;
    }

    void DecodeUtf16(const std::string& raw, bool littleEndian, std::wstring& out)
    {
        out.clear();
        if (raw.size() < 2u)
        {
            return;
        }
        out.reserve(raw.size() / 2u);

        size_t offset = 0;
        // 跳过 BOM
        const unsigned char first = static_cast<unsigned char>(raw[0]);
        const unsigned char second = static_cast<unsigned char>(raw[1]);
        if ((first == 0xFFu && second == 0xFEu) || (first == 0xFEu && second == 0xFFu))
        {
            offset = 2u;
        }

        for (; offset + 1u < raw.size(); offset += 2u)
        {
            const unsigned char low = static_cast<unsigned char>(raw[offset]);
            const unsigned char high = static_cast<unsigned char>(raw[offset + 1u]);
            const unsigned int unit = littleEndian
                ? (static_cast<unsigned int>(low) | (static_cast<unsigned int>(high) << 8))
                : (static_cast<unsigned int>(high) | (static_cast<unsigned int>(low) << 8));
            out.push_back(static_cast<wchar_t>(unit));
        }
    }

    // 是不是"值得挖"的文本文件。
    //
    // 之前用"合法 UTF-8 序列的字节占比"当判据是错的：二进制里 0x80-0xBF 这种续字节极多，
    // 占比反而高（会被误判成文本）；而 PSB 脚本头部是 "PSB\0" 的二进制结构（会被误拒）。
    // 改成**文件头魔数黑名单**：图片/音频/压缩包一眼认出直接跳过，其余（PSB / tjs / csv / ini…）
    // 都当文本处理。资源名字面量恰恰就藏在这些"容器"里，不能因为开头是二进制就放弃。
    bool HasBinaryMagic(const std::string& raw)
    {
        auto startsWith = [&](const char* magic, size_t length) -> bool
        {
            return raw.size() >= length && std::memcmp(raw.data(), magic, length) == 0;
        };

        // 图片
        if (startsWith("\x89PNG", 4)) return true;                       // PNG
        if (startsWith("\xFF\xD8\xFF", 3)) return true;                  // JPEG
        if (startsWith("GIF8", 4)) return true;                          // GIF
        if (startsWith("BM", 2)) return true;                            // BMP
        if (startsWith("TLG", 3)) return true;                           // KiriKiri 的 TLG 图
        // 音频
        if (startsWith("OggS", 4)) return true;                          // OGG/Opus
        if (startsWith("RIFF", 4)) return true;                          // WAV
        if (startsWith("fLaC", 4)) return true;                          // FLAC
        if (startsWith("ID3", 3)) return true;                           // MP3
        if (startsWith("MThd", 4)) return true;                          // MIDI
        // 压缩 / 归档
        if (startsWith("PK\x03\x04", 4)) return true;                    // ZIP
        if (startsWith("7z\xBC\xAF", 4)) return true;                    // 7z
        return false;
    }

    bool LooksLikeText(const std::string& raw, bool& utf16, bool& littleEndian)
    {
        utf16 = false;
        littleEndian = true;

        if (HasBinaryMagic(raw))
        {
            return false;
        }

        if (IsUtf16Text(raw, littleEndian))
        {
            utf16 = true;
            return true;
        }

        // 没有图片/音频/压缩魔数、也不是 UTF-16：一律按 UTF-8 文本挖。
        // 哪怕里面有二进制段（PSB 就是），DecodeUtf8 也会把非法字节丢掉，剩下的文本照挖。
        return true;
    }
}

namespace Yua
{
    namespace Miner
    {
        SaltInfo ReadSalt(const std::wstring& logDirectory)
        {
            SaltInfo info;

            std::wstring path = logDirectory;
            if (!path.empty() && path.back() != L'\\' && path.back() != L'/')
            {
                path += L'\\';
            }
            path += L"Universal.log";
            info.source = path;

            bool ok = false;
            const std::string raw = ReadWholeFile(path, ok);
            if (!ok)
            {
                info.message = L"没有读到 " + path
                             + L"（该文件由步骤2 的收集模块写出）。请先执行一次步骤2 再试推理补充还原。";
                return info;
            }

            const std::wstring text = Utf8ToWide(raw);

            bool hasSeed = false;
            size_t lineStart = 0;
            while (lineStart <= text.size())
            {
                size_t lineEnd = text.find(L'\n', lineStart);
                if (lineEnd == std::wstring::npos)
                {
                    lineEnd = text.size();
                }
                std::wstring line = text.substr(lineStart, lineEnd - lineStart);
                while (!line.empty() && (line.back() == L'\r' || line.back() == L' '))
                {
                    line.pop_back();
                }

                auto takeValue = [&](const wchar_t* key, std::wstring& target) -> bool
                {
                    const size_t position = line.find(key);
                    if (position == std::wstring::npos)
                    {
                        return false;
                    }
                    // 取 key 后面的整段（值可能是空串）
                    target = line.substr(position + std::wcslen(key));
                    while (!target.empty() && (target.front() == L' ' || target.front() == L'\t'))
                    {
                        target.erase(target.begin());
                    }
                    while (!target.empty() && (target.back() == L' ' || target.back() == L'\t'))
                    {
                        target.pop_back();
                    }
                    return true;
                };

                if (takeValue(L"Hash Seed:", info.seed))
                {
                    hasSeed = true;
                }
                else if (takeValue(L"FileNameHasherSalt:", info.fileNameHasherSalt))
                {
                    // 拿到了就行，空值也是有意义的（表示不带密钥）
                }
                else
                {
                    takeValue(L"PathNameHasherSalt:", info.pathNameHasherSalt);
                }

                if (lineEnd == text.size())
                {
                    break;
                }
                lineStart = lineEnd + 1u;
            }

            if (!hasSeed || info.seed.empty())
            {
                info.message = L"读到了 " + path + L"，但里面没有可用的 Hash Seed。";
                return info;
            }

            info.available = true;
            return info;
        }

        Result MineNames(const SaltInfo& salt, const std::set<std::wstring>& targets, const Options& options)
        {
            Result result;

            if (!salt.available)
            {
                result.message = salt.message.empty()
                               ? std::wstring(L"没有可用的 Hash Seed，无法做推理补充还原。")
                               : salt.message;
                return result;
            }

            // 带密钥的哈希：算法口径未经真实数据验证过，宁可明确报不支持，也不静默给错结果。
            if (!salt.fileNameHasherSalt.empty())
            {
                result.message = L"该游戏的 FileNameHasherSalt 非空（带密钥哈希），当前版本的推理补充还原不支持。";
                return result;
            }

            const std::string emptyKey;

            // 1) 扫描并抓候选
            std::set<std::wstring> candidates;
            for (const std::wstring& root : options.scanRoots)
            {
                if (options.cancelRequested && options.cancelRequested())
                {
                    result.message = L"用户中止";
                    return result;
                }

                std::vector<std::wstring> files;
                CollectFiles(root, options.excludeNames, files);

                for (const std::wstring& file : files)
                {
                    if (options.cancelRequested && options.cancelRequested())
                    {
                        result.message = L"用户中止";
                        return result;
                    }

                    bool ok = false;
                    const std::string raw = ReadWholeFile(file, ok);
                    if (!ok)
                    {
                        continue;
                    }

                    bool utf16 = false;
                    bool littleEndian = true;
                    if (!LooksLikeText(raw, utf16, littleEndian))
                    {
                        // 二进制文件（图片/音频/打包数据）：里面没有资源名字面量，
                        // 硬解码只会灌进海量垃圾候选，把候选上限吃光。
                        continue;
                    }

                    ++result.scannedFiles;

                    std::wstring text;
                    if (utf16)
                    {
                        DecodeUtf16(raw, littleEndian, text);
                    }
                    else
                    {
                        DecodeUtf8(raw, text);
                    }
                    ExtractCandidates(text, candidates, options.maxCandidates, result.limitHit);

                    if (options.progress && (result.scannedFiles % 64u) == 0u)
                    {
                        options.progress(result.scannedFiles, static_cast<unsigned int>(candidates.size()),
                                         static_cast<unsigned int>(result.files.size()));
                    }

                    if (result.limitHit)
                    {
                        break;
                    }
                }

                if (result.limitHit)
                {
                    break;
                }
            }

            result.candidates = static_cast<unsigned int>(candidates.size());

            // 2) 逐候选对撞。命中后**不 break** —— 同一个裸名可能同时存在 .ogg 和 .ogg.sli。
            //
            // 对撞阶段可能要几分钟（数百万候选 × 几十个变体 ≈ 上亿次哈希），期间扫描数
            // 和候选数都不再变化。必须定期报进度，否则界面在整段对撞时间里"命中 0"
            // 纹丝不动 —— 用户没法区分"正在算"和"已经死了"（实测被当成故障）。
            unsigned long long collisionTick = 0u;
            for (const std::wstring& candidate : candidates)
            {
                if (options.cancelRequested && options.cancelRequested())
                {
                    result.message = L"用户中止";
                    return result;
                }

                // 对撞进度：files/candidates 已定，命中数随对撞推进增长。
                if (options.progress && (++collisionTick % 65536ull) == 0ull)
                {
                    options.progress(result.scannedFiles, result.candidates,
                                     static_cast<unsigned int>(result.files.size()));
                }

                // 候选本身（脚本里可能已经写全了带扩展名的名字）
                {
                    const std::string hash = ComputeFileHash(emptyKey, candidate, salt.seed);
                    ++result.triedHashes;
                    if (targets.find(Utf8ToWide(hash)) != targets.end())
                    {
                        result.files[Utf8ToWide(hash)] = candidate;
                    }
                }

                for (const wchar_t* extension : kExtensions)
                {
                    std::wstring name = candidate + extension;
                    std::string hash = ComputeFileHash(emptyKey, name, salt.seed);
                    ++result.triedHashes;
                    if (targets.find(Utf8ToWide(hash)) != targets.end())
                    {
                        result.files[Utf8ToWide(hash)] = name;
                    }

                    const std::wstring ext(extension);
                    if (IsAudioExtension(ext))
                    {
                        name += L".sli";
                        hash = ComputeFileHash(emptyKey, name, salt.seed);
                        ++result.triedHashes;
                        if (targets.find(Utf8ToWide(hash)) != targets.end())
                        {
                            result.files[Utf8ToWide(hash)] = name;
                        }
                    }
                }
            }

            if (options.progress)
            {
                options.progress(result.scannedFiles, result.candidates,
                                 static_cast<unsigned int>(result.files.size()));
            }

            if (result.limitHit)
            {
                result.message = L"候选数达到上限，结果不完整。";
            }
            return result;
        }
    }
}
