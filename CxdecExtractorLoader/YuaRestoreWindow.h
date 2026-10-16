#pragma once

// YuaExtract 步骤3「资源名还原」独立窗口。
//
// 从主界面「步骤3」按钮打开。窗口自己按"一个封包一个任务"排队，用 N 个工作线程
// 并发还原，全部状态（任务终态、进度、日志）都在本窗口内部维护，跑完写
// Restored_Extractor_Output\RestoreReport.txt —— 主界面的"还原结果"概览读的就是
// 那份报告，所以两边不会各算一套数字。
//
// 之所以独立成窗口而不是塞进主界面：还原是一个可能跑几十分钟、有 N 个并发任务、
// 需要逐条日志的操作，塞在"主流程"面板里既看不清也没法中途查看。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <string>
#include <vector>

namespace Yua
{
    namespace Restore
    {
        struct WindowOptions
        {
            std::wstring GameDirectory;     // 工作区（游戏目录），仅用于标题/日志
            std::wstring SourceDirectory;   // Extractor_Output
            std::wstring HashDirectory;     // StringHashDumper_Output
            std::wstring DefaultOutputDirectory; // Restored_Extractor_Output
            std::wstring LogDirectory;      // 工具目录\Log
            HWND Owner = nullptr;
        };

        // 还原窗口的窗口类名。主界面按类名查找它来判断"还原窗口是不是还开着"。
        constexpr const wchar_t* kRestoreWindowClass = L"YuaExtractRestoreWindow";

        // 打开（或前置已有的）还原窗口。同一时间只允许一个，重复点击只是把它拉到前面。
        // 非阻塞：窗口自己管自己的消息，主界面消息循环照常跑。返回窗口句柄（失败为空）。
        HWND ShowRestoreWindow(HINSTANCE instance, const WindowOptions& options);

        // 请求关闭还原窗口（主界面退出时调用，会在关窗前等一下工作线程）。
        void CloseRestoreWindow(HWND window);

        // 还原窗口是否正在跑任务。主界面的「清理目录」要先问一句，
        // 免得把正在写输出的目录删掉。
        bool IsRestoreRunning(HWND window);

        // 还原窗口用来跟主界面报信的私有消息：
        //   wParam = 状态码（见 RestoreTaskState），lParam = 当前累计进度百分比。
        // 主界面收到后刷新概览即可 —— 还原窗口不直接改主界面的控件。
        constexpr UINT kRestoreNotifyMessage = WM_APP + 0x5A0;
        enum RestoreTaskState : unsigned int
        {
            kStateQueued = 0,
            kStateRunning = 1,
            kStateCompleted = 2,
            kStateFailed = 3
        };
    }
}
