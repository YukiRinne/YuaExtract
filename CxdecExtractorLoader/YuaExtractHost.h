#pragma once

// YuaExtract 新主界面与上游 Loader 实现之间的桥。
//
// 上游那一大堆 helper（浏览 exe、校验、定位模块 DLL、启动并注入、静态取 Key、
// 原版功能窗口……）都在 CxdecExtractorLoader.cpp 的匿名命名空间里，外部看不见。
// 与其把那个文件拆开重排（改动面大、回归风险高），不如在这里声明一层窄接口，
// 由 CxdecExtractorLoader.cpp 末尾实现转发。
//
// 新界面（YuaExtractMainWindow.cpp）只依赖本头文件。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <string>

#include "log.h"

namespace Yua
{
namespace Host
{
namespace Detail
{
    // 有界等待一个句柄，同时保持 owner 窗口可重绘。
    //
    // 约束（预检 / 注入路径共用，headless 测试也直接调这一份）：
    //   - 只从消息队列里取 WM_PAINT / WM_TIMER 并派发，其余消息原样留在队列；
    //   - WM_CLOSE 只用 PM_NOREMOVE 探测，登记 cancel 后立刻返回，不删除消息对象；
    //   - WM_QUIT 同样只探测，不 Consume —— 外层消息循环仍能正常退出；
    //   - 重入调用退化为纯 WaitForSingleObject，避免嵌套泵消息；
    //   - 超时 / 用户取消 / owner 失效都返回 false，调用方只清理自己新建的 child。
    inline bool WaitForHandleResponsive(HWND owner, HANDLE handle, ULONGLONG deadline,
                                        bool& cancelRequested)
    {
        cancelRequested = false;
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        static volatile LONG reentry = 0;
        if (::InterlockedCompareExchange(&reentry, 1, 0) != 0)
        {
            const ULONGLONG now = ::GetTickCount64();
            const DWORD remaining = deadline > now ? static_cast<DWORD>(deadline - now) : 0u;
            return ::WaitForSingleObject(handle, remaining) == WAIT_OBJECT_0;
        }
        struct ReentryGuard
        {
            volatile LONG* Flag;
            ~ReentryGuard() { ::InterlockedExchange(Flag, 0); }
        } guard{ &reentry };

        for (;;)
        {
            if (owner && !::IsWindow(owner))
            {
                cancelRequested = true;
                return false;
            }
            MSG probe{};
            // 只探测、不移除：WM_CLOSE 仍归原消息循环，预检只负责安全中止。
            if (owner && ::PeekMessageW(&probe, owner, WM_CLOSE, WM_CLOSE, PM_NOREMOVE))
            {
                cancelRequested = true;
                return false;
            }
            // WM_QUIT 是线程消息：不 Consume，等外层循环退出时再处理。
            if (::PeekMessageW(&probe, nullptr, WM_QUIT, WM_QUIT, PM_NOREMOVE))
            {
                cancelRequested = true;
                return false;
            }

            const ULONGLONG now = ::GetTickCount64();
            if (now >= deadline)
            {
                return false;
            }
            const ULONGLONG remaining = deadline - now;
            const DWORD slice = remaining > 100ull ? 100u : static_cast<DWORD>(remaining);
            const DWORD wait =
                ::MsgWaitForMultipleObjects(1, &handle, FALSE, slice, QS_PAINT | QS_TIMER);
            if (wait == WAIT_OBJECT_0)
            {
                return true;
            }
            if (wait == WAIT_FAILED)
            {
                return false;
            }

            MSG message{};
            while (::PeekMessageW(&message, nullptr, WM_PAINT, WM_PAINT, PM_REMOVE))
            {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            while (::PeekMessageW(&message, nullptr, WM_TIMER, WM_TIMER, PM_REMOVE))
            {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            // 其它消息（输入 / 完成通知等）留在队列，不吞、不派发业务重入。
        }
    }
}

    // Loader 自己的会话日志文件名（工具目录\Log\ 下）。
    // 上游叫 CxdecExtractorLoader.log；既然产品改名为 YuaExtract，日志也跟着改，
    // 界面上的「显示详细日志」读的就是这个文件。
    constexpr const wchar_t* kLogFileName = L"YuaExtract.log";

    // 界面标题与版本
    constexpr const wchar_t* kProductName = L"YuaExtract";
    constexpr const wchar_t* kProductVersion = L"1.0.0";

    std::wstring LoaderDirectory();
    std::wstring LogDirectory();

    std::wstring ResolveModulePath(const std::wstring& dllFileName);
    bool ModuleExists(const std::wstring& dllFileName);
    // 拼一句"缺哪个 DLL、该放哪儿"的提示，几个入口共用。
    std::wstring ModuleMissingHint(const std::wstring& dllFileName);

    std::wstring TargetExe();
    std::wstring TargetDirectory();
    void SetTargetExe(const std::wstring& exePath);
    bool ValidateGameExe(HWND owner, const std::wstring& path);
    bool PickGameExe(HWND owner, std::wstring& chosen);
    std::wstring BrowseForFolder(HWND owner, const wchar_t* title);

    // 启动游戏并注入模块。
    //   runtimeHashTargetDirectory 非空时写进 CXDEC_RUNTIME_HASH_TARGET_DIR，
    //   CxdecStringDumper 会用它预填"待恢复目录"。
    // 只负责"起进程 + 注入"，不关闭本窗口，也不接管进度回调。
    bool LaunchGameWithModule(HWND owner, const std::wstring& dllFileName,
                              const std::wstring& runtimeHashTargetDirectory, std::wstring& error);

    // 选定目标后触发上游的 SteamStub 检测/反篡改补丁流程。
    // 状态是明确的：调用方不得再把"没产出 _crack.exe"当成可继续。
    enum class PrepareStatus
    {
        Ready = 0,    // 无需补丁（原本就没壳 / 已经补过），可以按原 exe 继续启动
        Patched = 1,  // 本次确实产出了新的 _crack.exe，应改用 CrackedExePath
        Cancelled = 2,// 用户在脱壳确认处取消，本次启动应中止
        Failed = 3    // 检测/脱壳/注入/补丁失败，本次启动应中止并报 error
    };

    PrepareStatus PrepareTargetExe(HWND owner, const std::wstring& exePath, std::wstring& error);

    // 反篡改补丁流程产出的成品：<exePath 去掉扩展名>_crack.exe（存在才返回，否则空串）。
    // 调用方必须结合 PrepareStatus::Patched 使用，不能把上一次留下的旧文件当成本次成功。
    std::wstring CrackedExePath(const std::wstring& exePath);

    // 上游原版功能窗口（解包 / 运行时恢复 / Hook 撞库 / 提取Key / 封包）。
    void ShowLegacyFunctionsWindow(HWND owner);
    // 直接触发某一项原版功能（等价于在那个窗口里点对应按钮），不关闭主窗口。
    // action：extractor / dumper / hashRestore / keyStatic / repack。
    // 返回 true = 已经派发（对话框自己的流程接管后续）；false = action 未知或窗口没建起来。
    bool RunLegacyAction(HWND owner, const std::wstring& action, std::wstring& errorOut);
    // 静态 Key 提取（等价于原版窗口里的「提取Key」）。
    // 会在执行前明确确认"将运行游戏内嵌原生代码"；返回 true = 用户确认且提取成功。
    bool RunStaticKeyExtractionForTarget(HWND owner);

    void EnableDropOnWindow(HWND hwnd);

    void LogLine(Log::Level level, const std::wstring& text);

    // 日志镜像：每写一行日志就额外回调一次（WebView2 桥用它把 log 事件推给界面）。
    // 只允许一个接收者；传 nullptr 摘掉。回调可能在**任意线程**被调用，
    // 接收方必须自己保证线程安全（桥内部只做加锁入队 + PostMessage）。
    using LogSink = void (*)(Log::Level level, const std::wstring& text);
    void SetLogSink(LogSink sink);

    std::wstring Format(const wchar_t* format, ...);

    bool Confirm(HWND owner, const std::wstring& text, const std::wstring& caption);
    void ShowInfo(HWND owner, const std::wstring& text);
    void ShowError(HWND owner, const std::wstring& text);
    bool OpenInExplorer(HWND owner, const std::wstring& path);
    bool OpenWithShell(HWND owner, const std::wstring& path);
}
}
