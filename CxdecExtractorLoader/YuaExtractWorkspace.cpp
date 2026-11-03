#include "YuaExtractWorkspace.h"

#include "path.h"

#include <vector>

namespace Yua
{
namespace
{
    bool IsDirectory(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    // 递归深度上限：正常工作区远小于它；超过就不是资源目录，而是链接环或坏数据。
    constexpr unsigned int kMaxProbeDepth = 64u;

    bool IsReparsePoint(DWORD attributes)
    {
        return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    }

    // 返回 false 表示中途失败/取消/超深，调用方把 error 往上传。
    bool ProbeRecursive(const std::wstring& directory, unsigned int depth, DirProbe& probe,
                        const std::function<bool()>& cancelRequested)
    {
        if (depth > kMaxProbeDepth)
        {
            probe.error = L"目录递归超过深度上限：" + directory;
            return false;
        }
        if (cancelRequested && cancelRequested())
        {
            probe.cancelled = true;
            return false;
        }

        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(JoinPath(directory, L"*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            const DWORD code = ::GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            {
                return true;
            }
            probe.error = L"列目录失败（code=" + std::to_wstring(code) + L"）：" + directory;
            return false;
        }

        bool ok = true;
        for (;;)
        {
            if (cancelRequested && cancelRequested())
            {
                probe.cancelled = true;
                ok = false;
                break;
            }

            const std::wstring name = data.cFileName;
            if (name != L"." && name != L"..")
            {
                const std::wstring full = JoinPath(directory, name);
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    // 重解析点目录只算一个条目，绝不跟进去：junction 可以指向父目录，
                    // 跟进去就是无限循环；也可能指向工作区外面，统计会把别处算进来。
                    if (!IsReparsePoint(data.dwFileAttributes))
                    {
                        if (!ProbeRecursive(full, depth + 1u, probe, cancelRequested))
                        {
                            ok = false;
                            break;
                        }
                    }
                }
                else
                {
                    ++probe.files;
                    LARGE_INTEGER size{};
                    size.LowPart = data.nFileSizeLow;
                    size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
                    probe.bytes += static_cast<unsigned long long>(size.QuadPart);
                }
            }

            if (!::FindNextFileW(find, &data))
            {
                const DWORD code = ::GetLastError();
                if (code != ERROR_NO_MORE_FILES)
                {
                    probe.error = L"枚举目录失败（code=" + std::to_wstring(code) + L"）：" + directory;
                    ok = false;
                }
                break;
            }
        }
        ::FindClose(find);
        return ok;
    }

    bool DeleteRecursive(const std::wstring& directory, unsigned int depth, std::wstring& error,
                         const std::function<bool()>& cancelRequested)
    {
        if (depth > kMaxProbeDepth)
        {
            error = L"目录递归超过深度上限：" + directory;
            return false;
        }
        if (cancelRequested && cancelRequested())
        {
            error = L"用户中止清理：" + directory;
            return false;
        }

        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(JoinPath(directory, L"*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            const DWORD code = ::GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
            {
                return true;
            }
            error = L"列目录失败（code=" + std::to_wstring(code) + L"）：" + directory;
            return false;
        }

        bool ok = true;
        for (;;)
        {
            if (cancelRequested && cancelRequested())
            {
                error = L"用户中止清理：" + directory;
                ok = false;
                break;
            }

            const std::wstring name = data.cFileName;
            if (name != L"." && name != L"..")
            {
                const std::wstring full = JoinPath(directory, name);
                const bool isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (isDirectory && IsReparsePoint(data.dwFileAttributes))
                {
                    // 目录重解析点：RemoveDirectoryW 删的是链接本身，不会动目标内容。
                    if (!::RemoveDirectoryW(full.c_str()))
                    {
                        const DWORD code = ::GetLastError();
                        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
                        {
                            error = L"删除目录链接失败（code=" + std::to_wstring(code) + L"）：" + full;
                            ok = false;
                            break;
                        }
                    }
                }
                else if (isDirectory)
                {
                    if (!DeleteRecursive(full, depth + 1u, error, cancelRequested))
                    {
                        ok = false;
                        break;
                    }
                }
                else
                {
                    // 文件重解析点同样只删链接（DeleteFileW 不会跟到目标）。
                    // 只读位要先摘掉，否则清理会在第一个只读文件上停下。
                    if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                    {
                        ::SetFileAttributesW(full.c_str(), data.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                    }
                    if (!::DeleteFileW(full.c_str()))
                    {
                        const DWORD code = ::GetLastError();
                        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
                        {
                            error = L"删除文件失败（code=" + std::to_wstring(code) + L"）：" + full;
                            ok = false;
                            break;
                        }
                    }
                }
            }

            if (!::FindNextFileW(find, &data))
            {
                const DWORD code = ::GetLastError();
                if (code != ERROR_NO_MORE_FILES)
                {
                    error = L"枚举目录失败（code=" + std::to_wstring(code) + L"）：" + directory;
                    ok = false;
                }
                break;
            }
        }
        ::FindClose(find);

        if (!ok)
        {
            return false;
        }
        if (!::RemoveDirectoryW(directory.c_str()))
        {
            const DWORD code = ::GetLastError();
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
            {
                error = L"删除目录失败（code=" + std::to_wstring(code) + L"）：" + directory;
                return false;
            }
        }
        return true;
    }
}

WorkspacePaths BuildWorkspace(const std::wstring& gameExePath)
{
    WorkspacePaths paths;
    paths.GameExe = gameExePath;
    if (gameExePath.empty())
    {
        return paths;
    }

    paths.GameDir = Path::GetDirectoryName(gameExePath);
    if (paths.GameDir.empty())
    {
        return paths;
    }

    paths.ExtractDir = JoinPath(paths.GameDir, L"Extractor_Output");
    paths.HashDir = JoinPath(paths.GameDir, L"StringHashDumper_Output");
    paths.RestoreDir = JoinPath(paths.GameDir, L"Restored_Extractor_Output");
    paths.RestoreReport = JoinPath(paths.RestoreDir, L"RestoreReport.txt");
    paths.KeyDir = JoinPath(paths.GameDir, L"ExtractKey_Output");
    return paths;
}

DirProbe ProbeDirectory(const std::wstring& directory, bool deep,
                        const std::function<bool()>& cancelRequested)
{
    DirProbe probe;
    const DWORD rootAttributes = ::GetFileAttributesW(directory.c_str());
    if (rootAttributes == INVALID_FILE_ATTRIBUTES || (rootAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
    {
        return probe;
    }
    probe.exists = true;
    if (IsReparsePoint(rootAttributes))
    {
        // 根目录本身是 junction/symlink 时，FindFirst 会直接枚举链接目标 —— 只挡子目录不够。
        probe.error = L"探测根目录是重解析点（junction / symlink），拒绝递归：" + directory;
        return probe;
    }

    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW(JoinPath(directory, L"*").c_str(), &data);
    if (find == INVALID_HANDLE_VALUE)
    {
        const DWORD code = ::GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
        {
            probe.error = L"列目录失败（code=" + std::to_wstring(code) + L"）：" + directory;
        }
        return probe;
    }

    bool ok = true;
    for (;;)
    {
        if (cancelRequested && cancelRequested())
        {
            probe.cancelled = true;
            ok = false;
            break;
        }

        const std::wstring name = data.cFileName;
        if (name != L"." && name != L"..")
        {
            ++probe.topLevelEntries;
            if (deep)
            {
                const bool isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                if (isDirectory)
                {
                    // 顶层就是重解析点时不跟进去（与递归层同一口径）。
                    if (!IsReparsePoint(data.dwFileAttributes))
                    {
                        if (!ProbeRecursive(JoinPath(directory, name), 1u, probe, cancelRequested))
                        {
                            ok = false;
                            break;
                        }
                    }
                }
                else
                {
                    ++probe.files;
                    LARGE_INTEGER size{};
                    size.LowPart = data.nFileSizeLow;
                    size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
                    probe.bytes += static_cast<unsigned long long>(size.QuadPart);
                }
            }
        }

        if (!::FindNextFileW(find, &data))
        {
            const DWORD code = ::GetLastError();
            if (code != ERROR_NO_MORE_FILES)
            {
                probe.error = L"枚举目录失败（code=" + std::to_wstring(code) + L"）：" + directory;
                ok = false;
            }
            break;
        }
    }
    ::FindClose(find);
    (void)ok;
    return probe;
}

unsigned int CountTextFileLines(const std::wstring& path)
{
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return 0u;
    }

    unsigned int lines = 0u;
    bool sawAnyByte = false;
    char buffer[64 * 1024];
    DWORD read = 0;
    while (::ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0)
    {
        sawAnyByte = true;
        for (DWORD index = 0; index < read; ++index)
        {
            if (buffer[index] == '\n')
            {
                ++lines;
            }
        }
    }
    ::CloseHandle(file);
    // 最后一行没有换行符时也要算上。
    if (sawAnyByte && lines == 0u)
    {
        ++lines;
    }
    return lines;
}

std::wstring ReadTextFileTail(const std::wstring& path, unsigned int maxLines)
{
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
    {
        return std::wstring();
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file, &size) || size.QuadPart <= 0)
    {
        ::CloseHandle(file);
        return std::wstring();
    }

    // 只看尾巴 256 KB：日志可能有几十 MB，界面每两秒刷一次不能整份读。
    constexpr long long kTailBytes = 256ll * 1024ll;
    const long long start = size.QuadPart > kTailBytes ? size.QuadPart - kTailBytes : 0;
    LARGE_INTEGER offset{};
    offset.QuadPart = start;
    if (!::SetFilePointerEx(file, offset, nullptr, FILE_BEGIN))
    {
        ::CloseHandle(file);
        return std::wstring();
    }

    std::string bytes(static_cast<size_t>(size.QuadPart - start), '\0');
    DWORD read = 0;
    if (!::ReadFile(file, &bytes[0], static_cast<DWORD>(bytes.size()), &read, nullptr))
    {
        ::CloseHandle(file);
        return std::wstring();
    }
    ::CloseHandle(file);
    bytes.resize(read);

    // 不是从文件头读的，第一行可能是半行，直接丢掉。
    if (start > 0)
    {
        const size_t newline = bytes.find('\n');
        bytes = newline == std::string::npos ? std::string() : bytes.substr(newline + 1);
    }

    // 日志统一 UTF-8（见 README「日志位置」），按 UTF-8 解。
    std::wstring text;
    if (!bytes.empty())
    {
        const int need = ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                                               nullptr, 0);
        if (need > 0)
        {
            text.assign(static_cast<size_t>(need), L'\0');
            ::MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), &text[0], need);
        }
    }

    if (maxLines == 0u)
    {
        return text;
    }

    // 从尾部往前数 maxLines 个换行，切出最后几行。
    size_t cursor = text.size();
    unsigned int counted = 0u;
    while (cursor > 0u)
    {
        --cursor;
        if (text[cursor] == L'\n')
        {
            if (++counted > maxLines)
            {
                return text.substr(cursor + 1u);
            }
        }
    }
    return text;
}

bool RemoveDirectoryTree(const std::wstring& directory, std::wstring& error,
                         const std::function<bool()>& cancelRequested)
{
    error.clear();
    if (!IsDirectory(directory))
    {
        return true;
    }

    // 根目录本身就是 junction/symlink 时先处理：RemoveDirectoryW 只摘链接，
    // 绝不能让 DeleteRecursive 进去枚举目标内容。
    {
        const DWORD attributes = ::GetFileAttributesW(directory.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && IsReparsePoint(attributes))
        {
            if (!::RemoveDirectoryW(directory.c_str()))
            {
                const DWORD code = ::GetLastError();
                if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
                {
                    error = L"删除目录链接失败（code=" + std::to_wstring(code) + L"）：" + directory;
                    return false;
                }
            }
            return true;
        }
    }

    return DeleteRecursive(directory, 0u, error, cancelRequested);
}

std::wstring FileTimestampText(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
    {
        return std::wstring();
    }
    FILETIME local{};
    if (!::FileTimeToLocalFileTime(&data.ftLastWriteTime, &local))
    {
        return std::wstring();
    }
    SYSTEMTIME time{};
    if (!::FileTimeToSystemTime(&local, &time))
    {
        return std::wstring();
    }
    wchar_t buffer[64]{};
    ::swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u", time.wYear, time.wMonth, time.wDay, time.wHour,
                 time.wMinute);
    return buffer;
}
}
