#include "runtime_hook.h"
#include "tjs_patcher.h"
#include "kmp_search.h"
#include "antimalform_log.h"
#include <cstdio>
#include <cstdarg>
#include <vector>
#include <shlwapi.h>
#include <detours.h>
#//pragma comment(lib, "detours.lib")

namespace {

const uint8_t kHookPattern[] = { 0x55, 0x8B, 0xEC, 0xF6, 0x45, 0x2A, 0x2A, 0x74 };
const uint8_t kHookMask[]    = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0xFF };
const size_t  kHookPatternLen = 8;

typedef void* (__cdecl *HookTargetFn)(int, int, int);

struct TjsStreamVtbl {
    int64_t  (__cdecl *Seek)(void* stream, int64_t offset, int whence);
    uint32_t (__cdecl *Read)(void* stream, void* buf, uint32_t size);
    uint32_t (__cdecl *Write)(void* stream, const void* buf, uint32_t size, int reserved);
    void     (__cdecl *SetEndOfStorage)(void* stream);
    uint64_t (__cdecl *GetSize)(void* stream);
};

struct TjsStream {
    TjsStreamVtbl* vtable;
    uint8_t _pad[0x0C];
    TjsStream* writeTarget;
};

static void* FindHookTarget() {
    HMODULE hMod = GetModuleHandleW(nullptr);
    if (!hMod) return nullptr;
    auto* base = reinterpret_cast<const uint8_t*>(hMod);
    auto* dos  = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return nullptr;
    SIZE_T imageSize = nt->OptionalHeader.SizeOfImage;
    const uint8_t* end = base + imageSize - kHookPatternLen;
    for (const uint8_t* p = base; p <= end; ++p) {
        bool match = true;
        for (size_t i = 0; i < kHookPatternLen; ++i) {
            if (kHookMask[i] == 0) continue;
            if (p[i] != kHookPattern[i]) { match = false; break; }
        }
        if (match) return const_cast<uint8_t*>(p);
    }
    return nullptr;
}

static HookTargetFn g_originalFn = nullptr;
static bool         g_hookInstalled = false;

// ---------------- 辅助函数 ----------------

static uint8_t* ReadFileToBuf(const wchar_t* path, size_t* outSize) {
    *outSize = 0;
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return nullptr; }
    auto* buf = static_cast<uint8_t*>(malloc(sz));
    if (!buf) { fclose(f); return nullptr; }
    if (fread(buf, 1, sz, f) != (size_t)sz) { free(buf); fclose(f); return nullptr; }
    fclose(f);
    *outSize = sz;
    return buf;
}

static ptrdiff_t KmpFindPos(const uint8_t* haystack, size_t haySize,
                              const uint8_t* needle, size_t needleSize) {
    const uint8_t* found = Kmp::Search(haystack, haySize, needle, needleSize);
    if (!found) return -1;
    return found - haystack;
}

// 按 XOR 增量改写：needle 与 replacement 不同的字节才动
static bool KmpDiffPatch(uint8_t* haystack, size_t haySize,
                         const uint8_t* needle, size_t needleSize,
                         const uint8_t* replacement, size_t replSize) {
    if (replSize > needleSize) return false;   // 否则下面按 replSize 读 needle 会越界
    ptrdiff_t pos = KmpFindPos(haystack, haySize, needle, needleSize);
    if (pos < 0) return false;
    if (pos + (ptrdiff_t)replSize > (ptrdiff_t)haySize) return false;
    for (size_t i = 0; i < replSize; i++) {
        haystack[pos + i] ^= needle[i] ^ replacement[i];
    }
    return true;
}

// KMP 找到后直接覆盖（steam 串只改 1 字节）
static bool KmpMemcpyPatch(uint8_t* haystack, size_t haySize,
                           const uint8_t* needle, size_t needleSize,
                           const uint8_t* replacement, size_t replSize) {
    ptrdiff_t pos = KmpFindPos(haystack, haySize, needle, needleSize);
    if (pos < 0) return false;
    if (pos + (ptrdiff_t)replSize > (ptrdiff_t)haySize) return false;
    std::memcpy(haystack + pos, replacement, replSize);
    return true;
}

// 从流里读满 size 字节，再复位到 0。
// 短读（介质错、流被截断、GetSize 与 Read 不一致）一律返回 false：
// 绝不能把 malloc 出来但只写了一半的缓冲区当成数据往下走。
static bool ReadAllAndRewind(TjsStream* stream, uint8_t* buf, uint32_t size) {
    if (stream == nullptr || stream->vtable == nullptr || buf == nullptr) return false;
    if (stream->vtable->Read == nullptr || stream->vtable->Seek == nullptr) return false;
    const uint32_t got = stream->vtable->Read(stream, buf, size);
    if (got != size) return false;
    return stream->vtable->Seek(stream, 0, 0) == 0;
}

// 第二遍读（密文态）：临时把 writeTarget 置空，读完再放回。
// __finally 保证任何退出/异常展开路径都会恢复原值，不会把流留在半改状态。
static bool ReadCipherAndRewind(TjsStream* stream, uint8_t* buf, uint32_t size) {
    if (stream == nullptr) return false;
    auto* slot = reinterpret_cast<TjsStream**>(reinterpret_cast<uint8_t*>(stream) + 0x10);
    if (slot == nullptr) return false;
    TjsStream* saved = *slot;
    bool ok = false;
    __try {
        *slot = nullptr;
        ok = ReadAllAndRewind(stream, buf, size);
    } __finally {
        *slot = saved;
    }
    return ok;
}

// 把补丁后的解密态写回运行时流。这是失败可能被上层当成"成功"的地方，
// 所以单独成一个可测入口：只有写满 size 才算成功。
static bool WriteBackToStream(TjsStream* target, const uint8_t* data, uint32_t size) {
    if (target == nullptr || target->vtable == nullptr || target->vtable->Write == nullptr ||
        data == nullptr)
        return false;
    const uint32_t wrote = target->vtable->Write(target, data, size, 0);
    return wrote == size;
}

// 写 _crack.exe。needle 是密文原文，replacement 是密文补丁态。
// outChanged：这次是否真的改到了东西（都没改到就不写文件）
// outFailed ：需要补但没补上——此时一律不写文件，由调用方决定怎么收场
static bool PatchAndWrite(const wchar_t* origPath, const wchar_t* crackPath,
                          const uint8_t* origTjs, size_t origTjsSize,
                          const uint8_t* patchedTjs, size_t patchedTjsSize,
                          bool* outChanged, bool* outFailed) {
    if (outChanged) *outChanged = false;
    if (outFailed)  *outFailed = false;

    size_t exeSize = 0;
    uint8_t* exeData = ReadFileToBuf(origPath, &exeSize);
    if (!exeData) {
        MessageBoxW(nullptr,
            L"ERROR: Failed to read original executable file.",
            L"AntiMalform", MB_ICONERROR);
        return false;
    }

    // 补丁前后完全一样 = 本来就已经补过了
    const bool tjsNeeded = (origTjsSize == patchedTjsSize) &&
                           (origTjsSize > 0) &&
                           (std::memcmp(origTjs, patchedTjs, origTjsSize) != 0);

    if (tjsNeeded &&
        !KmpDiffPatch(exeData, exeSize, origTjs, origTjsSize, patchedTjs, patchedTjsSize)) {
        // 需要补却补不上：绝不能写出一个只改了 steam、字节码没动的半成品。
        MessageBoxW(nullptr,
            L"WARNING: Startup TJS patch not applied.",
            L"AntiMalform", MB_ICONWARNING);
        AmLog(L"[AntiMalform] TJS patch could not be applied, not writing _crack.exe");
        if (outFailed) *outFailed = true;
        free(exeData);
        return false;
    }

    bool steamChanged = false;
    {
        // 先看 exe 里有没有 Steam 串：没有说明这作本来就没 Steam 壳，跳过即可。
        // 换过之后也找不到原串，所以这一项天然幂等
        const char steamStr[] = "steam=\"yes\"";
        const char steamRepl  = 'a';
        const uint8_t* steamHit = Kmp::Search(
            exeData, exeSize,
            reinterpret_cast<const uint8_t*>(steamStr), sizeof(steamStr) - 1);
        if (steamHit) {
            exeData[steamHit - exeData] = (uint8_t)steamRepl;
            steamChanged = true;
            AmLog(L"[AntiMalform] Steam patch applied ('s' -> 'a')");
        } else {
            AmLog(L"[AntiMalform] no Steam shell found, skipping Steam patch");
        }
    }

    const bool changed = tjsNeeded || steamChanged;
    if (outChanged) *outChanged = changed;

    if (!changed) {
        AmLog(L"[AntiMalform] nothing to patch, not writing _crack.exe");
        free(exeData);
        return false;
    }

    FILE* f = nullptr;
    _wfopen_s(&f, crackPath, L"wb");
    if (!f || fwrite(exeData, 1, exeSize, f) != exeSize) {
        if (f) fclose(f);
        MessageBoxW(nullptr,
            L"ERROR: Failed to write patched executable file.",
            L"AntiMalform", MB_ICONERROR);
        free(exeData);
        return false;
    }
    fclose(f);
    free(exeData);
    return true;
}

// 读流、改字节码、回写，并产出 _crack.exe
static void __cdecl PatchAndDump(TjsStream* stream) {
    AmLog(L"[AntiMalform] PatchAndDump: stream=0x%p", stream);

    if (stream == nullptr || stream->vtable == nullptr) {
        AmLog(L"[AntiMalform] PatchAndDump: null stream/vtable, skip");
        return;
    }
    if (stream->vtable->GetSize == nullptr || stream->vtable->Read == nullptr ||
        stream->vtable->Seek == nullptr || stream->vtable->Write == nullptr) {
        AmLog(L"[AntiMalform] PatchAndDump: incomplete vtable, skip");
        return;
    }

    // 所有堆分配都在开头声明，cleanup 里可以无条件释放
    // free(nullptr) 是安全的，所以不需要判断
    uint8_t* blockDec     = nullptr;
    uint8_t* blockDecOrig = nullptr;
    uint8_t* v5enc        = nullptr;
    uint8_t* v5encPatched = nullptr;
    TjsStream* savedWt    = nullptr;
    bool ok               = false;
    // 解密态真的被 TjsPatcher 改过才允许写回运行时流。
    bool plainPatched     = false;
    // 默认 true：只有 PatchAndWrite 明确报告「什么都没改到」时才会变 false。
    // 这样「根本没走到写文件那一步」（例如分配失败）不会被误判成「无需补丁」。
    bool changed          = true;
    bool failed           = false;

    AmLog(L"[AntiMalform] A: calling GetSize...");
    uint64_t rawSize = stream->vtable->GetSize(stream);
    AmLog(L"[AntiMalform] B: GetSize=%llu", rawSize);
    if (rawSize == 0 || rawSize > 10 * 1024 * 1024) {
        AmLog(L"[AntiMalform] Invalid size %llu", rawSize);
        return;
    }
    int dataSize = (int)rawSize;
    AmLog(L"[AntiMalform] C: dataSize=%d", dataSize);

    // 第一遍读：拿解密态，用来定位补丁点
    blockDec = (uint8_t*)malloc(dataSize);
    if (!blockDec) { AmLog(L"D: alloc fail"); return; }
    if (!ReadAllAndRewind(stream, blockDec, (uint32_t)dataSize)) {
        AmLog(L"[AntiMalform] E: Read1 short/seek failed (size=%d), abort without writing", dataSize);
        goto cleanup;
    }
    AmLog(L"[AntiMalform] E: Read1 OK, size=%d", dataSize);

    // 先存一份解密原文，后面要算增量
    blockDecOrig = (uint8_t*)malloc(dataSize);
    if (!blockDecOrig) { AmLog(L"G: orig alloc fail"); goto cleanup; }
    std::memcpy(blockDecOrig, blockDec, dataSize);
    AmLog(L"[AntiMalform] G: orig saved");

    // 第二遍读：把 writeTarget 置空，拿到密文态
    savedWt = *(TjsStream**)((uint8_t*)stream + 0x10);
    AmLog(L"[AntiMalform] H: writeTarget=0, alloc v5enc...");

    v5enc = (uint8_t*)malloc(dataSize);
    if (!v5enc) {
        AmLog(L"I: v5enc alloc fail");
        goto cleanup;
    }
    if (!ReadCipherAndRewind(stream, v5enc, (uint32_t)dataSize)) {
        AmLog(L"[AntiMalform] J: Read2 short/seek failed (size=%d), abort without writing", dataSize);
        goto cleanup;  // writeTarget 已由 ReadCipherAndRewind 恢复
    }
    AmLog(L"[AntiMalform] J: Read2 OK");
    AmLog(L"[AntiMalform] L: writeTarget restored");

    // 解析并修改解密态
    {
        auto patchResult = TjsPatcher::PatchBytecode(blockDec, dataSize);
        AmLog(L"[AntiMalform] M: patch done, mod=%d pat=%d", patchResult.modified, patchResult.patchesApplied);

        if (patchResult.modified) {
            // 自检：补丁后长度必须不变
            if (patchResult.bytes.size() != (size_t)dataSize) {
                AmLog(L"[AntiMalform] ERROR: PatchBytecode returned wrong size %zu (expected %d)",
                        patchResult.bytes.size(), dataSize);
                goto write_runtime;
            }
            std::memcpy(blockDec, patchResult.bytes.data(), dataSize);
            plainPatched = true;
            AmLog(L"[AntiMalform] TJS patched: %d patches", patchResult.patchesApplied);
        }
    }

    // 把明文增量换算到密文上，得到密文补丁态
    v5encPatched = (uint8_t*)malloc(dataSize);
    if (!v5encPatched) {
        AmLog(L"[AntiMalform] ERROR: v5encPatched alloc fail — crack.exe will NOT be generated");
        // 仍然要把解密态补丁写回运行时流
        goto write_runtime;
    }
    std::memcpy(v5encPatched, v5enc, dataSize);
    for (int i = 0; i < dataSize; i++) {
        uint8_t delta = blockDecOrig[i] ^ blockDec[i];
        if (delta)
            v5encPatched[i] ^= delta;
    }

write_runtime:
    // 写回运行时流，让引擎用上补丁。
    // 只有解密态真的被改过才写：把未修改的整段字节码重新写回去没有意义，
    // 而且会在流有副作用时制造额外风险。
    // 短写/失败必须标 failed，不能继续生成 _crack.exe 再 ExitProcess(0) 报成功。
    if (plainPatched && savedWt != nullptr) {
        if (!WriteBackToStream(savedWt, blockDec, (uint32_t)dataSize)) {
            AmLog(L"[AntiMalform] ERROR: write-back short/failed for %d bytes", dataSize);
            failed = true;
        }
    } else if (!plainPatched) {
        AmLog(L"[AntiMalform] no patch applied, not writing back to stream");
    }

    // 产出 _crack.exe（写回失败时 failed 已置位，绝不生成"成功"产物）
    if (!failed && v5encPatched) {
        wchar_t exePath[MAX_PATH];
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        wchar_t crackPath[MAX_PATH];
        wcscpy_s(crackPath, exePath);
        PathRemoveExtensionW(crackPath);
        size_t _len = wcslen(crackPath);
        if (_len > 4 && _wcsicmp(crackPath + _len - 4, L"_unp") == 0)
            crackPath[_len - 4] = L'\0';
        wcscat_s(crackPath, L"_crack");
        wcscat_s(crackPath, L".exe");

        // needle = 密文原文，能在 exe 里找到
        // replacement = 密文补丁态
        ok = PatchAndWrite(exePath, crackPath, v5enc, dataSize, v5encPatched, dataSize,
                           &changed, &failed);
    }

cleanup:
    free(blockDec);
    free(blockDecOrig);
    free(v5enc);
    free(v5encPatched);

    // 需要补但没补上（警告已经弹过了）：用退出码 3 通知 loader，
    // 免得被当成「无需处理」而掩盖掉一次失败。
    if (failed) {
        OutputDebugStringW(L"[AntiMalform] patch failed, exit code 3");
        ExitProcess(3);
    }

    // 这个 exe 已经不需要补丁——典型情况就是上一趟产出的 _crack.exe 又被拖回来了。
    // 用退出码 2 告诉 loader「没改到东西」，让它直接进主界面，
    // 不要再提示「把它拖到本程序上继续」，否则会一直套娃下去。
    if (!changed) {
        OutputDebugStringW(L"[AntiMalform] nothing to patch, exit code 2");
        ExitProcess(2);
    }

    if (ok) {
        OutputDebugStringW(L"[AntiMalform] _crack.exe created");
        ExitProcess(0);
    }
}

// 挂钩回调
static void* __cdecl HookCallback(int a1, int a2, int a3) {
    void* result = g_originalFn(a1, a2, a3);

    // 外层对象/结果都可能为空（挂错目标、引擎版本不同），先做合理 guard，
    // 不要把 0 当指针解引用。
    if (result == nullptr || a2 == 0)
        return result;

    const DWORD holder = *(DWORD*)a2;
    if (holder == 0)
        return result;

    const wchar_t* scriptPath = *(const wchar_t**)(holder + 4);
    if (!scriptPath)
        scriptPath = (const wchar_t*)(holder + 8);
    if (!scriptPath)
        return result;

    if (wcsstr(scriptPath, L"startup.tjs")) {
        PatchAndDump((TjsStream*)result);
    }
    return result;
}

// 对外接口
} // namespace

bool InstallRuntimeHook() {
    void* target = FindHookTarget();
    if (!target) {
        OutputDebugStringW(L"[AntiMalform] Hook target not found");
        return false;
    }
    g_originalFn = (HookTargetFn)target;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(&(PVOID&)g_originalFn, HookCallback);
    LONG err = DetourTransactionCommit();
    if (err == NO_ERROR) { g_hookInstalled = true; return true; }
    return false;
}

void RemoveRuntimeHook() {
    if (!g_hookInstalled) return;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourDetach(&(PVOID&)g_originalFn, HookCallback);
    DetourTransactionCommit();
    g_hookInstalled = false;
}
