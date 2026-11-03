// pin-host：验证"hook 模块在 DllMain 里被 PIN 住之后，FreeLibrary 不会卸载它"。
//
// 这是机制级回归：加载的是本目录自造的两个小 DLL（一个 PIN、一个不 PIN），
// 不加载生产运行库、不注入任何真实进程。生产模块是否调用 PIN 由源码审计 +
// 各模块编译通过确认；这里验证的是同一套 Win32 语义。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstdio>
#include <string>

namespace {

int g_failures = 0;

void Check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) ++g_failures;
}

bool ModuleLoaded(const wchar_t* name) {
    return ::GetModuleHandleW(name) != nullptr;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::printf("usage: pin-host <pin_fixture.dll> <unpinned_fixture.dll>\n");
        return 2;
    }

    // ---- 1. 会 PIN 自己的模块 ----
    {
        HMODULE pinned = ::LoadLibraryW(argv[1]);
        Check(pinned != nullptr, "pinned fixture loaded");
        if (pinned != nullptr) {
            using ValueFn = int(__stdcall*)();
            auto value = reinterpret_cast<ValueFn>(::GetProcAddress(pinned, "FixtureValue"));
            Check(value != nullptr && value() == 42, "pinned fixture executes");
            ::FreeLibrary(pinned);
            Check(ModuleLoaded(L"pin_fixture.dll"),
                  "pinned fixture remains loaded after FreeLibrary");
        }
    }

    // ---- 2. 不 PIN 的对照模块：FreeLibrary 后应卸载 ----
    {
        HMODULE unpinned = ::LoadLibraryW(argv[2]);
        Check(unpinned != nullptr, "unpinned fixture loaded");
        if (unpinned != nullptr) {
            using ValueFn = int(__stdcall*)();
            auto value = reinterpret_cast<ValueFn>(::GetProcAddress(unpinned, "FixtureValue"));
            Check(value != nullptr && value() == 42, "unpinned fixture executes");
            ::FreeLibrary(unpinned);
            Check(!ModuleLoaded(L"unpinned_fixture.dll"),
                  "unpinned fixture unloads after FreeLibrary (control)");
        }
    }

    std::printf("RESULT failures=%d\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
