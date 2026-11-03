#include "Application.h"

#pragma comment(linker, "/MERGE:\".detourd=.data\"")
#pragma comment(linker, "/MERGE:\".detourc=.rdata\"")

namespace
{
    // 这些运行时模块装的是**进程生命周期**的 inline hook / 后台线程：
    // 一旦被 FreeLibrary 卸掉，已写进目标函数的跳板就指向未映射的地址。
    // 所以在安装任何 hook、创建任何线程之前，先用 FROM_ADDRESS|PIN 把自己钉住。
    // 失败就拒绝初始化（DllMain 返回 FALSE），而不是带着定时炸弹继续跑。
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
            // 钉住之后本模块不再支持动态卸载，只随进程退出；这是 hook 模块的正确契约。
            if (!PinSelfForProcessLifetime())
            {
                return FALSE;
            }
            Engine::Application::Initialize(hModule);
            break;
        }
        case DLL_THREAD_ATTACH:
        case DLL_THREAD_DETACH:
            break;
        case DLL_PROCESS_DETACH:
        {
            // 进程生命周期模块：不在这里 FreeLibrary/卸 hook，避免 loader lock 死锁。
            Engine::Application::Release();
            break;
        }
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void Dummy() {}
