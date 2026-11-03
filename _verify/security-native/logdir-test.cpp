// Common/logdir.h 的日志目录回退与"存在但不可写"探测回归。
//
// 验证：
//   1) 可写目录：直接使用 <module>\Log，且探测文件不会残留；
//   2) 已有 Log 目录但 ACL 禁止写：ResolveLogDirectory 必须回退到 %LOCALAPPDATA%，
//      而不是把"目录存在"当成可写；
//   3) 主位置根本无法建目录：同样回退；
//   4) 结果按输入目录缓存（重复调用不反复写探测文件）。
//
// 用法：logdir-test.exe <临时根目录>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <aclapi.h>

#include "logdir.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

std::wstring LocalAppDataLog() {
    wchar_t buffer[MAX_PATH] = {};
    const DWORD n = ::GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, _countof(buffer));
    if (n == 0 || n >= _countof(buffer)) return std::wstring();
    return std::wstring(buffer, n) + L"\\KrkrExtract\\Log";
}

bool SetDenyWriteForCurrentUser(const std::wstring& path, std::vector<unsigned char>& savedAcl,
                                DWORD& error) {
    PACL oldDacl = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    error = ::GetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION, nullptr, nullptr, &oldDacl, nullptr,
                                    &sd);
    if (error != ERROR_SUCCESS) return false;

    if (oldDacl != nullptr) {
        savedAcl.assign(reinterpret_cast<unsigned char*>(oldDacl),
                        reinterpret_cast<unsigned char*>(oldDacl) + oldDacl->AclSize);
    } else {
        savedAcl.clear();
    }

    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error = ::GetLastError();
        ::LocalFree(sd);
        return false;
    }
    DWORD needed = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<unsigned char> tokenUser(needed);
    if (needed == 0 ||
        !::GetTokenInformation(token, TokenUser, tokenUser.data(), needed, &needed)) {
        error = ::GetLastError();
        ::CloseHandle(token);
        ::LocalFree(sd);
        return false;
    }
    ::CloseHandle(token);
    auto* user = reinterpret_cast<TOKEN_USER*>(tokenUser.data());

    EXPLICIT_ACCESSW ea{};
    ea.grfAccessPermissions = FILE_GENERIC_WRITE;
    ea.grfAccessMode = DENY_ACCESS;
    ea.grfInheritance = NO_INHERITANCE;
    ea.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    ea.Trustee.TrusteeType = TRUSTEE_IS_USER;
    ea.Trustee.ptstrName = reinterpret_cast<LPWSTR>(user->User.Sid);

    PACL newDacl = nullptr;
    error = ::SetEntriesInAclW(1, &ea, oldDacl, &newDacl);
    if (error == ERROR_SUCCESS) {
        error = ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                                        DACL_SECURITY_INFORMATION, nullptr, nullptr, newDacl,
                                        nullptr);
    }
    if (newDacl != nullptr) ::LocalFree(newDacl);
    ::LocalFree(sd);
    return error == ERROR_SUCCESS;
}

void RestoreAcl(const std::wstring& path, const std::vector<unsigned char>& savedAcl) {
    PACL dacl = savedAcl.empty() ? nullptr
                                : reinterpret_cast<PACL>(const_cast<unsigned char*>(savedAcl.data()));
    ::SetNamedSecurityInfoW(const_cast<LPWSTR>(path.c_str()), SE_FILE_OBJECT,
                            DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr);
}

int CountEntries(const std::wstring& path) {
    std::error_code ec;
    int n = 0;
    for (const auto& e : fs::directory_iterator(path, ec)) {
        (void)e;
        ++n;
    }
    return n;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::printf("usage: logdir-test <tempRoot>\n");
        return 2;
    }

    const fs::path root = fs::path(argv[1]) / L"logdir";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // ---- 1. 可写主目录 ----
    {
        const std::wstring moduleDir = (root / L"writable").wstring();
        fs::create_directories(moduleDir, ec);
        const Log::LogDirectory first = Log::ResolveLogDirectory(moduleDir);
        Check(!first.fallback, "writable primary used without fallback");
        Check(first.path == moduleDir + L"\\Log", "writable primary path is <module>\\Log");
        Check(CountEntries(first.path) == 0, "writability probe file removed");

        const Log::LogDirectory second = Log::ResolveLogDirectory(moduleDir);
        Check(second.path == first.path, "cached resolution is stable");
        Check(CountEntries(first.path) == 0, "cached resolution does not leave probe files");
    }

    // ---- 2. 主目录无法创建 ----
    {
        const fs::path blocker = root / L"blocker";
        FILE* f = nullptr;
        _wfopen_s(&f, blocker.c_str(), L"wb");
        if (f != nullptr) fclose(f);
        const Log::LogDirectory r = Log::ResolveLogDirectory(blocker.wstring());
        Check(r.fallback, "uncreatable primary falls back");
        Check(r.path == LocalAppDataLog(), "fallback path is %LOCALAPPDATA%\\KrkrExtract\\Log");
    }

    // ---- 3. 已有 Log 但不可写（真实写入探测，而不是看目录存在）----
    {
        const fs::path moduleDir = root / L"denied";
        const fs::path logDir = moduleDir / L"Log";
        fs::create_directories(logDir, ec);

        std::vector<unsigned char> savedAcl;
        DWORD aclError = 0;
        const bool denied = SetDenyWriteForCurrentUser(logDir.wstring(), savedAcl, aclError);
        if (!denied) {
            std::printf("SKIP cannot apply deny-write ACE (error=%lu)\n",
                        static_cast<unsigned long>(aclError));
        } else {
            const Log::LogDirectory r = Log::ResolveLogDirectory(moduleDir.wstring());
            Check(r.fallback, "existing-but-unwritable Log is detected by write probe");
            Check(r.path == LocalAppDataLog(), "unwritable primary falls back to LOCALAPPDATA");
            Check(r.primaryError == ERROR_ACCESS_DENIED,
                  "primary error records ACCESS_DENIED from the write probe");
            RestoreAcl(logDir.wstring(), savedAcl);
        }
    }

    // 只清理本次自造的临时根；ACL 已在上面恢复，不触碰机器级安全设置。
    std::error_code cleanup_ec;
    fs::remove_all(root, cleanup_ec);

    std::printf("RESULT failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
