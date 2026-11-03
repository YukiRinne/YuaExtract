// Headless 测试：还原窗口的停止/队列语义（直接编入生产 YuaRestoreWindow.cpp）。
//
// 只验证不需要真实窗口、不需要还原内核的行为：
//   - PopNextTask 按 FIFO 取任务；
//   - StopEvent 置位后不再取任务，并把排队任务清空（"停止后不开始后续封包"）；
//   - PublishProgress / PublishTerminalState 在 Window == nullptr（无 HWND）时不崩、
//     不 SendMessage，消息投递失败时自行释放载荷；
//   - 工作线程体只依赖 queue/stop 事件，不依赖同步控件调用（源码路径）。
//
// HashNameRestorer 的内核函数用桩替代：本测试不跑真实还原、不碰用户目录。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdio>
#include <string>

#include "YuaExtractHost.h"
#include "YuaRestoreWindow.h"

namespace Yua
{
namespace Host
{
    std::wstring LoaderDirectory() { return L"."; }
    std::wstring LogDirectory() { return L"."; }
    std::wstring ResolveModulePath(const std::wstring&) { return std::wstring(); }
    bool ModuleExists(const std::wstring&) { return false; }
    std::wstring ModuleMissingHint(const std::wstring&) { return std::wstring(); }
    std::wstring TargetExe() { return std::wstring(); }
    std::wstring TargetDirectory() { return std::wstring(); }
    void SetTargetExe(const std::wstring&) {}
    bool ValidateGameExe(HWND, const std::wstring&) { return false; }
    bool PickGameExe(HWND, std::wstring&) { return false; }
    std::wstring BrowseForFolder(HWND, const wchar_t*) { return std::wstring(); }
    bool LaunchGameWithModule(HWND, const std::wstring&, const std::wstring&, std::wstring&) { return false; }
    PrepareStatus PrepareTargetExe(HWND, const std::wstring&, std::wstring&) { return PrepareStatus::Ready; }
    std::wstring CrackedExePath(const std::wstring&) { return std::wstring(); }
    void ShowLegacyFunctionsWindow(HWND) {}
    bool RunStaticKeyExtractionForTarget(HWND) { return false; }
    void EnableDropOnWindow(HWND) {}
    void LogLine(Log::Level, const std::wstring&) {}
    std::wstring Format(const wchar_t*, ...) { return std::wstring(); }
    bool Confirm(HWND, const std::wstring&, const std::wstring&) { return false; }
    void ShowInfo(HWND, const std::wstring&) {}
    void ShowError(HWND, const std::wstring&) {}
    bool OpenInExplorer(HWND, const std::wstring&) { return false; }
    bool OpenWithShell(HWND, const std::wstring&) { return false; }
}
}

// 生产实现。
#include "../../CxdecExtractorLoader/YuaRestoreWindow.cpp"

namespace Yua
{
    // ---- HashNameRestorer 内核桩（本测试只测 UI 队列语义，不跑真实还原） ----
    HashMapTables LoadHashMapTables(const std::wstring&) { return HashMapTables{}; }
    std::vector<RestorePackageTask> EnumerateRestorePackages(const std::wstring&)
    {
        return std::vector<RestorePackageTask>{};
    }
    RestorePackageResult RestoreSinglePackage(const RestorePackageTask&,
                                              const HashMapTables&,
                                              const RestoreOptions&)
    {
        return RestorePackageResult{};
    }
    SupplementOutcome RunSupplementRestore(HashMapTables&, const SupplementOptions&,
                                           const std::function<void(const SupplementProgress&)>&)
    {
        return SupplementOutcome{};
    }
    RestoreTotals SumRestoreResults(const std::vector<RestorePackageResult>&)
    {
        return RestoreTotals{};
    }
    unsigned int ComputeTaskProgressPercent(RestoreProgressStep, unsigned int)
    {
        return 0u;
    }
    bool WriteRestoreReport(const RestoreReportData&, std::wstring& error)
    {
        error = L"stub";
        return false;
    }
}

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* what)
    {
        std::printf(condition ? "PASS %s\n" : "FAIL %s\n", what);
        if (!condition)
        {
            ++g_failures;
        }
    }
}

namespace Yua
{
namespace Restore
{
    int RunRestoreStopChecks()
    {
        UiContext context{};
        ::InitializeCriticalSection(&context.QueueLock);
        context.StopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        context.QueueEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!context.StopEvent || !context.QueueEvent)
        {
            Check(false, "create restore queue events");
            return 1;
        }

        context.Queue = { 11u, 12u, 13u };
        unsigned int taskId = 0u;
        Check(PopNextTask(&context, taskId) && taskId == 11u, "PopNextTask returns FIFO head");
        Check(!StopRequested(&context), "restore queue is not stopped initially");

        // 无 HWND 的进度/终态写入：PostMessage 失败必须自己释放载荷，且只改任务记录。
        {
            TaskRecord task;
            task.Id = 12u;
            task.State = kStateQueued;
            task.PackageName = L"pkg";
            context.Tasks.push_back(task);
        }
        PublishProgress(&context, 12u, RestoreProgressStep::Restoring, 50u, L"file");
        {
            bool running = false;
            unsigned int percent = 0u;
            {
                ScopedCriticalSection guard(&context.QueueLock);
                running = context.Tasks[0].State == kStateRunning;
                percent = context.Tasks[0].Percent;
            }
            Check(running && percent == 50u, "PublishProgress updates task without a window handle");
        }
        RestorePackageResult result;
        result.packageName = L"pkg";
        PublishTerminalState(&context, 12u, kStateCompleted, result, 5u);
        {
            bool finished = false;
            {
                ScopedCriticalSection guard(&context.QueueLock);
                finished = context.Tasks[0].Finished &&
                           context.Tasks[0].State == kStateCompleted;
            }
            Check(finished, "PublishTerminalState writes terminal state without a window handle");
        }

        // 停止后不再取后续封包，并清空排队项。
        ::SetEvent(context.StopEvent);
        Check(StopRequested(&context), "StopEvent makes StopRequested true");
        unsigned int ignored = 0u;
        Check(!PopNextTask(&context, ignored), "PopNextTask refuses work after stop");
        Check(context.Queue.empty(), "stop clears the remaining queued packages");

        ::CloseHandle(context.QueueEvent);
        ::CloseHandle(context.StopEvent);
        ::DeleteCriticalSection(&context.QueueLock);
        return 0;
    }
}
}

int wmain()
{
    g_failures += Yua::Restore::RunRestoreStopChecks();
    std::printf("RESULT %s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
