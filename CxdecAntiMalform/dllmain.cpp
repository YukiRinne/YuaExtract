#include <windows.h>
#include "detour_finder.h"
#include "runtime_hook.h"

namespace
{
    // AntiMalform 会改写自己映像里的载荷并在目标进程里装 inline hook。
    // 一旦模块被 FreeLibrary 卸掉，补丁/钩子全部悬空，所以加载时先 PIN 住。
    // 失败就拒绝加载，而不是"先 Hook 着、以后再被卸"。
    bool PinSelfForProcessLifetime()
    {
        HMODULE pinned = nullptr;
        return ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                        GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    reinterpret_cast<LPCWSTR>(&PinSelfForProcessLifetime),
                                    &pinned) != FALSE &&
               pinned != nullptr;
    }
}

BOOL APIENTRY DllMain(HMODULE hMod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hMod);
        OutputDebugStringW(L"[AntiMalform] Init");

        // 必须在装 detour / 起重试线程之前钉住自己。
        if (!PinSelfForProcessLifetime()) {
            OutputDebugStringW(L"[AntiMalform] pin failed, refusing to hook");
            return FALSE;
        }

        // 阶段 1：应用 .detour 载荷
        const uint8_t* entry = FindDetourEntry();
        if (entry) {
            ApplyDetourPatches(entry);
        } else {
            // 放到线程里重试（注入后才会有 .detour 节）
            CreateThread(NULL, 0, [](LPVOID)->DWORD {
                for (int i = 0; i < 50; ++i) {
                    Sleep(200);
                    const uint8_t* e = FindDetourEntry();
                    if (e) { ApplyDetourPatches(e); return 0; }
                }
                return 0;
            }, NULL, 0, NULL);
        }

        // 阶段 2：装运行时挂钩
        if (InstallRuntimeHook()) {
            OutputDebugStringW(L"[AntiMalform] Hook installed");
        } else {
            OutputDebugStringW(L"[AntiMalform] Hook FAILED");
        }
    }
    return TRUE;
}
