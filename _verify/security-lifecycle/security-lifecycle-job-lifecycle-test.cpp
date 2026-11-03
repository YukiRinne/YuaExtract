// Headless 测试：直接编入 YuaExtractMainWindow.cpp 的生产 helpers，
// 验证后台任务的生命周期约束（不复制一套算法）：
//   1) ResetJob 递增任务序号；
//   2) TryStopJobForClose 在超时时不得把线程当已结束、不得关闭句柄；
//   3) 只有线程真正退出后，才关闭句柄并释放 JobActive；
//   4) 旧 generation 的 Done 消息不得清掉当前任务的 JobActive / 句柄。
//
// 该测试不创建窗口、不启动游戏、不接触用户目录。Host / Restore 的界面接口用
// 桩函数实现，计数器只用于确认 busy 门禁会提示。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <string>

#include "YuaExtractHost.h"
#include "YuaRestoreWindow.h"

namespace Yua
{
namespace Host
{
    std::atomic<int> g_showInfoCount{ 0 };

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
    PrepareStatus PrepareTargetExe(HWND, const std::wstring&, std::wstring& error)
    {
        error.clear();
        return PrepareStatus::Ready;
    }
    std::wstring CrackedExePath(const std::wstring&) { return std::wstring(); }
    void ShowLegacyFunctionsWindow(HWND) {}
    bool RunStaticKeyExtractionForTarget(HWND) { return false; }
    void EnableDropOnWindow(HWND) {}
    void LogLine(Log::Level, const std::wstring&) {}
    std::wstring Format(const wchar_t*, ...) { return std::wstring(); }
    bool Confirm(HWND, const std::wstring&, const std::wstring&) { return false; }
    void ShowInfo(HWND, const std::wstring&) { ++g_showInfoCount; }
    void ShowError(HWND, const std::wstring&) {}
    bool OpenInExplorer(HWND, const std::wstring&) { return false; }
    bool OpenWithShell(HWND, const std::wstring&) { return false; }
}

namespace Restore
{
    HWND ShowRestoreWindow(HINSTANCE, const WindowOptions&) { return nullptr; }
    void CloseRestoreWindow(HWND) {}
    bool IsRestoreRunning(HWND) { return false; }
}
}

// 直接编入生产实现：下面的测试调用的是同一份 ResetJob / TryStopJobForClose /
// OnJobDone / JobBusy / RejectIfBusy，而不是另写一套状态机。
#include "../../CxdecExtractorLoader/YuaExtractMainWindow.cpp"

namespace
{
    int g_failures = 0;

    void Check(bool condition, const char* what)
    {
        if (condition)
        {
            std::printf("PASS %s\n", what);
        }
        else
        {
            std::printf("FAIL %s\n", what);
            ++g_failures;
        }
    }

    DWORD WINAPI SleepThread(LPVOID parameter)
    {
        ::Sleep(static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(parameter)));
        return 0;
    }
}

int wmain()
{
    Yua::UiContext context{};
    ::InitializeCriticalSection(&context.Job.Lock);
    context.Job.LockReady = true;

    // 1) 任务序号必须递增。
    Yua::ResetJob(context, Yua::JobKind::Generate, L"one");
    const unsigned int firstId = context.Job.Id;
    Yua::ResetJob(context, Yua::JobKind::DryRun, L"two");
    Check(firstId != 0u && context.Job.Id == firstId + 1u, "ResetJob increments job generation");

    // 2) 超时不得当成线程已结束。
    context.Job.Thread = ::CreateThread(nullptr, 0u, SleepThread, reinterpret_cast<LPVOID>(1200u), 0u, nullptr);
    context.JobActive = true;
    const bool stoppedEarly = Yua::TryStopJobForClose(context);
    Check(!stoppedEarly, "TryStopJobForClose returns false while worker still runs");
    Check(context.Job.Thread != nullptr, "timed-out worker handle is preserved");
    Check(context.Job.LockReady && context.JobActive, "timed-out state keeps lock and active flag");

    // 3) 线程真正退出后才允许收句柄。
    ::WaitForSingleObject(context.Job.Thread, 5000u);
    const bool stoppedAfterExit = Yua::TryStopJobForClose(context);
    Check(stoppedAfterExit, "TryStopJobForClose succeeds after worker exits");
    Check(context.Job.Thread == nullptr && !context.JobActive, "handle closed only after actual exit");

    // 4) 旧 generation 的完成消息不能影响当前任务。
    context.Job.Id = 10u;
    context.Job.Thread = ::CreateThread(nullptr, 0u, SleepThread, reinterpret_cast<LPVOID>(800u), 0u, nullptr);
    context.JobActive = true;
    Yua::OnJobDone(context, 9u);
    Check(context.JobActive && context.Job.Thread != nullptr, "stale Done message is ignored");
    ::WaitForSingleObject(context.Job.Thread, 5000u);
    Yua::TryStopJobForClose(context);

    // 5) busy 门禁：只提示，不 reset 当前任务状态。
    context.Job.Id = 42u;
    context.JobActive = true;
    const int before = Yua::Host::g_showInfoCount.load();
    const bool rejected = Yua::RejectIfBusy(context, L"测试动作");
    Check(rejected, "RejectIfBusy rejects while job active");
    Check(Yua::Host::g_showInfoCount.load() == before + 1, "RejectIfBusy notifies the user once");
    Check(context.Job.Id == 42u && context.JobActive, "RejectIfBusy does not disturb active job");

    context.JobActive = false;

    // 6) 关闭挂起 / 结论模态框期间也必须拒绝动作，但不改动任务状态。
    context.Job.Id = 43u;
    context.ClosePending = true;
    Check(Yua::RejectIfBusy(context, L"关闭等待"), "RejectIfBusy rejects while close is pending");
    Check(context.Job.Id == 43u && !context.JobActive, "close-pending gate leaves job id unchanged");
    context.ClosePending = false;
    context.JobDoneUi = true;
    Check(Yua::RejectIfBusy(context, L"结论框"), "RejectIfBusy rejects while job-done UI is modal");
    context.JobDoneUi = false;

    // 7) generation 匹配但 JobActive 已是 false 的迟到 Done：仍要做结果清理，不能直接丢掉。
    context.Job.Id = 77u;
    context.Job.Kind = Yua::JobKind::Generate;
    context.JobActive = false;
    Yua::OnJobDone(context, 77u);
    Check(context.Job.Kind == Yua::JobKind::None && context.Job.Thread == nullptr,
          "late Done with matching id still clears job results without disturbing state");

    // 8) 在线更新下载：job.Ok=true 也不能越过 cancel / ClosePending / Closing / JobDoneUi 入库。
    {
        Yua::OnlineDialogState online{};
        online.Job.Kind = Yua::OnlineJobKind::Download;
        online.Job.Ok = true;

        Check(!Yua::OnlineDownloadMustSkipInstall(online, Yua::OnlineJobKind::Download, false),
              "successful download installs when idle");
        Check(Yua::OnlineDownloadMustSkipInstall(online, Yua::OnlineJobKind::Download, true),
              "cancelled download never installs even when worker ok=true");
        online.ClosePending = true;
        Check(Yua::OnlineDownloadMustSkipInstall(online, Yua::OnlineJobKind::Download, false),
              "ClosePending download never installs");
        online.ClosePending = false;
        online.Closing = true;
        Check(Yua::OnlineDownloadMustSkipInstall(online, Yua::OnlineJobKind::Download, false),
              "Closing download never installs");
        online.Closing = false;
        online.JobDoneUi = true;
        Check(Yua::OnlineDownloadMustSkipInstall(online, Yua::OnlineJobKind::Download, false),
              "modal Done UI keeps download install gated");
        online.JobDoneUi = false;
        Check(!Yua::OnlineDownloadMustSkipInstall(online, Yua::OnlineJobKind::FetchIndex, false),
              "fetch-index is not an install task");

        Check(!Yua::OnlineActionBlocked(online), "idle online dialog is not blocked");
        online.JobActive = true;
        Check(Yua::OnlineActionBlocked(online), "online dialog blocks actions while job active");
        online.JobActive = false;
        online.ClosePending = true;
        Check(Yua::OnlineActionBlocked(online), "online dialog blocks actions while close pending");
        online.ClosePending = false;
        online.Closing = true;
        Check(Yua::OnlineActionBlocked(online), "online dialog blocks actions while closing");
        online.Closing = false;
        online.JobDoneUi = true;
        Check(Yua::OnlineActionBlocked(online), "online dialog blocks actions while Done UI is modal");

        // 9) 覆盖确认框期间新到达的 ClosePending / Cancel 必须在真正入库前被最终复核拦住。
        //    模拟：初始 skip=false → OnOnlineJobDone 置 JobDoneUi → 确认框期间 ClosePending=true
        //    → 用户点 Yes 后 final recheck 必须为 true（不 Install）。
        {
            Yua::OnlineDialogState recheck{};
            recheck.Job.Kind = Yua::OnlineJobKind::Download;
            recheck.Job.Ok = true;
            Check(!Yua::OnlineInstallRecheckAborts(recheck, recheck.Job),
                  "final install recheck passes for idle successful download");

            recheck.JobDoneUi = true; // OnOnlineJobDone 自置的模态保护标志
            Check(!Yua::OnlineInstallRecheckAborts(recheck, recheck.Job),
                  "final install recheck ignores self-set JobDoneUi");

            recheck.ClosePending = true; // 覆盖确认框期间收到 WM_CLOSE
            Check(Yua::OnlineInstallRecheckAborts(recheck, recheck.Job),
                  "final install recheck aborts after ClosePending during confirmation");
            recheck.ClosePending = false;

            ::InterlockedExchange(&recheck.Job.Cancel, 1);
            Check(Yua::OnlineInstallRecheckAborts(recheck, recheck.Job),
                  "final install recheck aborts after Cancel during confirmation");
            ::InterlockedExchange(&recheck.Job.Cancel, 0);

            recheck.Closing = true;
            Check(Yua::OnlineInstallRecheckAborts(recheck, recheck.Job),
                  "final install recheck aborts while closing");
        }
    }

    // 10) 只读但会弹模态框 / 碰磁盘的命令必须和控件锁定共用同一份 sensitive 列表。
    Check(Yua::IsJobSensitiveCommand(Yua::IDC_YUA_SETM_NOTE),
          "SetM note is a job-sensitive command");
    Check(Yua::IsJobSensitiveCommand(Yua::IDC_YUA_SET_OPEN_LIB),
          "Open library directory is a job-sensitive command");
    Check(Yua::IsJobSensitiveCommand(Yua::IDC_YUA_ADVANCED),
          "Legacy functions window is a job-sensitive command");
    Check(Yua::IsJobSensitiveCommand(Yua::IDC_YUA_AUX_WORKSPACE),
          "Open workspace is a job-sensitive command");
    Check(Yua::IsJobSensitiveCommand(Yua::IDC_YUA_AUX_REPORT),
          "Open restore report is a job-sensitive command");
    Check(!Yua::IsJobSensitiveCommand(Yua::IDC_YUA_EXE_EDIT),
          "target edit keeps silent EN_KILLFOCUS handling");

    ::DeleteCriticalSection(&context.Job.Lock);
    context.Job.LockReady = false;

    std::printf("RESULT %s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
