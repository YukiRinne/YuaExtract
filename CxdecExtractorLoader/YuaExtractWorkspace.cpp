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

    void ProbeRecursive(const std::wstring& directory, unsigned int& files, unsigned long long& bytes)
    {
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(JoinPath(directory, L"*").c_str(), &data);
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
            const std::wstring full = JoinPath(directory, name);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                ProbeRecursive(full, files, bytes);
            }
            else
            {
                ++files;
                LARGE_INTEGER size{};
                size.LowPart = data.nFileSizeLow;
                size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
                bytes += static_cast<unsigned long long>(size.QuadPart);
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
    }

    bool DeleteRecursive(const std::wstring& directory, std::wstring& error)
    {
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
        do
        {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            const std::wstring full = JoinPath(directory, name);
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!DeleteRecursive(full, error))
                {
                    ok = false;
                    break;
                }
            }
            else
            {
                // 只读文件删不掉，先摘掉只读位再删 —— 否则"清理目录"会在第一个只读文件上停下。
                if (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
                {
                    ::SetFileAttributesW(full.c_str(), data.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
                }
                if (!::DeleteFileW(full.c_str()))
                {
                    const DWORD code = ::GetLastError();
                    error = L"删除文件失败（code=" + std::to_wstring(code) + L"）：" + full;
                    ok = false;
                    break;
                }
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);

        if (!ok)
        {
            return false;
        }
        if (!::RemoveDirectoryW(directory.c_str()))
        {
            const DWORD code = ::GetLastError();
            error = L"删除目录失败（code=" + std::to_wstring(code) + L"）：" + directory;
            return false;
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

DirProbe ProbeDirectory(const std::wstring& directory, bool deep)
{
    DirProbe probe;
    if (!IsDirectory(directory))
    {
        return probe;
    }
    probe.exists = true;

    WIN32_FIND_DATAW data{};
    HANDLE find = ::FindFirstFileW(JoinPath(directory, L"*").c_str(), &data);
    if (find != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring name = data.cFileName;
            if (name == L"." || name == L"..")
            {
                continue;
            }
            ++probe.topLevelEntries;
            if (deep && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                ProbeRecursive(JoinPath(directory, name), probe.files, probe.bytes);
            }
            else if (deep)
            {
                ++probe.files;
                LARGE_INTEGER size{};
                size.LowPart = data.nFileSizeLow;
                size.HighPart = static_cast<LONG>(data.nFileSizeHigh);
                probe.bytes += static_cast<unsigned long long>(size.QuadPart);
            }
        } while (::FindNextFileW(find, &data));
        ::FindClose(find);
    }
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

bool RemoveDirectoryTree(const std::wstring& directory, std::wstring& error)
{
    error.clear();
    if (!IsDirectory(directory))
    {
        return true;
    }
    if (DeleteRecursive(directory, error))
    {
        return true;
    }
    return false;
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
