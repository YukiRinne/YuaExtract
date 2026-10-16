// 扩展集「在线更新」的网络与内核层实现。接口与口径见 OnlineUpdate.h，设计依据见
// docs\扩展集设计.md §14（尤其 §14.4 的信任链 8 条）。
//
// 本文件只做三件事：把字节从 https 拿回来、把拿回来的东西按信任链验一遍、把基址配置读回来 /
// 写回去。**一次都不碰本地扩展集库** —— 入库是界面调 ExtensionSet::InstallToLibrary 的事。
// 下到的字节只落在 staging 目录里，任何一条校验不过就把临时文件删掉走人（§14.4 第 6 条）。
//
// 两句"不要"写在这里，免得后来的人顺手加上：
//   · 不要自动联网、不要后台轮询（§14.4 第 8 条）：只有用户点了「在线更新」才走到这里；
//   · 不要声称能验证"名字对不对"：档案的 L3 哈希层默认不存（§3.1），本地**没有哈希可对照**，
//     信任根是索引（§14.4 第 4 条）。把某个名字改掉，本层发现不了 —— 名字成不成立，
//     最终仍由"拿它去还原、看还原率"说了算。

#include "OnlineUpdate.h"

// 工具目录（ini 落点、staging 落点）与产品版本（UA）都从这里取：它们必须和界面认的是
// 同一个来源，不能各写一份常量。本文件与界面同属一个 exe，不新增任何模块依赖。
#include "YuaExtractHost.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
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

            HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = ::GetLastError();
                return false;
            }

            std::vector<unsigned char> buffer(64u * 1024u);
            for (;;)
            {
                DWORD read = 0u;
                if (!::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
                {
                    error = ::GetLastError();
                    ::CloseHandle(file);
                    return false;
                }
                if (read == 0u)
                {
                    break;
                }
                out.insert(out.end(), buffer.begin(), buffer.begin() + read);
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
        //   · '/' 保持原样（它是路径分隔符）；
        //   · '%' 也保持原样 —— 用户配的镜像基址可能本来就带 %20 这种转义，再转一次会把地址弄坏；
        //     代价是会社名里真出现 '%' 时会被当成转义开头（现实中不存在，MakeSafePathComponent 也
        //     不会去掉它，但两害相权取"基址不会被弄坏"）。
        std::wstring EscapeUrlPath(const std::wstring& path)
        {
            const std::string utf8 = WideToUtf8(path);
            constexpr const char kHex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(utf8.size() + 16u);
            for (unsigned char ch : utf8)
            {
                const bool safe = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')
                    || (ch >= '0' && ch <= '9') || ch == L'-' || ch == L'.' || ch == L'_'
                    || ch == L'~' || ch == L'/' || ch == L'%';
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

        struct UrlParts
        {
            std::wstring host;
            INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
            std::wstring path;   // 已转义，以 '/' 开头
        };

        // 自己拆 URL（而不是 WinHttpCrackUrl）：一是要在拆的时候就断掉非 https，
        // 二是只对**路径**做转义、主机与端口一个字节不改。主机名支持 [IPv6] 字面量。
        bool ParseHttpsUrl(const std::wstring& url, UrlParts& out, std::wstring& error)
        {
            if (url.empty())
            {
                error = L"地址是空的。";
                return false;
            }
            if (!StartsWithHttps(url))
            {
                error = L"只允许 https 地址：\"" + url + L"\" 不是 https:// 开头的。";
                return false;
            }

            size_t slash = url.find(L'/', 8u);
            if (slash == std::wstring::npos)
            {
                slash = url.size();
            }
            const std::wstring authority = url.substr(8u, slash - 8u);
            std::wstring path = slash < url.size() ? url.substr(slash) : std::wstring(L"/");

            // 本层只 GET 静态文件：地址里带了 ?query 或 #fragment 就是拼错了，丢掉，
            // 免得把查询串当成文件名的一部分去请求。
            const size_t cut = path.find_first_of(L"?#");
            if (cut != std::wstring::npos)
            {
                path.erase(cut);
            }

            out = UrlParts();
            size_t portStart = std::wstring::npos;
            if (!authority.empty() && authority.front() == L'[')
            {
                const size_t close = authority.find(L']');
                if (close == std::wstring::npos)
                {
                    error = L"地址里的 IPv6 字面量没有收尾的 ']'：\"" + url + L"\"。";
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
                    error = L"地址里的端口不合法：\"" + url + L"\"。";
                    return false;
                }
                out.port = static_cast<INTERNET_PORT>(port);
            }
            if (out.host.empty())
            {
                error = L"地址里没有主机名：\"" + url + L"\"。";
                return false;
            }

            out.path = EscapeUrlPath(path);
            if (out.path.empty() || out.path.front() != L'/')
            {
                out.path.insert(out.path.begin(), L'/');
            }
            return true;
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

        struct HttpResponse
        {
            DWORD status = 0u;   // 无论成败都填：调用方要能区分"没连上"和"连上了但不是 200"
            std::vector<unsigned char> body;
        };

        // 一次只读 GET 的全部约束都在这里：仅 https、不跟随重定向、三段超时、
        // **边收边算**的体积上限。远端是不可信输入，任一条不满足就整次作废；
        // 本函数不落盘、不报错给用户看，把字节与原因交回调用方处置。
        bool HttpGet(const std::wstring& url, unsigned long long maxBytes, const std::wstring& stage,
                     const Request& request, HttpResponse& response, std::wstring& error)
        {
            error.clear();
            response = HttpResponse();

            UrlParts parts;
            if (!ParseHttpsUrl(url, parts, error))
            {
                return false;
            }
            if (request.cancel && request.cancel())
            {
                error = L"已取消。";
                return false;
            }

            InternetHandle session(::WinHttpOpen(UserAgent().c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
            if (!session)
            {
                // AUTOMATIC_PROXY 要 Win8.1 起；更老的系统上退一步用默认代理策略再试一次。
                session = InternetHandle(::WinHttpOpen(UserAgent().c_str(),
                                                       WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS,
                                                       0));
            }
            if (!session)
            {
                error = DescribeWinHttpFailure(stage, url, ::GetLastError());
                return false;
            }

            // 超时是**每一段**各算（§14.4 第 1 条）。DNS 解析没有单独的常量，
            // 按"连接"那一段算 —— 解析不出来和连不上都是同一类"到不了对方"。
            ::WinHttpSetTimeouts(session.get(), kConnectTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs,
                                 kReceiveTimeoutMs);

            DWORD optionError = 0u;
            DisableRedirects(session.get(), optionError);   // 会话上先设一次；失败也无所谓（请求上还会设）

            InternetHandle connection(::WinHttpConnect(session.get(), parts.host.c_str(), parts.port, 0));
            if (!connection)
            {
                error = DescribeWinHttpFailure(stage, url, ::GetLastError());
                return false;
            }

            InternetHandle httpRequest(::WinHttpOpenRequest(connection.get(), L"GET", parts.path.c_str(),
                                                            nullptr, WINHTTP_NO_REFERER,
                                                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                            WINHTTP_FLAG_SECURE));
            if (!httpRequest)
            {
                error = DescribeWinHttpFailure(stage, url, ::GetLastError());
                return false;
            }

            // 设不上就整次作废，不退而求其次（宁可不下载，也不接受"可能跟着跳到 http"）。
            if (!DisableRedirects(httpRequest.get(), optionError))
            {
                error = stage + L"失败：关不掉 HTTP 重定向（Win32 错误码 " + FormatUInt(optionError)
                      + L"）。本工具只走 https 且不跟随重定向，宁可不下载。地址：" + url;
                return false;
            }

            if (!::WinHttpSendRequest(httpRequest.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0))
            {
                error = DescribeWinHttpFailure(stage, url, ::GetLastError());
                return false;
            }
            if (!::WinHttpReceiveResponse(httpRequest.get(), nullptr))
            {
                error = DescribeWinHttpFailure(stage, url, ::GetLastError());
                return false;
            }

            DWORD status = 0u;
            DWORD statusSize = sizeof(status);
            if (!::WinHttpQueryHeaders(httpRequest.get(),
                                       WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                       WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                                       WINHTTP_NO_HEADER_INDEX))
            {
                error = DescribeWinHttpFailure(stage, url, ::GetLastError());
                return false;
            }
            response.status = status;

            if (status != 200u)
            {
                // 状态码已经拿到，这时说的话比"操作失败"有用得多，不再往下收字节。
                error = DescribeHttpStatus(stage, url, httpRequest.get(), status);
                return false;
            }

            // 体积上限**边收边算**：声明的上限只是上限，真正的把关是"下一块越界就立刻停"。
            // 收完再判 = 对方能给多少，就能把内存吃掉多少。
            std::vector<unsigned char> body;
            for (;;)
            {
                if (request.cancel && request.cancel())
                {
                    error = L"已取消。";
                    return false;   // 调用方负责删临时文件
                }

                DWORD available = 0u;
                if (!::WinHttpQueryDataAvailable(httpRequest.get(), &available))
                {
                    error = DescribeWinHttpFailure(stage + L"（读响应体）", url, ::GetLastError());
                    return false;
                }
                if (available == 0u)
                {
                    break;   // 收完了
                }

                const size_t already = body.size();
                if (static_cast<unsigned long long>(already) + available > maxBytes)
                {
                    error = stage + L"失败：对方给的内容超过上限（" + FormatUInt(maxBytes)
                          + L" 字节），已中止下载。地址：" + url
                          + L"\r\n正常的扩展集只有几百 KB，这么大的东西不是你想要的。";
                    return false;
                }

                body.resize(already + available);
                DWORD received = 0u;
                if (!::WinHttpReadData(httpRequest.get(), body.data() + already, available, &received))
                {
                    error = DescribeWinHttpFailure(stage + L"（读响应体）", url, ::GetLastError());
                    return false;
                }
                body.resize(already + received);
                if (received == 0u)
                {
                    break;   // 防死循环：要过一次还是 0 就不再要了
                }
            }

            response.body.swap(body);
            return true;
        }

        // 尽力删一个文件：删不掉不报错、不抛。调用方多半接下来会清掉整个 staging 目录。
        void DeleteFileBestEffort(const std::wstring& path)
        {
            if (!path.empty())
            {
                ::DeleteFileW(path.c_str());
            }
        }

        // 递归删一棵目录树。**只用于 staging**：那个目录是本层自己造的、里面全是下载来的
        // 临时文件，才敢整棵删。所有失败（占用、权限）一律忽略 —— 这里报错也帮不上谁。
        void RemoveDirectoryTree(const std::wstring& directory)
        {
            if (directory.empty() || directory.size() <= 3u)
            {
                return;   // 不删盘根：万一调用方传了个 "C:\"，那不是"尽力"该干的事
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
                    if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                    {
                        RemoveDirectoryTree(path);
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
    }

    // =====================================================================
    // 基址
    // =====================================================================

    std::wstring NormalizeBase(const std::wstring& base)
    {
        // 去首尾空白 + 末尾保证一个 '/'。
        //
        // **顺带把「仅 https」判在这里**（§14.4 第 1 条）：签名上没有 error 出参，
        // 所以"不可用"只能用**返回空串**表示；调用入口拿到空串必须显式报错并说明原因，
        // 不许拿空串去拼 URL（拼出来会变成相对地址，那才是真危险）。
        //
        // 这里**不再**顺带判"URL 里有没有非法字符"：那会让内置常量里的占位符
        //（`.../<owner>/<repo>/...`）也被折成空串，界面就没法把"你还没配过仓库地址"显示出来。
        // 非法字符改由 ResolveUsableBase 在**真正要联网 / 要落盘**的入口上判（措辞更准）。
        const std::wstring trimmed = TrimSpaces(base);
        if (trimmed.empty() || !StartsWithHttps(trimmed))
        {
            return std::wstring();
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
        // 带 BOM：基址里可能有非 ASCII，记事本打开时才知道这是 UTF-8，别让用户看成乱码。
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

    std::wstring IndexUrl(const std::wstring& base)
    {
        const std::wstring normalized = NormalizeBase(base);
        if (normalized.empty())
        {
            return std::wstring();   // 拼不出来就给空串，调用方必须据此报错
        }
        return normalized + kIndexFileName;
    }

    std::wstring ArchiveUrl(const std::wstring& base, const std::wstring& publisher,
                            const std::wstring& title)
    {
        // §14.2：远端按 <会社>/<作品>/rules.int 放，与 InstallToLibrary 的落点规则同构。
        // 会社 / 作品先过 MakeSafePathComponent（去掉 '/'、'\'、'..' 这类能跳出目录的东西）；
        // 去完为空就返回空串，让调用方报错，别拿空目录名去拼一个必然 404 的地址。
        const std::wstring normalized = NormalizeBase(base);
        const std::wstring safePublisher = ExtensionSet::MakeSafePathComponent(publisher);
        const std::wstring safeTitle = ExtensionSet::MakeSafePathComponent(title);
        if (normalized.empty() || safePublisher.empty() || safeTitle.empty())
        {
            return std::wstring();
        }
        return normalized + safePublisher + L"/" + safeTitle + L"/" + kRemoteArchiveFileName;
    }

    // =====================================================================
    // 拉索引
    // =====================================================================

    bool FetchIndex(const std::wstring& base, std::vector<ExtensionSet::LibraryEntry>& out,
                    IndexResult& info, std::wstring& error, const Request& request)
    {
        out.clear();
        error.clear();
        info = IndexResult();

        // 入口校基址：只 https、不能还是占位符、不能带 URL 非法字符 —— 三种情况各有各的措辞
        //（用户改了 ini 或环境变量却没生效时，靠的就是这句话）。
        std::wstring normalized;
        if (!ResolveUsableBase(base, normalized, error))
        {
            return false;
        }

        info.url = IndexUrl(normalized);
        if (request.progress)
        {
            request.progress(5, L"正在拉取索引：" + info.url);
        }

        HttpResponse response;
        const bool fetched = HttpGet(info.url, kMaxIndexBytes, L"拉取索引", request, response, error);
        info.httpStatus = static_cast<int>(response.status);
        if (!fetched)
        {
            return false;   // 措辞已经分门别类写好了，原样带上去
        }
        info.bytes = static_cast<unsigned long long>(response.body.size());

        if (!IsValidUtf8(response.body))
        {
            error = L"索引格式不对（" + info.url + L"）：内容不是合法的 UTF-8 文本。"
                    L"\r\n（索引就是一份 manifest.txt，UTF-8 的 TSV。）";
            return false;
        }

        if (request.progress)
        {
            request.progress(60, L"正在解析索引…");
        }

        const std::string text = response.body.empty()
            ? std::string()
            : std::string(reinterpret_cast<const char*>(response.body.data()), response.body.size());

        // 索引的解析**走本地库清单同一份实现**（§14.3）：两处口径一分叉，就会出现
        // "本地能读、远端读不了"这种怪事。空索引不算错（仓库刚开张就是 0 条）。
        std::vector<ExtensionSet::LibraryEntry> entries;
        std::wstring parseReport;
        if (!ExtensionSet::ParseManifestText(text, entries, parseReport))
        {
            // 整份不可用：某一行"写了值但值是坏的"。
            error = L"索引格式不对（" + info.url + L"）：\r\n" + parseReport;
            return false;
        }
        if (!parseReport.empty())
        {
            // 解析器对"列数不对 / 会社作品为空"是**跳过并报告**（本地库那边靠它保住其余条目）。
            // 索引不能这么办：它是仓库的全部内容，跳一行等于用户永远看不到那一部作品，
            // 而且 §14.3 明确要求缺列/多列必须报错点名行号 —— 所以在线这一侧把报告升级成硬错。
            error = L"索引里有不合规的行（索引必须整份干净，一行都不能跳过）：\r\n" + parseReport
                  + L"地址：" + info.url;
            return false;
        }

        // 只属于**在线索引**的要求（§14.3 / §14.4 第 2、4 条）：目标 exe 摘要与档案摘要
        // 必须非空。本地库清单允许空（那是用户自己装过的东西的记录），索引不行 ——
        // 少了目标 exe 摘要就没有信任根可对，少了档案摘要就没法把字节钉死。
        // 格式（64 位 hex）解析器已经保证过了（非空却不合法的行会整份作废）。
        for (const ExtensionSet::LibraryEntry& item : entries)
        {
            if (!ExtensionSet::IsHexLength(item.exeDigest, 64u))
            {
                error = L"索引里「" + item.publisher + L" / " + item.title
                      + L"」这一条没有目标 exe 摘要（该列是空的）。\r\n"
                        L"索引是在线的信任根，缺了它，下载回来的字节就没有东西可以对，拒绝。\r\n"
                        L"地址：" + info.url;
                return false;
            }
            if (!ExtensionSet::IsHexLength(item.archiveDigest, 64u))
            {
                error = L"索引里「" + item.publisher + L" / " + item.title
                      + L"」这一条没有档案摘要（该列是空的）。\r\n"
                        L"下载回来的档案要拿它对（§14.4 第 2 条），没有它就无法校验，拒绝。\r\n"
                        L"地址：" + info.url;
                return false;
            }
        }

        info.entryCount = static_cast<unsigned int>(entries.size());
        out = std::move(entries);
        if (request.progress)
        {
            request.progress(100, L"索引读取完成，共 " + FormatUInt(info.entryCount) + L" 条扩展集。");
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
        for (const ExtensionSet::LibraryEntry& candidate : local.entries)
        {
            const bool sameSlot = _wcsicmp(candidate.publisher.c_str(), remote.publisher.c_str()) == 0
                && _wcsicmp(candidate.title.c_str(), remote.title.c_str()) == 0;
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
        // 临时档案跟日志一起放：<工具目录>\Log\OnlineStaging。
        // 工具目录不可写时退到 %TEMP%\YuaExtract\OnlineStaging —— 下载回来的东西是
        // **不可信输入**，宁可放临时目录，也不去试别的地方（§14.4 第 6 条只要求"能删干净"）。
        //
        // 顺手把目录建出来：调用方拿到路径就能直接写，判"能不能写"也只能靠真去建一次
        //（目录属性判不出 ACL）。两个都建不出来时返回空串，调用方必须据此报错。
        const std::wstring primary = PrimaryStagingDirectory();
        if (!primary.empty())
        {
            DWORD createError = 0u;
            if (EnsureDirectoryTree(primary, createError))
            {
                return primary;
            }
        }

        const std::wstring fallback = FallbackStagingDirectory();
        if (!fallback.empty())
        {
            DWORD createError = 0u;
            if (EnsureDirectoryTree(fallback, createError))
            {
                return fallback;
            }
        }
        return std::wstring();
    }

    void CleanupStaging(const std::wstring& stagingDirectory)
    {
        // 尽力删：删不掉不是错误、不抛（文件可能正被占用、权限不够）。
        // 调用时机是"校验失败后"与"入库成功后" —— 两处都不该把下载来的东西留在地上。
        if (!stagingDirectory.empty())
        {
            RemoveDirectoryTree(stagingDirectory);
            return;
        }

        // 没指定就表示"用默认的那个"，而默认可能有迹可循的两个候选（工具目录 / %TEMP%）。
        // 这里只看**已经存在的**：清理不该为了确认能不能写再去建一个目录出来。
        const std::wstring primary = PrimaryStagingDirectory();
        if (!primary.empty() && DirectoryExists(primary))
        {
            RemoveDirectoryTree(primary);
        }
        const std::wstring fallback = FallbackStagingDirectory();
        if (!fallback.empty() && DirectoryExists(fallback))
        {
            RemoveDirectoryTree(fallback);
        }
    }

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

        const std::wstring url = ArchiveUrl(normalized, publisher, title);
        if (url.empty())
        {
            error = L"拼不出档案地址：会社 / 作品名不可用。";
            return false;
        }

        const std::wstring staging =
            stagingDirectory.empty() ? DefaultStagingDirectory() : stagingDirectory;
        if (staging.empty())
        {
            error = L"找不到可用的临时目录（工具目录下的 Log 与 %TEMP% 都写不了）。";
            return false;
        }
        DWORD createError = 0u;
        if (!EnsureDirectoryTree(staging, createError))
        {
            error = L"建不出临时目录：" + staging + L"（Win32 错误码 " + FormatUInt(createError)
                  + L"）";
            return false;
        }

        const std::wstring path =
            CombinePath(staging, L"Online_" + publisher + L"_" + title + L".rules.int");

        // 本函数**只负责拿到字节**：不校验、不动本地库（§14.4 第 2–5 条在 VerifyArchive，
        // 入库在 ExtensionSet::InstallToLibrary）。中间隔着两个显式函数调用，
        // "下载成功但校验没过"就不可能顺手改了本地库。
        if (request.progress)
        {
            request.progress(10, L"正在下载：" + url);
        }

        HttpResponse response;
        if (!HttpGet(url, kMaxArchiveBytes, L"下载档案", request, response, error))
        {
            // 失败路径一律不留临时文件（§14.4 第 6 条）。
            DeleteFileBestEffort(path);
            return false;
        }
        if (response.body.empty())
        {
            // 200 却一个字节都没有 = 这次下载没成功（正常档案至少也有个头）。
            // 这不是"校验"，不必等 VerifyArchive 去说"magic 不对"。
            error = L"下载失败：服务器返回 200，但内容是 0 字节。地址：" + url;
            DeleteFileBestEffort(path);
            return false;
        }

        if (!WriteAllBytes(path, response.body.data(), response.body.size(), createError))
        {
            error = L"写临时档案失败：" + path + L"（Win32 错误码 " + FormatUInt(createError) + L"）";
            DeleteFileBestEffort(path);
            return false;
        }

        out.path = path;
        out.bytes = static_cast<unsigned long long>(response.body.size());
        // 摘要就地对内存里的字节算，而不是再读一遍刚落盘的文件：刚落盘的文件理论上
        // 可能被别的进程换掉，对"我们真正拿到的那串字节"算 sha256 才是对的。
        out.sha256 = ExtensionSet::Sha256HexBytes(response.body.data(), response.body.size());

        if (request.progress)
        {
            request.progress(100, L"下载完成（" + FormatUInt(out.bytes) + L" 字节）。");
        }
        return true;
    }

    bool VerifyArchive(const std::wstring& archivePath, const ExtensionSet::LibraryEntry& entry,
                       std::wstring& detail, std::wstring& error)
    {
        detail.clear();
        error.clear();

        if (archivePath.empty())
        {
            error = L"没有给出要校验的档案路径。";
            return false;
        }
        if (entry.archiveDigest.empty() || entry.exeDigest.empty())
        {
            error = L"索引里这一条缺「档案摘要」或「目标 exe 摘要」，没有信任根可以对照，拒绝校验。";
            return false;
        }

        // ---------- 第 2 条：sha256 必须等于索引里的「档案摘要」 ----------
        // 这一条把"下到的字节"钉死在索引上：字节只要被动过一个比特就过不去。
        const std::wstring actualDigest = ExtensionSet::Sha256HexFile(archivePath);
        if (actualDigest.empty())
        {
            error = L"读不到要校验的档案：" + archivePath;
            return false;
        }
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
        if (!ExtensionSet::ReadArchiveFile(archivePath, archive, decodeError))
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
