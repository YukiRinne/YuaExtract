#include "Application.h"

#pragma comment(linker, "/MERGE:\".detourd=.data\"")
#pragma comment(linker, "/MERGE:\".detourc=.rdata\"")

namespace
{
    // 运行时 hook 模块：在挂 GetProcAddress 钩子之前把自己钉在进程里。
    // 一旦被 FreeLibrary 卸掉，已有的 inline hook 会跳到未映射地址。
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

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    UNREFERENCED_PARAMETER(lpReserved);
    switch (ul_reason_for_call)
    {
        case DLL_PROCESS_ATTACH:
        {
            ::DisableThreadLibraryCalls(hModule);
            if (!PinSelfForProcessLifetime())
            {
                return FALSE;  // 不带着会失效的 hook 继续跑
            }
            Engine::Application::Initialize(hModule);
            break;
        }
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;
        case DLL_PROCESS_DETACH:
        {
            // 已 pin：本模块不按可动态卸载处理，这里只做常规收尾。
            Engine::Application::Release();
            break;
        }
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void Dummy() {}
