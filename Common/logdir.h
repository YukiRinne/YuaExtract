#pragma once

// 全项目的日志落点都从这里算，别再一处一个目录。
//
// 规则：<工具根>\Log\ —— 工具根就是 loader.exe 所在目录。各模块自己的 DLL 通常
// 放在子目录 CxdecExtractordll 里，所以传进来的模块目录若是那一层就上跳一级。
// 工具根写不了（装在 Program Files、或整个目录只读）时退到
// %LOCALAPPDATA%\KrkrExtract\Log\，并把最终位置写进日志头。
//
// 写成头文件形式：CxdecExtractorUI / CxdecAntiMalform 这两个工程是独立的
// （不编 Common 的 cpp），头文件形式不用去改它们的源码列表。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>
#include <cwctype>
#include <string>

namespace Log
{
    struct LogDirectory
    {
        std::wstring path;         // 最终用的目录
        bool fallback = false;     // true = 工具根写不了，用了 %LOCALAPPDATA%
        std::wstring primaryPath;  // 本来该用的目录（写进日志头，说明为什么换了地方）
        DWORD primaryError = 0;    // 主位置建不出来/不可写的错误码；0 = 没失败
        std::wstring fallbackPath; // 实际尝试过的 %LOCALAPPDATA% 位置（诊断用）
        DWORD fallbackError = 0;   // 回退位置也不可用时的错误码
    };

    namespace DirectoryDetail
    {
        // SRW 独占锁的 noexcept RAII guard：锁住之后任何 wstring/容器分配
        // 抛异常，析构也会释放锁，不会把后续 logger 永久挂死在同一条日志目录上。
        struct SrwExclusiveGuard
        {
            SRWLOCK* lock;

            explicit SrwExclusiveGuard(SRWLOCK* value) noexcept : lock(value)
            {
                ::AcquireSRWLockExclusive(lock);
            }
            ~SrwExclusiveGuard() noexcept
            {
                ::ReleaseSRWLockExclusive(lock);
            }

            SrwExclusiveGuard(const SrwExclusiveGuard&) = delete;
            SrwExclusiveGuard& operator=(const SrwExclusiveGuard&) = delete;
        };

        inline std::wstring StripTrailingSeparators(const std::wstring& path)
        {
            size_t end = path.size();
            while (end > 0 && (path[end - 1] == L'\\' || path[end - 1] == L'/'))
            {
                --end;
            }
            return path.substr(0, end);
        }

        inline std::wstring LeafName(const std::wstring& path)
        {
            const size_t separator = path.find_last_of(L"\\/");
            return separator == std::wstring::npos ? path : path.substr(separator + 1);
        }

        inline std::wstring ParentDirectory(const std::wstring& path)
        {
            const size_t separator = path.find_last_of(L"\\/");
            return separator == std::wstring::npos ? std::wstring() : path.substr(0, separator);
        }

        inline std::wstring ToLower(const std::wstring& text)
        {
            std::wstring lower = text;
            for (wchar_t& ch : lower)
            {
                ch = static_cast<wchar_t>(::towlower(ch));
            }
            return lower;
        }

        // 逐级建目录。父级可能也不存在（%LOCALAPPDATA%\KrkrExtract\Log 的第一级）。
        // 已存在算成功——多个进程同时起，谁先建都正常。
        inline bool EnsureDirectory(const std::wstring& path, DWORD* errorOut = nullptr)
        {
            if (errorOut != nullptr)
            {
                *errorOut = 0;
            }
            if (path.empty())
            {
                return false;
            }

            const DWORD attributes = ::GetFileAttributesW(path.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES)
            {
                return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            }

            std::wstring current;
            size_t index = 0;
            if (path.size() >= 2 && path[1] == L':')
            {
                current.assign(path, 0, 2);  // 盘符前缀单独留着，别拼成 "C:foo"
                index = 2;
            }
            else if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\')
            {
                // UNC：\\server\share 这两段必须整体存在，逐段建会失败，
                // 于是"工具放在网络共享上"会直接把日志退到 %LOCALAPPDATA%。
                // 跳过它们，第一次 CreateDirectoryW 就是完整的 \\server\share\Log。
                const size_t shareEnd = path.find_first_of(L"\\/", 2);
                index = shareEnd == std::wstring::npos ? path.size() : shareEnd + 1;
            }

            while (index <= path.size())
            {
                const size_t separator = path.find_first_of(L"\\/", index);
                const size_t end = separator == std::wstring::npos ? path.size() : separator;
                if (end > index)
                {
                    current.assign(path.begin(), path.begin() + end);
                    if (!::CreateDirectoryW(current.c_str(), nullptr) &&
                        ::GetLastError() != ERROR_ALREADY_EXISTS)
                    {
                        if (errorOut != nullptr)
                        {
                            *errorOut = ::GetLastError();
                        }
                        return false;
                    }
                }
                if (separator == std::wstring::npos)
                {
                    break;
                }
                index = separator + 1;
            }

            const DWORD finalAttributes = ::GetFileAttributesW(path.c_str());
            if (finalAttributes != INVALID_FILE_ATTRIBUTES && (finalAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            {
                return true;
            }
            if (errorOut != nullptr)
            {
                // 建到一半失败、或路径被同名文件占着；重新拿一次错误码（可能是 0，看日志的人自己判断）
                *errorOut = ::GetLastError();
            }
            return false;
        }

        inline std::wstring EnvironmentValue(const wchar_t* name)
        {
            wchar_t buffer[MAX_PATH]{};
            const DWORD length = ::GetEnvironmentVariableW(name, buffer, _countof(buffer));
            if (length == 0 || length >= _countof(buffer))
            {
                return std::wstring();
            }
            return std::wstring(buffer, length);
        }

        // 目录存在 ≠ 能写：装在 Program Files、目录继承了一条只读 ACL、
        // 或者被别的进程占着时，GetFileAttributesW 仍然会成功。
        //
        // 所以用一个**自己唯一命名**的 CREATE_NEW 小文件做真实写入探测：
        //   - 不碰现有日志文件（不打开、不 rename、不删除任何既有文件）；
        //   - 写完立即关闭并删除；CREATE_NEW 保证不会覆盖别人的东西；
        //   - 只在解析日志目录时调一次（见 ResolveLogDirectory 的缓存），
        //     不会变成每条日志都探测一次。
        inline bool ProbeDirectoryWritable(const std::wstring& directory, DWORD* errorOut = nullptr)
        {
            if (errorOut != nullptr)
            {
                *errorOut = 0;
            }
            if (directory.empty())
            {
                if (errorOut != nullptr)
                {
                    *errorOut = ERROR_PATH_NOT_FOUND;
                }
                return false;
            }

            const std::wstring stem = directory + L"\\.yuaext-log-probe-" +
                                      std::to_wstring(::GetCurrentProcessId()) + L"-" +
                                      std::to_wstring(::GetTickCount64()) + L"-";

            for (int attempt = 0; attempt < 8; ++attempt)
            {
                const std::wstring probe = stem + std::to_wstring(attempt) + L".tmp";
                HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
                                            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
                if (file == INVALID_HANDLE_VALUE)
                {
                    const DWORD createError = ::GetLastError();
                    if (createError == ERROR_FILE_EXISTS || createError == ERROR_ALREADY_EXISTS)
                    {
                        continue;  // 名字撞了，换一个；绝不打开已有文件
                    }
                    if (errorOut != nullptr)
                    {
                        *errorOut = createError;
                    }
                    return false;
                }

                const char marker = 'x';
                DWORD written = 0;
                const BOOL wrote = ::WriteFile(file, &marker, 1, &written, nullptr);
                const DWORD writeError = wrote ? 0u : ::GetLastError();
                ::CloseHandle(file);
                // 探测文件由我们自己 CREATE_NEW 建出来的，删除失败也不影响结论
                ::DeleteFileW(probe.c_str());

                if (!wrote || written != 1)
                {
                    if (errorOut != nullptr)
                    {
                        *errorOut = writeError != 0 ? writeError : ERROR_WRITE_FAULT;
                    }
                    return false;
                }
                return true;
            }

            if (errorOut != nullptr)
            {
                *errorOut = ERROR_FILE_EXISTS;
            }
            return false;
        }
    }

    // ownModuleDirectory：调用方自己的模块所在目录（DLL 目录就行，不必是 exe）。
    //
    // 注意：这里会做一次**真实写探测**（CREATE_NEW 小文件），不是只看目录是否存在。
    // 探测结果按输入目录缓存 —— 日志目录在一次进程生命周期里不会变，Loader 的
    // Host::LogDirectory() 之类反复调用不会每次都落盘探测。
    inline LogDirectory ResolveLogDirectoryUncached(const std::wstring& ownModuleDirectory)
    {
        using namespace DirectoryDetail;

        LogDirectory result;

        std::wstring root = StripTrailingSeparators(ownModuleDirectory);
        if (ToLower(LeafName(root)) == L"cxdecextractordll")
        {
            root = ParentDirectory(root);
        }

        result.primaryPath = root.empty() ? L"Log" : root + L"\\Log";
        if (EnsureDirectory(result.primaryPath, &result.primaryError) &&
            ProbeDirectoryWritable(result.primaryPath, &result.primaryError))
        {
            result.path = result.primaryPath;
            return result;
        }

        // 工具根不可写就退到用户目录：日志是要用户发回来的东西，
        // 不能因为工具装在只读位置就整个丢掉。
        const std::wstring localAppData = EnvironmentValue(L"LOCALAPPDATA");
        if (!localAppData.empty())
        {
            const std::wstring base = localAppData + L"\\KrkrExtract";
            const std::wstring alternative = base + L"\\Log";
            result.fallbackPath = alternative;
            if (EnsureDirectory(alternative, &result.fallbackError) &&
                ProbeDirectoryWritable(alternative, &result.fallbackError))
            {
                result.path = alternative;
                result.fallback = true;
                return result;
            }
        }

        // 兜底：仍然用主位置，让后面的 Open 把真实错误码记下来。
        // fallbackError 说明 %LOCALAPPDATA% 那条路为什么也不行。
        result.path = result.primaryPath;
        result.fallback = true;
        return result;
    }

    inline LogDirectory ResolveLogDirectory(const std::wstring& ownModuleDirectory)
    {
        // 只缓存最近一次输入（Loader 只用一个模块目录）。用 SRWLOCK 是因为
        // UI 线程和后台任务都可能来取日志目录。
        static SRWLOCK cacheLock = SRWLOCK_INIT;
        static std::wstring cachedInput;
        static LogDirectory cachedResult;
        static bool cacheValid = false;

        {
            // 只读缓存也要 guard：返回时拷贝 cachedResult 可能因内存不足抛异常，
            // 不能把锁留在这里（否则后续 logger 永久等锁）。
            DirectoryDetail::SrwExclusiveGuard guard(&cacheLock);
            if (cacheValid && cachedInput == ownModuleDirectory)
            {
                return cachedResult;
            }
        }

        // 探测/建目录不在锁内做：它可能较慢，也不需要在持有缓存锁时分配。
        const LogDirectory computed = ResolveLogDirectoryUncached(ownModuleDirectory);

        {
            DirectoryDetail::SrwExclusiveGuard guard(&cacheLock);
            cachedInput = ownModuleDirectory;
            cachedResult = computed;
            cacheValid = true;
        }
        return computed;
    }

    inline std::wstring LogFilePath(const LogDirectory& directory, const wchar_t* fileName)
    {
        const std::wstring name = fileName != nullptr ? fileName : L"";
        if (directory.path.empty())
        {
            return name;
        }
        return directory.path + L"\\" + name;
    }

    // 手边没有模块句柄时的入口：传**本模块里任意一个函数/静态变量的地址**，
    // 由系统反查它属于哪个模块，再算出统一日志目录。
    //
    // 给 HashCore.cpp 那种"同一份源码编进两个模块"的场景用——运行时按地址
    // 反查出来的就是当前这个模块，不会认错。
    inline LogDirectory ResolveLogDirectoryFromCallerModule(const void* callerAddress)
    {
        HMODULE module = nullptr;
        if (callerAddress == nullptr ||
            !::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  reinterpret_cast<LPCWSTR>(callerAddress),
                                  &module) ||
            module == nullptr)
        {
            return ResolveLogDirectory(std::wstring());
        }

        wchar_t buffer[MAX_PATH]{};
        const DWORD length = ::GetModuleFileNameW(module, buffer, _countof(buffer));
        if (length == 0 || length >= _countof(buffer))
        {
            return ResolveLogDirectory(std::wstring());
        }
        return ResolveLogDirectory(DirectoryDetail::ParentDirectory(std::wstring(buffer, length)));
    }
}
