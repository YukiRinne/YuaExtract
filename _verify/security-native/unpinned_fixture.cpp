// 对照模块：不在 DllMain 里 PIN，FreeLibrary 后应正常卸载。

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        ::DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) int __stdcall FixtureValue() { return 42; }

#pragma comment(linker, "/EXPORT:FixtureValue=_FixtureValue@0")
