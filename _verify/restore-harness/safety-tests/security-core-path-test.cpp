// security-core-path-test
// 针对 Common/PathSafety.h 与 YuaExtractWorkspace 的路径边界 / 重解析点 / 递归清理回归。
// 所有合成数据只落在命令行给出的唯一临时根下，不碰用户目录。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "PathSafety.h"
#include "YuaExtractWorkspace.h"

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, const char* label) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s\n", label);
    } else {
        std::printf("ok   %s\n", label);
    }
}

void Check(bool condition, const wchar_t* label) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %ls\n", label);
    } else {
        std::printf("ok   %ls\n", label);
    }
}


std::wstring Arg(int argc, wchar_t** argv, int index) {
    return index < argc ? std::wstring(argv[index]) : std::wstring();
}

bool MakeDir(const std::wstring& path) {
    return ::CreateDirectoryW(path.c_str(), nullptr) != FALSE || ::GetLastError() == ERROR_ALREADY_EXISTS;
}

bool WriteFileText(const std::wstring& path, const char* text) {
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = ::WriteFile(file, text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
    ::CloseHandle(file);
    return ok && written == static_cast<DWORD>(std::strlen(text));
}

// mklink /J 不需要管理员权限；比 CreateSymbolicLink 更适合这台构建机。
bool CreateJunction(const std::wstring& link, const std::wstring& target) {
    std::wstring command = L"cmd.exe /c mklink /J \"" + link + L"\" \"" + target + L"\" >nul 2>nul";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    if (!::CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                          nullptr, nullptr, &startup, &process)) {
        return false;
    }
    ::WaitForSingleObject(process.hProcess, 30000);
    DWORD exitCode = 1;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    return exitCode == 0 && ::GetFileAttributesW(link.c_str()) != INVALID_FILE_ATTRIBUTES;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    const std::wstring root = Arg(argc, argv, 1);
    if (root.empty()) {
        std::printf("usage: security-core-path-test <scratch-root>\n");
        return 2;
    }
    MakeDir(root);
    const std::wstring work = root + L"\\paths";
    MakeDir(work);
    const std::wstring output = work + L"\\out";
    MakeDir(output);
    const std::wstring outside = root + L"\\sentinel-outside";
    MakeDir(outside);
    WriteFileText(outside + L"\\sentinel.txt", "sentinel-bytes-must-survive");

    std::wstring why;
    // ---- 正常名字：中文 / 空根域 / 多层 ----
    Check(PathSafety::ValidateRelativeName(L"%EmptyString%", PathSafety::Kind::Directory, &why),
          L"dir root %EmptyString% accepted");
    Check(PathSafety::ValidateRelativeName(L"/", PathSafety::Kind::Directory, &why),
          L"dir root / accepted");
    Check(PathSafety::ValidateRelativeName(L"bgimage", PathSafety::Kind::Directory, &why),
          L"single dir accepted");
    Check(PathSafety::ValidateRelativeName(L"bgimage\\cg01\\", PathSafety::Kind::Directory, &why),
          L"nested dir trailing slash accepted");
    Check(PathSafety::ValidateRelativeName(L"bgimage/cg01.png", PathSafety::Kind::RelativePath, &why),
          L"relative file with hierarchy accepted");
    Check(PathSafety::ValidateRelativeName(L"中文目录\\中文.png", PathSafety::Kind::RelativePath, &why),
          L"chinese path accepted");

    // ---- 恶意 / 歧义名字 ----
    const wchar_t* bad[] = {
        L"..\\..\\Windows\\Temp", L"../x", L"C:\\absolute", L"\\\\server\\share", L"\\\\?\\C:\\x",
        L"file.txt:stream", L"CON", L"NUL.txt", L"COM1", L"LPT9.log", L"trailing.", L"trailing ",
        L"a\\\\b", L"a\\..\\b", L"\\lead", L"bad\x01name", L"a/b/", L"",
    };
    for (const wchar_t* name : bad) {
        Check(!PathSafety::ValidateRelativeName(name, PathSafety::Kind::RelativePath, &why),
              "malicious relative name rejected");
    }

    // ---- 组件边界：C:\a 不包含 C:\ab ----
    const std::wstring underA = PathSafety::NormalizeFullPath(root + L"\\a");
    const std::wstring underAB = PathSafety::NormalizeFullPath(root + L"\\ab\\x.txt");
    Check(PathSafety::IsUnderRoot(underA, underAB) == false, L"component boundary C:\\a vs C:\\ab");

    std::wstring full;
    Check(PathSafety::TryResolveUnderRoot(output, L"bgimage\\cg01.png", PathSafety::Kind::RelativePath,
                                          full, &why),
          L"resolve normal target under root");
    Check(PathSafety::IsUnderRoot(output, full), L"resolved target still under root");
    Check(!PathSafety::TryResolveUnderRoot(output, L"..\\escape.txt", PathSafety::Kind::RelativePath,
                                           full, &why),
          L"resolve rejects parent escape");
    Check(!PathSafety::TryResolveUnderRoot(output, L"C:\\Windows\\x", PathSafety::Kind::RelativePath,
                                           full, &why),
          L"resolve rejects absolute");

    Check(PathSafety::PathsOverlap(output, output), L"same roots overlap");
    Check(PathSafety::PathsOverlap(output, output + L"\\nested"), L"nested output overlaps source");
    Check(PathSafety::PathsOverlap(output + L"\\nested", output), L"reverse nesting overlaps");

    // ---- Normalize 输入拒绝 / HasReparsePoint fail-closed ----
    {
        std::wstring withNul = root;
        withNul.push_back(L'\0');
        withNul += L"tail";
        Check(PathSafety::NormalizeFullPath(withNul).empty(), L"embedded NUL rejected");
        Check(PathSafety::NormalizeFullPath(L"\\\\?\\C:\\Windows").empty(),
              L"device namespace \\\\?\\ rejected");
        Check(PathSafety::NormalizeFullPath(L"\\\\.\\C:\\Windows").empty(),
              L"device namespace \\\\.\\ rejected");
        Check(PathSafety::NormalizeFullPath(L"\\\\server").empty(), L"incomplete UNC rejected");
        Check(PathSafety::NormalizeFullPath(L"\\\\server\\").empty(), L"UNC without share rejected");
        std::wstring offending;
        Check(PathSafety::HasReparsePoint(L"\\\\?\\C:\\Windows", &offending),
              L"reparse check fail-closed on invalid path");
    }

    // ---- 写根别名：尾点/尾空白 / 8.3 短名 ----
    {
        const std::wstring sourceRoot = work + L"\\srcRoot";
        MakeDir(sourceRoot);
        std::wstring why;
        Check(!PathSafety::ValidateWriteRoots(sourceRoot, sourceRoot + L".", &why),
              L"same root with trailing dot alias rejected");
        Check(!PathSafety::ValidateWriteRoots(sourceRoot, sourceRoot + L" ", &why),
              L"same root with trailing space alias rejected");
        Check(!PathSafety::ValidateWriteRoots(sourceRoot, sourceRoot + L"\\child", &why),
              L"nested output root rejected before creation");

        wchar_t shortPath[512]{};
        const DWORD shortLength = ::GetShortPathNameW(sourceRoot.c_str(), shortPath, _countof(shortPath));
        if (shortLength > 0 && shortLength < _countof(shortPath) &&
            _wcsicmp(shortPath, sourceRoot.c_str()) != 0) {
            const std::wstring shortChild = std::wstring(shortPath) + L"\\child83";
            Check(!PathSafety::ValidateWriteRoots(sourceRoot, shortChild, &why),
                  L"8.3 short-name child of source rejected by identity");
        } else {
            std::printf("NOTE 8.3 short name unavailable; short-name alias check skipped\n");
        }
    }

    // ---- junction：解析与清理都不得跟随 ----
    const std::wstring junction = work + L"\\out\\link";
    Check(CreateJunction(junction, outside), L"junction created");
    if (::GetFileAttributesW(junction.c_str()) != INVALID_FILE_ATTRIBUTES) {
        Check(!PathSafety::TryResolveUnderRoot(output, L"link\\x.txt", PathSafety::Kind::RelativePath,
                                               full, &why),
              L"resolve rejects target through junction");
        Yua::DirProbe probe = Yua::ProbeDirectory(work, true);
        // work 下没有普通文件；junction link 指向 work 外，若被跟随会出现 sentinel。
        Check(probe.files == 0u, L"deep probe does not follow junction into outside");
        std::wstring error;
        Check(Yua::RemoveDirectoryTree(work, error), L"remove tree succeeds with junction inside");
        Check(::GetFileAttributesW((outside + L"\\sentinel.txt").c_str()) != INVALID_FILE_ATTRIBUTES,
              L"junction target sentinel survived cleanup");
        Check(::GetFileAttributesW(junction.c_str()) == INVALID_FILE_ATTRIBUTES,
              L"junction link itself removed");
    }

    // ---- 根目录本身是 junction：只摘链接 ----
    const std::wstring work2 = root + L"\\paths2";
    MakeDir(work2);
    const std::wstring target2 = root + L"\\outside2";
    MakeDir(target2);
    WriteFileText(target2 + L"\\keep.txt", "keep");
    const std::wstring rootJunction = root + L"\\rootlink";
    if (CreateJunction(rootJunction, target2)) {
        std::wstring error;
        Check(Yua::RemoveDirectoryTree(rootJunction, error), L"remove root junction succeeds");
        Check(::GetFileAttributesW((target2 + L"\\keep.txt").c_str()) != INVALID_FILE_ATTRIBUTES,
              L"root junction target survived");
        Check(::GetFileAttributesW(rootJunction.c_str()) == INVALID_FILE_ATTRIBUTES,
              L"root junction link removed");
    } else {
        std::printf("NOTE junction creation unsupported; root-junction checks skipped\n");
    }

    // ---- 循环 junction：探测/清理不能 hang ----
    const std::wstring loopRoot = root + L"\\loop";
    MakeDir(loopRoot);
    const std::wstring loopA = loopRoot + L"\\a";
    MakeDir(loopA);
    if (CreateJunction(loopA + L"\\back", loopRoot)) {
        Yua::DirProbe probe = Yua::ProbeDirectory(loopRoot, true);
        Check(probe.error.empty(), L"loop probe terminates without error");
        std::wstring error;
        Check(Yua::RemoveDirectoryTree(loopRoot, error), L"loop cleanup terminates");
        Check(::GetFileAttributesW(loopRoot.c_str()) == INVALID_FILE_ATTRIBUTES,
              L"loop root removed");
    } else {
        std::printf("NOTE loop junction unsupported; loop checks skipped\n");
    }

    std::printf("RESULT checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
