// 可测试的 Steam API 替换事务。
//
// 约束：
//   - 原文件先完整移到一个**本次唯一**的备份名，绝不复用/覆盖 .bak；
//   - 替换文件先复制到本目录下 CREATE_NEW 的唯一 staging 名，再原子改名；
//   - 任一步失败，事务析构时用 MoveFileEx(REPLACE_EXISTING) 把备份移回；
//   - 回滚失败不谎报成功：保留唯一备份，实际状态写进 error。
//
// 本头文件不依赖 UI/游戏，可直接用临时文件做 headless 测试。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <string>

namespace Yua
{
namespace Host
{
namespace SteamSwap
{
    namespace Detail
    {
        inline std::wstring UniqueSibling(const std::wstring& original, const wchar_t* tag, unsigned int attempt)
        {
            wchar_t suffix[96]{};
            ::swprintf_s(suffix, L".%s-%lu-%llu-%u", tag,
                         static_cast<unsigned long>(::GetCurrentProcessId()),
                         static_cast<unsigned long long>(::GetTickCount64()), attempt);
            return original + suffix;
        }

        inline bool CopyToNewFile(const std::wstring& source, const std::wstring& destination,
                                  std::wstring& error)
        {
            HANDLE in = ::CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (in == INVALID_HANDLE_VALUE)
            {
                error = L"打不开替换源文件：" + source;
                return false;
            }
            HANDLE out = ::CreateFileW(destination.c_str(), GENERIC_WRITE, 0u, nullptr, CREATE_NEW,
                                       FILE_ATTRIBUTE_NORMAL, nullptr);
            if (out == INVALID_HANDLE_VALUE)
            {
                const DWORD code = ::GetLastError();
                ::CloseHandle(in);
                error = L"无法创建唯一 staging 文件（错误码 " + std::to_wstring(code) +
                        L"）：" + destination;
                return false;
            }

            bool ok = true;
            char buffer[64 * 1024];
            for (;;)
            {
                DWORD read = 0u;
                if (!::ReadFile(in, buffer, sizeof(buffer), &read, nullptr))
                {
                    error = L"读取替换源文件失败：" + source;
                    ok = false;
                    break;
                }
                if (read == 0u)
                {
                    break;
                }
                DWORD written = 0u;
                if (!::WriteFile(out, buffer, read, &written, nullptr) || written != read)
                {
                    error = L"写入 staging 文件失败：" + destination;
                    ok = false;
                    break;
                }
            }
            ::CloseHandle(in);
            if (ok && !::FlushFileBuffers(out))
            {
                error = L"刷新 staging 文件失败：" + destination;
                ok = false;
            }
            ::CloseHandle(out);
            if (!ok)
            {
                ::DeleteFileW(destination.c_str());
            }
            return ok;
        }
    }

    struct Transaction
    {
        std::wstring Original;
        std::wstring Backup;
        std::wstring Staging;
        bool Replaced = false;   // 原文件已移到 Backup
        bool Committed = false;

        Transaction() = default;
        Transaction(const Transaction&) = delete;
        Transaction& operator=(const Transaction&) = delete;
        ~Transaction()
        {
            if (Replaced && !Committed)
            {
                std::wstring ignored;
                Rollback(ignored);
            }
        }

        // 返回 false 时：若 Replaced 为真，原文件在 Backup；若为假，原文件未动。
        bool Begin(const std::wstring& original, const std::wstring& replacement, std::wstring& error)
        {
            error.clear();
            Original = original;
            if (::GetFileAttributesW(original.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                error = L"要替换的原文件不存在：" + original;
                return false;
            }
            if (::GetFileAttributesW(replacement.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                error = L"替换源文件不存在：" + replacement;
                return false;
            }

            for (unsigned int attempt = 0u; attempt < 64u; ++attempt)
            {
                const std::wstring candidate = Detail::UniqueSibling(original, L"yua-bak", attempt);
                if (::GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                    Backup = candidate;
                    break;
                }
            }
            if (Backup.empty())
            {
                error = L"无法为原文件生成唯一备份名，未改动任何文件。";
                return false;
            }

            if (!::MoveFileExW(original.c_str(), Backup.c_str(), MOVEFILE_WRITE_THROUGH))
            {
                const DWORD code = ::GetLastError();
                error = L"备份原文件失败（错误码 " + std::to_wstring(code) + L"），未改动任何文件：" +
                        original;
                Backup.clear();
                return false;
            }
            Replaced = true;

            for (unsigned int attempt = 0u; attempt < 64u && Staging.empty(); ++attempt)
            {
                const std::wstring candidate = Detail::UniqueSibling(original, L"yua-stage", attempt);
                if (::GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES)
                {
                    Staging = candidate;
                }
            }
            if (Staging.empty())
            {
                error = L"无法生成唯一 staging 名。";
                std::wstring rollbackError;
                Rollback(rollbackError);
                if (!rollbackError.empty())
                {
                    error += L" " + rollbackError;
                }
                return false;
            }

            if (!Detail::CopyToNewFile(replacement, Staging, error))
            {
                std::wstring rollbackError;
                Rollback(rollbackError);
                if (!rollbackError.empty())
                {
                    error += L" " + rollbackError;
                }
                return false;
            }

            if (!::MoveFileExW(Staging.c_str(), original.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                const DWORD code = ::GetLastError();
                error = L"原子替换失败（错误码 " + std::to_wstring(code) + L"）。";
                ::DeleteFileW(Staging.c_str());
                Staging.clear();
                std::wstring rollbackError;
                Rollback(rollbackError);
                if (!rollbackError.empty())
                {
                    error += L" " + rollbackError;
                }
                return false;
            }
            Staging.clear();
            return true;
        }

        void Commit()
        {
            Committed = true;
        }

        // 真回滚成功才返回 true。失败时保留唯一备份，并让 error 反映实际状态。
        bool Rollback(std::wstring& error)
        {
            error.clear();
            if (!Replaced)
            {
                return true;
            }
            if (Backup.empty())
            {
                error = L"回滚失败：备份路径为空，原文件状态不明。";
                return false;
            }
            // 直接把备份改名覆盖回原位置：不做"先删除原文件"的窗口。
            if (!::MoveFileExW(Backup.c_str(), Original.c_str(),
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                const DWORD code = ::GetLastError();
                error = L"回滚失败（错误码 " + std::to_wstring(code) + L"）；原文件仍保留在唯一备份：" +
                        Backup;
                return false;
            }
            Replaced = false;
            return true;
        }
    };
}
}
}
