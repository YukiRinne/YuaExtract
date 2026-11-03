// 原生执行授权与导出契约的 headless 回归。
//
// 只调用生产 DLL 的真实导出函数，不启动游戏、不注入进程：
//   1) 旧导出（ExtractKey/Repack/DeriveKeys）必须是安全默认（flags=0）：明确失败、无输出；
//   2) 新 Ex 导出的未知 flags 位必须立即拒绝；
//   3) 显式 flags bit0 的"允许分支"能进入（用无效输入得到业务错误，而不是授权拒绝）；
//   4) 已有 profile / 游戏旁侧 .hxv4p 的复用路径不需要原生执行，仍能打包成功。
//
// 用法：export-contract-test.exe <CxdecKeyStatic.dll> <CxdecRepacker.dll> <临时根目录>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include "Hxv4p.h"

#include <cstdio>
#include <cstring>
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

template <typename Fn>
bool Bind(HMODULE module, const char* name, Fn& out) {
    out = reinterpret_cast<Fn>(::GetProcAddress(module, name));
    if (out == nullptr) {
        std::printf("FAIL missing export %s\n", name);
        ++g_failures;
        return false;
    }
    return true;
}

bool PathExists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool HasExtensionRecursive(const fs::path& root, const wchar_t* ext) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return false;
    for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
        if (e.is_regular_file() && e.path().extension() == ext) return true;
    }
    return false;
}

void WriteBytes(const fs::path& p, const std::vector<unsigned char>& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    FILE* f = nullptr;
    _wfopen_s(&f, p.c_str(), L"wb");
    if (f != nullptr) {
        if (!bytes.empty()) fwrite(bytes.data(), 1, bytes.size(), f);
        fclose(f);
    }
}

bool Contains(const char* text, const char* needle) {
    return text != nullptr && std::strstr(text, needle) != nullptr;
}

bool IsAuthDenied(const char* text) {
    // 拒绝文案里一定带这两个 ASCII 标记；不依赖系统 ANSI 代码页。
    return Contains(text, "sandbox") && Contains(text, "--allow-native-code");
}

Hxv4p::Parameters MakeProfile() {
    Hxv4p::Parameters p;
    p.source_module = "bootstrap.dll";
    p.source_module_base = 0x10000000u;
    p.manager_va = 0x10001000u;
    p.context_va = 0x10002000u;
    p.drip_impl_va = 0x10003000u;
    for (int i = 0; i < 32; ++i) {
        p.hxv4_key[i] = static_cast<uint8_t>(i + 1);
        p.hash_key[i] = static_cast<uint8_t>(0xA0 + i);
    }
    for (int i = 0; i < 24; ++i) {
        p.hxv4_nonce0[i] = static_cast<uint8_t>(0x10 + i);
        p.hxv4_nonce1[i] = static_cast<uint8_t>(0x40 + i);
    }
    p.hash_domain = "testdomain";
    p.hash_domain_known = true;
    for (int i = 0; i < 6; ++i) p.holder_words.push_back(static_cast<uint32_t>(i));
    p.context_u32.assign(1024, 0u);
    p.lanes.assign(128, Hxv4p::Lane{});
    for (auto& lane : p.lanes) lane.push_back(Hxv4p::LaneRecord{0u, 0x51D90u});
    return p;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        std::printf("usage: export-contract-test <CxdecKeyStatic.dll> <CxdecRepacker.dll> <tempRoot>\n");
        return 2;
    }

    HMODULE keyStatic = ::LoadLibraryW(argv[1]);
    HMODULE repacker = ::LoadLibraryW(argv[2]);
    if (keyStatic == nullptr || repacker == nullptr) {
        std::printf("FAIL LoadLibrary keyStatic=%d repacker=%d\n", keyStatic != nullptr,
                    repacker != nullptr);
        return 1;
    }

    using ExtractKeyFn = BOOL(__stdcall*)(const wchar_t*, const wchar_t*, char*, int);
    using ExtractKeyExFn = BOOL(__stdcall*)(const wchar_t*, const wchar_t*, DWORD, char*, int);
    using RepackFn = BOOL(__stdcall*)(const wchar_t*, const wchar_t*, const wchar_t*,
                                      const wchar_t*, const wchar_t*, int, int, char*, int,
                                      char*, int);
    using RepackExFn = BOOL(__stdcall*)(const wchar_t*, const wchar_t*, const wchar_t*,
                                        const wchar_t*, const wchar_t*, int, int, DWORD, char*,
                                        int, char*, int);
    using DeriveKeysFn = BOOL(__stdcall*)(const wchar_t*, const wchar_t*, char*, int, char*, int);
    using DeriveKeysExFn = BOOL(__stdcall*)(const wchar_t*, const wchar_t*, DWORD, char*, int,
                                            char*, int);

    ExtractKeyFn extractKey = nullptr;
    ExtractKeyExFn extractKeyEx = nullptr;
    RepackFn repack = nullptr;
    RepackExFn repackEx = nullptr;
    DeriveKeysFn deriveKeys = nullptr;
    DeriveKeysExFn deriveKeysEx = nullptr;

    bool exportsOk = true;
    exportsOk = Bind(keyStatic, "ExtractKey", extractKey) && exportsOk;
    exportsOk = Bind(keyStatic, "ExtractKeyEx", extractKeyEx) && exportsOk;
    exportsOk = Bind(repacker, "Repack", repack) && exportsOk;
    exportsOk = Bind(repacker, "RepackEx", repackEx) && exportsOk;
    exportsOk = Bind(repacker, "DeriveKeys", deriveKeys) && exportsOk;
    exportsOk = Bind(repacker, "DeriveKeysEx", deriveKeysEx) && exportsOk;
    Check(exportsOk, "all legacy + Ex exports bind by undecorated name");
    if (!exportsOk) return 1;

    const fs::path root = fs::path(argv[3]) / L"export-contract";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    const fs::path exe = root / L"game.exe";
    std::vector<unsigned char> exeBytes(256, 0);
    exeBytes[0] = 'M';
    exeBytes[1] = 'Z';
    WriteBytes(exe, exeBytes);

    const fs::path flat = root / L"flat";
    fs::create_directories(flat, ec);
    WriteBytes(flat / L"foo.txt", std::vector<unsigned char>{'h', 'e', 'l', 'l', 'o'});

    // ---- 1. KeyStatic 安全默认 ----
    {
        const fs::path out = root / L"key-default";
        char err[1024] = {};
        const BOOL ok = extractKey(exe.c_str(), out.c_str(), err, sizeof(err));
        Check(ok == FALSE, "ExtractKey (legacy) default denies");
        Check(IsAuthDenied(err), "ExtractKey denial explains explicit authorization + not sandbox");
        Check(!PathExists(out), "ExtractKey default creates no output directory (gate before side effects)");
    }
    {
        const fs::path out = root / L"key-unknown";
        char err[1024] = {};
        const BOOL ok = extractKeyEx(exe.c_str(), out.c_str(), 2u, err, sizeof(err));
        Check(ok == FALSE, "ExtractKeyEx unknown flags rejected");
        Check(Contains(err, "0x00000002"), "ExtractKeyEx unknown flags names the bit");
        Check(!PathExists(out), "ExtractKeyEx unknown flags creates no output");
    }
    {
        const fs::path out = root / L"key-allowed";
        char err[1024] = {};
        const BOOL ok = extractKeyEx(exe.c_str(), out.c_str(), 1u, err, sizeof(err));
        Check(ok == FALSE, "ExtractKeyEx flags=1 on invalid EXE fails (allowed branch entered)");
        Check(!IsAuthDenied(err), "ExtractKeyEx flags=1 is past the authorization gate");
        Check(Contains(err, "STARTUP.TJS"), "allowed branch reaches real parsing (STARTUP.TJS)");
        Check(!HasExtensionRecursive(out, L".hxv4p"), "allowed invalid input leaves no .hxv4p");
    }

    // ---- 2. Repacker 安全默认 / 未知位 ----
    const fs::path keys = root / L"keys";
    {
        char err[2048] = {};
        char note[1024] = {};
        const BOOL ok = deriveKeys(exe.c_str(), keys.c_str(), note, sizeof(note), err, sizeof(err));
        Check(ok == FALSE, "DeriveKeys (legacy) default denies native derivation");
        Check(IsAuthDenied(err), "DeriveKeys denial explains authorization");
        Check(!PathExists(keys / L"manifest.txt"), "DeriveKeys default writes no manifest");
    }
    {
        char err[2048] = {};
        char note[1024] = {};
        const BOOL ok = deriveKeysEx(exe.c_str(), keys.c_str(), 2u, note, sizeof(note), err,
                                     sizeof(err));
        Check(ok == FALSE, "DeriveKeysEx unknown flags rejected");
        Check(Contains(err, "0x00000002"), "DeriveKeysEx unknown flags names the bit");
    }
    {
        char err[2048] = {};
        char note[1024] = {};
        const BOOL ok = deriveKeysEx(exe.c_str(), keys.c_str(), 1u, note, sizeof(note), err,
                                     sizeof(err));
        Check(ok == FALSE, "DeriveKeysEx flags=1 on invalid EXE fails after entering allowed branch");
        Check(!IsAuthDenied(err), "DeriveKeysEx flags=1 is past the authorization gate");
        Check(!PathExists(keys / L"manifest.txt"), "allowed branch on invalid EXE writes no manifest");
    }
    {
        char detail[2048] = {};
        char err[2048] = {};
        const fs::path outXp3 = root / L"denied.xp3";
        const BOOL ok = repack(flat.c_str(), outXp3.c_str(), exe.c_str(), keys.c_str(), nullptr,
                               -1, 0, detail, sizeof(detail), err, sizeof(err));
        Check(ok == FALSE, "Repack (legacy) with unknown EXE profile refuses instead of builtin fallback");
        Check(IsAuthDenied(err), "Repack denial explains authorization");
        Check(!PathExists(outXp3), "Repack denial leaves no output xp3");
    }

    // ---- 3. 旁侧 .hxv4p 收编（无原生执行）----
    {
        Hxv4p::Parameters p = MakeProfile();
        std::string encodeError;
        const std::vector<uint8_t> blob = Hxv4p::Encode(p, true, encodeError);
        Check(!blob.empty(), "synthetic .hxv4p encoded");
        const fs::path side = exe.parent_path() / L"ExtractKey_Output" / L"Static" /
                              (exe.stem().wstring() + L"_drip_program.hxv4p");
        WriteBytes(side, blob);

        char err[2048] = {};
        char note[1024] = {};
        const BOOL ok = deriveKeys(exe.c_str(), keys.c_str(), note, sizeof(note), err, sizeof(err));
        Check(ok == TRUE, "DeriveKeys legacy imports side-product without native execution");
        Check(Contains(note, "收编") || !IsAuthDenied(err), "side-product import path used");
        Check(PathExists(keys / L"manifest.txt"), "side-product import writes manifest");

        const fs::path outXp3 = root / L"cached.xp3";
        char detail[2048] = {};
        char err2[2048] = {};
        const BOOL packed = repack(flat.c_str(), outXp3.c_str(), exe.c_str(), keys.c_str(), nullptr,
                                   -1, 0, detail, sizeof(detail), err2, sizeof(err2));
        Check(packed == TRUE, "Repack legacy reuses cached profile with no native execution");
        Check(PathExists(outXp3), "cached-profile Repack produced output");
        Check(!Contains(err2, "sandbox"), "cached-profile Repack did not hit authorization denial");
    }

    // ---- 4. 另一份无 profile 的 EXE：默认必须失败且无输出 ----
    {
        const fs::path exe2 = root / L"other.exe";
        std::vector<unsigned char> b2 = exeBytes;
        b2[2] = 'X';
        WriteBytes(exe2, b2);
        const fs::path outXp3 = root / L"other.xp3";
        char detail[2048] = {};
        char err[2048] = {};
        const BOOL ok = repackEx(flat.c_str(), outXp3.c_str(), exe2.c_str(), keys.c_str(), nullptr,
                                 -1, 0, 0u, detail, sizeof(detail), err, sizeof(err));
        Check(ok == FALSE, "RepackEx flags=0 with missing profile refuses");
        Check(IsAuthDenied(err), "RepackEx flags=0 refusal is the authorization default");
        Check(!PathExists(outXp3), "RepackEx flags=0 leaves no output xp3");
    }

    ::FreeLibrary(keyStatic);
    ::FreeLibrary(repacker);

    std::printf("RESULT failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
