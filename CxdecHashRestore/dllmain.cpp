#include <windows.h>

#include "HashCrackApplication.h"
#include "HashRestoreUI.h"
#include "path.h"
#include "util.h"

namespace
{
    bool IsRestoreUiSuppressed()
    {
        wchar_t value[16]{};
        DWORD length = ::GetEnvironmentVariableW(L"CXDEC_HASH_CRACK_SUPPRESS_RESTORE_UI", value, _countof(value));
        return length > 0 && value[0] == L'1';
    }

    // hash 还原模块会挂 GetProcAddress 钩子并起后台线程。卸载后这些 hook 会悬空，
    // 所以加载时先把自己钉在进程生命周期里；失败就拒绝初始化。
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

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
    UNREFERENCED_PARAMETER(reserved);

    if (reason == DLL_PROCESS_ATTACH)
    {
        ::DisableThreadLibraryCalls(module);
        if (!PinSelfForProcessLifetime())
        {
            return FALSE;
        }
        ::SetEnvironmentVariableW(L"CXDEC_HASH_CRACK_MODE", L"1");
        Engine::HashCrackApplication::Initialize(module);
        if (!IsRestoreUiSuppressed())
        {
            std::wstring gameDirectory = Path::GetDirectoryName(Util::GetModulePathW(::GetModuleHandleW(nullptr)));
            Engine::HashRestoreUI::Start(module, gameDirectory);
        }
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // 已 pin：不按可动态卸载处理，只做常规收尾。
        Engine::HashCrackApplication::Release();
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void Dummy() {}
