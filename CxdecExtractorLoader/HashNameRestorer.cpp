#include "HashNameRestorer.h"

#include "HashNameMiner.h"
#include "YuaExtractWorkspace.h"
#include "logdir.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <algorithm>
#include <cwctype>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace Yua
{
namespace
{
    constexpr const wchar_t* kHashMapSeparator = L"##YSig##";
    constexpr const wchar_t* kRecoveredListFileName = L"HashRestore_RecoveredNames.lst";
    constexpr const wchar_t* kRootDirectoryName = L"%EmptyString%";
    constexpr const wchar_t* kProcessLogFileName = L"YuaExtractRestore.log";
    // 补充还原挖到的名字落这里。放在映射目录下，是因为该目录下所有 *.lst 是既有契约：
    // 下次步骤3 载入映射时会自动读到它，不必为了同一份结果重复挖一遍。
    constexpr const wchar_t* kMinedNamesFileName = L"MinedNames.lst";

    // ---------- 通用小工具 ----------

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

    std::wstring ToUpperString(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(::towupper(ch)); });
        return value;
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

    bool DirectoryExists(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    // FormatCount / FormatSize / TimestampNow 在 YuaExtractWorkspace.h 里共享，
    // 界面和报告必须用同一套口径，别再各写一份。

    // 还原过程日志。内核不写自己的日志文件就没法事后查"哪一条为什么没还原"；
    // 用 CRITICAL_SECTION 而不是文件锁，是因为同一进程里多个工作线程都会写。
    CRITICAL_SECTION& ProcessLogLock()
    {
        static CRITICAL_SECTION lock;
        static bool ready = false;
        // 静态初始化在首次调用时完成；C++11 起函数内静态变量初始化是线程安全的，
        // 但 CRITICAL_SECTION 得靠 InitializeCriticalSection 手动建，所以放在这里。
        if (!ready)
        {
            ::InitializeCriticalSection(&lock);
            ready = true;
        }
        return lock;
    }

    void AppendProcessLog(const std::wstring& logDirectory, const std::wstring& line)
    {
        if (logDirectory.empty())
        {
            return;
        }

        DWORD error = 0;
        if (!Log::DirectoryDetail::EnsureDirectory(logDirectory, &error))
        {
            return;
        }

        std::wstring payload = TimestampNow() + L" | " + line + L"\r\n";
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, payload.c_str(), static_cast<int>(payload.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (need <= 0)
        {
            return;
        }
        std::string utf8(static_cast<size_t>(need), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, payload.c_str(), static_cast<int>(payload.size()), &utf8[0], need,
                              nullptr, nullptr);

        CRITICAL_SECTION& lock = ProcessLogLock();
        ::EnterCriticalSection(&lock);
        HANDLE file = ::CreateFileW(CombinePath(logDirectory, kProcessLogFileName).c_str(),
                                    FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
            ::CloseHandle(file);
        }
        ::LeaveCriticalSection(&lock);
    }

    // ---------- 文本读取 ----------

    // 读整个文件。返回 false 时 error 里是 Win32 错误码。
    bool ReadAllBytes(const std::wstring& path, std::vector<unsigned char>& out, DWORD& error)
    {
        error = 0;
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
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

        // 映射库文件正常在几十 MB 量级；给一个上限，避免误读巨型文件把内存吃光。
        constexpr long long kMaxBytes = 512ll * 1024ll * 1024ll;
        if (size.QuadPart > kMaxBytes)
        {
            error = ERROR_FILE_TOO_LARGE;
            ::CloseHandle(file);
            return false;
        }

        out.assign(static_cast<size_t>(size.QuadPart), 0);
        size_t offset = 0;
        while (offset < out.size())
        {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(out.size() - offset, 1u << 20));
            DWORD read = 0;
            if (!::ReadFile(file, out.data() + offset, chunk, &read, nullptr))
            {
                error = ::GetLastError();
                ::CloseHandle(file);
                return false;
            }
            if (read == 0)
            {
                break;
            }
            offset += read;
        }
        out.resize(offset);
        ::CloseHandle(file);
        return true;
    }

    std::wstring Utf8ToWide(const char* data, size_t size)
    {
        if (size == 0)
        {
            return std::wstring();
        }
        const int need = ::MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size), nullptr, 0);
        if (need <= 0)
        {
            return std::wstring();
        }
        std::wstring wide(static_cast<size_t>(need), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, data, static_cast<int>(size), &wide[0], need);
        return wide;
    }

    // 自动识别 BOM 的文本读取：映射表既有 UTF-16（*.log）也有 UTF-8（*.lst），
    // 手工合并过的对照文件还可能不带 BOM，所以无 BOM 时按字节形态猜一次。
    bool ReadTextFileAuto(const std::wstring& path, std::wstring& out, DWORD& error)
    {
        std::vector<unsigned char> bytes;
        if (!ReadAllBytes(path, bytes, error))
        {
            return false;
        }
        out.clear();
        if (bytes.empty())
        {
            return true;
        }

        size_t offset = 0;
        bool utf16 = false;
        if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE)
        {
            utf16 = true;
            offset = 2;
        }
        else if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        {
            offset = 3;
        }
        else if (bytes.size() >= 2 && bytes[0] != 0 && bytes[1] == 0)
        {
            // 每两个字节的第二个是 0 —— 典型 UTF-16LE 英文/日文文本。
            utf16 = true;
        }

        if (utf16)
        {
            const size_t chars = (bytes.size() - offset) / 2;
            out.assign(reinterpret_cast<const wchar_t*>(bytes.data() + offset), chars);
        }
        else
        {
            out = Utf8ToWide(reinterpret_cast<const char*>(bytes.data() + offset), bytes.size() - offset);
        }
        return true;
    }

    // 逐行切分（同时接受 \r\n 与 \n），跳过空行。
    void ForEachLine(const std::wstring& content, const std::function<void(const std::wstring&)>& visitor)
    {
        size_t start = 0;
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
            while (start < content.length() && (content[start] == L'\r' || content[start] == L'\n'))
            {
                ++start;
            }
        }
    }

    // ---------- 路径规范 ----------

    // 与 HashRestoreUI.cpp 的 NormalizeRelativeDirectory 同口径：
    // %EmptyString% / "/" 都是根域，分隔符统一成反斜杠，去掉结尾分隔符。
    std::wstring NormalizeDirectoryName(const std::wstring& name)
    {
        if (name == kRootDirectoryName || name == L"/")
        {
            return std::wstring();
        }
        std::wstring normalized = name;
        std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
        return TrimTrailingSeparators(normalized);
    }

    std::wstring NormalizeFileName(const std::wstring& name)
    {
        std::wstring normalized = name;
        std::replace(normalized.begin(), normalized.end(), L'/', L'\\');
        return normalized;
    }

    // 把相对路径拆成组件（同时接受 / 与 \）。
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

    std::wstring JoinParts(const std::vector<std::wstring>& parts, size_t begin, size_t end)
    {
        std::wstring joined;
        for (size_t index = begin; index < end; ++index)
        {
            if (parts[index].empty())
            {
                continue;
            }
            if (!joined.empty())
            {
                joined.push_back(L'\\');
            }
            joined += parts[index];
        }
        return joined;
    }

    bool IsSkippedLogComponent(const std::wstring& name)
    {
        return _wcsicmp(name.c_str(), L"ExtractLog") == 0 || _wcsicmp(name.c_str(), L"Extractor_Log") == 0;
    }

    // ---------- 映射表 ----------

    void MergeHashMapLine(HashMapTables& tables, const std::wstring& name, const std::wstring& hash)
    {
        const std::wstring upper = ToUpperString(hash);
        if (IsHexLength(upper, 16u))
        {
            tables.directories.emplace(upper, NormalizeDirectoryName(name));
        }
        else if (IsHexLength(upper, 64u))
        {
            tables.files.emplace(upper, NormalizeFileName(name));
        }
    }

    // 运行时日志：真名##YSig##HASH
    unsigned int LoadSignatureLog(const std::wstring& path, HashMapTables& tables)
    {
        std::wstring content;
        DWORD error = 0;
        if (!ReadTextFileAuto(path, content, error))
        {
            return 0u;
        }

        unsigned int loaded = 0u;
        const std::wstring separator = kHashMapSeparator;
        ForEachLine(content,
                    [&](const std::wstring& line)
                    {
                        const size_t split = line.find(separator);
                        if (split == std::wstring::npos)
                        {
                            return;
                        }
                        const std::wstring name = line.substr(0, split);
                        const std::wstring hash = line.substr(split + separator.length());
                        const size_t before = tables.directories.size() + tables.files.size();
                        MergeHashMapLine(tables, name, hash);
                        if (tables.directories.size() + tables.files.size() != before)
                        {
                            ++loaded;
                        }
                    });
        return loaded;
    }

    // 恢复映射表 / 外部对照文件：HASH:name
    unsigned int LoadRecoveredList(const std::wstring& path, HashMapTables& tables)
    {
        std::wstring content;
        DWORD error = 0;
        if (!ReadTextFileAuto(path, content, error))
        {
            return 0u;
        }

        unsigned int loaded = 0u;
        ForEachLine(content,
                    [&](const std::wstring& line)
                    {
                        const size_t split = line.find(L':');
                        if (split == std::wstring::npos || split == 0)
                        {
                            return;
                        }
                        const std::wstring hash = line.substr(0, split);
                        const std::wstring name = line.substr(split + 1u);
                        if (!IsHexLength(hash, 16u) && !IsHexLength(hash, 64u))
                        {
                            return;
                        }
                        const size_t before = tables.directories.size() + tables.files.size();
                        MergeHashMapLine(tables, name == L"/" ? kRootDirectoryName : name, hash);
                        if (tables.directories.size() + tables.files.size() != before)
                        {
                            ++loaded;
                        }
                    });
        return loaded;
    }

    // 候选表（dirs_*.txt / files_*.txt）是"名字清单"不是映射表，要按文件名排除掉，
    // 否则会把整份候选表当成畸形映射读一遍。
    bool IsCandidateTable(const std::wstring& fileName)
    {
        std::wstring lower = fileName;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });
        return lower.rfind(L"dirs_", 0) == 0 || lower.rfind(L"files_", 0) == 0;
    }

    void CollectHashDirectoryMapFiles(const std::wstring& hashDirectory, std::vector<std::wstring>& logFiles,
                                      std::vector<std::wstring>& listFiles)
    {
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(CombinePath(hashDirectory, L"*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            return;
        }
        do
        {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }
            const std::wstring name = data.cFileName;
            const std::wstring full = CombinePath(hashDirectory, name);
            std::wstring lower = name;
            std::transform(lower.begin(), lower.end(), lower.begin(),
                           [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });

            if (lower == L"directoryhash.log" || lower == L"filenamehash.log")
            {
                logFiles.push_back(full);
            }
            else if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, L".lst") == 0 &&
                     !IsCandidateTable(name))
            {
                // 除了主映射表，这里也收用户手工放进来的对照文件——
                // "每游戏攒一份名字映射"的成果直接丢进这个目录就能被步骤3 用上。
                listFiles.push_back(full);
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);

        std::sort(logFiles.begin(), logFiles.end());
        std::sort(listFiles.begin(), listFiles.end());
    }

    // ---------- 补充还原（候选挖掘）用的小工具 ----------

    // WriteTextFileUtf8Bom 定义在下面的"输出"一节；MinedNames.lst 要用它，
    // 所以这里先声明一下（匿名命名空间内的前置声明，不对外暴露）。
    bool WriteTextFileUtf8Bom(const std::wstring& path, const std::wstring& text, DWORD& error);

    bool SameDirectory(const std::wstring& left, const std::wstring& right)
    {
        if (left.empty() || right.empty())
        {
            return false;
        }
        return _wcsicmp(TrimTrailingSeparators(left).c_str(), TrimTrailingSeparators(right).c_str()) == 0;
    }

    bool IsExcludedPackageName(const std::vector<std::wstring>& excludeNames, const std::wstring& name)
    {
        for (const std::wstring& excluded : excludeNames)
        {
            if (_wcsicmp(excluded.c_str(), name.c_str()) == 0)
            {
                return true;
            }
        }
        return false;
    }

    std::wstring MakeMinedListLine(const std::wstring& hash, const std::wstring& name)
    {
        return hash + L":" + name;
    }

    // 收集"本次要处理的、名字仍是 64 位 Hash"的哈希集合（大写）。
    //
    // 只收叶子本身就是 64 位 hex 的条目 —— 那是"完全没还原"的形态。叶子已经带真名的
    //（明文名躺在 Hash 目录里的残留）不在其列：它的名字已经有了，不需要再挖一次。
    // 跳过的目录与还原口径完全一致（日志目录不算游戏资源）。
    void CollectTargetHashes(const std::wstring& current, std::set<std::wstring>& targets)
    {
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(CombinePath(current, L"*").c_str(), &data);
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
            if (IsSkippedLogComponent(name))
            {
                continue;
            }

            const std::wstring full = CombinePath(current, name);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                CollectTargetHashes(full, targets);
                continue;
            }
            // 归一化大小写：NTFS 默认不区分大小写，同一个文件可能以不同大小写出现，
            // 不折叠的话集合里会出现重复项（大小写不同的同一个哈希）。
            const std::wstring upper = ToUpperString(name);
            if (IsHexLength(upper, 64u))
            {
                targets.insert(upper);
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
    }

    // 把挖到的名字并进映射表。**只接受内核返回的 files**：这里不做任何"猜名字"的事，
    // 每一条都已经在核心里过了一次哈希相等。表里已有的条目一律保留原值
    //（emplace 不覆盖）—— 运行时收集到的映射比挖出来的更权威。
    //
    // 返回真正新增的条数。这个数字要跟"输出目录里多还原了几条"对得上，
    // 所以只统计真正插进去的，命中了但表里已有的不算。
    unsigned int MergeMinedNames(HashMapTables& tables, const std::map<std::wstring, std::wstring>& mined)
    {
        unsigned int added = 0u;
        for (const auto& entry : mined)
        {
            const std::wstring hash = ToUpperString(entry.first);
            if (!IsHexLength(hash, 64u))
            {
                // 内核只该给出 64 位文件哈希。形态不对的条目宁可不要，也不放进映射表。
                continue;
            }
            const std::wstring name = NormalizeFileName(entry.second);
            if (name.empty())
            {
                continue;
            }
            // 映射表里的名字按口径只取"最后一段"（见 ResolveHashName：映射名带层级时
            // 层级是重复信息，封包与目录层由源路径和目录映射负责）。名字**不做**任何
            // 修改或推断 —— 内核给什么就是什么。
            const size_t separator = name.find_last_of(L"\\/");
            const std::wstring leaf = separator == std::wstring::npos ? name : name.substr(separator + 1u);
            if (leaf.empty())
            {
                continue;
            }
            if (tables.files.emplace(hash, leaf).second)
            {
                ++added;
            }
        }
        return added;
    }

    // 把挖掘结果落盘（哈希:名字，UTF-8 with BOM）。
    //
    // 这里是**整份重写**而不是"追加去重"：MinedNames.lst 的字段只有 哈希:名字，
    // 而 std::map 已经天然按哈希去重（同一个哈希只会有一条），所以重写出来的文件
    // 本身就是去重且稳定的（连跑两次逐字节一致，不会膨胀）。同目录下用户自己攒的
    // 其它 *.lst 一个都不动 —— 那些是别人的成果，本工具只维护自己这一份。
    //
    // mined 为空时**不写**：空结果通常意味着挖掘没真正跑（被中止/盐不可用），
    // 把上次挖到的几千条好数据重写成只剩注释头是纯破坏。要表达"清空"得用别的入口。
    bool WriteMinedNamesList(const std::wstring& path, const std::map<std::wstring, std::wstring>& mined,
                             unsigned int& writtenEntries, DWORD& error)
    {
        error = 0;
        writtenEntries = 0u;

        if (mined.empty())
        {
            // 文件已存在就如实报"本次 0 条、保留原文件"；不存在则不凭空创建。
            if (::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                writtenEntries = 0u;
                return true;
            }
            // 不存在时也返回 true（没东西可写不算失败），但不留一个只有注释头的空壳。
            return true;
        }

        std::wstring text;
        text.reserve(mined.size() * 96u + 256u);
        text += L"# YuaExtract 推理补充还原：从游戏资源里挖出并用哈希验证过的资源名\r\n";
        text += L"# 格式：<大写 64 位 Hash>:<名字>；本文件由工具维护，与同目录其它 *.lst 一并被步骤3 读取\r\n";
        text += L"# 生成时间：" + TimestampNow() + L"\r\n";

        unsigned int count = 0u;
        for (const auto& entry : mined)
        {
            const std::wstring hash = ToUpperString(entry.first);
            const std::wstring name = NormalizeFileName(entry.second);
            if (!IsHexLength(hash, 64u) || name.empty())
            {
                continue;
            }
            text += MakeMinedListLine(hash, name) + L"\r\n";
            ++count;
        }
        writtenEntries = count;

        DWORD writeError = 0;
        if (!WriteTextFileUtf8Bom(path, text, writeError))
        {
            error = writeError;
            return false;
        }
        return true;
    }

    // ---------- 目录枚举 ----------

    struct SourceFile
    {
        std::wstring fullPath;
        std::wstring relativePath;
        unsigned long long size = 0;
    };

    // 工具自己的产物不是"解包出来的资源"，别当成散文件再复制一遍。
    bool IsToolArtifactName(const std::wstring& name)
    {
        std::wstring lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });
        return lower.rfind(L"restorereport", 0) == 0 || lower.rfind(L"restoredetail_", 0) == 0 ||
               lower.rfind(L"yuaextractrestore.log", 0) == 0;
    }

    void CollectSourceFiles(const std::wstring& root, const std::wstring& current,
                            std::vector<SourceFile>& files, unsigned int& skippedLogFiles)
    {
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(CombinePath(current, L"*").c_str(), &data);
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
            if (IsSkippedLogComponent(name))
            {
                // 日志目录是工具自己的产物，不是游戏资源，别跟着一起还原/计数。
                continue;
            }

            const std::wstring full = CombinePath(current, name);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                CollectSourceFiles(root, full, files, skippedLogFiles);
            }
            else
            {
                SourceFile file;
                file.fullPath = full;
                file.relativePath = full.substr(root.length());
                while (!file.relativePath.empty() &&
                       (file.relativePath.front() == L'\\' || file.relativePath.front() == L'/'))
                {
                    file.relativePath.erase(file.relativePath.begin());
                }
                LARGE_INTEGER size{};
                size.LowPart = data.nFileSizeLow;
                size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
                file.size = static_cast<unsigned long long>(size.QuadPart);
                files.push_back(file);
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
    }

    // 只扫本层文件，不递归。给"根目录"那个任务用 —— 根下的散文件不是封包内容，
    // 硬套递归会把整个解包结果再走一遍（等于每个封包都扫两遍）。
    void CollectTopLevelFiles(const std::wstring& root, std::vector<SourceFile>& files)
    {
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(CombinePath(root, L"*").c_str(), &data);
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
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }
            if (IsSkippedLogComponent(name) || IsToolArtifactName(name))
            {
                continue;
            }

            SourceFile file;
            file.fullPath = CombinePath(root, name);
            file.relativePath = name;
            LARGE_INTEGER size{};
            size.LowPart = data.nFileSizeLow;
            size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
            file.size = static_cast<unsigned long long>(size.QuadPart);
            files.push_back(file);
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
    }

    unsigned int CountSkippedLogFiles(const std::wstring& root)
    {
        unsigned int count = 0u;
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(CombinePath(root, L"*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            return 0u;
        }
        do
        {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            const std::wstring full = CombinePath(root, name);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (IsSkippedLogComponent(name))
                {
                    WIN32_FIND_DATAW inner{};
                    HANDLE innerFind = ::FindFirstFileW(CombinePath(full, L"*").c_str(), &inner);
                    if (innerFind != INVALID_HANDLE_VALUE)
                    {
                        do
                        {
                            if ((inner.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                                wcscmp(inner.cFileName, L".") != 0 && wcscmp(inner.cFileName, L"..") != 0)
                            {
                                ++count;
                            }
                        } while (::FindNextFileW(innerFind, &inner));
                        ::FindClose(innerFind);
                    }
                }
                else
                {
                    count += CountSkippedLogFiles(full);
                }
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
        return count;
    }

    // 只数"解包目录根下直接躺着的日志目录/日志文件"里的文件，不往封包里递归。
    //
    // 为什么要单独来一个：CountSkippedLogFiles 会递归进子目录，用它数根目录会把
    // 各封包内部的日志目录再数一遍 —— 那些已经由各自封包任务统计过了，重复计数。
    // 根下的日志目录不属于任何封包（EnumerateRestorePackages 明确不把它们当封包），
    // 所以由报告统一数一次，两边口径拼起来刚好是"整棵源目录里的日志文件"。
    unsigned int CountTopLevelSkippedLogFiles(const std::wstring& root)
    {
        unsigned int count = 0u;
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(CombinePath(root, L"*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            return 0u;
        }
        do
        {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L".." || !IsSkippedLogComponent(name))
            {
                continue;
            }
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            {
                // 名字就叫 Extractor_Log 的散文件：同样不会被还原/复制，算跳过的一条。
                ++count;
                continue;
            }

            WIN32_FIND_DATAW inner{};
            HANDLE innerFind = ::FindFirstFileW(CombinePath(CombinePath(root, name), L"*").c_str(), &inner);
            if (innerFind != INVALID_HANDLE_VALUE)
            {
                do
                {
                    if ((inner.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                        wcscmp(inner.cFileName, L".") != 0 && wcscmp(inner.cFileName, L"..") != 0)
                    {
                        ++count;
                    }
                } while (::FindNextFileW(innerFind, &inner));
                ::FindClose(innerFind);
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
        return count;
    }

    // ---------- 输出 ----------

    bool EnsureDirectory(const std::wstring& path, DWORD& error)
    {
        error = 0;
        return Log::DirectoryDetail::EnsureDirectory(path, &error);
    }

    // 两个已打开的文件句柄内容是否逐字节一致（分块读，不整块进内存）。
    // 调用方已保证两文件大小相等 —— 大小不等直接判不等，不进这里。
    bool FilesContentEqual(HANDLE left, HANDLE right)
    {
        auto rewind = [](HANDLE file)
        {
            LARGE_INTEGER zero{};
            return ::SetFilePointerEx(file, zero, nullptr, FILE_BEGIN) != 0;
        };
        if (!rewind(left) || !rewind(right))
        {
            return false;
        }

        static constexpr DWORD kChunk = 1u << 20;   // 1 MB
        std::vector<unsigned char> leftBuffer(kChunk);
        std::vector<unsigned char> rightBuffer(kChunk);

        for (;;)
        {
            DWORD leftRead = 0;
            DWORD rightRead = 0;
            if (!::ReadFile(left, leftBuffer.data(), kChunk, &leftRead, nullptr) ||
                !::ReadFile(right, rightBuffer.data(), kChunk, &rightRead, nullptr))
            {
                return false;
            }
            if (leftRead != rightRead)
            {
                return false;
            }
            if (leftRead == 0u)
            {
                return true;
            }
            if (std::memcmp(leftBuffer.data(), rightBuffer.data(), leftRead) != 0)
            {
                return false;
            }
        }
    }

    // 目标重名时落到 名字__dupN.后缀，绝不覆盖已有还原结果。
    std::wstring MakeUniqueTargetPath(const std::wstring& target)
    {
        if (::GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return target;
        }

        const size_t dot = target.find_last_of(L'.');
        const size_t slash = target.find_last_of(L"\\/");
        std::wstring stem = target;
        std::wstring extension;
        if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        {
            stem = target.substr(0, dot);
            extension = target.substr(dot);
        }

        for (unsigned int index = 1; index < 100000u; ++index)
        {
            wchar_t suffix[32]{};
            ::swprintf_s(suffix, L"__dup%u", index);
            const std::wstring candidate = stem + suffix + extension;
            if (::GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                return candidate;
            }
        }
        return target;
    }

    bool WriteTextFileUtf8Bom(const std::wstring& path, const std::wstring& text, DWORD& error)
    {
        error = 0;
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                               nullptr, 0, nullptr, nullptr);
        if (need < 0)
        {
            error = ERROR_NO_UNICODE_TRANSLATION;
            return false;
        }
        std::string utf8(static_cast<size_t>(need), '\0');
        if (need > 0)
        {
            ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &utf8[0], need,
                                  nullptr, nullptr);
        }

        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = ::GetLastError();
            return false;
        }

        const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        bool ok = ::WriteFile(file, bom, sizeof(bom), &written, nullptr) != FALSE;
        if (ok && !utf8.empty())
        {
            ok = ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) != FALSE;
        }
        if (!ok)
        {
            error = ::GetLastError();
        }
        ::CloseHandle(file);
        return ok;
    }

    // 最后一段剥掉后缀（没有后缀就原样返回）。
    std::wstring StripExtension(const std::wstring& leaf)
    {
        const size_t dot = leaf.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0)
        {
            return leaf;
        }
        return leaf.substr(0, dot);
    }

    // 最后一个路径段是不是"Hash 形态"：<64位hash> 或 <64位hash>.ext。
    bool IsHashStyleLeaf(const std::wstring& leaf)
    {
        return IsHexLength(ToUpperString(StripExtension(leaf)), 64u);
    }

    // ---------- 文件头识别（给未还原文件补后缀） ----------
    //
    // 为什么需要它：解包器落盘时只有"这个文件在包里的偏移和长度"，没有名字——
    // 名字要等运行时收集到映射才知道。没映射的条目就成了一个光秃秃的 64 位 Hash 名，
    // Windows 不认识它（双击没反应、图片浏览器不列它）。文件头是这些文件身上唯一
    // 还带着类型信息的地方，用它补一个后缀，文件至少是"可打开"的。
    //
    // 判据只取前 64 字节：这已经覆盖了下面所有魔数，最多再加一行"像不像文本"的统计。
    constexpr DWORD kSniffBytes = 64u;

    bool StartsWithBytes(const std::vector<unsigned char>& bytes, const unsigned char* prefix, size_t length)
    {
        if (bytes.size() < length)
        {
            return false;
        }
        for (size_t index = 0; index < length; ++index)
        {
            if (bytes[index] != prefix[index])
            {
                return false;
            }
        }
        return true;
    }

    // 按顺序匹配文件头。返回的字符串都是字面量，不需要释放。
    const wchar_t* MatchBinarySignature(const std::vector<unsigned char>& bytes)
    {
        static const unsigned char kPng[] = { 0x89, 'P', 'N', 'G' };
        static const unsigned char kOgg[] = { 'O', 'g', 'g', 'S' };
        static const unsigned char kJpeg[] = { 0xFF, 0xD8, 0xFF };
        static const unsigned char kTlg[] = { 'T', 'L', 'G' };
        static const unsigned char kPsb[] = { 'P', 'S', 'B', 0x00 };
        static const unsigned char kRiff[] = { 'R', 'I', 'F', 'F' };
        static const unsigned char kBmp[] = { 'B', 'M' };
        static const unsigned char kZip[] = { 'P', 'K', 0x03, 0x04 };
        static const unsigned char k7z[] = { '7', 'z', 0xBC, 0xAF };

        if (StartsWithBytes(bytes, kPng, sizeof(kPng))) { return L".png"; }
        if (StartsWithBytes(bytes, kOgg, sizeof(kOgg))) { return L".ogg"; }
        if (StartsWithBytes(bytes, kJpeg, sizeof(kJpeg))) { return L".jpg"; }
        if (StartsWithBytes(bytes, kTlg, sizeof(kTlg))) { return L".tlg"; }
        if (StartsWithBytes(bytes, kPsb, sizeof(kPsb))) { return L".psb"; }
        if (StartsWithBytes(bytes, kRiff, sizeof(kRiff))) { return L".wav"; }
        if (StartsWithBytes(bytes, kBmp, sizeof(kBmp))) { return L".bmp"; }
        if (StartsWithBytes(bytes, kZip, sizeof(kZip))) { return L".zip"; }
        if (StartsWithBytes(bytes, k7z, sizeof(k7z))) { return L".7z"; }
        return nullptr;
    }

    // 前 64 字节里"可打印"的比例是否 ≥ 90%。制表/换行/回车算可打印 ——
    // 脚本和配置里这些字符成片出现，算成不可打印会把真文本判成二进制。
    bool LooksLikeText(const std::vector<unsigned char>& bytes)
    {
        if (bytes.empty())
        {
            return false;
        }
        size_t printable = 0u;
        for (unsigned char byte : bytes)
        {
            if ((byte >= 0x20u && byte <= 0x7Eu) || byte == 0x09u || byte == 0x0Au || byte == 0x0Du)
            {
                ++printable;
            }
        }
        return printable * 100u >= bytes.size() * 90u;
    }

    // 读文件头并判定后缀。任何一步失败都落 ".bin"：宁可给个通用的后缀，
    // 也不能让这个名字没有后缀（那样 Windows 就没法打开它了）。
    std::wstring SniffFileExtension(const std::wstring& path)
    {
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return L".bin";
        }

        std::vector<unsigned char> bytes(kSniffBytes, 0);
        DWORD read = 0;
        const BOOL ok = ::ReadFile(file, bytes.data(), kSniffBytes, &read, nullptr);
        ::CloseHandle(file);
        if (!ok)
        {
            return L".bin";
        }
        bytes.resize(std::min<size_t>(read, bytes.size()));

        if (const wchar_t* matched = MatchBinarySignature(bytes))
        {
            return matched;
        }
        return LooksLikeText(bytes) ? L".txt" : L".bin";
    }

    // 未还原文件在输出目录里的文件名：<64位 Hash> + 由文件头判定的后缀。
    //
    // 为什么要"剥掉已有后缀再补"而不是直接往后接：源文件名在解包器已经确定后缀时是
    // <hash>.png 这种形态（ResolveHashName 认得出来），直接接会得到 <hash>.png.png。
    // 对 64 位 hex 来说取"最后一段"与取"剥掉后缀"是同一件事，所以这里用剥后缀收口。
    std::wstring MakeHeaderSuffixedFileName(const std::wstring& sourceLeaf, const std::wstring& extension)
    {
        return StripExtension(sourceLeaf) + extension;
    }

    // 名字解析的结果。用显式状态而不是"名字空不空"来判 —— 映射名自带层级、或者
    // 目录名是合法的根域空串（%EmptyString%）时，靠空值判会把已还原误判成缺目录。
    struct NameResolution
    {
        std::wstring dirHash;   // 源路径里的目录 Hash（大写）
        std::wstring fileHash;  // 源路径里的文件 Hash（大写，已剥掉后缀）
        std::wstring dirName;   // 目录 Hash 对应的明文目录（空串 = 根域，未知目录 = 空且 dirResolved=false）
        std::wstring fileName;  // 文件 Hash 对应的明文名
        bool dirResolved = false;
        bool fileResolved = false;
        bool hashNamed = false; // 是不是 <目录Hash>\<文件Hash>[.后缀] 形态
    };

    // 取一个（可能是相对路径的）名字的最后一段。
    std::wstring LeafName(const std::wstring& path)
    {
        const size_t separator = path.find_last_of(L"\\/");
        return separator == std::wstring::npos ? path : path.substr(separator + 1);
    }

    // 给一条源文件的相对路径填出目录/文件两个 Hash 与它们对应的明文名。
    //
    // 这里要兼容解包器/dumper 实际会写出的几种形态，缺一个都会让整批文件"看起来
    // 全都没还原"：
    //   1) 文件名带后缀：解包器确定文件尾后会落成 <64位hash>.png / .ogg 之类，
    //      所以"最后一段"是 "<hash>.<ext>"，不能拿整段去比 64 位 hex。
    //   2) 映射表里的明文名可能是**包内相对路径**：dumper 记的是
    //      "bgimage\cg01.png##YSig##<hash>"，只有最后一段是文件名，前面的目录层级
    //      和目录 Hash 是重复信息，再叠一层会得到 data\bgimage\cg01.png。
    //
    // 两个 resolved 标志是判定"该不该进输出目录"的唯一依据：只有两边都对了才算
    // 真的还原（见 RestoreSinglePackage 里的决策）。
    NameResolution ResolveHashName(const HashMapTables& tables, const std::vector<std::wstring>& parts)
    {
        NameResolution out;

        if (parts.size() >= 2u)
        {
            out.dirHash = ToUpperString(parts[parts.size() - 2u]);
        }
        if (!parts.empty())
        {
            out.fileHash = ToUpperString(StripExtension(parts.back()));
        }
        if (parts.size() < 2u || !IsHexLength(out.dirHash, 16u) || !IsHashStyleLeaf(parts.back()))
        {
            // 非哈希名形态（.alst 清单、说明文件等）：调用方按"原样搬运"处理。
            return out;
        }
        out.hashNamed = true;

        // 目录名：%EmptyString% / "/" 都是根域，映射表里存成空串。
        const auto dirIt = tables.directories.find(out.dirHash);
        if (dirIt != tables.directories.end())
        {
            out.dirName = dirIt->second;
            out.dirResolved = true;
        }

        const std::wstring& lastPart = parts.back();
        std::wstring mapped;
        std::wstring extension; // 只有"整段 hash 没命中、剥掉后缀才命中"时才是有效的

        const auto exactIt = tables.files.find(ToUpperString(lastPart));
        if (exactIt != tables.files.end())
        {
            out.fileHash = ToUpperString(lastPart);
            mapped = exactIt->second;
        }
        else
        {
            const size_t dot = lastPart.find_last_of(L'.');
            if (dot != std::wstring::npos && dot > 0)
            {
                extension = lastPart.substr(dot);
                const auto stemIt = tables.files.find(out.fileHash);
                if (stemIt != tables.files.end())
                {
                    mapped = stemIt->second;
                }
            }
        }

        if (mapped.empty())
        {
            // 文件 Hash 查不到。注意这条**可能同时**缺目录 Hash（两个都查不到），
            // 所以要把 fileResolved=false 如实标出来 —— 计数口径
            //"缺目录 / 缺文件"是按两个维度各自统计的，不是互斥分类。
            return out;
        }

        out.fileName = mapped;
        out.fileResolved = true;

        const size_t separator = mapped.find_last_of(L"\\/");
        if (separator != std::wstring::npos)
        {
            // 形态 2：映射名自带层级。目录 Hash 这一边就不要了（重复信息），
            // 文件名只取最后一段 —— 封包层由调用方负责。
            out.fileName = mapped.substr(separator + 1);
            out.dirName.clear();
            out.dirResolved = true; // 层级已经由映射名自己带全了
        }

        if (extension.empty() || out.fileName.empty())
        {
            return out;
        }

        // 补后缀：映射表里的名字可能自带后缀（"cg01.png"），也可能什么都不带（"cg01"）。
        // 只在本名没有后缀、或后缀不同的时候补，避免出现 cg01.png.png。
        const size_t nameDot = out.fileName.find_last_of(L'.');
        const std::wstring nameExtension =
            nameDot == std::wstring::npos ? std::wstring() : out.fileName.substr(nameDot);
        if (_wcsicmp(nameExtension.c_str(), extension.c_str()) != 0)
        {
            out.fileName += extension;
        }
        return out;
    }

    // 报告明细里的状态。它描述的是"这个名字最终是不是全靠真名字拼出来的"，
    // 而不是"这份文件有没有写出去"—— 默认模式下未还原的会被丢掉，但报告里仍要
    // 留下它为什么没还原，否则用户无从下手补映射。
    const wchar_t* OutcomeText(bool resolved, bool delivered)
    {
        if (resolved)
        {
            return L"RESTORED";
        }
        return delivered ? L"KEPT_UNRESOLVED" : L"DROPPED_UNRESOLVED";
    }
}

// ---------- 映射载入 ----------

HashMapTables LoadHashMapTables(const std::wstring& hashDirectory)
{
    HashMapTables tables;
    if (!DirectoryExists(hashDirectory))
    {
        return tables;
    }

    std::vector<std::wstring> logFiles;
    std::vector<std::wstring> listFiles;
    CollectHashDirectoryMapFiles(hashDirectory, logFiles, listFiles);

    for (const std::wstring& path : logFiles)
    {
        const unsigned int loaded = LoadSignatureLog(path, tables);
        wchar_t line[512]{};
        ::swprintf_s(line, L"%s（%u 条）", path.c_str(), loaded);
        tables.sources.push_back(line);
    }
    for (const std::wstring& path : listFiles)
    {
        const unsigned int loaded = LoadRecoveredList(path, tables);
        wchar_t line[512]{};
        ::swprintf_s(line, L"%s（%u 条）", path.c_str(), loaded);
        tables.sources.push_back(line);
    }
    return tables;
}

bool HasUnresolvedHashSegment(const std::wstring& relativePath)
{
    const std::vector<std::wstring> parts = SplitRelativePath(relativePath);
    if (parts.size() < 2u)
    {
        return false;
    }
    // 父目录是 16 位 hex（目录 Hash 没解析掉）、或者是 64 位 hex 的叶子（文件 Hash
    // 没解析掉）都算"还有未解析 Hash 段"。
    // 叶子可能是 "<64位hash>"，也可能是 "<64位hash>.png"（解包器确定后缀之后的
    // 形态）—— 两种都得认出来，只比整段会把带后缀的那种漏掉。
    return IsHexLength(ToUpperString(parts[parts.size() - 2u]), 16u) || IsHashStyleLeaf(parts.back());
}

// ---------- 任务枚举 ----------

std::vector<RestorePackageTask> EnumerateRestorePackages(const std::wstring& sourceDirectory)
{
    std::vector<RestorePackageTask> tasks;
    if (!DirectoryExists(sourceDirectory))
    {
        return tasks;
    }

    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW(CombinePath(sourceDirectory, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        return tasks;
    }
    do
    {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..")
        {
            continue;
        }
        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
        {
            // 根目录下的散文件（.alst 清单之类）不成包，还原时会跳过并在报告里注明。
            continue;
        }
        if (IsSkippedLogComponent(name))
        {
            // 解包器自己的日志目录不是"封包"，混进任务列表只会让人以为漏了什么。
            continue;
        }

        RestorePackageTask task;
        task.packageName = name;
        task.packageDirectory = CombinePath(sourceDirectory, name);
        tasks.push_back(task);
    } while (::FindNextFileW(find, &data));
    ::FindClose(find);

    // 按名字排序：任务列表和报告的封包顺序必须稳定，否则每次跑出来的报告都对不上。
    std::sort(tasks.begin(), tasks.end(),
              [](const RestorePackageTask& left, const RestorePackageTask& right)
              { return _wcsicmp(left.packageName.c_str(), right.packageName.c_str()) < 0; });

    // 顺手数一遍文件数：界面上"排队中"要显示总量，而这项工作放在工作线程里做
    // 会让界面在开始时完全没反应。
    for (RestorePackageTask& task : tasks)
    {
        unsigned int skipped = 0u;
        std::vector<SourceFile> files;
        CollectSourceFiles(task.packageDirectory, task.packageDirectory, files, skipped);
        for (const SourceFile& file : files)
        {
            task.bytes += file.size;
        }
        task.fileCount = static_cast<unsigned int>(files.size());
    }

    // 根下的散文件也要有归宿，否则"勾选保留时输出文件数 == 源文件数"就不成立。
    // 只在真的有散文件时才建这个任务，免得多出一个空行。
    {
        std::vector<SourceFile> rootFiles;
        CollectTopLevelFiles(sourceDirectory, rootFiles);
        unsigned int countable = 0u;
        unsigned long long rootBytes = 0u;
        for (const SourceFile& file : rootFiles)
        {
            const size_t separator = file.relativePath.find_last_of(L"\\/");
            const std::wstring leaf =
                separator == std::wstring::npos ? file.relativePath : file.relativePath.substr(separator + 1);
            if (IsToolArtifactName(leaf))
            {
                continue;
            }
            ++countable;
            rootBytes += file.size;
        }

        if (countable > 0u)
        {
            RestorePackageTask rootTask;
            rootTask.packageName = kRootPackageName;
            rootTask.packageDirectory = sourceDirectory;
            rootTask.rootLevel = true;
            rootTask.fileCount = countable;
            rootTask.bytes = rootBytes;
            tasks.push_back(rootTask);
        }
    }
    return tasks;
}

// ---------- 单封包还原 ----------

RestorePackageResult RestoreSinglePackage(const RestorePackageTask& task, const HashMapTables& tables,
                                          const RestoreOptions& options)
{
    RestorePackageResult result;
    result.packageName = task.packageName;

    if (!DirectoryExists(task.packageDirectory))
    {
        result.succeeded = false;
        result.completed = true;
        result.info = L"封包目录不存在";
        return result;
    }

    DWORD error = 0;
    if (!EnsureDirectory(options.outputDirectory, error))
    {
        result.succeeded = false;
        result.completed = true;
        result.info = L"建不出输出目录：" + options.outputDirectory;
        return result;
    }

    std::vector<SourceFile> files;
    if (task.rootLevel)
    {
        // 根任务只扫本层：根下的散文件不属于任何封包，也就没有"包内相对路径"。
        CollectTopLevelFiles(task.packageDirectory, files);
        // 根下的日志目录故意不在这里计数：它们不属于任何封包，而且根任务只在
        // "根下真有散文件"时才存在，记在这里会随"这次有没有散文件"时有时无。
        // 报告统一数一次（见 WriteRestoreReport 里的 CountTopLevelSkippedLogFiles）。
    }
    else
    {
        CollectSourceFiles(task.packageDirectory, task.packageDirectory, files, result.skippedLogFiles);
        result.skippedLogFiles += CountSkippedLogFiles(task.packageDirectory);
    }
    result.totalFiles = static_cast<unsigned int>(files.size());

    // 还原结果落在哪一层：正常封包要保留 <封包> 这一层（多个封包里的同名资源否则会
    // 在输出目录里撞成一份）；根任务是"输出目录根下的散文件"，就没有这一层。
    const std::wstring outputPrefix = task.rootLevel ? std::wstring() : task.packageName;

    // 报告明细：只记与哈希名有关的部分（非哈希名文件动辄上千条，写进去只会淹掉重点）。
    std::wstring details;
    details.reserve(files.size() * 96u);

    const unsigned int total = static_cast<unsigned int>(files.size());
    unsigned int processed = 0u;
    unsigned int lastPercent = 0u;
    bool cancelled = false;

    for (const SourceFile& file : files)
    {
        if (options.cancelRequested && options.cancelRequested())
        {
            cancelled = true;
            break;
        }

        ++processed;
        const std::vector<std::wstring> parts = SplitRelativePath(file.relativePath);

        const NameResolution resolution = ResolveHashName(tables, parts);
        const unsigned long long fileSize = file.size;
        const std::wstring sourcePath = file.fullPath;

        if (resolution.hashNamed)
        {
            ++result.hashNamedFiles;
            // "缺目录 Hash"与"缺文件 Hash"是两个正交维度，不是互斥分类 ——
            // 两个都查不到的条目要同时计入两边，报告里的数字才对得上
            // "按维度独立枚举"的口径。
            if (!resolution.dirResolved)
            {
                ++result.missingDirectory;
                if (resolution.fileResolved)
                {
                    // 文件名解析得出来、只是缺目录名：补目录候选表最有效，
                    // 和"两个都缺"是两回事，报告里单独列一行。
                    ++result.missingDirectoryWithKnownFile;
                }
            }
            if (!resolution.fileResolved)
            {
                ++result.missingFileName;
            }
        }
        else
        {
            // .alst 清单、说明文件之类：名字本来就不带 Hash，不需要"还原"。
            // 计数留到后面 —— 只有真的写进输出目录才算"原样交付"，
            // 免得出现"统计里说原样 3 个、目录里只有 2 个"这种对不上的数字。
        }

        // 最终目标相对路径 = 封包层 + 目录名（真名，或退化的 Hash 名）+ 文件名
        //（真名，或退化的 Hash 名 / Hash 名 + 按文件头补的后缀）。
        //
        // 非哈希名文件（.alst 清单等）只换掉"文件名"这一段：它的目录层级必须照原样
        // 保留，否则会被摊平到输出目录根下 —— 多个封包的 .alst 会互相覆盖。
        //
        // 关键点：**判据是"最终路径里还有没有未解析的 Hash 段"，不是"还原函数有没有
        // 返回真名"**。这样"明文名躺在 Hash 目录下"（voice\<16hex>\anj_0001.ogg，
        // 旧版步骤2 就地改名后的残留）也会被正确认成未还原，而不是当成普通明文文件
        // 带着 Hash 目录名搬出去。
        std::wstring targetRelative = outputPrefix;
        if (resolution.hashNamed)
        {
            const std::wstring targetDirectoryName =
                resolution.dirResolved ? resolution.dirName : resolution.dirHash;
            if (!targetDirectoryName.empty())
            {
                targetRelative = targetRelative.empty() ? targetDirectoryName
                                                        : CombinePath(targetRelative, targetDirectoryName);
            }

            std::wstring targetFileName = parts.back();
            if (resolution.fileResolved)
            {
                targetFileName = resolution.fileName;
            }
            else if (options.keepUnrestored)
            {
                // 只处理"名字还是 Hash"这一种未还原形态：目录 Hash 没解析掉、但文件名
                // 已经是明文（旧版步骤2 就地改名的残留）的条目，名字本身是好的，
                // 补后缀反而会把它改坏。
                // 输出目录里才这么命名 —— 源目录全程只读，绝不在那边改任何名字。
                const std::wstring extension = SniffFileExtension(sourcePath);
                targetFileName = MakeHeaderSuffixedFileName(parts.back(), extension);
                ++result.headerSuffixedFiles;
                ++result.headerExtensionCounts[extension];
            }

            targetRelative =
                targetRelative.empty() ? targetFileName : CombinePath(targetRelative, targetFileName);
        }
        else
        {
            targetRelative = targetRelative.empty() ? file.relativePath
                                                    : CombinePath(targetRelative, file.relativePath);
        }

        const bool resolved = resolution.hashNamed && resolution.dirResolved && resolution.fileResolved;
        // 非哈希名文件（.alst 等）天然不带 Hash 段，这一句就是 false。
        const bool unresolved = HasUnresolvedHashSegment(targetRelative);
        // 目标末尾是 '\\' 说明文件名那一段是空的（映射表里存了条畸形记录）。
        // 那样写下去会建出一个目录而不是文件，宁可算失败也别在输出目录里留陷阱。
        const bool validTarget = !targetRelative.empty() && targetRelative.back() != L'\\' &&
                                 targetRelative.back() != L'/';

        bool delivered = false;
        if (unresolved && !options.keepUnrestored)
        {
            // 默认语义：名字里还有没解析掉的 Hash 段就不进输出目录
            //（文件名仍是 64 位 Hash、或路径里仍有未解析 Hash 目录）。
            // 默认模式下输出目录里只留"名字全是真名"的文件。
            ++result.droppedUnrestoredFiles;
        }
        else if (!validTarget)
        {
            ++result.failedFiles;
        }
        else
        {
            const std::wstring baseTarget = CombinePath(options.outputDirectory, targetRelative);

            // 两遍式还原：这一遍（补充还原遍）遇到第一遍已交付过的目标就跳过。
            // 挖掘只补第一遍没还原掉的文件，第一遍已交付的不该再被碰 ——
            // 跳过既不重复复制，也不会把第一遍的成果计成本遍的。
            if (options.alreadyDelivered.find(targetRelative) != options.alreadyDelivered.end())
            {
                ++result.skippedAlreadyDelivered;
                if (!resolution.hashNamed)
                {
                    // 非哈希名文件照样计进"原样"，保持"输出文件数 == 已还原 + 原样"对账成立。
                    ++result.passthroughFiles;
                }
                if (options.progress && total > 0u)
                {
                    const unsigned int percent =
                        static_cast<unsigned int>((static_cast<unsigned long long>(processed) * 100ull) / total);
                    if (percent != lastPercent)
                    {
                        lastPercent = percent;
                        options.progress(percent, file.relativePath);
                    }
                }
                continue;
            }

            // 重跑同一输出目录时的语义：目标已存在时，**内容相同就跳过**，
            // 只有内容不同才落 __dupN 副本。
            //
            // 为什么要这样：判重依据是"源文件相对路径 + 源文件大小"，上次 run 复制的就是
            // 同一个源文件，重跑必然得到同一份内容。之前的实现无条件落 __dupN，用户在同一
            // 输出目录连跑两次（比如先普通还原、再勾补充还原重跑），整个目录直接翻倍
            //（实测 60,145 个 __dup、多占 7.31 GB），而内容一字不差。
            //
            // 大小不等（或比对失败）才走原来的 __dupN 路径 —— 那才是真正"目标被占用、
            // 内容可能不同"的场景，保守处理不覆盖。
            bool identicalExisting = false;
            {
                // 前缀（<hash>.）匹配的第一遍残留也要算"已存在且等同"：
                // 两遍式 + 保留时，第一遍交付的名字是 <hash><补的后缀>，本遍真名解析出来后
                // 目标不同（真名），但源文件是同一个 —— 删残留、写真名，不落副本。
                // 这里只做"不再重复复制"的判定；删残留放在真名交付成功之后。
                HANDLE existing = ::CreateFileW(baseTarget.c_str(), GENERIC_READ,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (existing != INVALID_HANDLE_VALUE)
                {
                    LARGE_INTEGER existingSize{};
                    if (::GetFileSizeEx(existing, &existingSize) &&
                        static_cast<unsigned long long>(existingSize.QuadPart) == fileSize)
                    {
                        // 大小一致才逐字节比（源文件此刻在读，FILE_SHARE_READ 足够）
                        HANDLE source = ::CreateFileW(sourcePath.c_str(), GENERIC_READ,
                                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                        if (source != INVALID_HANDLE_VALUE)
                        {
                            identicalExisting = FilesContentEqual(existing, source);
                            ::CloseHandle(source);
                        }
                    }
                    ::CloseHandle(existing);
                }
            }

            if (identicalExisting)
            {
                // 内容相同：什么都不做。这不算"失败"也不算"重名改名"，
                // 但要计入 totalFiles 已有的统计口径里（ delivered=false 且不进任何失败计数，
                // 报告的"已还原"数与磁盘文件数保持一致）。
                result.copiedBytes += 0u;
                // 与上次 run 相同的文件不再复制，语义上等同"已经在输出目录里"，
                // 不改变 restoredFiles（它统计的是本次实际交付的还原结果）。
            }
            else
            {
                const std::wstring targetPath = MakeUniqueTargetPath(baseTarget);
                if (_wcsicmp(targetPath.c_str(), baseTarget.c_str()) != 0)
                {
                    ++result.renamedForConflict;
                }

                const size_t separator = targetPath.find_last_of(L"\\/");
                const std::wstring targetDirectory =
                    separator == std::wstring::npos ? options.outputDirectory : targetPath.substr(0, separator);

                DWORD createError = 0;
                bool copied = false;
                if (EnsureDirectory(targetDirectory, createError))
                {
                    copied = ::CopyFileW(sourcePath.c_str(), targetPath.c_str(), FALSE) != FALSE;
                }
                if (!copied)
                {
                    ++result.failedFiles;
                }
                else
                {
                    delivered = true;
                    result.copiedBytes += fileSize;
                    // 记录交付目标（改名前的基名），供两遍式的下一遍做"已交付"跳过。
                    result.deliveredRelative.push_back(targetRelative);

                    // 两遍式 + 保留未还原：第一遍把这个文件以「<hash><补的后缀>」交付过了
                    //（当时没解析出真名）。本遍挖出了真名、落的是真名路径 —— 两份都留在
                    // 输出目录就是重复。真名已交付，把第一遍的哈希名残留删掉。
                    //
                    // **残留的位置要按源目录哈希找，不能按解析出的目录名找**：这批文件
                    // 常常连目录 Hash 都不在映射里（"缺目录 Hash"），第一遍交付时目录段
                    // 就是哈希本身；本遍真名解析出来后落在 <同一路径前缀>\真名.png。
                    // 两者的目录段可能一个是哈希、一个是明文（目录解析出来了），所以
                    // "明文目录"和"哈希目录"两个位置都要找一遍。源目录永远不动。
                    if (resolution.hashNamed && !options.alreadyDelivered.empty()
                        && resolution.fileResolved)
                    {
                        // 第一遍交付时的目录段：目录解析出来了用明文，没解析出来用哈希。
                        // 但第一遍可能两种情况都发生过（映射在两次 run 之间变化），所以
                        // 两个候选目录都清一遍。文件名段固定是 <64位哈希>。
                        const wchar_t* staleDirectories[2] = {
                            resolution.dirResolved ? resolution.dirName.c_str() : resolution.dirHash.c_str(),
                            resolution.dirResolved ? resolution.dirHash.c_str() : resolution.dirName.c_str()
                        };

                        for (int dirIndex = 0; dirIndex < 2; ++dirIndex)
                        {
                            const std::wstring staleDirectory = outputPrefix.empty()
                                ? CombinePath(options.outputDirectory, staleDirectories[dirIndex])
                                : CombinePath(CombinePath(options.outputDirectory, outputPrefix),
                                              staleDirectories[dirIndex]);

                            WIN32_FIND_DATAW staleData{};
                            HANDLE staleFind = ::FindFirstFileW(
                                CombinePath(staleDirectory, parts.back() + L".*").c_str(), &staleData);
                            if (staleFind == INVALID_HANDLE_VALUE)
                            {
                                continue;
                            }

                            // 记下每个真删掉的残留的后缀，用于把第一遍的补后缀计数退掉
                            //（那些计数描述的是"磁盘上带补后缀的文件"，残留删了就该退）。
                            std::vector<std::wstring> removedExtensions;
                            do
                            {
                                if ((staleData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                                {
                                    continue;
                                }
                                const std::wstring stalePath = CombinePath(staleDirectory, staleData.cFileName);
                                if (::DeleteFileW(stalePath.c_str()))
                                {
                                    const size_t dot = std::wstring(staleData.cFileName).find_last_of(L'.');
                                    removedExtensions.push_back(
                                        dot == std::wstring::npos ? std::wstring()
                                                                  : std::wstring(staleData.cFileName).substr(dot));
                                }
                            } while (::FindNextFileW(staleFind, &staleData));
                            ::FindClose(staleFind);

                            for (const std::wstring& removed : removedExtensions)
                            {
                                if (result.headerSuffixedFiles > 0u)
                                {
                                    --result.headerSuffixedFiles;
                                }
                                auto countIt = result.headerExtensionCounts.find(removed);
                                if (countIt != result.headerExtensionCounts.end() && countIt->second > 0u)
                                {
                                    --countIt->second;
                                }
                            }
                        }
                    }

                    if (resolved)
                    {
                        // "已还原"按实际交付计：名字解析出来了但没写出去（比如目标畸形）的
                        // 归到失败里，否则"输出文件数 == 已还原 + 原样"这条对账就对不上。
                        ++result.restoredFiles;
                    }
                }
            }
            if (!resolution.hashNamed)
            {
                // 非哈希名文件（.alst 等）走的是"原样搬运"，交付与否都计进"原样"，
                // 这样"输出目录里的文件数 == 已还原 + 原样"永远成立，方便对账。
                ++result.passthroughFiles;
            }
        }

        // 明细只写哈希名条目与失败项（非哈希名文件动辄上千条，写进去只会淹掉重点）。
        if (resolution.hashNamed || !delivered)
        {
            details += file.relativePath;
            details += L'\t';
            details += resolution.dirHash;
            details += L'\t';
            details += resolution.fileHash;
            details += L'\t';
            details += resolution.dirName;
            details += L'\t';
            details += resolution.fileName;
            details += L'\t';
            details += targetRelative;
            details += L'\t';
            details += OutcomeText(resolved, delivered);
            details += L"\r\n";
        }

        if (options.progress && total > 0u)
        {
            const unsigned int percent =
                static_cast<unsigned int>((static_cast<unsigned long long>(processed) * 100ull) / total);
            if (percent != lastPercent)
            {
                lastPercent = percent;
                options.progress(percent, file.relativePath);
            }
        }
    }

    if (options.progress)
    {
        options.progress(100u, L"写入还原报告");
    }

    // 明细落到 <工具日志目录>\RestoreDetail_<封包>.txt。
    //
    // 为什么不放输出目录：那个目录是"给重封包/扩展集用的还原结果"，里面出现
    // RestoreDetail_*.txt 这种工具产物会污染下游（验证脚手架的 A-7 就是这么判的：
    // 输出文件数必须不多不少）。工具产物统一进 Log\，跟别的工具日志作伴。
    // 单个封包内多个线程不会同时写同一个文件（一个封包一个任务），不需要额外加锁。
    if (!details.empty())
    {
        const std::wstring detailRoot = options.logDirectory.empty() ? options.outputDirectory
                                                                     : options.logDirectory;
        DWORD rootError = 0;
        EnsureDirectory(detailRoot, rootError);

        wchar_t name[512]{};
        ::swprintf_s(name, L"RestoreDetail_%s.txt", task.packageName.c_str());
        const std::wstring detailPath = CombinePath(detailRoot, name);

        std::wstring detailText;
        detailText.reserve(details.size() + 256u);
        detailText += L"封包：" + task.packageName + L"\r\n";
        detailText += L"生成时间：" + TimestampNow() + L"\r\n";
        detailText += L"源目录：" + options.sourceDirectory + L"\r\n";
        detailText += L"输出目录：" + options.outputDirectory + L"\r\n";
        detailText += L"源相对路径\t目录Hash\t文件Hash\t目录名\t文件名\t目标相对路径\t状态\r\n";
        detailText += details;

        DWORD writeError = 0;
        if (!WriteTextFileUtf8Bom(detailPath, detailText, writeError))
        {
            AppendProcessLog(options.logDirectory, L"写明细文件失败：" + detailPath);
        }
    }

    result.cancelled = cancelled;
    result.completed = true;
    result.succeeded = !cancelled && result.failedFiles == 0u;

    // 界面"详细信息"列的那一行摘要：一眼能看出这个封包到底怎么回事。
    {
        wchar_t summary[512]{};
        ::swprintf_s(summary, L"还原 %s/%s，丢弃 %s，失败 %s",
                     FormatCount(result.restoredFiles).c_str(),
                     FormatCount(result.hashNamedFiles).c_str(),
                     FormatCount(result.droppedUnrestoredFiles).c_str(),
                     FormatCount(result.failedFiles).c_str());
        result.info = summary;
    }

    AppendProcessLog(options.logDirectory,
                     L"封包 " + task.packageName + L"：" + result.info +
                         (cancelled ? L"（已中止）" : L""));

    return result;
}

// ---------- 进度口径 ----------

unsigned int ComputeTaskProgressPercent(RestoreProgressStep step, unsigned int percent)
{
    if (percent > 100u)
    {
        percent = 100u;
    }
    switch (step)
    {
        case RestoreProgressStep::Queued: return 0u;
        case RestoreProgressStep::Restoring:
            // 还原阶段占前 90%：剩下 10% 留给写明细/写报告，进度条不会在最后一个
            // 文件处跳到 100% 之后又卡着不动。
            return (percent * 90u) / 100u;
        case RestoreProgressStep::Finishing: return 100u;
    }
    return 0u;
}

// ---------- 报告 ----------

RestoreTotals SumRestoreResults(const std::vector<RestorePackageResult>& results)
{
    RestoreTotals totals;
    totals.packageCount = static_cast<unsigned int>(results.size());
    for (const RestorePackageResult& result : results)
    {
        totals.totalFiles += result.totalFiles;
        totals.hashNamedFiles += result.hashNamedFiles;
        totals.restoredFiles += result.restoredFiles;
        totals.passthroughFiles += result.passthroughFiles;
        totals.missingDirectory += result.missingDirectory;
        totals.missingDirectoryWithKnownFile += result.missingDirectoryWithKnownFile;
        totals.missingFileName += result.missingFileName;
        totals.renamedForConflict += result.renamedForConflict;
        totals.failedFiles += result.failedFiles;
        totals.skippedLogFiles += result.skippedLogFiles;
        totals.droppedUnrestoredFiles += result.droppedUnrestoredFiles;
        totals.headerSuffixedFiles += result.headerSuffixedFiles;
        totals.skippedAlreadyDelivered += result.skippedAlreadyDelivered;
        for (const auto& entry : result.headerExtensionCounts)
        {
            totals.headerExtensionCounts[entry.first] += entry.second;
        }
        totals.copiedBytes += result.copiedBytes;
        totals.cancelled = totals.cancelled || result.cancelled;
    }
    return totals;
}

bool WriteRestoreReport(const RestoreReportData& data, std::wstring& error)
{
    error.clear();

    DWORD createError = 0;
    if (!EnsureDirectory(data.outputDirectory, createError))
    {
        error = L"建不出输出目录：" + data.outputDirectory;
        return false;
    }

    const std::wstring reportPath =
        CombinePath(data.outputDirectory, data.reportFileName.empty() ? std::wstring(L"RestoreReport.txt")
                                                                     : data.reportFileName);
    const RestoreTotals totals = SumRestoreResults(data.packages);

    // 成功率的分母是"哈希名文件"：非哈希名文件本来就不需要还原，算进去只会稀释数字。
    const unsigned int successRate = totals.hashNamedFiles > 0u
        ? static_cast<unsigned int>((static_cast<unsigned long long>(totals.restoredFiles) * 100ull) /
                                    totals.hashNamedFiles)
        : 0u;

    // 跳过日志文件的全量 = 各封包任务数到的（其内部的日志目录）
    //                    + 解包目录根下的那些（不属于任何封包，也不随根任务是否存在而变）。
    // 两部分作用域互不重叠，加起来才是"整棵源目录里被跳过的日志文件"。
    const unsigned int skippedLogFiles =
        totals.skippedLogFiles + CountTopLevelSkippedLogFiles(data.sourceDirectory);

    std::wstring report;
    report.reserve(8192u);
    report += L"YuaExtract 资源名还原报告\r\n";
    report += L"生成时间：" + TimestampNow() + L"\r\n";
    report += L"解包目录：" + data.sourceDirectory + L"\r\n";
    report += L"映射目录：" + data.hashDirectory + L"\r\n";
    report += L"输出目录：" + data.outputDirectory + L"\r\n";
    report += L"还原方式：复制（源解包目录保持不变）\r\n";
    report += totals.cancelled ? L"本次运行：被用户中止，结果不完整\r\n" : L"本次运行：已完成\r\n";
    report += L"\r\n";
    report += L"—— 统计 ——\r\n";
    report += L"封包数              " + FormatCount(totals.packageCount) + L"\r\n";
    report += L"总文件数            " + FormatCount(totals.totalFiles) + L"\r\n";
    report += L"哈希名文件          " + FormatCount(totals.hashNamedFiles) + L"\r\n";
    report += L"  已还原            " + FormatCount(totals.restoredFiles) + L"\r\n";
    report += L"  缺目录 Hash       " + FormatCount(totals.missingDirectory) + L"\r\n";
    report += L"    其中文件名已知  " + FormatCount(totals.missingDirectoryWithKnownFile) + L"\r\n";
    report += L"  缺文件 Hash       " + FormatCount(totals.missingFileName) + L"\r\n";
    report += L"未还原被丢弃        " + FormatCount(totals.droppedUnrestoredFiles) + L"\r\n";
    report += L"按文件头补后缀      " + FormatCount(totals.headerSuffixedFiles) + L"\r\n";
    if (totals.skippedAlreadyDelivered > 0u)
    {
        // 只在两遍式的第二遍才会非零：这些文件第一遍已经交付过，本遍按设计跳过。
        report += L"第一遍已交付（跳过）" + FormatCount(totals.skippedAlreadyDelivered) + L"\r\n";
    }
    if (totals.headerSuffixedFiles > 0u)
    {
        // 分布按条数从多到少排，再按后缀名兜底排序 —— 这个顺序必须稳定，
        // 否则同一份数据两次跑出来的报告不可比对。
        std::vector<std::pair<std::wstring, unsigned int>> distribution(totals.headerExtensionCounts.begin(),
                                                                       totals.headerExtensionCounts.end());
        std::sort(distribution.begin(), distribution.end(),
                  [](const std::pair<std::wstring, unsigned int>& left,
                     const std::pair<std::wstring, unsigned int>& right)
                  {
                      if (left.second != right.second)
                      {
                          return left.second > right.second;
                      }
                      return _wcsicmp(left.first.c_str(), right.first.c_str()) < 0;
                  });
        std::wstring line = L"  后缀分布         ";
        for (size_t index = 0; index < distribution.size(); ++index)
        {
            if (index > 0u)
            {
                line += L" / ";
            }
            line += distribution[index].first + L" " + FormatCount(distribution[index].second);
        }
        report += line + L"\r\n";
    }
    report += L"非哈希名文件（原样）" + FormatCount(totals.passthroughFiles) + L"\r\n";
    report += L"目标重名改名        " + FormatCount(totals.renamedForConflict) + L"\r\n";
    report += L"失败                " + FormatCount(totals.failedFiles) + L"\r\n";
    report += L"跳过日志文件        " + FormatCount(skippedLogFiles) + L"\r\n";
    report += L"已复制数据量        " + FormatSize(totals.copiedBytes) + L"\r\n";
    report += L"还原成功率          ";
    report += std::to_wstring(successRate);
    report += L"%  （";
    report += std::to_wstring(totals.restoredFiles);
    report += L" / ";
    report += std::to_wstring(totals.hashNamedFiles);
    report += L"，缺目录名 ";
    report += std::to_wstring(totals.missingDirectory);
    report += L" 条、缺文件名 ";
    report += std::to_wstring(totals.missingFileName);
    report += L" 条）\r\n";
    report += L"\r\n";
    report += L"—— 映射来源 ——\r\n";
    report += L"目录映射 " + FormatCount(data.directoryMapEntries) + L" 条 / 文件映射 " +
              FormatCount(data.fileNameMapEntries) + L" 条\r\n";
    for (const std::wstring& source : data.mapSources)
    {
        report += L"  " + source + L"\r\n";
    }
    report += L"\r\n";

    // 补充还原这一节只在真的勾过复选框时出现 —— 没跑过的功能不该在报告里占位置。
    if (data.supplement.attempted)
    {
        const SupplementOutcome& supplement = data.supplement;
        report += L"—— 推理补充还原（候选挖掘）——\r\n";
        report += L"状态                " +
                  std::wstring(supplement.saltAvailable ? (supplement.ran ? L"已执行" : L"未执行")
                                                         : L"未执行（盐不可用）") +
                  L"\r\n";
        report += L"结论                " + supplement.message + L"\r\n";
        if (supplement.saltAvailable)
        {
            report += L"Hash Seed 来源      " + supplement.saltSource + L"\r\n";
        }
        report += L"扫描文件数          " + FormatCount(supplement.scannedFiles) + L"\r\n";
        report += L"候选名数            " + FormatCount(supplement.candidateCount) + L"\r\n";
        report += L"试算哈希数          " + FormatCount(supplement.triedHashes) + L"\r\n";
        report += L"命中（已通过验证）  " + FormatCount(supplement.hitCount) + L"\r\n";
        report += L"  其中本次新增      " + FormatCount(supplement.newEntries) + L"\r\n";
        report += L"待还原目标哈希      " + FormatCount(supplement.targetHashes) + L"\r\n";
        report += L"扫描根目录数        " + FormatCount(supplement.scanRoots) + L"\r\n";
        report += L"候选触顶            " +
                  std::wstring(supplement.limitHit ? L"是（结果不完整）" : L"否") + L"\r\n";
        if (!data.minedListPath.empty())
        {
            report += L"落盘                " + data.minedListPath + L"（" +
                      FormatCount(supplement.writtenEntries) + L" 条）\r\n";
        }
        report += L"\r\n";
    }

    report += L"—— 各封包还原情况 ——\r\n";
    report += L"封包\t文件数\t哈希名\t已还原\t缺目录\t缺文件\t丢弃未还原\t原样复制\t失败\t耗时\t说明\r\n";
    for (const RestorePackageResult& package : data.packages)
    {
        report += package.packageName;
        report += L'\t';
        report += FormatCount(package.totalFiles);
        report += L'\t';
        report += FormatCount(package.hashNamedFiles);
        report += L'\t';
        report += FormatCount(package.restoredFiles);
        report += L'\t';
        report += FormatCount(package.missingDirectory);
        report += L'\t';
        report += FormatCount(package.missingFileName);
        report += L'\t';
        report += FormatCount(package.droppedUnrestoredFiles);
        report += L'\t';
        report += FormatCount(package.passthroughFiles);
        report += L'\t';
        report += FormatCount(package.failedFiles);
        report += L'\t';
        report += package.info;
        report += L"\r\n";
    }
    report += L"\r\n";
    report += L"—— 说明 ——\r\n";
    report += L"· 每个封包的逐条明细（哪一条还原成什么名、哪一条被丢弃）另存为工具日志目录下的\r\n";
    report += L"  RestoreDetail_<封包>.txt，不放进输出目录以免污染重封包用的资源树。\r\n";
    report += L"· 默认模式（不勾选）输出目录里只留「名字全是真名」的文件：已还原的，\r\n";
    report += L"  以及本来就叫明文名的（.alst 清单、说明文件等）。名字里还有未解析 Hash 段\r\n";
    report += L"  （文件名仍是 64 位 Hash、或路径里仍有 Hash 目录）的一律不写出去，\r\n";
    report += L"  条数见上面的「未还原被丢弃」。\r\n";
    report += L"· 需要在输出目录里也看到那些未还原的文件时，在还原窗口勾选\r\n";
    report += L"  「保留未还原文件名的文件（文件后缀已确定）」后重跑：它们保持 64 位 Hash 名，\r\n";
    report += L"  但会按**文件头**补一个后缀（PNG/OggS/JPEG/TLG/PSB/RIFF/BM/PK/7z 魔数，\r\n";
    report += L"  前 64 字节里可打印字符 ≥90% 的算 .txt，都认不出就落 .bin），\r\n";
    report += L"  这样 Windows 至少能按类型打开它们。后缀是在**输出目录**里补的，源目录不动。\r\n";
    report += L"· 勾选「推理补充还原（从游戏资源中挖掘候选名，哈希验证后写入）」时，本次运行分两遍：\r\n";
    report += L"  先用步骤2 收集到的映射做一遍普通还原，再扫一遍非二进制包里的明文资源挖候选名、\r\n";
    report += L"  逐个用哈希验证，命中才并进映射表，最后拿补全后的映射再补一遍（只处理第一遍没交付的文件）。\r\n";
    report += L"  它需要 <工具目录>\\Log\\Universal.log 里的 Hash Seed（先跑一次步骤2）；\r\n";
    report += L"  没有盐就只做普通还原，原因记在日志和本报告的「推理补充还原」一节。\r\n";
    report += L"· 源目录 Extractor_Output 全程只读，本工具不会修改它。\r\n";

    DWORD writeError = 0;
    if (!WriteTextFileUtf8Bom(reportPath, report, writeError))
    {
        wchar_t buffer[512]{};
        ::swprintf_s(buffer, L"写不出还原报告：%s（错误 %lu）", reportPath.c_str(), writeError);
        error = buffer;
        return false;
    }
    return true;
}

// ---------- 映射概览 ----------

MappingSummary LoadMappingSummary(const std::wstring& hashDirectory)
{
    MappingSummary summary;
    if (!DirectoryExists(hashDirectory))
    {
        summary.message = L"映射目录不存在";
        return summary;
    }

    const HashMapTables tables = LoadHashMapTables(hashDirectory);
    summary.directoryEntries = static_cast<unsigned int>(tables.directories.size());
    summary.fileNameEntries = static_cast<unsigned int>(tables.files.size());
    summary.sources = tables.sources;
    summary.ok = !tables.directories.empty() || !tables.files.empty();
    summary.message = summary.ok ? std::wstring() : L"没有可用的 Hash 映射";
    return summary;
}

// ---------- 补充还原（候选挖掘） ----------

SupplementOutcome RunSupplementRestore(HashMapTables& tables, const SupplementOptions& options,
                                       const std::function<void(const SupplementProgress&)>& onProgress)
{
    SupplementOutcome outcome;
    outcome.attempted = true;

    AppendProcessLog(options.logDirectory, L"推理补充还原：开始读取 Hash Seed");

    // 1) 读盐。读不到不是异常，但**绝不能静默跳过** —— 用户勾了这个框，
    //    必须知道"为什么这次没补上"，否则会以为功能坏了。
    const Miner::SaltInfo salt = Miner::ReadSalt(options.logDirectory);
    outcome.saltAvailable = salt.available;
    outcome.saltSource = salt.source;
    if (!salt.available)
    {
        outcome.message = salt.message.empty() ? std::wstring(L"没有读到 Hash Seed，请先执行一次步骤2")
                                               : salt.message;
        AppendProcessLog(options.logDirectory, L"推理补充还原：盐不可用 —— " + outcome.message);
        return outcome;
    }

    // 2) 确定扫哪些包、排除哪些包。
    //
    //    排除名单（voice / voice2 / video）不是"不还原它们"，而是"不去它们里面挖名字"：
    //    那三个包是纯二进制（音频/视频），里面不会有明文资源名，扫它们只是白花时间。
    //    真正需要还原的 targets 仍然覆盖**整个**解包目录 —— 一个都不能漏，
    //    否则挖出来的名字会因为"目标集合里没有它"而被判成没命中。
    const std::vector<std::wstring> excludeNames{ L"voice", L"voice2", L"video" };

    std::vector<std::wstring> scanRoots;
    {
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(CombinePath(options.sourceDirectory, L"*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                const std::wstring name = data.cFileName;
                if (name == L"." || name == L"..")
                {
                    continue;
                }
                if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                {
                    continue;
                }
                if (IsSkippedLogComponent(name) || IsExcludedPackageName(excludeNames, name))
                {
                    continue;
                }

                const std::wstring full = CombinePath(options.sourceDirectory, name);
                // 输出目录可能在解包目录下。挖它等于把刚还原出去的资源当成候选来源，
                // 除了浪费还会让"连跑两次"的结果不稳定 —— 直接跳过。
                if (SameDirectory(full, options.outputDirectory))
                {
                    continue;
                }
                scanRoots.push_back(full);
            } while (::FindNextFileW(find, &data));
            ::FindClose(find);
        }
        std::sort(scanRoots.begin(), scanRoots.end());
    }
    outcome.scanRoots = static_cast<unsigned int>(scanRoots.size());

    // 3) 目标哈希集合：仍然是 64 位 Hash 名的文件（大写）。
    std::set<std::wstring> targets;
    CollectTargetHashes(options.sourceDirectory, targets);
    outcome.targetHashes = static_cast<unsigned int>(targets.size());

    Miner::Options minerOptions;
    minerOptions.scanRoots = scanRoots;
    minerOptions.excludeNames = excludeNames;
    minerOptions.maxCandidates = options.maxCandidates;

    // 进度回调与取消都要经过界面：内核不知道界面长什么样，这两个回调是唯一的通道。
    if (onProgress)
    {
        minerOptions.progress = [&onProgress](unsigned int files, unsigned int candidates, unsigned int hits)
        {
            SupplementProgress progress;
            progress.scannedFiles = files;
            progress.candidates = candidates;
            progress.hits = hits;
            onProgress(progress);
        };
    }
    // 取消同样只经回调：内核不知道界面长什么样，也不知道"窗口关了没有"。
    minerOptions.cancelRequested = options.cancelRequested;

    // 4) 挖。
    const Miner::Result mined = Miner::MineNames(salt, targets, minerOptions);
    outcome.ran = true;
    outcome.scannedFiles = mined.scannedFiles;
    outcome.candidateCount = mined.candidates;
    outcome.triedHashes = mined.triedHashes;
    outcome.hitCount = static_cast<unsigned int>(mined.files.size());
    outcome.limitHit = mined.limitHit;

    // 5) 并入映射表。只有内核返回的 files 会进表 —— 这里一条名字都不许自己造。
    outcome.newEntries = MergeMinedNames(tables, mined.files);

    // 6) 落盘。写不进去不影响本次还原（表已经补过了），但必须如实说出来。
    bool listOk = false;
    DWORD listError = 0;
    if (!options.minedListPath.empty())
    {
        DWORD directoryError = 0;
        const size_t separator = options.minedListPath.find_last_of(L"\\/");
        if (separator != std::wstring::npos)
        {
            EnsureDirectory(options.minedListPath.substr(0u, separator), directoryError);
        }
        listOk = WriteMinedNamesList(options.minedListPath, mined.files, outcome.writtenEntries, listError);
    }

    wchar_t summary[512]{};
    ::swprintf_s(summary, L"扫描 %s 个文件 / 候选 %s / 试算 %s / 命中 %s（新增 %s）",
                 FormatCount(outcome.scannedFiles).c_str(), FormatCount(outcome.candidateCount).c_str(),
                 FormatCount(outcome.triedHashes).c_str(), FormatCount(outcome.hitCount).c_str(),
                 FormatCount(outcome.newEntries).c_str());
    outcome.message = summary;
    if (outcome.limitHit)
    {
        outcome.message += L"；候选数触顶，结果不完整";
    }
    if (!outcome.ran)
    {
        // 走不到这里（盐不可用在上面就返回了），留着是为了将来加"提前中止"时语义不反转。
        outcome.message = L"未执行候选挖掘";
    }
    if (!options.minedListPath.empty())
    {
        if (listOk)
        {
            outcome.message += L"；已写入 " + std::to_wstring(outcome.writtenEntries) + L" 条到 MinedNames.lst";
        }
        else
        {
            wchar_t failed[256]{};
            ::swprintf_s(failed, L"；MinedNames.lst 写出失败（错误 %lu）", listError);
            outcome.message += failed;
        }
    }
    if (!mined.message.empty())
    {
        outcome.message += L"；内核说明：" + mined.message;
    }

    AppendProcessLog(options.logDirectory, L"推理补充还原：" + outcome.message);
    return outcome;
}
}
