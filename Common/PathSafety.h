#pragma once

// Windows 路径边界判定（header-only，各工程共享）。
//
// 用途是把"映射表/日志里读来的相对名字"和"实际要在哪个根下落盘"这两件事分开：
//   1) 相对名字先过词法校验（绝对路径、盘符、ADS、设备名、..、尾点/尾空白一律拒绝）；
//   2) 再拼到声明根下，用 GetFullPathNameW 标准化（分隔符、./..、盘根、UNC）；
//   3) 最后按**组件边界**比较是否仍在根下（忽略大小写），并检查已存在路径上
//      每一级是不是重解析点（junction / symlink），避免写到链接目标里。
//
// 纯词法边界 + 重解析点检查，不声称能防同权限进程的所有 TOCTOU；本工具不新增驱动/沙箱。
// 目录名允许正常的空根域（""、"/"、"%EmptyString%"）与多层结构；文件名则只允许单段。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>

namespace PathSafety
{
    enum class Kind
    {
        Directory,      // 目录映射：允许空根域与多层，末段是目录名
        File,           // 单段文件名
        RelativePath,   // 多层相对路径，末段是文件名（文件映射允许自带目录）
    };

    inline bool IsSeparator(wchar_t ch)
    {
        return ch == L'\\' || ch == L'/';
    }

    inline bool IsRootDomainName(const std::wstring& name)
    {
        if (name.empty())
        {
            return true;
        }
        if (name == L"%EmptyString%")
        {
            return true;
        }
        for (wchar_t ch : name)
        {
            if (!IsSeparator(ch))
            {
                return false;
            }
        }
        return true;
    }

    inline std::wstring TrimTrailingSeparators(std::wstring path)
    {
        while (path.size() > 1u && IsSeparator(path.back()))
        {
            // 盘根 "C:\" 与 UNC 根 "\\server\share\" 的结尾分隔符要留着
            if (path.size() == 3u && path[1] == L':')
            {
                break;
            }
            path.pop_back();
        }
        return path;
    }

    inline std::wstring ToBackslashes(std::wstring text)
    {
        for (wchar_t& ch : text)
        {
            if (ch == L'/')
            {
                ch = L'\\';
            }
        }
        return text;
    }

    // 动态 GetFullPathNameW：路径长度不受 MAX_PATH 限制；失败返回空串。
    // 保留了盘根（C:\）与 UNC 根（\\server\share\），并把 ./.. 折叠掉。
    inline std::wstring NormalizeFullPath(const std::wstring& path)
    {
        if (path.empty())
        {
            return std::wstring();
        }
        // embedded NUL：Windows API 只看前半段，比较层看到的是后半段，必须直接拒绝。
        if (path.find(L'\0') != std::wstring::npos)
        {
            return std::wstring();
        }
        // 设备命名空间 / 全局根：不是普通文件系统根，比较时容易绕过边界。
        if (path.rfind(L"\\\\?\\", 0) == 0 || path.rfind(L"\\\\.\\", 0) == 0 ||
            path.rfind(L"\\??\\", 0) == 0 || path.rfind(L"//?/", 0) == 0 ||
            path.rfind(L"//./", 0) == 0)
        {
            return std::wstring();
        }
        // 盘符相对路径（"C:foo"）语义取决于每盘当前目录，不能当声明根。
        if (path.size() == 2u && path[1] == L':')
        {
            return std::wstring();
        }
        // UNC 必须至少有 \\server\share 两段。
        if ((path.size() >= 2u && path[0] == L'\\' && path[1] == L'\\') ||
            (path.size() >= 2u && path[0] == L'/' && path[1] == L'/'))
        {
            const size_t first = path.find_first_of(L"\\/", 2u);
            if (first == std::wstring::npos)
            {
                return std::wstring();
            }
            const size_t second = path.find_first_of(L"\\/", first + 1u);
            if (first == 2u || first + 1u >= path.size() ||
                (second != std::wstring::npos && second == first + 1u))
            {
                return std::wstring();
            }
        }

        DWORD size = ::GetFullPathNameW(path.c_str(), 0u, nullptr, nullptr);
        if (size == 0u)
        {
            return std::wstring();
        }
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            std::wstring buffer(static_cast<size_t>(size), L'\0');
            const DWORD length =
                ::GetFullPathNameW(path.c_str(), size, &buffer[0], nullptr);
            if (length == 0u)
            {
                return std::wstring();
            }
            if (length < size)
            {
                buffer.resize(static_cast<size_t>(length));
                buffer = ToBackslashes(buffer);
                if (buffer.size() > 3u)
                {
                    // 非根目录去掉结尾分隔符；根目录（C:\）保持原样。
                    std::wstring trimmed = TrimTrailingSeparators(buffer);
                    if (!trimmed.empty())
                    {
                        buffer.swap(trimmed);
                    }
                }
                return buffer;
            }
            size = length;
        }
        return std::wstring();
    }

    // 拆出"根前缀 + 组件"。根前缀形如 "C:\" 或 "\\server\share\"。
    // 只处理本工具会遇到的本地盘 / UNC；卷 GUID 之类按普通路径处理（仍能比较）。
    struct SplitPath
    {
        std::wstring prefix;                 // 含结尾分隔符（盘根 / UNC 共享根）
        std::vector<std::wstring> parts;     // 根之后的组件
        bool valid = false;
    };

    inline SplitPath SplitFullPath(const std::wstring& normalized)
    {
        SplitPath out;
        if (normalized.empty())
        {
            return out;
        }

        size_t index = 0;
        if (normalized.size() >= 2u && normalized[0] == L'\\' && normalized[1] == L'\\')
        {
            // UNC：\\server\share
            size_t first = normalized.find(L'\\', 2u);
            if (first == std::wstring::npos)
            {
                out.prefix = normalized + L"\\";
                out.valid = true;
                return out;
            }
            size_t second = normalized.find(L'\\', first + 1u);
            if (second == std::wstring::npos)
            {
                out.prefix = normalized + L"\\";
                out.valid = true;
                return out;
            }
            out.prefix = normalized.substr(0u, second + 1u);
            index = second + 1u;
        }
        else if (normalized.size() >= 2u && normalized[1] == L':')
        {
            size_t separator = normalized.find(L'\\', 2u);
            if (separator == std::wstring::npos)
            {
                out.prefix = normalized.substr(0u, 2u) + L"\\";
                out.valid = true;
                return out;
            }
            out.prefix = normalized.substr(0u, separator + 1u);
            index = separator + 1u;
        }
        else
        {
            // 相对路径不该走到这里；把当前根当分隔符处理会误导，直接判无效。
            return out;
        }

        size_t start = index;
        while (start < normalized.size())
        {
            size_t end = normalized.find(L'\\', start);
            if (end == std::wstring::npos)
            {
                end = normalized.size();
            }
            if (end > start)
            {
                out.parts.push_back(normalized.substr(start, end - start));
            }
            start = end + 1u;
        }
        out.valid = true;
        return out;
    }

    inline bool SamePath(const std::wstring& left, const std::wstring& right)
    {
        const std::wstring a = NormalizeFullPath(left);
        const std::wstring b = NormalizeFullPath(right);
        if (a.empty() || b.empty())
        {
            return false;
        }
        return _wcsicmp(a.c_str(), b.c_str()) == 0;
    }

    // path 是否在 root 下（含相等，allowEqual 控制）。按组件比较，C:\a 不会匹配 C:\ab。
    inline bool IsUnderRoot(const std::wstring& root, const std::wstring& path, bool allowEqual = true)
    {
        const std::wstring normalizedRoot = NormalizeFullPath(root);
        const std::wstring normalizedPath = NormalizeFullPath(path);
        if (normalizedRoot.empty() || normalizedPath.empty())
        {
            return false;
        }

        const SplitPath r = SplitFullPath(normalizedRoot);
        const SplitPath p = SplitFullPath(normalizedPath);
        if (!r.valid || !p.valid || _wcsicmp(r.prefix.c_str(), p.prefix.c_str()) != 0)
        {
            return false;
        }
        if (r.parts.size() > p.parts.size())
        {
            return false;
        }
        for (size_t i = 0; i < r.parts.size(); ++i)
        {
            if (_wcsicmp(r.parts[i].c_str(), p.parts[i].c_str()) != 0)
            {
                return false;
            }
        }
        if (r.parts.size() == p.parts.size() && !allowEqual)
        {
            return false;
        }
        return true;
    }

    // 两者相同、或者任一方向嵌套（用于拒绝"输出根在源根里"这类递归复制）。
    inline bool PathsOverlap(const std::wstring& left, const std::wstring& right)
    {
        return IsUnderRoot(left, right, true) || IsUnderRoot(right, left, true);
    }

    inline std::wstring ComponentBaseName(const std::wstring& component)
    {
        const size_t dot = component.find(L'.');
        return dot == std::wstring::npos ? component : component.substr(0u, dot);
    }

    inline bool IsReservedDeviceName(const std::wstring& component)
    {
        std::wstring base = ComponentBaseName(component);
        while (!base.empty() && (base.back() == L' ' || base.back() == L'.'))
        {
            base.pop_back();
        }
        if (base.empty())
        {
            return false;
        }
        std::wstring upper;
        upper.reserve(base.size());
        for (wchar_t ch : base)
        {
            upper.push_back(static_cast<wchar_t>(::towupper(ch)));
        }

        if (upper == L"CON" || upper == L"PRN" || upper == L"AUX" || upper == L"NUL" ||
            upper == L"CONIN$" || upper == L"CONOUT$")
        {
            return true;
        }
        if (upper.size() == 4u && (upper.compare(0u, 3u, L"COM") == 0 || upper.compare(0u, 3u, L"LPT") == 0) &&
            upper[3] >= L'1' && upper[3] <= L'9')
        {
            return true;
        }
        return false;
    }

    inline bool ValidateComponent(const std::wstring& component, bool isFileLeaf, std::wstring* why)
    {
        if (component.empty())
        {
            if (why) *why = L"路径里有空组件";
            return false;
        }
        if (component.size() > 255u)
        {
            if (why) *why = L"单个名字超过 255 字符";
            return false;
        }
        if (component == L"." || component == L"..")
        {
            if (why) *why = L"含 . 或 .. 组件";
            return false;
        }

        for (wchar_t ch : component)
        {
            if (ch < 0x20 || ch == 0x7F)
            {
                if (why) *why = L"含控制字符/NUL";
                return false;
            }
            if (ch == L':' || ch == L'<' || ch == L'>' || ch == L'"' || ch == L'|' ||
                ch == L'?' || ch == L'*')
            {
                if (why) *why = (ch == L':') ? L"含冒号（盘符/备用数据流）" : L"含 Windows 文件名非法字符";
                return false;
            }
        }

        if (component.back() == L'.' || component.back() == L' ')
        {
            if (why) *why = L"名字以点或空格结尾";
            return false;
        }
        if (IsReservedDeviceName(component))
        {
            if (why) *why = L"使用了 Windows 设备保留名";
            return false;
        }

        (void)isFileLeaf;
        return true;
    }

    // 校验一个相对名字。kind=Directory 时允许空根域。
    inline bool ValidateRelativeName(const std::wstring& name, Kind kind, std::wstring* why)
    {
        if (why) why->clear();

        if (kind == Kind::Directory && IsRootDomainName(name))
        {
            return true;
        }
        if (name.empty())
        {
            if (why) *why = L"名字为空";
            return false;
        }
        if (name.size() > 32000u)
        {
            if (why) *why = L"相对路径过长";
            return false;
        }

        // 绝对路径 / 盘符 / UNC / 设备前缀
        if (IsSeparator(name[0]))
        {
            if (why) *why = L"是绝对路径或设备路径";
            return false;
        }
        if (name.size() >= 2u && name[1] == L':')
        {
            if (why) *why = L"带盘符";
            return false;
        }
        for (wchar_t ch : name)
        {
            if (ch == L':')
            {
                if (why) *why = L"含冒号（备用数据流）";
                return false;
            }
            if (ch < 0x20 || ch == 0x7F)
            {
                if (why) *why = L"含控制字符/NUL";
                return false;
            }
        }

        std::wstring normalized = ToBackslashes(name);
        if (kind == Kind::Directory)
        {
            while (!normalized.empty() && normalized.back() == L'\\')
            {
                normalized.pop_back();
            }
            if (normalized.empty())
            {
                return true;  // 全是分隔符 = 根域
            }
        }
        if (normalized.empty() || normalized.back() == L'\\')
        {
            if (why) *why = L"相对路径以分隔符结尾";
            return false;
        }
        if (normalized.find(L"\\\\") != std::wstring::npos)
        {
            if (why) *why = L"路径里有连续分隔符";
            return false;
        }

        std::vector<std::wstring> parts;
        size_t start = 0u;
        while (start <= normalized.size())
        {
            size_t end = normalized.find(L'\\', start);
            if (end == std::wstring::npos)
            {
                end = normalized.size();
            }
            parts.push_back(normalized.substr(start, end - start));
            if (end == normalized.size())
            {
                break;
            }
            start = end + 1u;
        }

        if (kind == Kind::File && parts.size() != 1u)
        {
            if (why) *why = L"文件名里带目录分隔符";
            return false;
        }
        for (size_t i = 0; i < parts.size(); ++i)
        {
            const bool leaf = (i + 1u == parts.size());
            const bool fileLeaf = leaf && (kind == Kind::File || kind == Kind::RelativePath);
            if (!ValidateComponent(parts[i], fileLeaf, why))
            {
                return false;
            }
        }
        return true;
    }

    // 从路径根开始逐级检查已存在的组件；发现重解析点就返回 false 并给出那一级。
    // 组件不存在时停止（后面的组件不可能单独存在）。
    inline bool HasReparsePoint(const std::wstring& fullPath, std::wstring* offending = nullptr)
    {
        const std::wstring normalized = NormalizeFullPath(fullPath);
        if (normalized.empty())
        {
            // 标准化失败时不能当"没有重解析点"放行；调用方按不安全处理。
            if (offending) *offending = fullPath;
            return true;
        }
        const SplitPath split = SplitFullPath(normalized);
        if (!split.valid)
        {
            if (offending) *offending = normalized;
            return false;
        }

        std::wstring current = split.prefix;
        // 盘根 "C:\" 要整段保留（"C:" 是"盘上当前目录"，不是根）；UNC 根去掉结尾分隔符即可。
        if (current.size() > 3u && current.back() == L'\\')
        {
            current.pop_back();
        }

        const DWORD rootAttributes = ::GetFileAttributesW(current.c_str());
        if (rootAttributes != INVALID_FILE_ATTRIBUTES &&
            (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        {
            if (offending) *offending = current;
            return true;
        }

        for (const std::wstring& part : split.parts)
        {
            if (!current.empty() && current.back() != L'\\')
            {
                current.push_back(L'\\');
            }
            current += part;

            const DWORD attributes = ::GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                break;
            }
            if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            {
                if (offending) *offending = current;
                return true;
            }
        }
        return false;
    }

    // 把相对名字解析到 root 下，并做全部边界检查。
    // 成功时 fullPath 是标准化后的绝对路径；失败时 why 说明原因。
    inline bool TryResolveUnderRoot(const std::wstring& root, const std::wstring& relative, Kind kind,
                                    std::wstring& fullPath, std::wstring* why)
    {
        fullPath.clear();
        if (why) why->clear();

        const std::wstring normalizedRoot = NormalizeFullPath(root);
        if (normalizedRoot.empty())
        {
            if (why) *why = L"根目录无效";
            return false;
        }
        if (!ValidateRelativeName(relative, kind, why))
        {
            return false;
        }

        std::wstring candidate = normalizedRoot;
        if (!IsRootDomainName(relative))
        {
            const std::wstring normalizedRelative = ToBackslashes(relative);
            if (!candidate.empty() && candidate.back() != L'\\')
            {
                candidate.push_back(L'\\');
            }
            candidate += normalizedRelative;
        }

        const std::wstring resolved = NormalizeFullPath(candidate);
        if (resolved.empty())
        {
            if (why) *why = L"路径标准化失败";
            return false;
        }
        if (!IsUnderRoot(normalizedRoot, resolved, true))
        {
            if (why) *why = L"解析后越出声明根";
            return false;
        }
        if (kind == Kind::File || kind == Kind::RelativePath)
        {
            const SplitPath split = SplitFullPath(resolved);
            if (!split.valid || split.parts.empty())
            {
                if (why) *why = L"目标不是文件路径";
                return false;
            }
        }

        std::wstring offending;
        if (HasReparsePoint(resolved, &offending))
        {
            if (why) *why = L"路径上有重解析点：" + offending;
            return false;
        }

        fullPath = resolved;
        return true;
    }

    // 供解析器做无回绕算术。返回 false 表示溢出/不合法。
    inline bool AddChecked(uint64_t a, uint64_t b, uint64_t& out)
    {
        if (a > UINT64_MAX - b)
        {
            return false;
        }
        out = a + b;
        return true;
    }

    inline bool MulChecked(uint64_t a, uint64_t b, uint64_t& out)
    {
        if (a != 0u && b > UINT64_MAX / a)
        {
            return false;
        }
        out = a * b;
        return true;
    }

    inline bool FitsSizeT(uint64_t value)
    {
        return value <= static_cast<uint64_t>(static_cast<size_t>(-1));
    }

    // Win32 会把每个组件的尾点/尾空白折叠掉（"dir." / "dir " 常等于 "dir"），
    // 但 GetFullPathNameW 不做这件事。写根比较必须按折叠后的形态再看一次。
    // 折叠后组件为空（例如全是点）时返回空串，调用方按无效处理。
    inline std::wstring CollapseWin32AliasPath(const std::wstring& path)
    {
        const std::wstring normalized = NormalizeFullPath(path);
        if (normalized.empty())
        {
            return std::wstring();
        }
        const SplitPath split = SplitFullPath(normalized);
        if (!split.valid)
        {
            return std::wstring();
        }
        std::wstring rebuilt = split.prefix;
        bool first = true;
        for (const std::wstring& part : split.parts)
        {
            std::wstring collapsed = part;
            while (!collapsed.empty() &&
                   (collapsed.back() == L'.' || collapsed.back() == L' '))
            {
                collapsed.pop_back();
            }
            if (collapsed.empty())
            {
                return std::wstring();
            }
            if (!first || (!rebuilt.empty() && rebuilt.back() != L'\\'))
            {
                rebuilt.push_back(L'\\');
            }
            rebuilt += collapsed;
            first = false;
        }
        while (rebuilt.size() > 3u && rebuilt.back() == L'\\')
        {
            rebuilt.pop_back();
        }
        return rebuilt;
    }

    // 把 \\?\ 前缀还原成普通盘符 / UNC，便于组件比较。
    inline std::wstring StripExtendedPrefix(std::wstring path)
    {
        if (path.rfind(L"\\\\?\\UNC\\", 0) == 0)
        {
            return L"\\\\" + path.substr(8u);
        }
        if (path.rfind(L"\\\\?\\", 0) == 0)
        {
            return path.substr(4u);
        }
        return path;
    }

    // 已存在路径的真实身份：句柄取 GetFinalPathNameByHandle（FILE_NAME_NORMALIZED），
    // 再用 GetLongPathName 展开 8.3 短名。任何一步失败都返回 false（fail closed）。
    inline bool TryResolveExistingIdentity(const std::wstring& path, std::wstring& identity,
                                           std::wstring* why)
    {
        const std::wstring normalized = NormalizeFullPath(path);
        if (normalized.empty())
        {
            if (why) *why = L"路径无效";
            return false;
        }
        HANDLE handle = ::CreateFileW(normalized.c_str(), FILE_READ_ATTRIBUTES,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            if (why) *why = L"打开路径取真实身份失败：" + normalized;
            return false;
        }
        struct HandleGuard
        {
            HANDLE handle;
            ~HandleGuard() { if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle); }
        } handleGuard{handle};

        std::wstring finalPath;
        DWORD length = ::GetFinalPathNameByHandleW(handle, nullptr, 0,
                                                   FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
        if (length > 0)
        {
            finalPath.assign(static_cast<size_t>(length), L'\0');
            const DWORD got = ::GetFinalPathNameByHandleW(handle, &finalPath[0], length,
                                                          FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (got > 0 && got < length)
            {
                finalPath.resize(static_cast<size_t>(got));
            }
            else
            {
                finalPath.clear();
            }
        }
        if (finalPath.empty())
        {
            if (why) *why = L"取路径真实身份失败：" + normalized;
            return false;
        }
        finalPath = StripExtendedPrefix(finalPath);

        std::wstring longPath;
        const DWORD longLength = ::GetLongPathNameW(finalPath.c_str(), nullptr, 0);
        if (longLength > 0)
        {
            longPath.assign(static_cast<size_t>(longLength), L'\0');
            const DWORD got = ::GetLongPathNameW(finalPath.c_str(), &longPath[0], longLength);
            if (got > 0 && got <= longLength)
            {
                longPath.resize(static_cast<size_t>(got));
            }
            else
            {
                longPath.clear();
            }
        }
        if (longPath.empty())
        {
            // 8.3 展开失败时不能拿短名去比较（不同名字可能指向同一目录），直接拒绝。
            if (why) *why = L"无法展开 8.3 长名：" + finalPath;
            return false;
        }

        identity = NormalizeFullPath(longPath);
        if (identity.empty())
        {
            if (why) *why = L"真实身份路径标准化失败：" + longPath;
            return false;
        }
        return true;
    }

    // 解析路径身份；目标不存在时，用"最深的已存在祖先身份 + 折叠后的缺失组件"。
    inline bool TryResolvePathIdentity(const std::wstring& path, std::wstring& identity, bool& exists,
                                       std::wstring* why)
    {
        identity.clear();
        const std::wstring normalized = NormalizeFullPath(path);
        if (normalized.empty())
        {
            if (why) *why = L"路径无效";
            return false;
        }

        const DWORD attributes = ::GetFileAttributesW(normalized.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES)
        {
            exists = true;
            return TryResolveExistingIdentity(normalized, identity, why);
        }
        const DWORD error = ::GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
        {
            if (why) *why = L"探测路径失败（code=" + std::to_wstring(error) + L"）：" + normalized;
            return false;
        }

        exists = false;
        const SplitPath split = SplitFullPath(normalized);
        if (!split.valid || split.parts.empty())
        {
            if (why) *why = L"路径没有可用的已存在祖先：" + normalized;
            return false;
        }

        size_t existingCount = split.parts.size();
        std::wstring ancestor;
        for (size_t count = split.parts.size(); count > 0u; --count)
        {
            std::wstring candidate = split.prefix;
            for (size_t index = 0; index < count; ++index)
            {
                if (candidate.empty() || candidate.back() != L'\\')
                {
                    candidate.push_back(L'\\');
                }
                candidate += split.parts[index];
            }
            if (::GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                existingCount = count;
                ancestor = candidate;
                break;
            }
        }
        if (ancestor.empty())
        {
            if (why) *why = L"找不到已存在的祖先目录：" + normalized;
            return false;
        }

        std::wstring ancestorIdentity;
        if (!TryResolveExistingIdentity(ancestor, ancestorIdentity, why))
        {
            return false;
        }
        std::wstring rebuilt = ancestorIdentity;
        for (size_t index = existingCount; index < split.parts.size(); ++index)
        {
            std::wstring component = split.parts[index];
            while (!component.empty() && (component.back() == L'.' || component.back() == L' '))
            {
                component.pop_back();
            }
            if (component.empty())
            {
                if (why) *why = L"目标组件折叠后为空：" + split.parts[index];
                return false;
            }
            if (rebuilt.back() != L'\\')
            {
                rebuilt.push_back(L'\\');
            }
            rebuilt += component;
        }
        identity = NormalizeFullPath(rebuilt);
        if (identity.empty())
        {
            if (why) *why = L"身份路径标准化失败";
            return false;
        }
        return true;
    }

    // 写根校验：词法/尾点空白别名 + 现存真实身份（展开 8.3）任一方向相同或嵌套都拒绝；
    // 输出根已存在路径上的重解析点也在这里拒绝。必须在建任何目录之前调用。
    inline bool ValidateWriteRoots(const std::wstring& sourceRoot, const std::wstring& outputRoot,
                                   std::wstring* why)
    {
        if (why) why->clear();
        const std::wstring sourceLexical = NormalizeFullPath(sourceRoot);
        const std::wstring outputLexical = NormalizeFullPath(outputRoot);
        if (sourceLexical.empty() || outputLexical.empty())
        {
            if (why) *why = L"源/输出根无效";
            return false;
        }

        const std::wstring sourceCollapsed = CollapseWin32AliasPath(sourceRoot);
        const std::wstring outputCollapsed = CollapseWin32AliasPath(outputRoot);
        if (sourceCollapsed.empty() || outputCollapsed.empty())
        {
            if (why) *why = L"源/输出根包含不可折叠的尾点/空白组件";
            return false;
        }
        if (IsUnderRoot(sourceCollapsed, outputCollapsed, true) ||
            IsUnderRoot(outputCollapsed, sourceCollapsed, true))
        {
            if (why) *why = L"输出根与源根相同/嵌套（含 Win32 尾点或尾空白别名）";
            return false;
        }

        std::wstring sourceIdentity;
        std::wstring outputIdentity;
        bool sourceExists = false;
        bool outputExists = false;
        if (!TryResolvePathIdentity(sourceRoot, sourceIdentity, sourceExists, why))
        {
            return false;
        }
        if (!TryResolvePathIdentity(outputRoot, outputIdentity, outputExists, why))
        {
            return false;
        }
        if (IsUnderRoot(sourceIdentity, outputIdentity, true) ||
            IsUnderRoot(outputIdentity, sourceIdentity, true))
        {
            if (why) *why = L"输出根与源根真实身份相同/嵌套（含 8.3 短名别名）";
            return false;
        }

        std::wstring offending;
        if (HasReparsePoint(outputRoot, &offending))
        {
            if (why) *why = L"输出根路径上有重解析点：" + offending;
            return false;
        }
        (void)sourceExists;
        (void)outputExists;
        return true;
    }
}
