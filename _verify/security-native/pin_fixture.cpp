// 自造的"进程生命周期 hook 模块"：DllMain 里用 FROM_ADDRESS|PIN 钉住自己。
// 与生产运行库（KeyDumper/StringDumper/HashRestore/AntiMalform）用的是同一套语义。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace {
bool PinSelf() {
    HMODULE pinned = nullptr;
    return ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                    GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&PinSelf), &pinned) != FALSE &&
           pinned != nullptr;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(module);
        if (!PinSelf()) return FALSE;
    }
    return TRUE;
}

extern "C" __declspec(dllexport) int __stdcall FixtureValue() { return 42; }

#pragma comment(linker, "/EXPORT:FixtureValue=_FixtureValue@0")
