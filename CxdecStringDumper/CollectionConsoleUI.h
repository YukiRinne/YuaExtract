#pragma once

#include <Windows.h>
#include <string>

namespace Engine
{
    // 步骤2「只收集」控制台。
    //
    // 与上游 RuntimeHashUI 的分工：
    //   RuntimeHashUI   —— 边收集边就地还原（改纯Hash目录里的文件名），上游回归路径，保持原样；
    //   CollectionConsoleUI —— 只把 HashCore 收集到的 名字##YSig##哈希 配对显示出来，
    //                          一个文件名都不改，还原留给步骤3 的离线流程。
    //
    // 两者互斥，由环境变量 CXDEC_HASH_COLLECT_ONLY 选择（见 IsCollectOnlyMode）。
    class CollectionConsoleUI
    {
    public:
        // 是否处于「只收集」模式。
        // 契约：Loader 在注入本 DLL 前设置 CXDEC_HASH_COLLECT_ONLY=1；这里只负责读。
        static bool IsCollectOnlyMode();

        // 开控制台窗口。窗口在自己的线程上跑，函数立刻返回，不阻塞 DllMain——
        // DllMain 里做任何等消息循环的事都会把宿主进程卡死。
        // moduleHandle：本 DLL 模块句柄；gameDirectory：宿主（游戏）exe 所在目录
        static void Start(HMODULE moduleHandle, const std::wstring& gameDirectory);
    };
}
