// Headless 测试：Steam API 替换事务（直接编入生产 SteamApiSwap.h）。
//
// 只操作自造临时文件，绝不碰真实游戏目录：
//   - 唯一 backup / 唯一 CREATE_NEW staging（不是固定的 api.yua-new）；
//   - 成功后原文件是新字节，backup 保有旧字节；
//   - 显式回滚 / 析构回滚都把旧字节放回原位置；
//   - 预置的固定哨兵文件（steam_api.dll.yua-new）必须原样保留、不被删除；
//   - 替换源缺失时 Begin 失败且原文件不动。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "SteamApiSwap.h"

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* what)
    {
        std::printf(condition ? "PASS %s\n" : "FAIL %s\n", what);
        if (!condition)
        {
            ++g_failures;
        }
    }

    std::wstring Join(const std::wstring& directory, const std::wstring& name)
    {
        if (directory.empty() || directory.back() == L'\\')
        {
            return directory + name;
        }
        return directory + L"\\" + name;
    }

    bool WriteBytes(const std::wstring& path, const std::string& bytes)
    {
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0u, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD written = 0u;
        const BOOL ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                                    nullptr);
        ::CloseHandle(file);
        return ok != FALSE && written == bytes.size();
    }

    bool ReadBytes(const std::wstring& path, std::string& bytes)
    {
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        LARGE_INTEGER size{};
        ::GetFileSizeEx(file, &size);
        bytes.assign(static_cast<size_t>(size.QuadPart), '\0');
        DWORD read = 0u;
        const BOOL ok = bytes.empty() ||
                        (::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) &&
                         read == bytes.size());
        ::CloseHandle(file);
        return ok != FALSE;
    }

    bool Exists(const std::wstring& path)
    {
        return ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    bool IsUniqueTagName(const std::wstring& path, const wchar_t* tag)
    {
        return path.find(tag) != std::wstring::npos && path.find(L".yua-new") == std::wstring::npos;
    }

    bool PrepareCase(const std::wstring& root, const wchar_t* name, std::wstring& original,
                     std::wstring& replacement)
    {
        const std::wstring directory = Join(root, name);
        ::CreateDirectoryW(directory.c_str(), nullptr);
        original = Join(directory, L"steam_api.dll");
        replacement = Join(directory, L"steamapi_cra_steam_api.dll");
        return WriteBytes(original, "ORIGINAL-BYTES") && WriteBytes(replacement, "REPLACEMENT-BYTES");
    }

    void CleanupCase(const std::wstring& root, const wchar_t* name)
    {
        WIN32_FIND_DATAW data{};
        const std::wstring pattern = Join(Join(root, name), L"*");
        HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    continue;
                }
                ::DeleteFileW(Join(Join(root, name), data.cFileName).c_str());
            } while (::FindNextFileW(find, &data));
            ::FindClose(find);
        }
        ::RemoveDirectoryW(Join(root, name).c_str());
    }
}

int wmain()
{
    wchar_t tempPath[MAX_PATH]{};
    ::GetTempPathW(_countof(tempPath), tempPath);
    const std::wstring root =
        Join(tempPath, L"yua-security-lifecycle-steamswap-" + std::to_wstring(::GetCurrentProcessId()));
    ::CreateDirectoryW(root.c_str(), nullptr);

    // 0) 固定哨兵：旧实现会用固定 api.yua-new；这里必须原样保留。
    const std::wstring sentinel = Join(root, L"steam_api.dll.yua-new");
    Check(WriteBytes(sentinel, "SENTINEL-MUST-SURVIVE"), "write fixed sentinel file");

    // 1) 成功事务 + Commit。
    {
        std::wstring original;
        std::wstring replacement;
        Check(PrepareCase(root, L"success", original, replacement), "prepare success case");
        Yua::Host::SteamSwap::Transaction transaction;
        std::wstring error;
        const bool began = transaction.Begin(original, replacement, error);
        Check(began, "Begin succeeds on valid files");
        Check(IsUniqueTagName(transaction.Backup, L".yua-bak-"), "backup uses unique yua-bak name");
        Check(Exists(transaction.Backup), "backup file exists after Begin");

        std::string current;
        std::string backupBytes;
        Check(ReadBytes(original, current) && current == "REPLACEMENT-BYTES",
              "original path carries replacement bytes after Begin");
        Check(ReadBytes(transaction.Backup, backupBytes) && backupBytes == "ORIGINAL-BYTES",
              "unique backup preserves the original bytes");
        transaction.Commit();
        Check(Exists(transaction.Backup), "Commit keeps backup for user recovery");
        Check(Exists(sentinel), "fixed sentinel still exists after committed transaction");
        CleanupCase(root, L"success");
    }

    // 2) 显式回滚。
    {
        std::wstring original;
        std::wstring replacement;
        Check(PrepareCase(root, L"rollback", original, replacement), "prepare rollback case");
        Yua::Host::SteamSwap::Transaction transaction;
        std::wstring error;
        Check(transaction.Begin(original, replacement, error), "Begin succeeds before rollback");
        const std::wstring backup = transaction.Backup;
        std::wstring rollbackError;
        Check(transaction.Rollback(rollbackError), "explicit Rollback reports success");
        std::string bytes;
        Check(ReadBytes(original, bytes) && bytes == "ORIGINAL-BYTES",
              "Rollback restores original bytes atomically");
        Check(!Exists(backup), "Rollback consumes the unique backup");
        CleanupCase(root, L"rollback");
    }

    // 3) 未 Commit 的析构回滚。
    {
        std::wstring original;
        std::wstring replacement;
        Check(PrepareCase(root, L"destructor", original, replacement), "prepare destructor case");
        {
            Yua::Host::SteamSwap::Transaction transaction;
            std::wstring error;
            Check(transaction.Begin(original, replacement, error), "Begin succeeds for destructor rollback");
        }
        std::string bytes;
        Check(ReadBytes(original, bytes) && bytes == "ORIGINAL-BYTES",
              "destructor rollback restores original bytes when not committed");
        CleanupCase(root, L"destructor");
    }

    // 4) 替换源缺失：Begin 失败且原文件完全不动。
    {
        std::wstring original;
        std::wstring replacement;
        Check(PrepareCase(root, L"missing-source", original, replacement), "prepare missing-source case");
        ::DeleteFileW(replacement.c_str());
        Yua::Host::SteamSwap::Transaction transaction;
        std::wstring error;
        const bool began = transaction.Begin(original, replacement, error);
        Check(!began && !error.empty(), "Begin fails when replacement source is missing");
        std::string bytes;
        Check(ReadBytes(original, bytes) && bytes == "ORIGINAL-BYTES",
              "failed Begin leaves original file untouched");
        Check(transaction.Backup.empty() && transaction.Staging.empty(),
              "failed Begin keeps backup/staging empty");
        CleanupCase(root, L"missing-source");
    }

    // 5) staging 命名：让替换源无法复制（目录），观察 Begin 在回滚前已生成唯一 staging 名，
    //    且绝不使用固定的 .yua-new。
    {
        const std::wstring directory = Join(root, L"bad-stage");
        ::CreateDirectoryW(directory.c_str(), nullptr);
        const std::wstring original = Join(directory, L"steam_api.dll");
        const std::wstring replacement = Join(directory, L"replacement_is_a_directory");
        ::CreateDirectoryW(replacement.c_str(), nullptr);
        Check(WriteBytes(original, "ORIGINAL-BYTES"), "prepare bad-stage original");
        Yua::Host::SteamSwap::Transaction transaction;
        std::wstring error;
        const bool began = transaction.Begin(original, replacement, error);
        Check(!began && !error.empty(), "Begin fails when replacement cannot be copied");
        Check(IsUniqueTagName(transaction.Staging, L".yua-stage-"),
              "staging candidate uses unique yua-stage name");
        std::string bytes;
        Check(ReadBytes(original, bytes) && bytes == "ORIGINAL-BYTES",
              "failed staging copy rolls original back");
        ::RemoveDirectoryW(replacement.c_str());
        CleanupCase(root, L"bad-stage");
    }

    std::string sentinelBytes;
    Check(ReadBytes(sentinel, sentinelBytes) && sentinelBytes == "SENTINEL-MUST-SURVIVE",
          "fixed sentinel bytes never modified");
    ::DeleteFileW(sentinel.c_str());
    ::RemoveDirectoryW(root.c_str());

    std::printf("RESULT %s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
