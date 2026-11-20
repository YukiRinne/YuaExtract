// 扩展集「在线更新」的网络与内核层实现。接口与口径见 OnlineUpdate.h。
//
// 本文件只做三件事：把字节从 https 拿回来、把拿回来的东西按信任链验一遍、把基址配置读回来 /
// 写回去。**一次都不碰本地扩展集库** —— 入库是界面调 ExtensionSet::InstallToLibrary 的事。
// 下到的字节只落在 staging 目录里，任何一条校验不过就把临时文件删掉走人。
//
// 两句"不要"写在这里，免得后来的人顺手加上：
//   · 不要自动联网、不要后台轮询：只有用户点了「在线更新」才走到这里；
//   · 不要声称能验证“名字对不对”：常规哈希层不入档，本地没有独立哈希可对照，
//     信任根是索引。把某个名字改掉，本层发现不了，名字成不成立最终仍由实际还原结果判断。

#include "OnlineUpdate.h"

// 工具目录（ini 落点、staging 落点）与产品版本（UA）都从这里取：它们必须和界面认的是
// 同一个来源，不能各写一份常量。本文件与界面同属一个 exe，不新增任何模块依赖。
#include "YuaExtractHost.h"

#include "AtomicFile.h"
#include "PathSafety.h"
#include "OnlineHostPolicy.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

// 与 YuaExtractMainWindow.cpp 的 "#pragma comment(lib, \"shell32.lib\")" 同一手法：
// 链接项留在源码里，不动 vcxproj（同一时刻可能有别人在改工程文件）。
#pragma comment(lib, "winhttp.lib")

namespace Yua
{
namespace Online
{
    namespace
    {
        // =====================================================================
        // 1) 基础小工具（编码 / 路径 / 文件 / 环境变量）
        // =====================================================================
        // ExtensionSet.cpp 里有一套同样用途的匿名命名空间实现，但它在别的 TU 里、外面看不见；
        // 本层要能用，只能自己留一份小的。刻意不引第三方库，也不去动 Common 的公开接口。

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

        std::wstring ToLowerString(const std::wstring& value)
        {
            std::wstring out = value;
            for (wchar_t& ch : out)
            {
                ch = static_cast<wchar_t>(::towlower(ch));
            }
            return out;
        }

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

        // 逐级建目录（已存在算成功）。staging 目录与 %LOCALAPPDATA%\YuaExtract 都可能是好几级。
        bool EnsureDirectoryTree(const std::wstring& path, DWORD& error)
        {
            error = 0;
            if (path.empty())
            {
                return false;   // 空路径不是"成功"：调用方该报"没有可用的目录"
            }
            if (DirectoryExists(path))
            {
                return true;
            }

            std::wstring current;
            size_t index = 0u;
            if (path.size() >= 2u && path[1] == L':')
            {
                current = path.substr(0, 2u);   // 盘符前缀别拆坏
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
                    if (!::CreateDirectoryW(current.c_str(), nullptr))
                    {
                        const DWORD createError = ::GetLastError();
                        if (createError != ERROR_ALREADY_EXISTS)
                        {
                            error = createError;
                            return false;
                        }
                        // ERROR_ALREADY_EXISTS **不能一律当成功**：CON / NUL 这类保留设备名
                        // 也会回这个码，可目录根本没建出来。先当候选码记着，等最后复核。
                        if (error == 0u)
                        {
                            error = createError;
                        }
                    }
                }
                if (separator == std::wstring::npos)
                {
                    break;
                }
                index = separator + 1u;
            }

            // 复核：目录真的在才算成功（与 ExtensionSet.cpp 那份同一口径）。
            if (DirectoryExists(path))
            {
                error = 0u;
                return true;
            }
            if (error == 0u)
            {
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
            std::wstring offending;
            if (PathSafety::HasReparsePoint(path, &offending))
            {
                error = ERROR_CANT_ACCESS_FILE;
                return false;
            }

            // 共享读、不允许写；不跟随重解析点。
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
            if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0
                || !PathSafety::FitsSizeT(static_cast<uint64_t>(size.QuadPart)))
            {
                error = (::GetLastError() == 0u) ? ERROR_FILE_TOO_LARGE : ::GetLastError();
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
                DWORD read = 0u;
                if (!::ReadFile(file, out.data() + offset, chunk, &read, nullptr))
                {
                    error = ::GetLastError();
                    ::CloseHandle(file);
                    return false;
                }
                if (read == 0u)
                {
                    error = ERROR_HANDLE_EOF;   // 读短 = 文件在读取期间被截断
                    ::CloseHandle(file);
                    return false;
                }
                offset += read;
            }
            LARGE_INTEGER finalSize{};
            if (!::GetFileSizeEx(file, &finalSize) || finalSize.QuadPart != size.QuadPart)
            {
                error = ERROR_FILE_INVALID;     // 读取期间大小变了
                ::CloseHandle(file);
                return false;
            }
            ::CloseHandle(file);
            return true;
        }

        bool WriteAllBytes(const std::wstring& path, const void* data, size_t size, DWORD& error)
        {
            error = 0;
            HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = ::GetLastError();
                return false;
            }

            const unsigned char* bytes = static_cast<const unsigned char*>(data);
            size_t offset = 0u;
            while (offset < size)
            {
                const DWORD chunk = static_cast<DWORD>(
                    (size - offset) > 0x10000000u ? 0x10000000u : (size - offset));
                DWORD written = 0u;
                if (!::WriteFile(file, bytes + offset, chunk, &written, nullptr))
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

        // 索引是别人给的字节：非法 UTF-8 会被解码器悄悄替成 U+FFFD（名字变乱码、还照样"解析成功"）。
        // 与其让用户看到一串乱码会社名，不如当场说清楚是"不是合法 UTF-8"。
        bool IsValidUtf8(const std::vector<unsigned char>& bytes)
        {
            if (bytes.empty())
            {
                return true;   // 空索引是合法的（仓库刚开张，一条都没有）
            }
            const int need = ::MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS,
                reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()),
                nullptr, 0);
            return need > 0;
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

        std::wstring EnvironmentValue(const wchar_t* name)
        {
            wchar_t buffer[32768]{};
            const DWORD length = ::GetEnvironmentVariableW(name, buffer, _countof(buffer));
            if (length == 0u || length >= _countof(buffer))
            {
                return std::wstring();
            }
            return std::wstring(buffer, length);
        }

        bool SameHash(const std::wstring& left, const std::wstring& right)
        {
            return !left.empty() && !right.empty() && _wcsicmp(left.c_str(), right.c_str()) == 0;
        }

        // 切行（\r\n 与 \n 都认），**保留空行**：ini 整份重写时要把用户写的空行也带回去。
        std::vector<std::wstring> SplitLines(const std::wstring& text)
        {
            std::vector<std::wstring> lines;
            size_t start = 0u;
            while (start <= text.size())
            {
                size_t end = text.find_first_of(L"\r\n", start);
                if (end == std::wstring::npos)
                {
                    end = text.size();
                }
                lines.push_back(text.substr(start, end - start));
                if (end == text.size())
                {
                    break;
                }
                start = end;
                while (start < text.size() && (text[start] == L'\r' || text[start] == L'\n'))
                {
                    ++start;
                }
                if (start == text.size())
                {
                    break;   // 结尾那个换行不算多出一行
                }
            }
            return lines;
        }

        // =====================================================================
        // 2) 工具目录（ini 与 staging 落点都从它算）
        // =====================================================================

        // **工具目录只有这一个来源**：Host::LoaderDirectory()（= loader exe 所在目录）。
        // 界面的扩展集库也用同一个来源，两处必须一致，否则"库在这儿、配置在那儿"没法解释。
        std::wstring ToolDirectory()
        {
            return Host::LoaderDirectory();
        }

        // 探针式判可写：目录属性位判不出 ACL（Program Files 下的目录属性不带只读，
        // 但普通用户就是写不进去），所以只能真建一个文件试试。
        bool DirectoryWritable(const std::wstring& directory)
        {
            if (directory.empty())
            {
                return false;
            }
            const std::wstring probe = CombinePath(
                directory, L".yuaext_probe_" + FormatUInt(::GetCurrentProcessId()) + L".tmp");
            HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_TEMPORARY | FILE_ATTRIBUTE_HIDDEN, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                return false;
            }
            ::CloseHandle(file);
            ::DeleteFileW(probe.c_str());
            return true;
        }

        // 已经有了的 ini 就试开它；没有就在目录里放一个探针文件。
        bool CanWriteConfig(const std::wstring& path)
        {
            if (FileExists(path))
            {
                HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE)
                {
                    return false;
                }
                ::CloseHandle(file);
                return true;
            }
            return DirectoryWritable(ParentDirectory(path));
        }

        // =====================================================================
        // 3) ini 的 [online] base=
        // =====================================================================

        // ini 是我们自己按 UTF-8（带 BOM）写的，用户也可能拿记事本另存成别的编码。
        // 只认这几种，其余按 UTF-8 解（与工程里其它读文本的地方同口径）。
        std::wstring DecodeIniText(const std::vector<unsigned char>& bytes)
        {
            if (bytes.size() >= 2u && bytes[0] == 0xFFu && bytes[1] == 0xFEu)
            {
                std::wstring out;
                out.reserve(bytes.size() / 2u);
                for (size_t i = 2u; i + 1u < bytes.size(); i += 2u)
                {
                    out.push_back(static_cast<wchar_t>(static_cast<unsigned int>(bytes[i])
                                                       | (static_cast<unsigned int>(bytes[i + 1u]) << 8)));
                }
                return out;
            }
            if (bytes.size() >= 2u && bytes[0] == 0xFEu && bytes[1] == 0xFFu)
            {
                std::wstring out;
                out.reserve(bytes.size() / 2u);
                for (size_t i = 2u; i + 1u < bytes.size(); i += 2u)
                {
                    out.push_back(static_cast<wchar_t>(
                        (static_cast<unsigned int>(bytes[i]) << 8) | bytes[i + 1u]));
                }
                return out;
            }
            size_t offset = 0u;
            if (bytes.size() >= 3u && bytes[0] == 0xEFu && bytes[1] == 0xBBu && bytes[2] == 0xBFu)
            {
                offset = 3u;
            }
            return Utf8ToWide(reinterpret_cast<const char*>(bytes.data() + offset),
                              bytes.size() - offset);
        }

        // 只在 [online] 段里找第一处 `base=`。段名与键名大小写不敏感（ini 的老规矩）。
        bool ReadIniBase(const std::wstring& path, std::wstring& value)
        {
            value.clear();
            if (path.empty() || !FileExists(path))
            {
                return false;   // 没有 ini = 这一级没命中，不算错误
            }
            std::vector<unsigned char> bytes;
            DWORD readError = 0;
            if (!ReadAllBytes(path, bytes, readError))
            {
                return false;
            }

            bool inOnline = false;
            for (const std::wstring& raw : SplitLines(DecodeIniText(bytes)))
            {
                const std::wstring line = TrimSpaces(raw);
                if (line.empty() || line.front() == L';' || line.front() == L'#')
                {
                    continue;
                }
                if (line.size() >= 2u && line.front() == L'[' && line.back() == L']')
                {
                    inOnline = (ToLowerString(line) == L"[online]");
                    continue;
                }
                if (!inOnline)
                {
                    continue;
                }
                const size_t equals = line.find(L'=');
                if (equals == std::wstring::npos)
                {
                    continue;
                }
                if (ToLowerString(TrimSpaces(line.substr(0u, equals))) != L"base")
                {
                    continue;
                }
                const std::wstring parsed = TrimSpaces(line.substr(equals + 1u));
                if (!parsed.empty())
                {
                    value = parsed;
                    return true;
                }
            }
            return false;
        }

        // 只改 [online] 段的 base=：别的段、注释、空行原样留着 —— 用户手写的 ini 不该被我们
        // 顺手抹掉。段不存在就追加，段在但没有 base= 就在段尾补一行。
        std::vector<std::wstring> ReplaceIniBase(const std::vector<std::wstring>& lines,
                                                 const std::wstring& value)
        {
            const std::wstring baseLine = L"base=" + value;
            std::vector<std::wstring> out;
            out.reserve(lines.size() + 3u);

            bool inOnline = false;
            bool sawOnline = false;
            bool wrote = false;
            for (const std::wstring& raw : lines)
            {
                const std::wstring line = TrimSpaces(raw);
                if (line.size() >= 2u && line.front() == L'[' && line.back() == L']')
                {
                    if (inOnline && !wrote)
                    {
                        out.push_back(baseLine);   // 段尾补一行
                        wrote = true;
                    }
                    inOnline = (ToLowerString(line) == L"[online]");
                    sawOnline = sawOnline || inOnline;
                    out.push_back(raw);
                    continue;
                }
                if (inOnline && !wrote)
                {
                    const size_t equals = line.find(L'=');
                    if (equals != std::wstring::npos
                        && ToLowerString(TrimSpaces(line.substr(0u, equals))) == L"base")
                    {
                        out.push_back(baseLine);
                        wrote = true;
                        continue;
                    }
                }
                out.push_back(raw);
            }
            if (inOnline && !wrote)
            {
                out.push_back(baseLine);
                wrote = true;
            }
            if (!sawOnline)
            {
                while (!out.empty() && TrimSpaces(out.back()).empty())
                {
                    out.pop_back();   // 空文件就别留一串空行在前面
                }
                if (!out.empty())
                {
                    out.push_back(std::wstring());
                }
                out.push_back(L"[online]");
                out.push_back(baseLine);
            }
            return out;
        }

        bool IsSourceIniKey(const std::wstring& trimmedLine)
        {
            const size_t equals = trimmedLine.find(L'=');
            if (equals == std::wstring::npos)
            {
                return false;
            }
            const std::wstring key = ToLowerString(TrimSpaces(trimmedLine.substr(0u, equals)));
            if (key == L"source_count")
            {
                return true;
            }
            if (key.compare(0u, 7u, L"source_") != 0)
            {
                return false;
            }
            size_t pos = 7u;
            bool digits = false;
            while (pos < key.size() && key[pos] >= L'0' && key[pos] <= L'9')
            {
                digits = true;
                ++pos;
            }
            if (!digits || pos >= key.size() || key[pos] != L'_')
            {
                return false;
            }
            const std::wstring field = key.substr(pos + 1u);
            return field == L"name" || field == L"prefix" || field == L"enabled";
        }

        std::vector<std::wstring> ReplaceIniOnlineSources(const std::vector<std::wstring>& lines,
                                                          const std::vector<DownloadSource>& sources)
        {
            std::vector<std::wstring> sourceLines;
            sourceLines.push_back(L"source_count=" + FormatUInt(sources.size()));
            for (size_t i = 0u; i < sources.size(); ++i)
            {
                const std::wstring idx = L"source_" + FormatUInt(i) + L"_";
                sourceLines.push_back(idx + L"name=" + sources[i].name);
                sourceLines.push_back(idx + L"prefix=" + sources[i].prefix);
                sourceLines.push_back(idx + L"enabled=" + (sources[i].enabled ? L"1" : L"0"));
            }

            std::vector<std::wstring> out;
            out.reserve(lines.size() + sourceLines.size() + 3u);
            bool inOnline = false;
            bool sawOnline = false;
            bool wrote = false;
            for (const std::wstring& raw : lines)
            {
                const std::wstring line = TrimSpaces(raw);
                if (line.size() >= 2u && line.front() == L'[' && line.back() == L']')
                {
                    if (inOnline && !wrote)
                    {
                        out.insert(out.end(), sourceLines.begin(), sourceLines.end());
                        wrote = true;
                    }
                    inOnline = (ToLowerString(line) == L"[online]");
                    sawOnline = sawOnline || inOnline;
                    out.push_back(raw);
                    continue;
                }
                if (inOnline && IsSourceIniKey(line))
                {
                    continue; // 移除旧 source_* 键，保持其余配置（如 base=）不变
                }
                out.push_back(raw);
            }
            if (inOnline && !wrote)
            {
                out.insert(out.end(), sourceLines.begin(), sourceLines.end());
                wrote = true;
            }
            if (!sawOnline)
            {
                while (!out.empty() && TrimSpaces(out.back()).empty())
                {
                    out.pop_back();
                }
                if (!out.empty())
                {
                    out.push_back(std::wstring());
                }
                out.push_back(L"[online]");
                out.insert(out.end(), sourceLines.begin(), sourceLines.end());
            }
            return out;
        }

        // =====================================================================
        // 4) HTTP（WinHTTP）
        // =====================================================================

        // 句柄的 RAII：下面有十几处提前 return，手写 WinHttpCloseHandle 迟早漏一个。
        struct InternetHandle
        {
            HINTERNET value = nullptr;

            InternetHandle() = default;
            explicit InternetHandle(HINTERNET handle) : value(handle) {}
            InternetHandle(const InternetHandle&) = delete;
            InternetHandle& operator=(const InternetHandle&) = delete;
            // 可移动：WinHttpOpen 退回默认代理策略时要在原变量上重来一次。
            InternetHandle(InternetHandle&& other) noexcept : value(other.value)
            {
                other.value = nullptr;
            }
            InternetHandle& operator=(InternetHandle&& other) noexcept
            {
                if (this != &other)
                {
                    if (value != nullptr)
                    {
                        ::WinHttpCloseHandle(value);
                    }
                    value = other.value;
                    other.value = nullptr;
                }
                return *this;
            }
            ~InternetHandle()
            {
                if (value != nullptr)
                {
                    ::WinHttpCloseHandle(value);
                }
            }

            HINTERNET get() const { return value; }
            explicit operator bool() const { return value != nullptr; }
        };

        // UA 标注产品与版本（§14.4 第 1 条）：仓库侧看访问日志时要能认出是谁在拉。
        std::wstring UserAgent()
        {
            return std::wstring(Host::kProductName) + L"/" + Host::kProductVersion;
        }

        // 前缀是不是 "https://"（大小写不敏感），**不判主机名在不在**。
        // 报错分层要用它：「https://」这种确实**是** https 地址，只是不完整，不能对它说
        // "它不是 https 的地址"（独立验证 N-P2）。
        bool HasHttpsScheme(const std::wstring& url)
        {
            constexpr const wchar_t kPrefix[] = L"https://";
            if (url.size() < 8u)
            {
                return false;
            }
            for (size_t i = 0u; i < 8u; ++i)
            {
                if (::towlower(url[i]) != kPrefix[i])
                {
                    return false;
                }
            }
            return true;
        }

        // 唯一允许的协议（§14.4 第 1 条）。大小写不敏感，主机名不能为空。
        bool StartsWithHttps(const std::wstring& url)
        {
            return HasHttpsScheme(url) && url.size() > 8u && url[8] != L'/' && url[8] != L'\\';
        }

        // 路径部分的百分号转义：日文会社 / 作品名与空格**必须**转义后再发出去，
        // 否则请求行里会出现非 ASCII 字节，很多服务器直接 400。
        //   · '/' 与 ':' 保持原样（'/' 是路径分隔符；':' 允许镜像代理如 /https://github.com/...）；
        //   · '%' 也保持原样 —— 用户配的镜像基址可能本来就带 %20 这种转义，再转一次会把地址弄坏。
        std::wstring EscapeUrlPath(const std::wstring& path)
        {
            const std::string utf8 = WideToUtf8(path);
            constexpr const char kHex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(utf8.size() + 16u);
            for (unsigned char ch : utf8)
            {
                const bool safe = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
                    || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_'
                    || ch == '~' || ch == '/' || ch == '%' || ch == ':';
                if (safe)
                {
                    out.push_back(static_cast<char>(ch));
                }
                else
                {
                    out.push_back('%');
                    out.push_back(kHex[ch >> 4]);
                    out.push_back(kHex[ch & 0x0Fu]);
                }
            }
            return Utf8ToWide(out);
        }

        bool EndsWithJsonSuffix(const std::wstring& url)
        {
            const std::wstring lower = ToLowerString(TrimSpaces(url));
            return lower.size() > 13u && lower.compare(lower.size() - 5u, 5u, L".json") == 0;
        }

        std::wstring RedactUrlForError(const std::wstring& url)
        {
            const size_t cut = url.find_first_of(L"?#");
            return cut == std::wstring::npos ? url : (url.substr(0u, cut) + L"?...");
        }

        struct UrlParts
        {
            std::wstring host;
            INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
            std::wstring path;   // 已转义，以 '/' 开头（preserveQuery=true 时可带原样 ?query）
        };

        // 自己拆 URL（而不是 WinHttpCrackUrl）：一是要在拆的时候就断掉非 https，
        // 二是只对**路径**做转义、主机与端口一个字节不改。主机名支持 [IPv6] 字面量。
        bool ParseHttpsUrl(const std::wstring& url, UrlParts& out, bool preserveQuery, std::wstring& error)
        {
            if (url.empty())
            {
                error = L"地址是空的。";
                return false;
            }
            if (!StartsWithHttps(url))
            {
                error = L"只允许 https 地址：\"" + RedactUrlForError(url) + L"\" 不是 https:// 开头的。";
                return false;
            }
            for (wchar_t ch : url)
            {
                if (ch <= L' ')
                {
                    error = L"地址里包含空白或控制字符。";
                    return false;
                }
            }

            size_t slash = url.find(L'/', 8u);
            if (slash == std::wstring::npos)
            {
                slash = url.size();
            }
            const std::wstring authority = url.substr(8u, slash - 8u);
            std::wstring path = slash < url.size() ? url.substr(slash) : std::wstring(L"/");

            const size_t hashCut = path.find(L'#');
            if (hashCut != std::wstring::npos)
            {
                path.erase(hashCut);
            }
            std::wstring query;
            const size_t queryCut = path.find(L'?');
            if (queryCut != std::wstring::npos)
            {
                if (preserveQuery)
                {
                    query = path.substr(queryCut);
                }
                path.erase(queryCut);
            }

            out = UrlParts();
            size_t portStart = std::wstring::npos;
            if (!authority.empty() && authority.front() == L'[')
            {
                const size_t close = authority.find(L']');
                if (close == std::wstring::npos)
                {
                    error = L"地址里的 IPv6 字面量没有收尾的 ']'。";
                    return false;
                }
                out.host = authority.substr(0u, close + 1u);
                if (close + 1u < authority.size() && authority[close + 1u] == L':')
                {
                    portStart = close + 2u;
                }
            }
            else
            {
                const size_t colon = authority.find(L':');
                if (colon == std::wstring::npos)
                {
                    out.host = authority;
                }
                else
                {
                    out.host = authority.substr(0u, colon);
                    portStart = colon + 1u;
                }
            }

            if (portStart != std::wstring::npos)
            {
                unsigned long long port = 0ull;
                if (!ParseUInt(authority.substr(portStart), port) || port == 0ull || port > 65535ull)
                {
                    error = L"地址里的端口不合法。";
                    return false;
                }
                out.port = static_cast<INTERNET_PORT>(port);
            }
            if (out.host.empty())
            {
                error = L"地址里没有主机名。";
                return false;
            }

            out.path = EscapeUrlPath(path);
            if (out.path.empty() || out.path.front() != L'/')
            {
                out.path.insert(out.path.begin(), L'/');
            }
            if (!query.empty())
            {
                out.path += query;
            }
            return true;
        }

        bool ParseHttpsUrl(const std::wstring& url, UrlParts& out, std::wstring& error)
        {
            return ParseHttpsUrl(url, out, false, error);
        }

        // 基址的来路。诊断措辞里要说清"这个地址是从哪来的" —— "改了 ini 却没生效"这类问题
        // 全靠这一句，不然用户只能对着一个地址发呆。
        enum class BaseOrigin
        {
            Caller = 0,       // 调用方传进来的（多半是用户刚填的，也可能已被上游归一化成空串）
            Environment = 1,  // 环境变量 YUAEXT_ENDPOINT
            Ini = 2,          // YuaExtract.ini 的 [online] base=
            Constant = 3      // 内置常量 kDefaultRepositoryBase
        };

        struct ConfiguredBase
        {
            std::wstring raw;                        // 原文（未归一化）
            BaseOrigin origin = BaseOrigin::Caller;
        };

        const wchar_t* DescribeBaseOrigin(BaseOrigin origin)
        {
            switch (origin)
            {
                case BaseOrigin::Environment: return L"环境变量 YUAEXT_ENDPOINT";
                case BaseOrigin::Ini:         return L"YuaExtract.ini 的 [online] base=";
                case BaseOrigin::Constant:    return L"内置常量 kDefaultRepositoryBase";
                case BaseOrigin::Caller:      break;
            }
            return L"";
        }

        // §14.5 的解析顺序（先命中先用）：环境变量 -> ini -> 内置常量。
        // 这里只**读原文**，不归一化 —— 归一化会把"非 https"折成空串，那是
        // ResolveRepositoryBase 的事；诊断要的恰恰是没被折过的原文。
        ConfiguredBase ReadConfiguredBase()
        {
            ConfiguredBase configured;

            const std::wstring fromEnvironment = EnvironmentValue(L"YUAEXT_ENDPOINT");
            if (!fromEnvironment.empty())
            {
                configured.raw = fromEnvironment;
                configured.origin = BaseOrigin::Environment;
                return configured;
            }

            std::wstring fromIni;
            if (ReadIniBase(ConfigFilePath(), fromIni) && !fromIni.empty())
            {
                configured.raw = fromIni;
                configured.origin = BaseOrigin::Ini;
                return configured;
            }

            configured.raw = kDefaultRepositoryBase;
            configured.origin = BaseOrigin::Constant;
            return configured;
        }

        // "还没配过地址"的两种样子（2026-10-15 加）：
        //   1. 还留着内置常量里的 <owner> / <repo> 占位符 —— 正常用户第一次打开就是这个；
        //   2. 只填到了主机名、路径为空 —— 但**只对** raw.githubusercontent.com 这么判：
        //      自建镜像的根目录是合法落点（index.txt 可以就放在根上），不能一律拒。
        bool IsPlaceholderBase(const std::wstring& base)
        {
            const std::wstring lower = ToLowerString(base);
            if (lower.find(L"<owner>") != std::wstring::npos
                || lower.find(L"<repo>") != std::wstring::npos)
            {
                return true;
            }

            size_t start = 0u;
            if (lower.size() >= 8u && lower.compare(0u, 8u, L"https://") == 0)
            {
                start = 8u;
            }
            const size_t slash = lower.find(L'/', start);
            const std::wstring host =
                slash == std::wstring::npos ? lower.substr(start) : lower.substr(start, slash - start);
            const std::wstring path = slash == std::wstring::npos ? std::wstring() : lower.substr(slash);
            bool pathEmpty = true;
            for (wchar_t ch : path)
            {
                if (ch != L'/')
                {
                    pathEmpty = false;
                    break;
                }
            }
            return host == L"raw.githubusercontent.com" && pathEmpty;
        }

        // URL 里不允许出现的字符。'%' 不判 —— 它本来就是转义引导符，基址里可能带 %20 这种。
        bool IsUrlSafe(const std::wstring& url)
        {
            for (wchar_t ch : url)
            {
                if (ch <= L' ' || ch == L'<' || ch == L'>' || ch == L'"' || ch == L'`' || ch == L'{'
                    || ch == L'}' || ch == L'|' || ch == L'\\' || ch == L'^')
                {
                    return false;
                }
            }
            return true;
        }

        // 入口共用的基址校验（FetchIndex / DownloadArchive / SetRepositoryBase 都走它）：
        // 能不能用、不能用的话**为什么**，必须说清，而且**引用的一定要是用户真正写下的那个
        // 字符串**（§14.6：每种失败各有各的措辞；独立验证 P-2：非 https 时那句"地址也不能为空"
        // 与事实相反，因为用户填的是 ftp://…）。
        //
        // rawBase 的语义：调用方手里的原文。它**可能已经被上游归一化成空串**（界面「仓库地址」
        // 那个框显示的值来自 ResolveRepositoryBase，非 https 在那里就成了空串），所以为空时
        // 再去环境变量与 ini 里回捞一次原文 —— 只为了把措辞说准，不改变任何判定结果。
        bool ResolveUsableBase(const std::wstring& rawBase, std::wstring& normalized,
                              std::wstring& error)
        {
            std::wstring raw = TrimSpaces(rawBase);
            BaseOrigin origin = BaseOrigin::Caller;
            if (raw.empty())
            {
                const ConfiguredBase configured = ReadConfiguredBase();
                raw = TrimSpaces(configured.raw);
                origin = configured.origin;
            }
            else if (raw == kDefaultRepositoryBase)
            {
                // 传进来的正好是内置常量（界面把 ResolveRepositoryBase 的结果原样递了过来）：
                // 点明出处，用户才知道该去改哪儿（常量、ini、还是环境变量）。
                origin = BaseOrigin::Constant;
            }

            const wchar_t* originLabel = DescribeBaseOrigin(origin);
            const std::wstring originNote = *originLabel == L'\0'
                ? std::wstring()
                : (std::wstring(L"\r\n出处：") + originLabel);

            // ① 地址为空（连内置常量都没拿到 —— 实际只会在调用方传空、且配置里也没写东西时出现）。
            if (raw.empty())
            {
                error = L"没拿到基址：地址是空的。\r\n"
                        L"本工具只在 https 上拉扩展集。请在「在线更新」窗口里填一次仓库地址，"
                        L"或者设置环境变量 YUAEXT_ENDPOINT。\r\n"
                        L"（如果你确实填过地址却看到这句，那说明那个值被判成了不可用 —— "
                        L"只接受 https:// 开头的地址。）" + originNote;
                return false;
            }

            normalized = NormalizeBase(raw);

            // ② 地址非空、但不是能用的 https 地址：**把用户填的原文引出来**，不许印成空串。
            //    再分一层：「https://」「https:///x」这种**确实是 https 地址**，只是不完整
            //    （没有主机名），对它说"它不是 https 的地址"是**事实错误**（独立验证 N-P2）。
            if (normalized.empty())
            {
                if (HasHttpsScheme(raw))
                {
                    error = L"基址不完整：\"" + raw + L"\" 确实是 https 的地址，但缺少主机名，"
                            L"拼不出能用的 URL。\r\n"
                            L"要写成 https://<主机名>/<路径>/ 的样子，例如 "
                            L"https://raw.githubusercontent.com/<owner>/<repo>/main/ —— "
                            L"本工具只在 https 上拉扩展集。" + originNote;
                }
                else
                {
                    error = L"基址不可用：只允许 https:// 开头的地址，你填的是 \"" + raw
                          + L"\" —— 它不是 https 的地址。\r\n"
                            L"http / ftp 都是明文，内容中途谁都能改，本工具只在 https 上拉扩展集。"
                          + originNote;
                }
                return false;
            }

            // ③ 其他不可用：还是内置占位符；或者 URL 里带了不允许出现的字符。
            if (IsPlaceholderBase(normalized))
            {
                // 占位符单独说：报成"地址不合法 / 连不上"，用户不知道该干什么。
                // 最后那句是给"刚在窗口里填了地址、点保存又被挡回来"的人看的：到这一步，
                // 用户填的原文可能已经被上游归一化成空串了（**我们这里捞不回来**），
                // 与其印一句"你填的是空的"，不如把最可能的原因直接摆出来。
                error = L"还没配置扩展集仓库地址：请先设置环境变量 YUAEXT_ENDPOINT，"
                        L"或在「在线更新」窗口里填一次仓库地址。\r\n"
                        L"（当前基址还是内置常量里的占位符：" + normalized + L"）\r\n"
                        L"如果你刚在窗口里填过地址：只接受 https:// 开头的，http / ftp 一律拒绝。";
                return false;
            }
            if (!IsUrlSafe(normalized))
            {
                error = L"基址里不可以出现这些字符（空格、< > \" ` { } | \\ ^ 之类）：\"" + raw
                      + L"\"。" + originNote;
                return false;
            }
            return true;
        }

        // 失败措辞**一种一条**（§14.6）：DNS 解析不了、连不上、TLS 过不去、超时，
        // 对用户来说要做的事完全不同（改域名 / 换镜像 / 修系统时间 / 过会儿再试），
        // 糊成一句"更新失败"就什么都查不出来了。
        std::wstring DescribeWinHttpFailure(const std::wstring& stage, const std::wstring& url,
                                            DWORD code)
        {
            switch (code)
            {
                case ERROR_WINHTTP_TIMEOUT:
                    return stage + L"失败：等不到对方的回应（超时：连接 "
                         + FormatUInt(kConnectTimeoutMs) + L" ms / 发送 " + FormatUInt(kSendTimeoutMs)
                         + L" ms / 接收 " + FormatUInt(kReceiveTimeoutMs)
                         + L" ms）。地址：" + url + L"\r\n网络慢或这个基址不通，可以换一个镜像再试。";
                case ERROR_WINHTTP_NAME_NOT_RESOLVED:
                    return stage + L"失败：DNS 解析不了主机名（域名写错了，或本机的 DNS 查不到）。地址："
                         + url;
                case ERROR_WINHTTP_CANNOT_CONNECT:
                    return stage + L"失败：连不上服务器（对方拒绝连接，或本地防火墙 / 安全软件拦了，"
                         L"也可能是需要走代理）。地址：" + url;
                case ERROR_WINHTTP_SECURE_FAILURE:
                    return stage + L"失败：TLS 握手没通过（证书链不受信任、证书过期，或系统时间不对）。"
                         L"地址：" + url;
                case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED:
                    return stage + L"失败：对方要求客户端证书。地址：" + url;
                case ERROR_WINHTTP_CONNECTION_ERROR:
                    return stage + L"失败：连接被中断（中途断了，可能是网络不稳或对方掐了连接）。地址："
                         + url;
                case ERROR_WINHTTP_INVALID_URL:
                    return stage + L"失败：地址不合法。地址：" + url;
                case ERROR_WINHTTP_UNRECOGNIZED_SCHEME:
                    return stage + L"失败：地址里的协议认不出来（只支持 https）。地址：" + url;
                case ERROR_WINHTTP_OPERATION_CANCELLED:
                    return stage + L"失败：请求被取消。地址：" + url;
                default:
                    return stage + L"失败（Win32 错误码 " + FormatUInt(code) + L"）。地址：" + url;
            }
        }

        // 读一个响应头当纯文本（只用于把 "对方指向哪儿" 印给用户看）。
        // 头里的换行会让人误以为是两句提示，压平；过长截断。
        std::wstring QueryHeaderText(HINTERNET handle, DWORD query)
        {
            DWORD size = 0u;
            ::WinHttpQueryHeaders(handle, query, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                                  WINHTTP_NO_HEADER_INDEX);
            if (size == 0u)
            {
                return std::wstring();
            }
            std::wstring value(static_cast<size_t>(size) / sizeof(wchar_t), L'\0');
            if (!::WinHttpQueryHeaders(handle, query, WINHTTP_HEADER_NAME_BY_INDEX, &value[0], &size,
                                       WINHTTP_NO_HEADER_INDEX))
            {
                return std::wstring();
            }
            value.resize(static_cast<size_t>(size) / sizeof(wchar_t));
            for (wchar_t& ch : value)
            {
                if (ch == L'\r' || ch == L'\n' || ch == L'\t')
                {
                    ch = L' ';
                }
            }
            while (!value.empty() && value.back() == L'\0')
            {
                value.pop_back();
            }
            value = TrimSpaces(value);
            if (value.size() > 200u)
            {
                value.resize(200u);
                value += L"…";
            }
            return value;
        }

        // 非 200 的措辞也一种一条：404 是"仓库里没这个东西"、3xx 是"对方要重定向而我们不跟"、
        // 5xx 是"对方自己出问题"。这三件事用户要做的事完全不一样。
        std::wstring DescribeHttpStatus(const std::wstring& stage, const std::wstring& url,
                                        HINTERNET handle, DWORD status)
        {
            if (status >= 300u && status < 400u)
            {
                std::wstring message = stage + L"失败：服务器要求重定向（HTTP " + FormatUInt(status)
                                     + L"），本工具不跟随重定向，已中止。地址：" + url;
                const std::wstring location = QueryHeaderText(handle, WINHTTP_QUERY_LOCATION);
                if (!location.empty())
                {
                    // 这是**对方给的内容**，只当提示原样印出来，绝不拿它去跳转（§14.4 第 1 条）。
                    message += L"\r\n对方指向：" + location;
                }
                return message;
            }
            if (status == 404u)
            {
                return stage + L"失败：服务器上没有这个文件（HTTP 404）。地址：" + url
                     + L"\r\n可能是仓库里还没放这份东西，或者基址多写了 / 漏写了尾部路径。";
            }
            if (status == 403u)
            {
                return stage + L"失败：服务器拒绝访问（HTTP 403）。地址：" + url
                     + L"\r\n若基址指向私有仓库，匿名拉取会被拒。";
            }
            if (status >= 500u)
            {
                return stage + L"失败：对方服务器出错（HTTP " + FormatUInt(status)
                     + L"），不是本工具的问题，稍后再试。地址：" + url;
            }
            return stage + L"失败：服务器返回 HTTP " + FormatUInt(status) + L"（不是 200）。地址：" + url;
        }

        // 关掉重定向。**必须成功**：跟随重定向的话"仅 https"就形同虚设 ——
        // 首跳 https、次跳 http 一样能把明文内容灌进来（§14.4 第 1 条）。
        bool DisableRedirects(HINTERNET handle, DWORD& code)
        {
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            if (::WinHttpSetOption(handle, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy)))
            {
                code = 0u;
                return true;
            }
            code = ::GetLastError();
            return false;
        }

        // 允许安全的受限 HTTPS→HTTPS 重定向（用于 GitHub Releases 302 跳转到 objects.githubusercontent.com / CDN），
        // 但始终关闭 WinHTTP 自动重定向，每一跳都重新校 https:// + 公网主机策略，绝不降级到 http 或跳到私网。
        std::wstring QueryRedirectLocation(HINTERNET handle)
        {
            DWORD size = 0u;
            ::WinHttpQueryHeaders(handle, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, nullptr,
                                  &size, WINHTTP_NO_HEADER_INDEX);
            if (size == 0u || size > 16384u)
            {
                return std::wstring();
            }
            std::wstring value(static_cast<size_t>(size) / sizeof(wchar_t), L'\0');
            if (!::WinHttpQueryHeaders(handle, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                       &value[0], &size, WINHTTP_NO_HEADER_INDEX))
            {
                return std::wstring();
            }
            value.resize(static_cast<size_t>(size) / sizeof(wchar_t));
            while (!value.empty() && value.back() == L'\0')
            {
                value.pop_back();
            }
            return TrimSpaces(value);
        }

        bool ResolveRedirectTarget(const UrlParts& current, const std::wstring& location,
                                   std::wstring& nextUrl, std::wstring& error)
        {
            nextUrl.clear();
            const std::wstring loc = TrimSpaces(location);
            if (loc.empty())
            {
                error = L"服务器返回重定向但未给出 Location 头。";
                return false;
            }
            if (loc.size() >= 7u && ToLowerString(loc.substr(0u, 7u)) == L"http://")
            {
                error = L"重定向试图降级到明文 http://，出于安全已中止。";
                return false;
            }
            if (loc.size() >= 2u && loc[0] == L'/' && loc[1] != L'/')
            {
                nextUrl = L"https://" + current.host;
                if (current.port != INTERNET_DEFAULT_HTTPS_PORT)
                {
                    nextUrl += L":" + FormatUInt(current.port);
                }
                nextUrl += loc;
                return true;
            }
            if (loc.size() >= 2u && loc[0] == L'/' && loc[1] == L'/')
            {
                nextUrl = L"https:" + loc;
                return true;
            }
            if (!StartsWithHttps(loc))
            {
                error = L"不支持非 https 或相对路径重定向。";
                return false;
            }
            nextUrl = loc;
            return true;
        }

        struct HttpResponse
        {
            DWORD status = 0u;   // 无论成败都填：调用方要能区分"没连上"和"连上了但不是 200"
            std::vector<unsigned char> body;
        };

        bool HttpGet(const std::wstring& url, unsigned long long maxBytes, const std::wstring& stage,
                     const Request& request, HttpResponse& response, std::wstring& error)
        {
            error.clear();
            response = HttpResponse();

            InternetHandle session(::WinHttpOpen(UserAgent().c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
            if (!session)
            {
                session = InternetHandle(::WinHttpOpen(UserAgent().c_str(),
                                                       WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                                       0));
            }
            if (!session)
            {
                error = DescribeWinHttpFailure(stage, RedactUrlForError(url), ::GetLastError());
                return false;
            }

            ::WinHttpSetTimeouts(session.get(), kConnectTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
                                 kReceiveTimeoutMs);
            DWORD optionError = 0u;
            DisableRedirects(session.get(), optionError);

            std::wstring currentUrl = url;
            for (int hop = 0; hop <= kMaxRedirects; ++hop)
            {
                const std::wstring safeDisplayUrl = RedactUrlForError(currentUrl);
                UrlParts parts;
                if (!ParseHttpsUrl(currentUrl, parts, hop > 0, error))
                {
                    return false;
                }

                // 每一跳都要校验主机策略，防止 302 跳转把连接引向 localhost / 私网。
                if (HostIsBlocked(parts.host, error)
                    || (!HostIsNumericAddress(parts.host) && ResolveBlocked(parts.host, error)))
                {
                    error = L"拒绝连接 \"" + parts.host + L"\"：" + error
                          + L"本工具只访问公网 https 地址，不访问本机 / 内网 / 保留段。";
                    return false;
                }

                if (request.cancel && request.cancel())
                {
                    error = L"已取消。";
                    return false;
                }

                InternetHandle connection(::WinHttpConnect(session.get(), parts.host.c_str(), parts.port, 0));
                if (!connection)
                {
                    error = DescribeWinHttpFailure(stage, safeDisplayUrl, ::GetLastError());
                    return false;
                }

                InternetHandle httpRequest(::WinHttpOpenRequest(connection.get(), L"GET", parts.path.c_str(),
                                                                nullptr, WINHTTP_NO_REFERER,
                                                                WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                                WINHTTP_FLAG_SECURE));
                if (!httpRequest)
                {
                    error = DescribeWinHttpFailure(stage, safeDisplayUrl, ::GetLastError());
                    return false;
                }

                if (!DisableRedirects(httpRequest.get(), optionError))
                {
                    error = stage + L"失败：关不掉 HTTP 重定向（Win32 错误码 " + FormatUInt(optionError)
                          + L"）。本工具只走 https，宁可不下载。地址：" + safeDisplayUrl;
                    return false;
                }

                if (!::WinHttpSendRequest(httpRequest.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
                {
                    error = DescribeWinHttpFailure(stage, safeDisplayUrl, ::GetLastError());
                    return false;
                }
                if (!::WinHttpReceiveResponse(httpRequest.get(), nullptr))
                {
                    error = DescribeWinHttpFailure(stage, safeDisplayUrl, ::GetLastError());
                    return false;
                }

                DWORD status = 0u;
                DWORD statusSize = sizeof(status);
                if (!::WinHttpQueryHeaders(httpRequest.get(),
                                           WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                           WINHTTP_NO_HEADER_INDEX))
                {
                    error = DescribeWinHttpFailure(stage, safeDisplayUrl, ::GetLastError());
                    return false;
                }
                response.status = status;

                if (status == 301u || status == 302u || status == 303u || status == 307u || status == 308u)
                {
                    if (hop >= kMaxRedirects)
                    {
                        error = stage + L"失败：HTTPS 重定向次数超过上限（" + FormatUInt(kMaxRedirects)
                              + L" 跳）。地址：" + safeDisplayUrl;
                        return false;
                    }
                    const std::wstring location = QueryRedirectLocation(httpRequest.get());
                    std::wstring nextUrl;
                    if (!ResolveRedirectTarget(parts, location, nextUrl, error))
                    {
                        error = stage + L"失败：" + error + L" 原地址：" + safeDisplayUrl;
                        return false;
                    }
                    currentUrl = std::move(nextUrl);
                    continue;
                }

                if (status != 200u)
                {
                    error = DescribeHttpStatus(stage, safeDisplayUrl, httpRequest.get(), status);
                    return false;
                }

                std::vector<unsigned char> body;
                for (;;)
                {
                    if (request.cancel && request.cancel())
                    {
                        error = L"已取消。";
                        return false;
                    }

                    DWORD available = 0u;
                    if (!::WinHttpQueryDataAvailable(httpRequest.get(), &available))
                    {
                        error = DescribeWinHttpFailure(stage + L"（读响应体）", safeDisplayUrl, ::GetLastError());
                        return false;
                    }
                    if (available == 0u)
                    {
                        break;
                    }

                    const size_t already = body.size();
                    if (static_cast<unsigned long long>(already) + available > maxBytes)
                    {
                        error = stage + L"失败：对方给的内容超过上限（" + FormatUInt(maxBytes)
                              + L" 字节），已中止下载。地址：" + safeDisplayUrl
                              + L"\r\n正常的扩展集只有几百 KB，这么大的东西不是你想要的。";
                        return false;
                    }

                    body.resize(already + available);
                    DWORD received = 0u;
                    if (!::WinHttpReadData(httpRequest.get(), body.data() + already, available, &received))
                    {
                        error = DescribeWinHttpFailure(stage + L"（读响应体）", safeDisplayUrl, ::GetLastError());
                        return false;
                    }
                    body.resize(already + received);
                    if (received == 0u)
                    {
                        break;
                    }
                }

                response.body.swap(body);
                return true;
            }
            return false;
        }

        bool HttpGetWithSources(const std::wstring& url, const std::vector<DownloadSource>& sources,
                                unsigned long long maxBytes, const std::wstring& stage,
                                const Request& request, HttpResponse& response,
                                std::wstring& usedSource, bool& sawHttp404, std::wstring& error)
        {
            error.clear();
            usedSource.clear();
            sawHttp404 = false;

            const std::vector<DownloadCandidate> candidates = BuildDownloadCandidates(url, sources);
            if (candidates.empty())
            {
                error = stage + L"失败：没有可用的下载源。";
                return false;
            }

            std::vector<std::wstring> failures;
            for (size_t i = 0u; i < candidates.size(); ++i)
            {
                const DownloadCandidate& candidate = candidates[i];
                if (request.cancel && request.cancel())
                {
                    error = L"已取消。";
                    return false;
                }
                if (request.progress)
                {
                    const int basePct = static_cast<int>(5 + (i * 40 / candidates.size()));
                    request.progress(basePct, stage + L"（" + candidate.name + L"）…");
                }

                HttpResponse attemptResponse;
                std::wstring attemptError;
                if (HttpGet(candidate.url, maxBytes, stage, request, attemptResponse, attemptError))
                {
                    response = std::move(attemptResponse);
                    usedSource = candidate.name;
                    return true;
                }
                if (request.cancel && request.cancel())
                {
                    error = L"已取消。";
                    return false;
                }
                // 体积超限不属于源临时故障，不重试其它镜像，直接终止。
                if (attemptError.find(L"超过上限") != std::wstring::npos)
                {
                    error = attemptError;
                    return false;
                }
                if (attemptResponse.status == 404u)
                {
                    sawHttp404 = true;
                }
                failures.push_back(candidate.name + L"：" + attemptError);
            }

            if (failures.size() == 1u)
            {
                error = failures.front();
            }
            else
            {
                error = stage + L"失败，已尝试全部 " + FormatUInt(failures.size()) + L" 个下载源：";
                for (const std::wstring& item : failures)
                {
                    error += L"\r\n- " + item;
                }
            }
            return false;
        }

        // 本进程创建并**登记过的** private staging 目录（规范化 + casefold）。
        // 只有登记过的目录才允许 CleanupStaging 删除；explicit 用户目录一律拒绝。
        std::mutex g_ownedStagingMutex;
        std::set<std::wstring> g_ownedStagingDirectories;

        std::wstring StagingKey(const std::wstring& path)
        {
            const std::wstring normalized = PathSafety::NormalizeFullPath(path);
            return normalized.empty() ? std::wstring() : ToLowerString(normalized);
        }

        void RegisterOwnedStaging(const std::wstring& path)
        {
            const std::wstring key = StagingKey(path);
            if (key.empty())
            {
                return;
            }
            std::lock_guard<std::mutex> guard(g_ownedStagingMutex);
            g_ownedStagingDirectories.insert(key);
        }

        bool IsOwnedStaging(const std::wstring& path)
        {
            const std::wstring key = StagingKey(path);
            if (key.empty())
            {
                return false;
            }
            std::lock_guard<std::mutex> guard(g_ownedStagingMutex);
            return g_ownedStagingDirectories.find(key) != g_ownedStagingDirectories.end();
        }

        void UnregisterOwnedStaging(const std::wstring& path)
        {
            const std::wstring key = StagingKey(path);
            if (key.empty())
            {
                return;
            }
            std::lock_guard<std::mutex> guard(g_ownedStagingMutex);
            g_ownedStagingDirectories.erase(key);
        }

        // 检查 path 的**所有祖先**（不含末段本身）是否有重解析点。
        // 清理前必须做：否则父目录被换成 junction 时，FindFirstFile 会穿过去，
        // 把外部目录里的同名结构当成本次 owned 内容处理。
        bool AncestorHasReparse(const std::wstring& path, std::wstring& offending)
        {
            offending.clear();
            const std::wstring normalized = PathSafety::NormalizeFullPath(path);
            if (normalized.empty())
            {
                return false;
            }
            const PathSafety::SplitPath split = PathSafety::SplitFullPath(normalized);
            if (!split.valid)
            {
                return false;
            }
            std::wstring current = split.prefix;
            if (current.size() > 3u && current.back() == L'\\')
            {
                current.pop_back();
            }
            const DWORD rootAttributes = ::GetFileAttributesW(current.c_str());
            if (rootAttributes != INVALID_FILE_ATTRIBUTES
                && (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                offending = current;
                return true;
            }
            for (size_t i = 0u; i + 1u < split.parts.size(); ++i)
            {
                if (!current.empty() && current.back() != L'\\')
                {
                    current.push_back(L'\\');
                }
                current += split.parts[i];
                const DWORD attributes = ::GetFileAttributesW(current.c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES)
                {
                    break;   // 祖先不存在，更深的路径不可能存在
                }
                if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                {
                    offending = current;
                    return true;
                }
            }
            return false;
        }

        // 递归删除**本进程 owned 的** staging 目录。遇重解析点只 unlink 链接本身
        // （RemoveDirectoryW 不会进入 junction 目标），绝不递归进链接指向的目录。
        void RemoveOwnedDirectoryNoFollow(const std::wstring& directory)
        {
            if (directory.empty() || directory.size() <= 3u)
            {
                return;
            }
            const DWORD attributes = ::GetFileAttributesW(directory.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                return;
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                ::RemoveDirectoryW(directory.c_str());   // 只摘链接，不跟目标
                return;
            }
            // 递归过程中祖先被换成链接：停止，绝不穿过链接去枚举外部目录。
            std::wstring ancestorOffending;
            if (PathSafety::HasReparsePoint(directory, &ancestorOffending))
            {
                return;
            }

            WIN32_FIND_DATAW data{};
            HANDLE find = ::FindFirstFileW(CombinePath(directory, L"*").c_str(), &data);
            if (find != INVALID_HANDLE_VALUE)
            {
                do
                {
                    const std::wstring name = data.cFileName;
                    if (name == L"." || name == L"..")
                    {
                        continue;
                    }
                    const std::wstring path = CombinePath(directory, name);
                    if ((data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
                    {
                        // 链接：目录链接用 RemoveDirectoryW 摘掉，文件链接用 DeleteFileW 摘掉。
                        if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                        {
                            ::RemoveDirectoryW(path.c_str());
                        }
                        else
                        {
                            ::DeleteFileW(path.c_str());
                        }
                    }
                    else if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    {
                        RemoveOwnedDirectoryNoFollow(path);
                        ::RemoveDirectoryW(path.c_str());
                    }
                    else
                    {
                        ::DeleteFileW(path.c_str());
                    }
                } while (::FindNextFileW(find, &data));
                ::FindClose(find);
            }
            ::RemoveDirectoryW(directory.c_str());
        }

        // staging 的两个候选落点。建得出来才算"这里可写"（看属性位判不出 ACL）。
        std::wstring PrimaryStagingDirectory()
        {
            const std::wstring toolDirectory = ToolDirectory();
            if (toolDirectory.empty())
            {
                return std::wstring();
            }
            return CombinePath(CombinePath(toolDirectory, L"Log"), L"OnlineStaging");
        }

        std::wstring FallbackStagingDirectory()
        {
            const std::wstring temp = EnvironmentValue(L"TEMP");
            if (temp.empty())
            {
                return std::wstring();
            }
            return CombinePath(CombinePath(temp, L"YuaExtract"), L"OnlineStaging");
        }

        // 在一个父根下建**本次调用独占**的 session_<guid> 子目录（CREATE_NEW 防碰撞）。
        bool CreateOwnedSessionDirectory(const std::wstring& base, std::wstring& out,
                                         std::wstring& error)
        {
            error.clear();
            out.clear();
            if (base.empty())
            {
                return false;
            }
            // 父根与已存在的祖先都不能是重解析点（绝不能"跟着链接"建到别处）。
            std::wstring offending;
            if (PathSafety::HasReparsePoint(base, &offending))
            {
                error = L"临时目录根是/含有重解析点，拒绝使用：" + offending;
                return false;
            }
            DWORD createError = 0u;
            if (!EnsureDirectoryTree(base, createError))
            {
                error = L"建不出临时目录根：" + base + L"（Win32 错误码 "
                      + FormatUInt(createError) + L"）";
                return false;
            }
            if (PathSafety::HasReparsePoint(base, &offending))
            {
                error = L"临时目录根在创建后变成/含有重解析点，拒绝使用：" + offending;
                return false;
            }

            for (int attempt = 0; attempt < 16; ++attempt)
            {
                const std::wstring candidate =
                    CombinePath(base, L"session_" + Common::AtomicFile::UniqueSuffix());
                if (::CreateDirectoryW(candidate.c_str(), nullptr))
                {
                    if (PathSafety::HasReparsePoint(candidate, &offending))
                    {
                        ::RemoveDirectoryW(candidate.c_str());
                        error = L"新建的临时目录是/含有重解析点，拒绝使用：" + offending;
                        return false;
                    }
                    // 创建成功本身就是"真可写"的证明，不靠 Exists 猜。
                    RegisterOwnedStaging(candidate);
                    out = PathSafety::NormalizeFullPath(candidate);
                    if (out.empty())
                    {
                        UnregisterOwnedStaging(candidate);
                        ::RemoveDirectoryW(candidate.c_str());
                        error = L"临时目录路径无效：" + candidate;
                        return false;
                    }
                    return true;
                }
                const DWORD mkdirError = ::GetLastError();
                if (mkdirError != ERROR_ALREADY_EXISTS)
                {
                    error = L"建不出本次独占的临时目录：" + candidate + L"（Win32 错误码 "
                          + FormatUInt(mkdirError) + L"）";
                    return false;
                }
            }
            error = L"连续 16 次建独占临时目录都撞名：" + base;
            return false;
        }

        // CREATE_NEW 写一个本次独占的临时文件；失败只删**自己创建**的那个文件。
        bool WriteOwnedStagingFile(const std::wstring& path, const std::vector<unsigned char>& bytes,
                                   std::wstring& error)
        {
            error.clear();
            HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                        CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = L"创建临时文件失败：" + path + L"（Win32 错误码 "
                      + FormatUInt(::GetLastError()) + L"）";
                return false;
            }
            bool ok = true;
            size_t offset = 0u;
            while (offset < bytes.size())
            {
                const DWORD chunk = static_cast<DWORD>(
                    std::min<size_t>(bytes.size() - offset, 1u << 20));
                DWORD written = 0u;
                if (!::WriteFile(file, bytes.data() + offset, chunk, &written, nullptr)
                    || written != chunk)
                {
                    error = L"写临时文件失败：" + path + L"（Win32 错误码 "
                          + FormatUInt(::GetLastError()) + L"）";
                    ok = false;
                    break;
                }
                offset += written;
            }
            if (ok && !::FlushFileBuffers(file))
            {
                error = L"刷临时文件失败：" + path + L"（Win32 错误码 "
                      + FormatUInt(::GetLastError()) + L"）";
                ok = false;
            }
            ::CloseHandle(file);
            if (!ok)
            {
                ::DeleteFileW(path.c_str());   // 只删自己刚创建的文件
            }
            return ok;
        }

        // ---- 极简严格 JSON 解析（仅供 ParseCatalogJson 使用，零外部依赖） ----

        void AppendCatalogUtf8CodePoint(std::string& out, unsigned int code)
        {
            if (code <= 0x7Fu)
            {
                out.push_back(static_cast<char>(code));
            }
            else if (code <= 0x7FFu)
            {
                out.push_back(static_cast<char>(0xC0u | (code >> 6)));
                out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
            }
            else if (code <= 0xFFFFu)
            {
                out.push_back(static_cast<char>(0xE0u | (code >> 12)));
                out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
            }
            else if (code <= 0x10FFFFu)
            {
                out.push_back(static_cast<char>(0xF0u | (code >> 18)));
                out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
            }
        }

        struct CatalogJsonValue
        {
            enum class Kind { Null, Bool, Number, String, Array, Object };
            Kind kind = Kind::Null;
            bool boolean = false;
            double number = 0.0;
            bool isInteger = false;
            long long integer = 0;
            std::string text;
            std::vector<CatalogJsonValue> items;
            std::vector<std::string> keys;
            std::vector<CatalogJsonValue> values;

            const CatalogJsonValue* Member(const char* key) const
            {
                for (size_t i = 0u; i < keys.size() && i < values.size(); ++i)
                {
                    if (keys[i] == key)
                    {
                        return &values[i];
                    }
                }
                return nullptr;
            }

            std::wstring MemberText(const char* key) const
            {
                const CatalogJsonValue* v = Member(key);
                return (v != nullptr && v->kind == Kind::String) ? Utf8ToWide(v->text) : std::wstring();
            }

            bool MemberBool(const char* key, bool fallback) const
            {
                const CatalogJsonValue* v = Member(key);
                return (v != nullptr && v->kind == Kind::Bool) ? v->boolean : fallback;
            }

            bool MemberUInt64(const char* key, unsigned long long& out) const
            {
                const CatalogJsonValue* v = Member(key);
                if (v == nullptr || v->kind != Kind::Number || !v->isInteger || v->integer < 0)
                {
                    return false;
                }
                out = static_cast<unsigned long long>(v->integer);
                return true;
            }
        };

        struct CatalogJsonParser
        {
            const std::string& Text;
            size_t Pos = 0u;
            int Depth = 0;

            explicit CatalogJsonParser(const std::string& text) : Text(text) {}

            void SkipSpace()
            {
                while (Pos < Text.size())
                {
                    const char ch = Text[Pos];
                    if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n')
                    {
                        ++Pos;
                    }
                    else
                    {
                        break;
                    }
                }
            }

            bool Literal(const char* word)
            {
                const size_t len = std::strlen(word);
                if (Text.compare(Pos, len, word) != 0)
                {
                    return false;
                }
                Pos += len;
                return true;
            }

            bool ParseHex4(unsigned int& out)
            {
                if (Pos + 4u > Text.size())
                {
                    return false;
                }
                unsigned int code = 0u;
                for (int i = 0; i < 4; ++i)
                {
                    const char d = Text[Pos++];
                    code <<= 4u;
                    if (d >= '0' && d <= '9') code |= static_cast<unsigned int>(d - '0');
                    else if (d >= 'a' && d <= 'f') code |= static_cast<unsigned int>(d - 'a' + 10);
                    else if (d >= 'A' && d <= 'F') code |= static_cast<unsigned int>(d - 'A' + 10);
                    else return false;
                }
                out = code;
                return true;
            }

            bool ParseString(std::string& out)
            {
                if (Pos >= Text.size() || Text[Pos] != '"')
                {
                    return false;
                }
                ++Pos;
                out.clear();
                while (Pos < Text.size())
                {
                    const char ch = Text[Pos++];
                    if (ch == '"')
                    {
                        return true;
                    }
                    if (ch != '\\')
                    {
                        out.push_back(ch);
                        continue;
                    }
                    if (Pos >= Text.size())
                    {
                        return false;
                    }
                    const char esc = Text[Pos++];
                    switch (esc)
                    {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case 'u':
                        {
                            unsigned int code = 0u;
                            if (!ParseHex4(code))
                            {
                                return false;
                            }
                            if (code >= 0xD800u && code <= 0xDBFFu && Pos + 2u <= Text.size()
                                && Text[Pos] == '\\' && Text[Pos + 1u] == 'u')
                            {
                                Pos += 2u;
                                unsigned int low = 0u;
                                if (!ParseHex4(low))
                                {
                                    return false;
                                }
                                if (low >= 0xDC00u && low <= 0xDFFFu)
                                {
                                    code = 0x10000u + ((code - 0xD800u) << 10u) + (low - 0xDC00u);
                                }
                            }
                            AppendCatalogUtf8CodePoint(out, code);
                            break;
                        }
                        default:
                            return false;
                    }
                }
                return false;
            }

            bool ParseValue(CatalogJsonValue& out)
            {
                if (Depth > 24)
                {
                    return false;
                }
                SkipSpace();
                if (Pos >= Text.size())
                {
                    return false;
                }
                const char ch = Text[Pos];
                if (ch == '{')
                {
                    ++Pos;
                    ++Depth;
                    out.kind = CatalogJsonValue::Kind::Object;
                    SkipSpace();
                    if (Pos < Text.size() && Text[Pos] == '}')
                    {
                        ++Pos;
                        --Depth;
                        return true;
                    }
                    for (;;)
                    {
                        SkipSpace();
                        std::string key;
                        if (!ParseString(key))
                        {
                            return false;
                        }
                        SkipSpace();
                        if (Pos >= Text.size() || Text[Pos] != ':')
                        {
                            return false;
                        }
                        ++Pos;
                        CatalogJsonValue value;
                        if (!ParseValue(value))
                        {
                            return false;
                        }
                        out.keys.push_back(std::move(key));
                        out.values.push_back(std::move(value));
                        SkipSpace();
                        if (Pos < Text.size() && Text[Pos] == ',')
                        {
                            ++Pos;
                            continue;
                        }
                        if (Pos < Text.size() && Text[Pos] == '}')
                        {
                            ++Pos;
                            --Depth;
                            return true;
                        }
                        return false;
                    }
                }
                if (ch == '[')
                {
                    ++Pos;
                    ++Depth;
                    out.kind = CatalogJsonValue::Kind::Array;
                    SkipSpace();
                    if (Pos < Text.size() && Text[Pos] == ']')
                    {
                        ++Pos;
                        --Depth;
                        return true;
                    }
                    for (;;)
                    {
                        CatalogJsonValue value;
                        if (!ParseValue(value))
                        {
                            return false;
                        }
                        out.items.push_back(std::move(value));
                        SkipSpace();
                        if (Pos < Text.size() && Text[Pos] == ',')
                        {
                            ++Pos;
                            continue;
                        }
                        if (Pos < Text.size() && Text[Pos] == ']')
                        {
                            ++Pos;
                            --Depth;
                            return true;
                        }
                        return false;
                    }
                }
                if (ch == '"')
                {
                    out.kind = CatalogJsonValue::Kind::String;
                    return ParseString(out.text);
                }
                if (ch == 't')
                {
                    out.kind = CatalogJsonValue::Kind::Bool;
                    out.boolean = true;
                    return Literal("true");
                }
                if (ch == 'f')
                {
                    out.kind = CatalogJsonValue::Kind::Bool;
                    out.boolean = false;
                    return Literal("false");
                }
                if (ch == 'n')
                {
                    out.kind = CatalogJsonValue::Kind::Null;
                    return Literal("null");
                }

                const size_t start = Pos;
                if (ch == '-')
                {
                    ++Pos;
                }
                bool digits = false;
                while (Pos < Text.size() && Text[Pos] >= '0' && Text[Pos] <= '9')
                {
                    ++Pos;
                    digits = true;
                }
                bool hasFraction = false;
                if (Pos < Text.size() && Text[Pos] == '.')
                {
                    hasFraction = true;
                    ++Pos;
                    bool fracDigits = false;
                    while (Pos < Text.size() && Text[Pos] >= '0' && Text[Pos] <= '9')
                    {
                        ++Pos;
                        fracDigits = true;
                    }
                    if (!fracDigits)
                    {
                        return false;
                    }
                }
                if (Pos < Text.size() && (Text[Pos] == 'e' || Text[Pos] == 'E'))
                {
                    hasFraction = true;
                    ++Pos;
                    if (Pos < Text.size() && (Text[Pos] == '+' || Text[Pos] == '-'))
                    {
                        ++Pos;
                    }
                    bool expDigits = false;
                    while (Pos < Text.size() && Text[Pos] >= '0' && Text[Pos] <= '9')
                    {
                        ++Pos;
                        expDigits = true;
                    }
                    if (!expDigits)
                    {
                        return false;
                    }
                }
                if (!digits)
                {
                    Pos = start;
                    return false;
                }
                const std::string slice = Text.substr(start, Pos - start);
                out.kind = CatalogJsonValue::Kind::Number;
                out.number = ::strtod(slice.c_str(), nullptr);
                if (!hasFraction)
                {
                    char* endPtr = nullptr;
                    const long long parsedInt = ::_strtoi64(slice.c_str(), &endPtr, 10);
                    if (endPtr != nullptr && *endPtr == '\0')
                    {
                        out.isInteger = true;
                        out.integer = parsedInt;
                    }
                }
                return true;
            }
        };

        bool ParseCatalogJsonDocument(const std::string& rawText, CatalogJsonValue& root)
        {
            std::string clean = rawText;
            if (clean.size() >= 3u
                && static_cast<unsigned char>(clean[0]) == 0xEFu
                && static_cast<unsigned char>(clean[1]) == 0xBBu
                && static_cast<unsigned char>(clean[2]) == 0xBFu)
            {
                clean.erase(0u, 3u);
            }
            CatalogJsonParser parser(clean);
            if (!parser.ParseValue(root))
            {
                return false;
            }
            parser.SkipSpace();
            return parser.Pos == clean.size();
        }

        bool WriteIniLinesUtf8Bom(const std::wstring& path, const std::vector<std::wstring>& lines,
                                  std::wstring& error)
        {
            std::wstring rebuilt;
            for (const std::wstring& line : lines)
            {
                rebuilt += line;
                rebuilt += L"\r\n";
            }

            DWORD createError = 0u;
            const std::wstring directory = ParentDirectory(path);
            if (!directory.empty() && !EnsureDirectoryTree(directory, createError))
            {
                error = L"建不出配置目录：" + directory + L"（Win32 错误码 " + FormatUInt(createError)
                      + L"）";
                return false;
            }

            const std::string utf8 = WideToUtf8(rebuilt);
            std::vector<unsigned char> bytes;
            bytes.reserve(utf8.size() + 3u);
            bytes.push_back(0xEFu);
            bytes.push_back(0xBBu);
            bytes.push_back(0xBFu);
            bytes.insert(bytes.end(), utf8.begin(), utf8.end());

            if (!WriteAllBytes(path, bytes.data(), bytes.size(), createError))
            {
                error = L"写配置文件失败：" + path + L"（Win32 错误码 " + FormatUInt(createError) + L"）";
                return false;
            }
            return true;
        }
    }

    // =====================================================================
    // 有序下载源（镜像自动回退）
    // =====================================================================

    std::vector<DownloadSource> DefaultDownloadSources()
    {
        return {
            { L"gh-proxy.com", L"https://gh-proxy.com/", true },
            { L"ghproxy.vip", L"https://ghproxy.vip/", true },
            { L"GitHub 官方", L"", true },
        };
    }

    bool ValidateDownloadSources(const std::vector<DownloadSource>& sources, std::wstring& error)
    {
        error.clear();
        if (sources.empty())
        {
            error = L"下载源列表不能为空。";
            return false;
        }
        if (sources.size() > 16u)
        {
            error = L"下载源数量过多（最多 16 个）。";
            return false;
        }

        size_t enabledCount = 0u;
        for (size_t i = 0u; i < sources.size(); ++i)
        {
            const std::wstring name = TrimSpaces(sources[i].name);
            if (name.empty())
            {
                error = L"第 " + FormatUInt(i + 1u) + L" 个下载源缺少名称。";
                return false;
            }
            for (wchar_t ch : name)
            {
                if (ch == L'\r' || ch == L'\n')
                {
                    error = L"下载源名称不能包含换行符。";
                    return false;
                }
            }

            const std::wstring prefix = TrimSpaces(sources[i].prefix);
            if (!prefix.empty())
            {
                for (wchar_t ch : prefix)
                {
                    if (ch == L'\r' || ch == L'\n')
                    {
                        error = L"下载源 \"" + name + L"\" 前缀不能包含换行符。";
                        return false;
                    }
                }
                if (prefix.find(L'@') != std::wstring::npos || prefix.find(L'#') != std::wstring::npos
                    || prefix.find(L'?') != std::wstring::npos)
                {
                    error = L"下载源 \"" + name + L"\" 前缀不能包含 @ / ? / #。";
                    return false;
                }
                UrlParts parts;
                if (!ParseHttpsUrl(prefix, parts, false, error))
                {
                    error = L"下载源 \"" + name + L"\" 地址无效：" + error;
                    return false;
                }
                std::wstring policyErr;
                if (HostIsBlocked(parts.host, policyErr))
                {
                    error = L"下载源 \"" + name + L"\" 主机被拒：" + policyErr;
                    return false;
                }
            }

            if (sources[i].enabled)
            {
                ++enabledCount;
            }
        }
        if (enabledCount == 0u)
        {
            error = L"至少要启用一个下载源。";
            return false;
        }
        return true;
    }

    std::vector<DownloadSource> LoadDownloadSources()
    {
        const std::wstring path = ConfigFilePath();
        if (path.empty() || !FileExists(path))
        {
            return DefaultDownloadSources();
        }
        std::vector<unsigned char> bytes;
        DWORD readError = 0u;
        if (!ReadAllBytes(path, bytes, readError))
        {
            return DefaultDownloadSources();
        }

        unsigned long long count = 0ull;
        bool hasCount = false;
        std::vector<std::wstring> names;
        std::vector<std::wstring> prefixes;
        std::vector<bool> enableds;
        std::vector<std::wstring> keys;
        std::vector<std::wstring> vals;

        bool inOnline = false;
        for (const std::wstring& raw : SplitLines(DecodeIniText(bytes)))
        {
            const std::wstring line = TrimSpaces(raw);
            if (line.empty() || line.front() == L';' || line.front() == L'#')
            {
                continue;
            }
            if (line.size() >= 2u && line.front() == L'[' && line.back() == L']')
            {
                inOnline = (ToLowerString(line) == L"[online]");
                continue;
            }
            if (!inOnline)
            {
                continue;
            }
            const size_t equals = line.find(L'=');
            if (equals == std::wstring::npos)
            {
                continue;
            }
            const std::wstring key = ToLowerString(TrimSpaces(line.substr(0u, equals)));
            const std::wstring val = TrimSpaces(line.substr(equals + 1u));
            if (key == L"source_count")
            {
                if (ParseUInt(val, count) && count > 0ull && count <= 16ull)
                {
                    hasCount = true;
                }
                continue;
            }
            if (key.compare(0u, 7u, L"source_") == 0)
            {
                keys.push_back(key);
                vals.push_back(val);
            }
        }

        if (!hasCount)
        {
            return DefaultDownloadSources();
        }

        std::vector<DownloadSource> loaded(static_cast<size_t>(count));
        for (size_t i = 0u; i < keys.size(); ++i)
        {
            const std::wstring& k = keys[i];
            const size_t sep = k.find(L'_', 7u);
            if (sep == std::wstring::npos)
            {
                continue;
            }
            unsigned long long idx = 0ull;
            if (!ParseUInt(k.substr(7u, sep - 7u), idx) || idx >= count)
            {
                continue;
            }
            const std::wstring field = k.substr(sep + 1u);
            DownloadSource& slot = loaded[static_cast<size_t>(idx)];
            if (field == L"name")
            {
                slot.name = vals[i];
            }
            else if (field == L"prefix")
            {
                slot.prefix = vals[i];
            }
            else if (field == L"enabled")
            {
                slot.enabled = (vals[i] != L"0" && ToLowerString(vals[i]) != L"false");
            }
        }

        std::wstring validationError;
        if (!ValidateDownloadSources(loaded, validationError))
        {
            return DefaultDownloadSources();
        }
        return loaded;
    }

    bool SaveDownloadSources(const std::vector<DownloadSource>& sources, std::wstring& error)
    {
        std::vector<DownloadSource> normalized;
        normalized.reserve(sources.size());
        for (const DownloadSource& src : sources)
        {
            DownloadSource item;
            item.name = TrimSpaces(src.name);
            item.prefix = TrimSpaces(src.prefix);
            if (!item.prefix.empty() && item.prefix.back() != L'/')
            {
                item.prefix.push_back(L'/');
            }
            item.enabled = src.enabled;
            normalized.push_back(std::move(item));
        }

        if (!ValidateDownloadSources(normalized, error))
        {
            return false;
        }

        const std::wstring path = ConfigFilePath();
        if (path.empty())
        {
            error = L"找不到可写的配置文件位置：工具目录与 %LOCALAPPDATA% 都写不了。";
            return false;
        }

        std::wstring text;
        if (FileExists(path))
        {
            std::vector<unsigned char> bytes;
            DWORD readError = 0u;
            if (ReadAllBytes(path, bytes, readError))
            {
                text = DecodeIniText(bytes);
            }
        }
        const std::vector<std::wstring> lines = ReplaceIniOnlineSources(SplitLines(text), normalized);
        return WriteIniLinesUtf8Bom(path, lines, error);
    }

    std::vector<DownloadCandidate> BuildDownloadCandidates(const std::wstring& url,
                                                           const std::vector<DownloadSource>& sources)
    {
        UrlParts parts;
        std::wstring parseErr;
        if (!ParseHttpsUrl(url, parts, true, parseErr))
        {
            return {};
        }

        const std::wstring host = ToLowerString(parts.host);
        const bool isGitHubHost = (host == L"github.com"
                                   || host == L"raw.githubusercontent.com"
                                   || host == L"codeload.github.com");
        if (!isGitHubHost)
        {
            return { { parts.host, url } };
        }

        std::vector<DownloadCandidate> out;
        for (const DownloadSource& src : sources)
        {
            if (!src.enabled)
            {
                continue;
            }
            const std::wstring prefix = TrimSpaces(src.prefix);
            std::wstring candidateUrl;
            if (prefix.empty())
            {
                candidateUrl = url;
            }
            else
            {
                std::wstring trimmedPrefix = prefix;
                while (!trimmedPrefix.empty() && trimmedPrefix.back() == L'/')
                {
                    trimmedPrefix.pop_back();
                }
                candidateUrl = trimmedPrefix + L"/" + url;
            }
            out.push_back({ src.name.empty() ? L"GitHub" : src.name, candidateUrl });
        }
        if (out.empty())
        {
            out.push_back({ L"GitHub 官方", url });
        }
        return out;
    }

    // =====================================================================
    // 基址
    // =====================================================================

    std::wstring NormalizeBase(const std::wstring& base)
    {
        // 去首尾空白；若为 .json 清单直链（如 catalog.json）则保持原样，否则末尾保证一个 '/'。
        const std::wstring trimmed = TrimSpaces(base);
        if (trimmed.empty() || !StartsWithHttps(trimmed))
        {
            return std::wstring();
        }
        if (EndsWithJsonSuffix(trimmed))
        {
            return trimmed;
        }

        std::wstring out = trimmed;
        if (out.back() != L'/')
        {
            out.push_back(L'/');
        }
        return out;
    }

    std::wstring DefaultRepositoryBase()
    {
        return NormalizeBase(kDefaultRepositoryBase);
    }

    std::wstring ConfigFilePath()
    {
        // 优先 <工具目录>\YuaExtract.ini（跟工具放一起，用户一眼看得见、能直接改，绿色化也方便）；
        // 工具目录不可写（装在 Program Files）时用 %LOCALAPPDATA%\YuaExtract\YuaExtract.ini。
        //
        // 结果缓存一份：判可写要真去建探针文件，而弹窗每次刷新都会问这个路径。
        // 运行期内工具目录的权限不会变，缓存是安全的。
        static const std::wstring cached = []
        {
            const std::wstring toolDirectory = ToolDirectory();
            if (!toolDirectory.empty())
            {
                const std::wstring primary = CombinePath(toolDirectory, L"YuaExtract.ini");
                if (CanWriteConfig(primary))
                {
                    return primary;
                }
            }
            const std::wstring localAppData = EnvironmentValue(L"LOCALAPPDATA");
            if (!localAppData.empty())
            {
                return CombinePath(CombinePath(localAppData, L"YuaExtract"), L"YuaExtract.ini");
            }
            return std::wstring();
        }();
        return cached;
    }

    std::wstring ResolveRepositoryBase()
    {
        // 顺序（§14.5，先命中先用）：环境变量 YUAEXT_ENDPOINT -> ini 的 [online] base= -> 内置常量，
        // 全部收在 ReadConfiguredBase 里（诊断那边要用同一套顺序，不能各写一遍）。
        //
        // 这里**仍然返回归一化后的值**（非 https 折成空串），保持既有语义不变 —— 界面拿它当
        // "当前基址"显示。命中但不合法的**不往下落**：静默改用别的地址，用户会对着"明明改了
        // 却没生效"发呆；返回空串，由调用入口（ResolveUsableBase）把原因说清楚，而它会回到
        // 配置里捞原文，所以措辞里引用的是用户真正写下的那个字符串。
        const ConfiguredBase configured = ReadConfiguredBase();
        return NormalizeBase(configured.raw);
    }

    bool SetRepositoryBase(const std::wstring& base, std::wstring& error)
    {
        error.clear();

        // 与联网入口用同一个校验器（只要一处措辞）：http、空地址、占位符、URL 非法字符
        // 都在这里被挡掉 —— 用户多半就是把内置常量里的占位符原样点了保存。
        std::wstring normalized;
        if (!ResolveUsableBase(base, normalized, error))
        {
            return false;
        }

        // 主机策略与联网入口（HttpGet）同一套：本机 / 内网 / 保留段的主机不许保存为仓库
        // 基址 —— 存进 ini 之后，每次在线更新都会往那边发请求。必须在动 ini **之前**挡掉，
        // 拒绝时一个字节都不落盘（不读不改不写）。
        {
            UrlParts parts;
            if (!ParseHttpsUrl(normalized, parts, error))
            {
                // ResolveUsableBase 已经保证 normalized 是带主机的 https 地址，走到这里
                // 说明两边对"能用的地址"认知不一致，宁可拒绝也不往下写。
                return false;
            }
            if (HostIsBlocked(parts.host, error)
                || (!HostIsNumericAddress(parts.host) && ResolveBlocked(parts.host, error)))
            {
                error = L"仓库地址被拒（" + parts.host + L"）：" + error
                      + L"配置没有保存。本工具只访问公网 https 地址，"
                        L"不访问本机 / 内网 / 保留段。";
                return false;
            }
        }

        const std::wstring path = ConfigFilePath();
        if (path.empty())
        {
            error = L"找不到可写的配置文件位置：工具目录与 %LOCALAPPDATA% 都写不了。";
            return false;
        }

        // 读回来 -> 只改那一行 -> 整份写回：用户在 ini 里手写的别的东西（别的段、注释）
        // 一个字节都不该被我们抹掉。行尾统一成 CRLF（这个文件只有我们和记事本在看）。
        std::wstring text;
        if (FileExists(path))
        {
            std::vector<unsigned char> bytes;
            DWORD readError = 0;
            if (ReadAllBytes(path, bytes, readError))
            {
                text = DecodeIniText(bytes);
            }
        }

        std::vector<std::wstring> lines = ReplaceIniBase(SplitLines(text), normalized);
        return WriteIniLinesUtf8Bom(path, lines, error);
    }

    std::wstring IndexUrl(const std::wstring& base)
    {
        const std::wstring normalized = NormalizeBase(base);
        if (normalized.empty())
        {
            return std::wstring();   // 拼不出来就给空串，调用方必须据此报错
        }
        if (EndsWithJsonSuffix(normalized))
        {
            return normalized;
        }
        return normalized + kIndexFileName;
    }

    std::wstring ArchiveUrl(const std::wstring& base, const std::wstring& publisher,
                            const std::wstring& title)
    {
        // §14.2：传统目录仓库按 <会社>/<作品>/rules.int 放；
        // catalog.json 模式由条目自带 remoteUrl，不从 catalog.json 路径后缀拼子路径。
        const std::wstring normalized = NormalizeBase(base);
        if (normalized.empty() || EndsWithJsonSuffix(normalized))
        {
            return std::wstring();
        }
        const std::wstring safePublisher = ExtensionSet::MakeSafePathComponent(publisher);
        const std::wstring safeTitle = ExtensionSet::MakeSafePathComponent(title);
        if (safePublisher.empty() || safeTitle.empty())
        {
            return std::wstring();
        }
        return normalized + safePublisher + L"/" + safeTitle + L"/" + kRemoteArchiveFileName;
    }

    // =====================================================================
    // catalog.json v1 解析与拉索引
    // =====================================================================

    bool ParseCatalogJson(const std::string& jsonUtf8, const std::wstring& sourceUrl,
                          std::vector<ExtensionSet::LibraryEntry>& out, std::wstring& error)
    {
        out.clear();
        error.clear();

        CatalogJsonValue root;
        if (!ParseCatalogJsonDocument(jsonUtf8, root) || root.kind != CatalogJsonValue::Kind::Object)
        {
            error = L"目录 JSON 格式无效（" + sourceUrl + L"）。";
            return false;
        }

        unsigned long long schemaVersion = 0ull;
        if (!root.MemberUInt64("schema_version", schemaVersion) || schemaVersion != 1ull)
        {
            error = L"不支持的目录 schema_version（需要 1）。地址：" + sourceUrl;
            return false;
        }

        const CatalogJsonValue* entriesVal = root.Member("entries");
        if (entriesVal == nullptr || entriesVal->kind != CatalogJsonValue::Kind::Array)
        {
            error = L"目录缺少 entries 数组。地址：" + sourceUrl;
            return false;
        }

        std::vector<ExtensionSet::LibraryEntry> parsed;
        parsed.reserve(entriesVal->items.size());
        for (size_t i = 0u; i < entriesVal->items.size(); ++i)
        {
            const CatalogJsonValue& obj = entriesVal->items[i];
            if (obj.kind != CatalogJsonValue::Kind::Object)
            {
                error = L"目录第 " + FormatUInt(i + 1u) + L" 项不是 JSON 对象。";
                return false;
            }
            if (obj.MemberBool("yanked", false))
            {
                continue; // 已撤回条目不提供下载安装
            }

            ExtensionSet::LibraryEntry item;
            item.publisher = TrimSpaces(obj.MemberText("publisher"));
            item.title = TrimSpaces(obj.MemberText("title"));
            if (item.publisher.empty() || item.title.empty())
            {
                error = L"目录第 " + FormatUInt(i + 1u) + L" 项缺少 publisher 或 title。";
                return false;
            }
            item.contributor = obj.MemberText("contributor");
            item.version = obj.MemberText("version");
            item.date = obj.MemberText("date");
            item.comment = obj.MemberText("comment");
            item.generatedAt = obj.MemberText("generated_at");
            item.exeDigest = obj.MemberText("exe_sha256");
            item.siblingDigest = obj.MemberText("sibling_sha256");
            item.saltDigest = obj.MemberText("salt_sha256");
            item.archiveDigest = obj.MemberText("archive_sha256");

            if (!ExtensionSet::IsHexLength(item.exeDigest, 64u))
            {
                error = L"目录条目「" + item.publisher + L" / " + item.title
                      + L"」缺少有效的 64 位 exe_sha256。";
                return false;
            }
            if (!ExtensionSet::IsHexLength(item.archiveDigest, 64u))
            {
                error = L"目录条目「" + item.publisher + L" / " + item.title
                      + L"」缺少有效的 64 位 archive_sha256。";
                return false;
            }

            unsigned long long n = 0ull;
            if (obj.MemberUInt64("directory_count", n)) item.directoryCount = static_cast<unsigned int>(n);
            if (obj.MemberUInt64("file_count", n)) item.fileCount = static_cast<unsigned int>(n);
            if (obj.MemberUInt64("exception_count", n)) item.exceptionCount = static_cast<unsigned int>(n);
            if (obj.MemberUInt64("entry_count", n))
            {
                item.entryCount = static_cast<unsigned int>(n);
            }
            else
            {
                item.entryCount = item.directoryCount + item.fileCount;
            }

            const CatalogJsonValue* pkg = obj.Member("package");
            if (pkg == nullptr || pkg->kind != CatalogJsonValue::Kind::Object)
            {
                error = L"目录条目「" + item.publisher + L" / " + item.title
                      + L"」缺少有效的 package 信息。";
                return false;
            }
            item.remoteUrl = TrimSpaces(pkg->MemberText("url"));
            UrlParts pkgUrlParts;
            std::wstring urlErr;
            if (!ParseHttpsUrl(item.remoteUrl, pkgUrlParts, false, urlErr))
            {
                error = L"目录条目「" + item.publisher + L" / " + item.title
                      + L"」下载地址不是合法 https 地址：" + urlErr;
                return false;
            }
            unsigned long long pkgSize = 0ull;
            if (!pkg->MemberUInt64("size", pkgSize) || pkgSize == 0ull || pkgSize > kMaxArchiveBytes)
            {
                error = L"目录条目「" + item.publisher + L" / " + item.title
                      + L"」安装包大小超出合理范围。";
                return false;
            }
            item.remoteSize = pkgSize;

            const std::wstring pkgSha = pkg->MemberText("sha256");
            if (!ExtensionSet::IsHexLength(pkgSha, 64u) || !SameHash(pkgSha, item.archiveDigest))
            {
                error = L"目录条目「" + item.publisher + L" / " + item.title
                      + L"」package.sha256 与 archive_sha256 不一致。";
                return false;
            }
            item.source = L"yua-extensions-catalog";
            parsed.push_back(std::move(item));
        }

        out = std::move(parsed);
        return true;
    }

    namespace
    {
        bool ParseManifestIndexText(const std::vector<unsigned char>& body, const std::wstring& sourceUrl,
                                    std::vector<ExtensionSet::LibraryEntry>& out, std::wstring& error)
        {
            if (!IsValidUtf8(body))
            {
                error = L"索引格式不对（" + sourceUrl + L"）：内容不是合法的 UTF-8 文本。"
                        L"\r\n（索引就是一份 manifest.txt，UTF-8 的 TSV。）";
                return false;
            }
            const std::string text = body.empty()
                ? std::string()
                : std::string(reinterpret_cast<const char*>(body.data()), body.size());

            std::vector<ExtensionSet::LibraryEntry> entries;
            std::wstring parseReport;
            if (!ExtensionSet::ParseManifestText(text, entries, parseReport))
            {
                error = L"索引格式不对（" + sourceUrl + L"）：\r\n" + parseReport;
                return false;
            }
            if (!parseReport.empty())
            {
                error = L"索引里有不合规的行（索引必须整份干净，一行都不能跳过）：\r\n" + parseReport
                      + L"地址：" + sourceUrl;
                return false;
            }
            for (const ExtensionSet::LibraryEntry& item : entries)
            {
                if (!ExtensionSet::IsHexLength(item.exeDigest, 64u))
                {
                    error = L"索引里「" + item.publisher + L" / " + item.title
                          + L"」这一条没有目标 exe 摘要（该列是空的）。\r\n"
                            L"索引是在线的信任根，缺了它，下载回来的字节就没有东西可以对，拒绝。\r\n"
                            L"地址：" + sourceUrl;
                    return false;
                }
                if (!ExtensionSet::IsHexLength(item.archiveDigest, 64u))
                {
                    error = L"索引里「" + item.publisher + L" / " + item.title
                          + L"」这一条没有档案摘要（该列是空的）。\r\n"
                            L"下载回来的档案要拿它对（§14.4 第 2 条），没有它就无法校验，拒绝。\r\n"
                            L"地址：" + sourceUrl;
                    return false;
                }
            }
            out = std::move(entries);
            return true;
        }
    }

    bool FetchIndex(const std::wstring& base, std::vector<ExtensionSet::LibraryEntry>& out,
                    IndexResult& info, std::wstring& error, const Request& request)
    {
        out.clear();
        error.clear();
        info = IndexResult();

        std::wstring normalized;
        if (!ResolveUsableBase(base, normalized, error))
        {
            return false;
        }

        const std::vector<DownloadSource> sources = LoadDownloadSources();
        info.url = IndexUrl(normalized);
        if (request.progress)
        {
            request.progress(5, L"正在拉取索引：" + info.url);
        }

        HttpResponse response;
        std::wstring usedSource;
        bool sawHttp404 = false;
        bool fetched = HttpGetWithSources(info.url, sources, kMaxIndexBytes, L"拉取索引", request,
                                          response, usedSource, sawHttp404, error);
        info.httpStatus = static_cast<int>(response.status);
        bool usedLegacyFallback = false;

        // 默认指向 GitHub Releases 的 catalog.json；若仓库尚未发布首个 Release（404），
        // 自动兼容回退到旧版 raw main/index.txt，确保平滑过渡。
        if (!fetched && sawHttp404 && normalized == DefaultRepositoryBase())
        {
            const std::wstring legacyBase = NormalizeBase(kLegacyFallbackRepositoryBase);
            const std::wstring legacyUrl = IndexUrl(legacyBase);
            if (request.progress)
            {
                request.progress(25, L"Releases 目录尚不可用，兼容尝试 raw 索引…");
            }
            HttpResponse legacyResp;
            std::wstring legacyUsed;
            bool legacy404 = false;
            std::wstring legacyErr;
            if (HttpGetWithSources(legacyUrl, sources, kMaxIndexBytes, L"拉取兼容索引", request,
                                   legacyResp, legacyUsed, legacy404, legacyErr))
            {
                response = std::move(legacyResp);
                usedSource = legacyUsed;
                info.url = legacyUrl;
                info.httpStatus = static_cast<int>(response.status);
                fetched = true;
                usedLegacyFallback = true;
                error.clear();
            }
        }

        if (!fetched)
        {
            return false;
        }

        info.usedSource = usedSource;
        info.bytes = static_cast<unsigned long long>(response.body.size());
        if (request.progress)
        {
            request.progress(65, L"正在解析索引…");
        }

        std::vector<ExtensionSet::LibraryEntry> entries;
        if (EndsWithJsonSuffix(info.url) && !usedLegacyFallback)
        {
            if (!IsValidUtf8(response.body))
            {
                error = L"目录 JSON 不是合法的 UTF-8 文本：" + info.url;
                return false;
            }
            const std::string text(reinterpret_cast<const char*>(response.body.data()), response.body.size());
            if (!ParseCatalogJson(text, info.url, entries, error))
            {
                return false;
            }
            info.format = L"catalog-v1";
        }
        else
        {
            if (!ParseManifestIndexText(response.body, info.url, entries, error))
            {
                return false;
            }
            info.format = L"index-tsv";
            if (usedLegacyFallback)
            {
                for (ExtensionSet::LibraryEntry& item : entries)
                {
                    if (item.remoteUrl.empty())
                    {
                        item.remoteUrl = ArchiveUrl(kLegacyFallbackRepositoryBase, item.publisher, item.title);
                    }
                }
            }
        }

        info.entryCount = static_cast<unsigned int>(entries.size());
        out = std::move(entries);
        if (request.progress)
        {
            const std::wstring via = usedSource.empty() ? std::wstring() : (L"（" + usedSource + L"）");
            request.progress(100, L"索引读取完成，共 " + FormatUInt(info.entryCount) + L" 条扩展集" + via + L"。");
        }
        return true;
    }

    // =====================================================================
    // 与本地库的差异
    // =====================================================================

    EntryState Classify(const ExtensionSet::LibraryEntry& remote,
                        const ExtensionSet::LibraryCatalog& local)
    {
        // "本地这一条"的认定口径与 InstallToLibrary 的 upsert 保持一致：命中
        // 「同一 会社/作品 槽位」或「同一目标 exe 摘要」的旧条目，都会被这一条取代。
        // 命中多条的（同一 exe 摘要挂在不同作品名下）取条数最多的那条来比 —— 只有远端
        // 条数不少于**最全的那条**，才敢说"覆盖不会减少覆盖度"。
        const ExtensionSet::LibraryEntry* best = nullptr;
        bool sameDigest = false;
        // 统一用 LibrarySlotKey（sanitize + casefold）判"同一物理槽位"：
        // "Game:A" 与 "GameA" 落到同一个库目录，必须算 sameSlot，不能被判成 New。
        const std::wstring remoteSlot =
            ExtensionSet::LibrarySlotKey(remote.publisher, remote.title);
        for (const ExtensionSet::LibraryEntry& candidate : local.entries)
        {
            const bool sameSlot = !remoteSlot.empty()
                && ExtensionSet::LibrarySlotKey(candidate.publisher, candidate.title) == remoteSlot;
            const bool sameExe = SameHash(candidate.exeDigest, remote.exeDigest);
            if (!sameSlot && !sameExe)
            {
                continue;
            }
            if (best == nullptr || candidate.entryCount > best->entryCount)
            {
                best = &candidate;
            }
            if (SameHash(candidate.archiveDigest, remote.archiveDigest))
            {
                sameDigest = true;
            }
        }

        if (best == nullptr)
        {
            return EntryState::New;
        }
        if (sameDigest)
        {
            return EntryState::Same;   // 同一份档案，条数不用再比
        }
        if (remote.entryCount >= best->entryCount)
        {
            return EntryState::Update;
        }
        return EntryState::LocalNewer;
    }

    const wchar_t* DescribeState(EntryState state)
    {
        switch (state)
        {
            case EntryState::New:
                return L"新增（本地还没有）";
            case EntryState::Same:
                return L"已是最新（档案摘要与本地相同）";
            case EntryState::Update:
                return L"可更新（档案摘要不同，且条数不少于本地）";
            case EntryState::LocalNewer:
                return L"本地更全（条数比远端多）—— 覆盖会减少覆盖度，默认不覆盖";
        }
        return L"（未知状态）";
    }

    // =====================================================================
    // 下载 + 校验
    // =====================================================================

    std::wstring DefaultStagingDirectory()
    {
        // 临时档案跟日志一起放：<工具目录>\Log\OnlineStaging\session_<guid>。
        // 工具目录不可写时退到 %TEMP%\YuaExtract\OnlineStaging\session_<guid>。
        // 每次调用一个**全新独占**子目录：父根验收（无重解析点 + 真能创建）之后，
        // 用 CREATE_NEW 建 session 目录，并登记为 owned，之后只由 CleanupStaging 清理。
        std::wstring error;
        const std::wstring primary = PrimaryStagingDirectory();
        if (!primary.empty())
        {
            std::wstring session;
            if (CreateOwnedSessionDirectory(primary, session, error) && !session.empty())
            {
                return session;
            }
        }

        const std::wstring fallback = FallbackStagingDirectory();
        if (!fallback.empty())
        {
            std::wstring session;
            if (CreateOwnedSessionDirectory(fallback, session, error) && !session.empty())
            {
                return session;
            }
        }
        return std::wstring();
    }

    void CleanupStaging(const std::wstring& stagingDirectory)
    {
        // 空串 = no-op：**绝不**去猜"共享默认根"再整棵删（那是老实现的危险行为）。
        if (stagingDirectory.empty())
        {
            return;
        }
        // 只清理本进程创建并登记的 owned 目录。explicit 的 existing 用户目录一律拒绝。
        if (!IsOwnedStaging(stagingDirectory))
        {
            return;
        }
        // 祖先被换成 junction：**不跟进去清理**。保留 owned 登记（不 unregister），
        // 让调用方以后在路径恢复后还能重试，也避免"报干净实际没清"。
        std::wstring offending;
        if (AncestorHasReparse(stagingDirectory, offending))
        {
            return;
        }
        RemoveOwnedDirectoryNoFollow(stagingDirectory);
        UnregisterOwnedStaging(stagingDirectory);
    }

#ifdef YUA_SECURITY_EXTENSION_TEST_SEAM
    bool& TestFailDownloadAfterStaging()
    {
        static bool failAfterStaging = false;
        return failAfterStaging;
    }
#endif

    bool DownloadArchive(const std::wstring& base, const ExtensionSet::LibraryEntry& entry,
                         const std::wstring& stagingDirectory, DownloadResult& out,
                         std::wstring& error, const Request& request)
    {
        error.clear();
        out = DownloadResult();

        // 基址的校验与 FetchIndex 用同一个入口函数，措辞不会两样。
        std::wstring normalized;
        if (!ResolveUsableBase(base, normalized, error))
        {
            return false;
        }

        // 会社 / 作品先过 MakeSafePathComponent：它们既要拼 URL 也要拼落盘的临时文件名，
        // 直接拿索引里的原样字符串去拼，一个 "/" 就能跳出目录。
        const std::wstring publisher = ExtensionSet::MakeSafePathComponent(entry.publisher);
        const std::wstring title = ExtensionSet::MakeSafePathComponent(entry.title);
        if (publisher.empty() || title.empty())
        {
            error = L"索引里这一条的会社 / 作品名做不出合法的目录名（会社：\"" + entry.publisher
                  + L"\"，作品：\"" + entry.title + L"\"）。";
            return false;
        }

        const std::wstring url = !entry.remoteUrl.empty()
            ? entry.remoteUrl
            : ArchiveUrl(normalized, publisher, title);
        if (url.empty())
        {
            error = L"拼不出档案地址：请先重新拉取目录索引或检查会社 / 作品名。";
            return false;
        }

        // 自定义 staging 必须是**已存在**的普通目录；不替用户创建/接管既有目录，
        // 也不把它登记为 owned（CleanupStaging 会拒绝删除未 owned 的 explicit 目录）。
        // 默认路径由 DefaultStagingDirectory 每次生成独占 session_<guid> 并登记。
        std::wstring staging;
        if (stagingDirectory.empty())
        {
            staging = DefaultStagingDirectory();
            if (staging.empty())
            {
                error = L"找不到可用的临时目录（工具目录下的 Log 与 %TEMP% 都写不了）。";
                return false;
            }
        }
        else
        {
            if (!DirectoryExists(stagingDirectory))
            {
                error = L"指定的临时目录不存在：" + stagingDirectory;
                return false;
            }
            std::wstring offending;
            if (PathSafety::HasReparsePoint(stagingDirectory, &offending))
            {
                error = L"指定的临时目录是/含有重解析点，拒绝写入：" + offending;
                return false;
            }
            staging = PathSafety::NormalizeFullPath(stagingDirectory);
            if (staging.empty())
            {
                error = L"指定的临时目录路径无效：" + stagingDirectory;
                return false;
            }
        }

        // 默认 staging 是本函数刚建出来的 owned session：若后续任何一步失败，
        // 由 RAII 守卫自动清掉（explicit 用户目录绝不接管/删除）。
        struct OwnedStagingGuard
        {
            const std::wstring* directory = nullptr;
            ~OwnedStagingGuard()
            {
                if (directory != nullptr)
                {
                    try
                    {
                        CleanupStaging(*directory);
                    }
                    catch (...)
                    {
                    }
                }
            }
        } stagingGuard;
        if (stagingDirectory.empty())
        {
            stagingGuard.directory = &staging;   // 不复制、不再分配
        }

        // **唯一 CREATE_NEW 文件名**：不覆盖任何预存在文件（哪怕同名）。
        std::wstring path;
        {
            for (int attempt = 0; attempt < 16; ++attempt)
            {
                const std::wstring candidate = CombinePath(
                    staging, L"Online_" + Common::AtomicFile::UniqueSuffix() + L".rules.int");
                if (!FileExists(candidate))
                {
                    path = candidate;
                    break;
                }
            }
            if (path.empty())
            {
                error = L"生成唯一临时文件名失败：" + staging;
                return false;
            }
        }

        // 本函数**只负责拿到字节**：不校验、不动本地库（§14.4 第 2–5 条在 VerifyArchive，
        // 入库在 ExtensionSet::InstallBytesToLibrary）。中间隔着两个显式函数调用，
        // "下载成功但校验没过"就不可能顺手改了本地库。
        if (request.progress)
        {
            request.progress(10, L"正在下载：" + url);
        }

#ifdef YUA_SECURITY_EXTENSION_TEST_SEAM
        if (TestFailDownloadAfterStaging())
        {
            error = L"测试故障注入：建好私有 staging 后、下载前失败。";
            return false;   // stagingGuard 负责清掉本次 owned session
        }
#endif

        const std::vector<DownloadSource> sources = LoadDownloadSources();
        HttpResponse response;
        std::wstring usedSource;
        bool allHttp404 = false;
        if (!HttpGetWithSources(url, sources, kMaxArchiveBytes, L"下载档案", request, response,
                                usedSource, allHttp404, error))
        {
            // 还没创建任何文件：不调用 DeleteFile（避免误删别的进程刚建出来的同名文件）。
            return false;
        }
        if (response.body.empty())
        {
            // 200 却一个字节都没有 = 这次下载没成功（正常档案至少也有个头）。
            // 这不是"校验"，不必等 VerifyArchive 去说"magic 不对"。
            error = L"下载失败：服务器返回 200，但内容是 0 字节。地址：" + url;
            return false;
        }

        if (!WriteOwnedStagingFile(path, response.body, error))
        {
            // WriteOwnedStagingFile 只在**自己 CREATE_NEW 成功**后才负责删除；
            // 这里不再补删，避免删掉预存在/别人创建的同名文件。
            return false;
        }

        out.path = path;
        out.stagingDirectory = staging;
        out.bytes = static_cast<unsigned long long>(response.body.size());
        out.blob = std::move(response.body);
        // 摘要与 blob 同源：对**我们真正拿到的那串字节**算 sha256，绝不重读 path。
        out.sha256 = ExtensionSet::Sha256HexBytes(out.blob.data(), out.blob.size());
        stagingGuard.directory = nullptr;   // 成功：目录所有权交给调用方（CleanupStaging 可清）

        if (request.progress)
        {
            request.progress(100, L"下载完成（" + FormatUInt(out.bytes) + L" 字节）。");
        }
        return true;
    }

    bool VerifyArchive(const std::wstring& archivePath, const ExtensionSet::LibraryEntry& entry,
                       std::wstring& detail, std::wstring& error)
    {
        // 兼容 wrapper：**只读一次**完整字节，之后与字节接口走同一条路。
        detail.clear();
        error.clear();
        if (archivePath.empty())
        {
            error = L"没有给出要校验的档案路径。";
            return false;
        }
        std::vector<unsigned char> blob;
        DWORD readError = 0u;
        if (!ReadAllBytes(archivePath, blob, readError))
        {
            error = L"读不到要校验的档案：" + archivePath + L"（Win32 错误码 "
                  + FormatUInt(readError) + L"）";
            return false;
        }
        return VerifyArchiveBytes(blob, entry, detail, error);
    }

    bool VerifyArchiveBytes(const std::vector<unsigned char>& blob,
                            const ExtensionSet::LibraryEntry& entry,
                            std::wstring& detail, std::wstring& error)
    {
        detail.clear();
        error.clear();

        if (entry.archiveDigest.empty() || entry.exeDigest.empty())
        {
            error = L"索引里这一条缺「档案摘要」或「目标 exe 摘要」，没有信任根可以对照，拒绝校验。";
            return false;
        }
        if (blob.empty())
        {
            error = L"要校验的档案字节为空。";
            return false;
        }
        if (static_cast<unsigned long long>(blob.size()) > kMaxArchiveBytes)
        {
            error = L"要校验的档案超过上限（" + FormatUInt(blob.size()) + L" 字节，上限 "
                  + FormatUInt(kMaxArchiveBytes) + L" 字节），拒绝校验。";
            return false;
        }

        // ---------- 第 2 条：sha256 必须等于索引里的「档案摘要」 ----------
        // 这一条把"下到的字节"钉死在索引上：字节只要被动过一个比特就过不去。
        // **摘要与后面的解码都针对传入的同一份 blob**，绝不重开路径。
        const std::wstring actualDigest =
            ExtensionSet::Sha256HexBytes(blob.data(), blob.size());
        if (!SameHash(actualDigest, entry.archiveDigest))
        {
            error = L"档案摘要不符：这一份不是索引里指的那一份，已丢弃。\r\n"
                    L"索引写的是：" + entry.archiveDigest + L"\r\n"
                    L"下载到的字节算出来是：" + actualDigest + L"\r\n"
                    L"（下载不完整，或内容在中间被改过。）";
            return false;
        }

        // ---------- 第 3 条：走内核既有的解码路径 ----------
        // magic / 版本 / headerSize / CRC32 / chunk 结构全部由 DecodeArchive 把着，
        // 本层不重复实现一遍容器解析（两份实现必然分叉）。
        ExtensionSet::Archive archive;
        std::wstring decodeError;
        if (!ExtensionSet::DecodeArchive(blob, archive, decodeError))
        {
            error = L"档案结构校验不通过：" + decodeError;
            return false;
        }

        // ---------- 第 4 条：内嵌目标 exe 摘要必须等于索引里的 ----------
        // 信任根是**索引**，不是本机的 exe：拿本机 exe 当根的话，本机没跑过这部游戏时
        // 根本没有根可比，而"下到别的作品的档案"反倒会在两个摘要都为空时被放过。
        if (archive.identity.targetExe.sha256.empty())
        {
            error = L"档案里没有目标 exe 摘要（IDENTITY 的 target.sha256 是空的），无法与索引对照，"
                    L"拒绝。";
            return false;
        }
        if (!SameHash(archive.identity.targetExe.sha256, entry.exeDigest))
        {
            error = L"档案内嵌的目标 exe 摘要与索引不一致：这一份不是索引里那一条描述的东西。\r\n"
                    L"档案里是：" + archive.identity.targetExe.sha256 + L"\r\n"
                    L"索引写的是：" + entry.exeDigest;
            return false;
        }

        // ---------- 交叉校验：索引行的会社 / 作品必须等于档案自己的 META ----------
        //（2026-10-16 加，独立验证 P-1）
        // 索引提供的两个字符串只用来拼下载 URL，而**落库目录**是 InstallToLibrary 按档案
        // 自己的 META 算的。两边不校验的后果（验证者实跑过）：界面报"已新增 A / B"，
        // 磁盘上落在 C / D；写出的清单行自相矛盾（名字列来自 META、来源列来自索引）；
        // 更重的一路——覆盖确认框按**索引名**找本地条目问用户，真正被换掉的却是**META 名**
        // 那个槽位，用户确认的和实际被替换的不是同一份。
        // 所以这不是"照实显示然后照装"能了事的：索引是信任根，安装槽位必须可预测。
        // 比较口径用 _wcsicmp（与库自己的 upsert 键、与 SameHash 的宽松度一致）。
        if (_wcsicmp(archive.meta.publisher.c_str(), entry.publisher.c_str()) != 0
            || _wcsicmp(archive.meta.title.c_str(), entry.title.c_str()) != 0)
        {
            error = L"索引与档案不是同一条，已丢弃：两边的会社 / 作品对不上。\r\n"
                    L"索引说：" + entry.publisher + L" / " + entry.title + L"\r\n"
                    L"档案自己的 META 说：" + archive.meta.publisher + L" / "
                  + archive.meta.title + L"\r\n"
                    L"索引是信任根，装进库里放哪个目录必须可预测 —— 不一致意味着索引写错了，"
                    L"或者这个位置上的档案被换成了别的东西，本工具不装。";
            return false;
        }

        // ---------- 第 5 条：档案自洽性 ----------
        // (a) 例外必须为 0。有例外 = 生成时就有名字对不上，等于这份档案自带"补错"。
        if (!archive.exceptions.empty() || archive.stats.exceptions != 0u)
        {
            error = L"档案自洽性检查不过：EXCEPTIONS 有 "
                  + FormatUInt(archive.exceptions.size()) + L" 条、STATS 自称 "
                  + FormatUInt(archive.stats.exceptions)
                  + L" 条，两者都必须为 0。有例外的档案本工具不接受。";
            return false;
        }

        // (b) 名字层必须能用内嵌盐展开，且条数与名字列表、与 STATS 都对得上。
        //     防的是"名字列表被改短 / 被截断"：更短的名字列表会让还原率静默变差，
        //     而用户只会觉得"这个扩展集没什么用"。
        //     **注意**：档案里没有 L3 哈希层（§3.1），所以这里证明的只是"条数自洽"，
        //     **不是**"每个名字对得上原游戏的哈希" —— 本地没有哈希可以对照，验不了。
        std::wstring lookupError;
        if (!ExtensionSet::RebuildLookup(archive, lookupError))
        {
            error = L"档案名字层展开失败：" + lookupError;
            return false;
        }
        if (archive.directoryNames.size() != archive.directories.size()
            || archive.fileNames.size() != archive.files.size())
        {
            error = L"档案自洽性检查不过：展开出的名字条数（目录 "
                  + FormatUInt(archive.directoryNames.size()) + L" / 文件 "
                  + FormatUInt(archive.fileNames.size()) + L"）与名字列表本身（目录 "
                  + FormatUInt(archive.directories.size()) + L" / 文件 "
                  + FormatUInt(archive.files.size()) + L"）对不上 —— 名字列表里有重复或空条目。";
            return false;
        }
        if (archive.directories.size() != archive.stats.directories
            || archive.files.size() != archive.stats.files)
        {
            error = L"档案自洽性检查不过：名字条数与 STATS 对不上（名字列表是 目录 "
                  + FormatUInt(archive.directories.size()) + L" / 文件 "
                  + FormatUInt(archive.files.size()) + L"，STATS 说 目录 "
                  + FormatUInt(archive.stats.directories) + L" / 文件 "
                  + FormatUInt(archive.stats.files) + L"）—— 名字列表被改短或档案被截断。";
            return false;
        }

        // ---------- 结论 ----------
        // 给人看的一段话：能直接显示、也能进覆盖确认框。这里写清楚**验过哪几条**，
        // 以及**验不了什么**（名字对不对），免得用户以为这份档案被证明"名字全对"。
        const unsigned long long total = static_cast<unsigned long long>(archive.directories.size())
            + static_cast<unsigned long long>(archive.files.size());
        // 会社 / 作品：交叉校验已经过了，两边一致（最多差大小写），所以印档案自己的 META ——
        // 安装槽位就是按它落盘的。（empty 兜底只为手工构造的 entry，正常走不到。）
        const std::wstring publisher = archive.meta.publisher.empty() ? entry.publisher
                                                                      : archive.meta.publisher;
        const std::wstring title = archive.meta.title.empty() ? entry.title : archive.meta.title;

        detail = L"会社：" + publisher + L"\r\n";
        detail += L"作品：" + title + L"\r\n";
        detail += L"条数：目录 " + FormatUInt(archive.directories.size()) + L" 条 + 文件 "
                + FormatUInt(archive.files.size()) + L" 条 = " + FormatUInt(total) + L" 条\r\n";
        if (static_cast<unsigned long long>(entry.entryCount) != total)
        {
            detail += L"索引写的条数：" + FormatUInt(entry.entryCount)
                    + L"（与档案里的实际条数不一致，请留意）\r\n";
        }
        detail += L"盐：" + (archive.identity.salt.empty() ? std::wstring(L"（空串 —— 合法的空盐）")
                                                           : archive.identity.salt)
                + L"\r\n";
        detail += L"目标 exe：" + archive.identity.targetExe.name + L"（"
                + FormatUInt(archive.identity.targetExe.size) + L" 字节）\r\n";
        detail += L"目标 exe 摘要：" + archive.identity.targetExe.sha256 + L"\r\n";
        detail += L"档案摘要：" + actualDigest + L"\r\n";
        detail += L"已核：字节与索引一致（sha256）、结构完整（magic / 版本 / CRC32 / chunk）、"
                  L"内嵌 exe 摘要等于索引、会社 / 作品与索引一致、无自校验例外。\r\n";
        detail += L"说明：档案里不含哈希对照表，本工具无法校验「名字对不对」；"
                  L"名字成不成立以实际还原率为准。\r\n";
        return true;
    }
}
}
