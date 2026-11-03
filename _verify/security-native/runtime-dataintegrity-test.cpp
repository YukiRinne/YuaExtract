// AntiMalform 运行时 PatchAndDump 的数据完整性回归（headless、合成流）。
//
// 直接 include 生产实现的 runtime_hook.cpp，用假 vtable 驱动真实函数：
//   - 短读（第一遍/第二遍）必须立即放弃，不能拿未初始化尾部当字节码；
//   - Seek 失败同样放弃；
//   - 无论哪条退出路径，流的 writeTarget 必须恢复到原值；
//   - 没有真正改动解密态时不得把整段数据强行写回流（子进程 + 标记文件验证）。
//
// 不注入真实进程、不启动游戏。用法：
//   runtime-dataintegrity-test.exe            （父进程：跑内存用例 + 子进程用例）
//   runtime-dataintegrity-test.exe --child-unmodified <marker>

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../../CxdecAntiMalform/runtime_hook.cpp"

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

struct FakeState {
    uint64_t size = 64;
    uint32_t readCalls = 0;
    uint32_t firstRead = 64;
    uint32_t secondRead = 64;
    int64_t seekResult = 0;
    uint32_t writeCalls = 0;
    bool shortWrite = false;
    const wchar_t* markerPath = nullptr;
};

FakeState g_fake;

int64_t __cdecl FakeSeek(void*, int64_t, int) { return g_fake.seekResult; }

uint32_t __cdecl FakeRead(void*, void* buffer, uint32_t size) {
    ++g_fake.readCalls;
    const uint32_t want = (g_fake.readCalls == 1) ? g_fake.firstRead : g_fake.secondRead;
    const uint32_t got = want < size ? want : size;
    if (got > 0 && buffer != nullptr) std::memset(buffer, 0, got);
    return got;
}

uint32_t __cdecl FakeWrite(void*, const void* buffer, uint32_t size, int) {
    ++g_fake.writeCalls;
    if (g_fake.markerPath != nullptr) {
        FILE* f = nullptr;
        _wfopen_s(&f, g_fake.markerPath, L"wb");
        if (f != nullptr) {
            fwrite(buffer != nullptr ? buffer : "", 1, size, f);
            fclose(f);
        }
    }
    if (g_fake.shortWrite && size > 0) return size - 1;
    return size;
}

void __cdecl FakeSetEnd(void*) {}

uint64_t __cdecl FakeGetSize(void*) { return g_fake.size; }

struct StreamPair {
    TjsStreamVtbl vtable{};
    TjsStream writeTarget{};
    TjsStream stream{};

    StreamPair() {
        vtable.Seek = FakeSeek;
        vtable.Read = FakeRead;
        vtable.Write = FakeWrite;
        vtable.SetEndOfStorage = FakeSetEnd;
        vtable.GetSize = FakeGetSize;
        stream.vtable = &vtable;
        writeTarget.vtable = &vtable;
        stream.writeTarget = &writeTarget;
    }
};

void ResetFake() { g_fake = FakeState{}; }

bool RunInProcessCase(const char* name, uint64_t size, uint32_t first, uint32_t second,
                      int64_t seekResult) {
    ResetFake();
    g_fake.size = size;
    g_fake.firstRead = first;
    g_fake.secondRead = second;
    g_fake.seekResult = seekResult;

    StreamPair pair;
    TjsStream* expectedTarget = pair.stream.writeTarget;

    PatchAndDump(&pair.stream);

    const bool targetRestored = pair.stream.writeTarget == expectedTarget;
    const bool noWriteBack = g_fake.writeCalls == 0;
    std::printf("  [%s] readCalls=%u writeCalls=%u targetRestored=%d\n", name,
                g_fake.readCalls, g_fake.writeCalls, targetRestored);
    return targetRestored && noWriteBack;
}

std::wstring Quote(const std::wstring& s) {
    std::wstring out = L"\"";
    for (wchar_t ch : s) {
        if (ch == L'"') out += L'\\';
        out += ch;
    }
    out += L"\"";
    return out;
}

int RunChildUnmodified(const wchar_t* markerPath) {
    ResetFake();
    g_fake.size = 64;
    g_fake.markerPath = markerPath;
    StreamPair pair;
    // 全零、长度足够：TjsPatcher 不会把它当成有效 TJS2 字节码，modified=false。
    PatchAndDump(&pair.stream);
    // 正常情况下 PatchAndDump 会因 changed=false 调用 ExitProcess(2)，不会走到这里。
    return 0;
}

int RunParent() {
    // 1. 第一遍短读
    Check(RunInProcessCase("short-first-read", 64, 16, 64, 0),
          "short first read aborts, writeTarget restored, no write-back");
    // 2. 第二遍短读
    Check(RunInProcessCase("short-second-read", 64, 64, 8, 0),
          "short second read aborts, writeTarget restored, no write-back");
    // 3. 第一遍 Seek 失败
    Check(RunInProcessCase("seek-first-fail", 64, 64, 64, -1),
          "seek failure on first pass aborts cleanly");
    // 4. 第二遍 Seek 失败（writeTarget 已被置空过，必须恢复）
    {
        ResetFake();
        g_fake.size = 64;
        g_fake.firstRead = 64;
        g_fake.secondRead = 64;
        StreamPair pair;
        TjsStream* expected = pair.stream.writeTarget;
        // 用全局标志让 FakeSeek 第二次返回 -1
        static int seekCalls = 0;
        seekCalls = 0;
        struct Local {
            static int64_t __cdecl Seek(void*, int64_t, int) {
                ++seekCalls;
                return seekCalls >= 2 ? -1 : 0;
            }
        };
        pair.vtable.Seek = Local::Seek;
        PatchAndDump(&pair.stream);
        Check(pair.stream.writeTarget == expected && g_fake.writeCalls == 0,
              "seek failure after cipher read restores writeTarget, no write-back");
    }
    // 5. 超大尺寸直接拒绝
    Check(RunInProcessCase("oversize", 10ull * 1024 * 1024 + 1, 0, 0, 0),
          "oversize stream rejected before allocation");

    // 5b. 生产写回入口：短写必须返回 false，写满才返回 true。
    {
        ResetFake();
        StreamPair pair;
        uint8_t payload[8] = {};
        Check(WriteBackToStream(&pair.writeTarget, payload, sizeof(payload)),
              "WriteBackToStream accepts exact write");
        g_fake.shortWrite = true;
        Check(!WriteBackToStream(&pair.writeTarget, payload, sizeof(payload)),
              "WriteBackToStream rejects short write");
        g_fake.shortWrite = false;
        Check(!WriteBackToStream(nullptr, payload, sizeof(payload)),
              "WriteBackToStream rejects null target");
    }

    // 6. 子进程：未修改时不得 write-back
    {
        wchar_t self[MAX_PATH] = {};
        ::GetModuleFileNameW(nullptr, self, _countof(self));
        wchar_t temp[MAX_PATH] = {};
        ::GetTempPathW(_countof(temp), temp);
        const std::wstring marker = std::wstring(temp) + L"yua-antimalform-noback-" +
                                    std::to_wstring(::GetCurrentProcessId()) + L".bin";
        ::DeleteFileW(marker.c_str());

        std::wstring cmd = Quote(self) + L" --child-unmodified " + Quote(marker);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        const BOOL started = ::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0,
                                              nullptr, nullptr, &si, &pi);
        Check(started != FALSE, "child process started for unmodified write-back test");
        if (started) {
            ::WaitForSingleObject(pi.hProcess, 30000);
            DWORD code = 0xFFFFFFFFu;
            ::GetExitCodeProcess(pi.hProcess, &code);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            // 注意：本测试 exe 自己编进了生产源码，二进制里天然含有
            // "steam=\"yes\"" 字面量，PatchAndWrite 会把它当成 Steam 壳处理，
            // 所以退出码可能是 0（生成了 _crack.exe）或 2，不能拿来当判据。
            // 这里要验的是**流没有被写回**：FakeWrite 没被调用（marker 不存在）。
            Check(code == 0 || code == 2, "child completed patch pipeline");
            Check(::GetFileAttributesW(marker.c_str()) == INVALID_FILE_ATTRIBUTES,
                  "unmodified stream was not written back");
            ::DeleteFileW(marker.c_str());
            // PatchAndWrite 可能因测试 exe 自带 steam 字面量而生成 _crack.exe，清掉。
            std::wstring selfStem = self;
            const size_t dot = selfStem.find_last_of(L'.');
            if (dot != std::wstring::npos) selfStem.resize(dot);
            ::DeleteFileW((selfStem + L"_crack.exe").c_str());
        }
    }

    std::printf("RESULT failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && std::wstring(argv[1]) == L"--child-unmodified") {
        return RunChildUnmodified(argv[2]);
    }
    return RunParent();
}
