#pragma once

// 单文件 / 多文件安装用的最小事务原语（header-only，各工程共享）。
//
// 目标：**只改自己创建的东西**，正常失败能回滚，不借助"整目录 rm"。
//
// 写法固定为：
//   1) StageBytes：在目标同目录用 CREATE_NEW 建一个唯一临时文件，写满、FlushFileBuffers、
//      close；任何一步失败只删这个临时文件。
//   2) Commit：目标原本存在 -> ReplaceFileW（唯一 backup）；目标原本不存在 ->
//      MoveFileExW（不覆盖已出现的同名文件）。durability 由 temp 的 FlushFileBuffers 提供
//      —— ReplaceFileW 的 REPLACEFILE_WRITE_THROUGH 文档说明未实现，不能依赖。
//   3) Rollback：已提交的用 backup 原样换回；本次新建的删掉；**失败时保留 backup 并报出
//      它的路径**，绝不销毁唯一的旧数据。若 Commit 失败时可能已改动磁盘（stateUnknown），
//      Rollback 会按 target / temp / backup 的真实状态尝试恢复；恢复不了就明确报告，
//      **不删唯一 backup**。
//   4) Finalize：成功收尾，只删自己创建的 backup / 残留 temp。删除失败的 backup 路径保留，
//      不丢掉恢复句柄。
//   5) Discard：只删**自己的 temp**；绝不删 backup（backup 只能由 Finalize 在整事务成功后
//      清理，或成功回滚时由 ReplaceFileW 消耗）。
//
// 约束：
//   · 不跟随重解析点：目标路径上已有 reparse point 直接拒绝（PathSafety::HasReparsePoint）。
//   · 只做单文件原子替换；多文件事务由调用方按 StagedFile 列表逐项 Commit / 反向 Rollback。
//   · 不承诺突然掉电 / kill 的完全原子性；普通 I/O 失败保证可回滚。
//   · 只有本工具创建的 temp / backup 会被删除；调用方绝不能拿它去删用户已有文件或目录树。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "PathSafety.h"

#pragma comment(lib, "ole32.lib")

namespace Common
{
    namespace AtomicFile
    {
#ifdef YUA_ATOMICFILE_TEST_SEAM
        // **仅测试构建**（vcxproj 定义该宏）可用的故障注入点；生产构建完全不编译。
        enum class TestAction
        {
            None = 0,
            ReplacePartial = 1,   // 模拟 ReplaceFileW 半途：旧目标已挪到 backup，目标缺失
            ThrowAfterReplace = 2, // 模拟替换成功后下一行抛 bad_alloc（测试 catch 回滚）
            ThrowDuringFinalize = 3 // 模拟已完整提交后 Finalize 抛异常（只能 true + 提示）
        };
        inline TestAction& TestActionRef()
        {
            static TestAction action = TestAction::None;
            return action;
        }
#endif

        struct StagedFile
        {
            std::wstring target;        // 最终文件路径（绝对）
            std::wstring temp;          // 本次创建的临时文件（同目录、唯一）
            std::wstring backup;        // Commit 时 ReplaceFileW 产出的旧文件备份（唯一）
            bool targetExisted = false; // Commit 之前目标是否已经存在
            bool committed = false;     // 是否已经替换到目标上
            // Commit 失败时磁盘可能已被改动（目标缺失 / backup 已产生）：
            // 此时绝不能"只删 temp 报成功"，Rollback 必须按真实状态恢复。
            bool stateUnknown = false;
            // 回滚失败 / 状态不明：此时 backup 是唯一旧数据，**任何清理路径都不得删它**。
            bool rollbackFailed = false;
        };

        inline std::wstring FormatWin32(DWORD code)
        {
            wchar_t buffer[32]{};
            ::swprintf_s(buffer, L"%lu", static_cast<unsigned long>(code));
            return buffer;
        }

        inline std::wstring DirectoryOf(const std::wstring& path)
        {
            const size_t separator = path.find_last_of(L"\\/");
            return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
        }

        inline std::wstring FileNameOf(const std::wstring& path)
        {
            const size_t separator = path.find_last_of(L"\\/");
            return separator == std::wstring::npos ? path : path.substr(separator + 1u);
        }

        inline bool DirectoryExists(const std::wstring& path)
        {
            if (path.empty())
            {
                return false;
            }
            const DWORD attributes = ::GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES
                && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        }

        inline bool FileExists(const std::wstring& path)
        {
            if (path.empty())
            {
                return false;
            }
            const DWORD attributes = ::GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES
                && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
        }

        // 唯一后缀：进程号 + tick + 单调计数 + GUID。CREATE_NEW 再兜一次碰撞。
        inline std::wstring UniqueSuffix()
        {
            static std::atomic<unsigned long> counter{0};
            GUID guid{};
            if (FAILED(::CoCreateGuid(&guid)))
            {
                guid = GUID{};
            }
            wchar_t guidText[40]{};
            ::StringFromGUID2(guid, guidText, _countof(guidText));
            std::wstring compact;
            for (const wchar_t* cursor = guidText; *cursor != L'\0'; ++cursor)
            {
                if (*cursor != L'{' && *cursor != L'}' && *cursor != L'-')
                {
                    compact.push_back(*cursor);
                }
            }
            const unsigned long tick = static_cast<unsigned long>(::GetTickCount64() & 0xFFFFFFFFull);
            return FormatWin32(::GetCurrentProcessId()) + L"_" + FormatWin32(tick) + L"_"
                 + FormatWin32(static_cast<unsigned long>(counter.fetch_add(1))) + L"_" + compact;
        }

        // 在 target 同目录生成一个尚不存在的兄弟路径（只生成名字，不创建）。
        inline std::wstring UniqueSiblingPath(const std::wstring& target, const wchar_t* tag)
        {
            const std::wstring directory = DirectoryOf(target);
            const std::wstring name = FileNameOf(target);
            const std::wstring prefix = name + L"." + tag + L".";
            for (int attempt = 0; attempt < 16; ++attempt)
            {
                const std::wstring candidate = (directory.empty() ? std::wstring() : directory + L"\\")
                    + prefix + UniqueSuffix() + L".tmp";
                if (!FileExists(candidate) && !DirectoryExists(candidate))
                {
                    return candidate;
                }
            }
            return std::wstring();
        }

        // 只创建自己的唯一文件（CREATE_NEW）。失败返回 INVALID_HANDLE_VALUE。
        inline HANDLE CreateOwnedFile(const std::wstring& path)
        {
            return ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                 CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        }

        inline bool WriteAll(HANDLE file, const void* data, size_t size, DWORD& error)
        {
            error = 0;
            const unsigned char* bytes = static_cast<const unsigned char*>(data);
            size_t offset = 0u;
            while (offset < size)
            {
                const DWORD chunk = static_cast<DWORD>(
                    (size - offset) > (1u << 20) ? (1u << 20) : (size - offset));
                DWORD written = 0u;
                if (!::WriteFile(file, bytes + offset, chunk, &written, nullptr) || written != chunk)
                {
                    error = ::GetLastError();
                    if (error == 0u)
                    {
                        error = ERROR_WRITE_FAULT;
                    }
                    return false;
                }
                offset += written;
            }
            return true;
        }

        // 目标路径（含已存在的各级）上不允许有重解析点。
        inline bool CheckTargetPath(const std::wstring& target, std::wstring& error)
        {
            const std::wstring normalized = PathSafety::NormalizeFullPath(target);
            if (normalized.empty())
            {
                error = L"目标路径无效：" + target;
                return false;
            }
            std::wstring offending;
            if (PathSafety::HasReparsePoint(normalized, &offending))
            {
                error = L"拒绝写入：目标路径上有重解析点（junction / symlink）：" + offending;
                return false;
            }
            const std::wstring directory = DirectoryOf(normalized);
            if (directory.empty() || !DirectoryExists(directory))
            {
                error = L"目标目录不存在：" + directory;
                return false;
            }
            return true;
        }

        // 写一个唯一的同目录临时文件。成功时 out.target/temp/targetExisted 有效。
        inline bool StageBytes(const std::wstring& targetPath, const void* data, size_t size,
                               StagedFile& out, std::wstring& error)
        {
            error.clear();
            out = StagedFile();

            if (size > 0u && data == nullptr)
            {
                error = L"内部错误：要写入的字节指针为空但长度非 0。";
                return false;
            }
            const std::wstring normalized = PathSafety::NormalizeFullPath(targetPath);
            if (normalized.empty())
            {
                error = L"目标路径无效：" + targetPath;
                return false;
            }
            if (!CheckTargetPath(normalized, error))
            {
                return false;
            }

            out.target = normalized;
            out.targetExisted = FileExists(normalized);
            // backup 名字在这里就预生成：Commit 阶段只剩 ReplaceFileW，减少 commit 中途
            // 因分配失败而留下"半状态"的机会。
            if (out.targetExisted)
            {
                out.backup = UniqueSiblingPath(normalized, L"yua-bak");
                if (out.backup.empty())
                {
                    error = L"生成唯一备份文件名失败：" + normalized;
                    out = StagedFile();
                    return false;
                }
            }
            out.temp = UniqueSiblingPath(normalized, L"yua-tmp");
            if (out.temp.empty())
            {
                error = L"生成唯一临时文件名失败：" + normalized;
                out = StagedFile();
                return false;
            }

            HANDLE file = CreateOwnedFile(out.temp);
            if (file == INVALID_HANDLE_VALUE)
            {
                error = L"创建临时文件失败：" + out.temp + L"（Win32 错误码 "
                      + FormatWin32(::GetLastError()) + L"）";
                out.temp.clear();
                return false;
            }

            DWORD writeError = 0;
            if (!WriteAll(file, data, size, writeError))
            {
                ::CloseHandle(file);
                ::DeleteFileW(out.temp.c_str());
                error = L"写临时文件失败：" + out.temp + L"（Win32 错误码 "
                      + FormatWin32(writeError) + L"）";
                out.temp.clear();
                return false;
            }
            if (!::FlushFileBuffers(file))
            {
                const DWORD flushError = ::GetLastError();
                ::CloseHandle(file);
                ::DeleteFileW(out.temp.c_str());
                error = L"刷临时文件到磁盘失败：" + out.temp + L"（Win32 错误码 "
                      + FormatWin32(flushError) + L"）";
                out.temp.clear();
                return false;
            }
            if (!::CloseHandle(file))
            {
                const DWORD closeError = ::GetLastError();
                ::DeleteFileW(out.temp.c_str());
                error = L"关闭临时文件失败：" + out.temp + L"（Win32 错误码 "
                      + FormatWin32(closeError) + L"）";
                out.temp.clear();
                return false;
            }
            return true;
        }

        inline bool StageBytes(const std::wstring& targetPath,
                               const std::vector<unsigned char>& bytes,
                               StagedFile& out, std::wstring& error)
        {
            return StageBytes(targetPath, bytes.empty() ? nullptr : bytes.data(), bytes.size(), out,
                              error);
        }

        inline bool Commit(StagedFile& staged, std::wstring& error)
        {
            error.clear();
            if (staged.committed)
            {
                return true;
            }
            if (staged.target.empty() || staged.temp.empty())
            {
                error = L"内部错误：没有可提交的暂存文件。";
                return false;
            }

            // Commit 前再核一次：目标目录 / 目标 / temp / backup 的路径边界与重解析点。
            std::wstring checkError;
            if (!CheckTargetPath(staged.target, checkError))
            {
                error = L"提交前路径检查失败：" + checkError;
                return false;
            }
            std::wstring offending;
            if (PathSafety::HasReparsePoint(staged.temp, &offending))
            {
                error = L"提交前检查失败：暂存文件是重解析点：" + offending;
                return false;
            }
            if (!staged.backup.empty() && FileExists(staged.backup)
                && PathSafety::HasReparsePoint(staged.backup, &offending))
            {
                error = L"提交前检查失败：备份路径是重解析点：" + offending;
                return false;
            }
            if (!FileExists(staged.temp))
            {
                error = L"暂存文件不见了，拒绝提交：" + staged.temp;
                return false;
            }

#ifdef YUA_ATOMICFILE_TEST_SEAM
            if (TestActionRef() == TestAction::ReplacePartial && staged.targetExisted)
            {
                // 只用于测试：模拟 ReplaceFileW 半途——旧目标被挪到 backup，目标缺失。
                if (::MoveFileExW(staged.target.c_str(), staged.backup.c_str(), 0))
                {
                    staged.stateUnknown = true;
                    error = L"测试故障注入：ReplaceFileW 半途（旧目标已移到 backup，目标缺失）。";
                    return false;
                }
            }
#endif

            if (staged.targetExisted)
            {
                if (!FileExists(staged.target))
                {
                    error = L"提交失败：目标在暂存之后消失了：" + staged.target;
                    return false;
                }
                if (staged.backup.empty())
                {
                    staged.backup = UniqueSiblingPath(staged.target, L"yua-bak");
                    if (staged.backup.empty())
                    {
                        error = L"生成唯一备份文件名失败：" + staged.target;
                        return false;
                    }
                }
                if (!::ReplaceFileW(staged.target.c_str(), staged.temp.c_str(),
                                    staged.backup.c_str(), 0, nullptr, nullptr))
                {
                    // ReplaceFileW 不支持 REPLACEFILE_WRITE_THROUGH（文档明确：该 flag
                    // 未被实现）；durability 由 temp 的 FlushFileBuffers 提供。
                    const DWORD replaceError = ::GetLastError();
                    // 细分失败后的真实状态：可能已产生 backup、目标可能已变化。
                    // 这种情况必须让 Rollback 按事实恢复，不能"只删 temp 报成功"。
                    const bool targetNow = FileExists(staged.target);
                    const bool backupNow = FileExists(staged.backup);
                    const bool tempNow = FileExists(staged.temp);
                    staged.stateUnknown = backupNow || !targetNow || !tempNow;
                    error = L"原子替换失败：" + staged.target + L"（Win32 错误码 "
                          + FormatWin32(replaceError) + L"；替换后目标"
                          + (targetNow ? L"仍存在" : L"不存在") + L"，备份"
                          + (backupNow ? (L"已产生：" + staged.backup) : L"未产生") + L"，暂存"
                          + (tempNow ? L"仍在" : L"已消失") + L"）";
                    return false;
                }
                staged.temp.clear();
                staged.committed = true;
#ifdef YUA_ATOMICFILE_TEST_SEAM
                if (TestActionRef() == TestAction::ThrowAfterReplace)
                {
                    throw std::bad_alloc();
                }
#endif
                return true;
            }

            if (!::MoveFileExW(staged.temp.c_str(), staged.target.c_str(), MOVEFILE_WRITE_THROUGH))
            {
                const DWORD moveError = ::GetLastError();
                const bool targetNow = FileExists(staged.target);
                const bool tempNow = FileExists(staged.temp);
                staged.stateUnknown = targetNow || !tempNow;
                error = L"落盘失败（目标原本不存在）：" + staged.target + L"（Win32 错误码 "
                      + FormatWin32(moveError) + L"；目标"
                      + (targetNow ? L"已存在" : L"不存在") + L"，暂存"
                      + (tempNow ? L"仍在" : L"已消失") + L"）";
                return false;
            }
            staged.temp.clear();
            staged.committed = true;
#ifdef YUA_ATOMICFILE_TEST_SEAM
            if (TestActionRef() == TestAction::ThrowAfterReplace)
            {
                throw std::bad_alloc();
            }
#endif
            return true;
        }

        // 回滚一个 StagedFile。成功返回 true。
        //   · stateUnknown：Commit 失败时可能已改动磁盘。先看 target / temp / backup 的事实：
        //     backup 存在就优先用它恢复；恢复不了就明确失败并保留 backup，绝不删它。
        //   · 已提交且原本存在：用 backup 原子换回（backup 随后被 ReplaceFileW 消耗）。
        //   · 已提交且原本不存在：删除本次新建的目标。
        //   · 未提交且状态明确：只删本次 temp。
        inline bool Rollback(StagedFile& staged, std::wstring& error)
        {
            error.clear();
            if (staged.rollbackFailed)
            {
                error = L"回滚失败（此前已标记状态不明）：目标：" + staged.target
                      + L"；backup：" + (staged.backup.empty() ? std::wstring(L"（无）")
                                                              : staged.backup);
                return false;
            }

            std::wstring offending;
            if (PathSafety::HasReparsePoint(staged.target, &offending))
            {
                staged.rollbackFailed = true;
                error = L"回滚时发现目标路径是重解析点，拒绝跟随（backup 保留）：" + offending;
                return false;
            }
            if (!staged.backup.empty() && FileExists(staged.backup)
                && PathSafety::HasReparsePoint(staged.backup, &offending))
            {
                staged.rollbackFailed = true;
                error = L"回滚时发现备份路径是重解析点，拒绝跟随（backup 保留）：" + offending;
                return false;
            }
            if (!staged.temp.empty() && FileExists(staged.temp)
                && PathSafety::HasReparsePoint(staged.temp, &offending))
            {
                staged.rollbackFailed = true;
                error = L"回滚时发现暂存路径是重解析点，拒绝跟随：" + offending;
                return false;
            }

            if (staged.stateUnknown)
            {
                const bool targetNow = FileExists(staged.target);
                const bool backupNow = !staged.backup.empty() && FileExists(staged.backup);
                if (staged.targetExisted && backupNow)
                {
                    bool restored = false;
                    if (targetNow)
                    {
                        restored = ::ReplaceFileW(staged.target.c_str(), staged.backup.c_str(),
                                                  nullptr, 0, nullptr, nullptr) != FALSE;
                    }
                    else
                    {
                        restored = ::MoveFileExW(staged.backup.c_str(), staged.target.c_str(),
                                                 MOVEFILE_WRITE_THROUGH) != FALSE;
                    }
                    if (!restored)
                    {
                        staged.rollbackFailed = true;
                        const DWORD restoreError = ::GetLastError();
                        error = L"回滚失败：旧文件仍在备份里，未销毁。目标：" + staged.target
                              + L"；backup：" + staged.backup + L"（Win32 错误码 "
                              + FormatWin32(restoreError) + L"；目标现在"
                              + (FileExists(staged.target) ? L"存在" : L"不存在") + L"）";
                        return false;
                    }
                    staged.backup.clear();
                    staged.committed = false;
                    staged.stateUnknown = false;
                    if (!staged.temp.empty() && FileExists(staged.temp))
                    {
                        ::DeleteFileW(staged.temp.c_str());
                    }
                    staged.temp.clear();
                    return true;
                }
                if (!staged.targetExisted)
                {
                    // 目标原本不存在：temp 还在 = MoveFileEx 没生效；删掉自己的 temp 即可。
                    if (!staged.temp.empty() && FileExists(staged.temp))
                    {
                        ::DeleteFileW(staged.temp.c_str());
                        staged.temp.clear();
                        staged.committed = false;
                        staged.stateUnknown = false;
                        return true;
                    }
                }
                staged.rollbackFailed = true;
                error = L"回滚失败：状态不明且没有可用的旧文件备份，未删除任何现有数据。目标："
                      + staged.target + L"；backup："
                      + (staged.backup.empty() ? std::wstring(L"（无）") : staged.backup)
                      + L"；暂存：" + (staged.temp.empty() ? std::wstring(L"（无）") : staged.temp);
                return false;
            }

            if (!staged.committed)
            {
                if (!staged.temp.empty() && FileExists(staged.temp))
                {
                    ::DeleteFileW(staged.temp.c_str());
                }
                staged.temp.clear();
                return true;
            }

            if (staged.targetExisted)
            {
                if (staged.backup.empty() || !FileExists(staged.backup))
                {
                    staged.rollbackFailed = true;
                    error = L"回滚失败：找不到旧文件备份，未销毁任何现有数据。目标："
                          + staged.target + L"；backup："
                          + (staged.backup.empty() ? std::wstring(L"（无）") : staged.backup);
                    return false;
                }
                // 同样不要用 REPLACEFILE_WRITE_THROUGH（未被实现）。
                if (!::ReplaceFileW(staged.target.c_str(), staged.backup.c_str(), nullptr,
                                    0, nullptr, nullptr))
                {
                    staged.rollbackFailed = true;
                    const DWORD replaceError = ::GetLastError();
                    error = L"回滚失败：旧文件仍在备份里，未销毁。目标：" + staged.target
                          + L"；backup：" + staged.backup + L"（Win32 错误码 "
                          + FormatWin32(replaceError) + L"；目标现在"
                          + (FileExists(staged.target) ? L"存在" : L"不存在") + L"）";
                    return false;
                }
                staged.backup.clear();
                staged.committed = false;
                return true;
            }

            if (!::DeleteFileW(staged.target.c_str()) && ::GetLastError() != ERROR_FILE_NOT_FOUND)
            {
                staged.rollbackFailed = true;
                const DWORD deleteError = ::GetLastError();
                error = L"回滚失败：本次新建的文件删不掉：" + staged.target + L"（Win32 错误码 "
                      + FormatWin32(deleteError) + L"）";
                return false;
            }
            staged.committed = false;
            return true;
        }

        // 提交成功后收尾：只删本次自己的 backup / 残留 temp。
        // **只能在整个事务确认成功之后调用**；回滚失败 / 状态不明时调用它是错误的。
        inline bool Finalize(StagedFile& staged, std::wstring& error)
        {
            error.clear();
            if (staged.rollbackFailed || staged.stateUnknown)
            {
                error = L"拒绝收尾：这个文件处于回滚失败/状态不明，backup 必须保留。目标："
                      + staged.target + L"；backup：" + staged.backup;
                return false;
            }
#ifdef YUA_ATOMICFILE_TEST_SEAM
            if (TestActionRef() == TestAction::ThrowDuringFinalize)
            {
                throw std::bad_alloc();
            }
#endif
            bool ok = true;
            if (!staged.backup.empty())
            {
                if (FileExists(staged.backup))
                {
                    if (!::DeleteFileW(staged.backup.c_str()))
                    {
                        // 删除失败时**保留 backup 路径**，不丢掉恢复句柄。
                        error += L"备份文件删不掉（数据已安全提交，可手动删除）：" + staged.backup
                              + L"（Win32 错误码 " + FormatWin32(::GetLastError()) + L"）\r\n";
                        ok = false;
                    }
                    else
                    {
                        staged.backup.clear();
                    }
                }
                else
                {
                    staged.backup.clear();
                }
            }
            if (!staged.temp.empty())
            {
                if (FileExists(staged.temp))
                {
                    if (::DeleteFileW(staged.temp.c_str()))
                    {
                        staged.temp.clear();
                    }
                }
                else
                {
                    staged.temp.clear();
                }
            }
            return ok;
        }

        // 未提交时丢弃：只删自己的 temp。
        // **绝不在这里删 backup**：backup 只在"已经完整提交"后由 Finalize 清理，
        // 或"已经成功回滚"（此时 ReplaceFileW 已把它消耗）后自然消失。
        // 若 temp 路径的祖先/自身已被换成重解析点：**保留路径，不做路径删除**，
        // 避免顺着链接删掉外部同名文件。
        inline void Discard(StagedFile& staged)
        {
            if (staged.temp.empty())
            {
                return;
            }
            std::wstring offending;
            if (PathSafety::HasReparsePoint(staged.temp, &offending))
            {
                staged.stateUnknown = true;   // 保留 temp 路径作为恢复/排查线索
                return;
            }
            if (FileExists(staged.temp))
            {
                if (!::DeleteFileW(staged.temp.c_str()))
                {
                    staged.stateUnknown = true;
                    return;
                }
            }
            staged.temp.clear();
        }

        // 单文件便捷入口：Stage -> Commit -> Finalize。
        //   · Commit 抛异常（测试 seam 或 bad_alloc）：catch 尽力 Rollback；恢复报告写回 error；
        //   · Commit 返回 false：Rollback 的恢复报告**不能丢**，失败时保留 backup 路径；
        //   · 已成功提交后 Finalize 失败：数据已安全写入，返回 true 并在 error 里带清理提示；
        //   · 任何异常都不会穿出本函数，也不会留下无人管理的 temp / backup。
        inline bool WriteFileAtomic(const std::wstring& targetPath, const void* data, size_t size,
                                    std::wstring& error)
        {
            error.clear();
            StagedFile staged;
            struct Guard
            {
                StagedFile* file = nullptr;
                bool finished = false;
                ~Guard()
                {
                    if (finished || file == nullptr)
                    {
                        return;
                    }
                    try
                    {
                        std::wstring ignore;
                        Rollback(*file, ignore);
                    }
                    catch (...)
                    {
                    }
                    try
                    {
                        Discard(*file);
                    }
                    catch (...)
                    {
                    }
                }
            } guard{&staged, false};

            bool committed = false;
            try
            {
                if (!StageBytes(targetPath, data, size, staged, error))
                {
                    return false;
                }
                std::wstring commitError;
                if (!Commit(staged, commitError))
                {
                    std::wstring rollbackError;
                    bool rolledBack = false;
                    try
                    {
                        rolledBack = Rollback(staged, rollbackError);
                    }
                    catch (...)
                    {
                        rolledBack = false;
                    }
                    try
                    {
                        Discard(staged);
                    }
                    catch (...)
                    {
                    }
                    try
                    {
                        error = commitError;
                        if (!rolledBack)
                        {
                            error += L"\r\n回滚未能完成，恢复信息（请勿删除 backup）：\r\n"
                                   + rollbackError;
                        }
                    }
                    catch (...)
                    {
                        try
                        {
                            error = L"原子写入失败；回滚报告构造失败，"
                                    L"请保留 .yua-bak / .yua-tmp 并检查目标文件。";
                        }
                        catch (...)
                        {
                        }
                    }
                    return false;
                }

                // 已提交：此后只做 best-effort 清理，任何异常都返回 true（数据已安全写入）。
                committed = true;
                guard.finished = true;
                try
                {
                    std::wstring finalizeError;
                    bool finalizeOk = false;
                    try
                    {
                        finalizeOk = Finalize(staged, finalizeError);
                    }
                    catch (...)
                    {
                        finalizeOk = false;
                        try
                        {
                            finalizeError = L"清理 backup/temp 时发生异常。";
                        }
                        catch (...)
                        {
                        }
                    }
                    if (!finalizeOk)
                    {
                        try
                        {
                            error = L"数据已原子写入，但清理提示：" + finalizeError;
                        }
                        catch (...)
                        {
                        }
                    }
                }
                catch (...)
                {
                    try
                    {
                        error = L"数据已原子写入，但收尾清理异常；"
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
                if (committed)
                {
                    try
                    {
                        error = L"数据已原子写入；内存不足发生在收尾清理阶段，"
                                L"如目录中出现 .yua-bak / .yua-tmp，可手动删除。";
                    }
                    catch (...)
                    {
                    }
                    return true;
                }
                std::wstring rollbackError;
                bool rolledBack = false;
                try
                {
                    rolledBack = Rollback(staged, rollbackError);
                }
                catch (...)
                {
                }
                if (rolledBack)
                {
                    try
                    {
                        Discard(staged);
                    }
                    catch (...)
                    {
                    }
                }
                try
                {
                    error = L"内存不足：原子写入被中止。";
                    if (!rolledBack)
                    {
                        error += L"\r\n回滚未能完成，请保留恢复信息（backup / temp）："
                              + (staged.backup.empty() ? std::wstring(L"（无 backup）")
                                                      : staged.backup)
                              + L"\r\n" + rollbackError;
                    }
                }
                catch (...)
                {
                    try
                    {
                        error = L"内存不足：原子写入被中止；回滚报告构造失败，"
                                L"请保留 .yua-bak / .yua-tmp 并检查目标文件。";
                    }
                    catch (...)
                    {
                    }
                }
                return false;
            }
            catch (const std::length_error&)
            {
                if (committed)
                {
                    try
                    {
                        error = L"数据已原子写入；长度异常发生在收尾清理阶段，"
                                L"如目录中出现 .yua-bak / .yua-tmp，可手动删除。";
                    }
                    catch (...)
                    {
                    }
                    return true;
                }
                std::wstring rollbackError;
                bool rolledBack = false;
                try
                {
                    rolledBack = Rollback(staged, rollbackError);
                }
                catch (...)
                {
                }
                if (rolledBack)
                {
                    try
                    {
                        Discard(staged);
                    }
                    catch (...)
                    {
                    }
                }
                try
                {
                    error = L"内容超过可处理长度：原子写入被中止。";
                    if (!rolledBack)
                    {
                        error += L"\r\n回滚未能完成，请保留恢复信息（backup / temp）："
                              + (staged.backup.empty() ? std::wstring(L"（无 backup）")
                                                      : staged.backup)
                              + L"\r\n" + rollbackError;
                    }
                }
                catch (...)
                {
                    try
                    {
                        error = L"长度异常：原子写入被中止；回滚报告构造失败，"
                                L"请保留 .yua-bak / .yua-tmp 并检查目标文件。";
                    }
                    catch (...)
                    {
                    }
                }
                return false;
            }
        }

        inline bool WriteFileAtomic(const std::wstring& targetPath,
                                    const std::vector<unsigned char>& bytes,
                                    std::wstring& error)
        {
            return WriteFileAtomic(targetPath, bytes.empty() ? nullptr : bytes.data(), bytes.size(),
                                   error);
        }
    }
}
