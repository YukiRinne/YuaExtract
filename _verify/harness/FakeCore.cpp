// 验证用假引擎（不是产品的一部分，只在 _verify\harness 下构建）。
//
// 目的：在不启动游戏的前提下，驱动真实的 CxdecExtractorUI.dll 走完它的进度投递与
// 收尾逻辑。它复刻用户那次出问题的规模 —— voice2.xp3 的 7097 个条目、每项两条更新
// （共 14196 条），这正是把界面消息队列打满、丢掉收尾状态的那次。
//
// 环境变量：
//   YUA_FAKE_SKIP_COMPLETED=1  故意不发 ExtractTaskCompleted，
//                              用来验证"解包器的收尾通知丢了，界面还能不能收尾"
//                              （即工作线程的兜底是否生效）。
//
// 只导出 UI 需要的两个接口，名字与 CxdecExtractor.dll 一致。

#include <Windows.h>

#include <string>

#include "ExtractApi.h"

namespace
{
    Engine::tExtractProgressCallback g_callback = nullptr;
    void* g_context = nullptr;

    // 与用户那次 voice2.xp3 的条目数一致。
    constexpr unsigned int kEntries = 7097u;

    bool SkipCompletedNotification()
    {
        return ::GetEnvironmentVariableW(L"YUA_FAKE_SKIP_COMPLETED", nullptr, 0u) > 0u;
    }
}

extern "C" __declspec(dllexport) void WINAPI SetExtractProgressCallback(
    Engine::tExtractProgressCallback callback, void* context)
{
    g_callback = callback;
    g_context = context;
}

extern "C" __declspec(dllexport) BOOL WINAPI ExtractPackageEx(const wchar_t* packagePath,
                                                              const wchar_t* outputDirectory,
                                                              unsigned int taskId)
{
    UNREFERENCED_PARAMETER(outputDirectory);

    if (g_callback == nullptr)
    {
        return TRUE;
    }

    g_callback(taskId, packagePath, Engine::ExtractTaskPreparing, 0u, 0u, L"准备解包", g_context);
    g_callback(taskId, packagePath, Engine::ExtractTaskIndexLoaded, 0u, kEntries, L"索引读取完成", g_context);

    std::wstring detail;
    for (unsigned int index = 0u; index < kEntries; ++index)
    {
        detail = L"voice2\\94D4A97C61498621\\" + std::to_wstring(index);
        g_callback(taskId, packagePath, Engine::ExtractTaskExtracting, index, kEntries, detail.c_str(), g_context);
        g_callback(taskId, packagePath, Engine::ExtractTaskExtracting, index + 1u, kEntries, detail.c_str(), g_context);
    }

    if (!SkipCompletedNotification())
    {
        g_callback(taskId, packagePath, Engine::ExtractTaskCompleted, kEntries, kEntries, L"解包完成", g_context);
    }

    return TRUE;
}

// 导出表里同时放无装饰名，UI 是按 "ExtractPackageEx" 这种名字 GetProcAddress 的。
#pragma comment(linker, "/EXPORT:ExtractPackageEx=_ExtractPackageEx@12")
#pragma comment(linker, "/EXPORT:SetExtractProgressCallback=_SetExtractProgressCallback@8")
