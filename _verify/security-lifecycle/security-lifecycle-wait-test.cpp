// Headless 测试：预检 / 注入共用的有界等待 helper（生产实现，不是副本）。
//
// 覆盖：
//   1) deadline 到期返回 false（不会无限等）；
//   2) 句柄已 signal 返回 true；
//   3) owner 队列里的 WM_CLOSE 被识别为 cancel，且**不被 helper 吃掉**；
//   4) WM_QUIT 被识别为 cancel，且同样保留在队列里；
//   5) 自建 sleep 子进程：未退出时超时返回 false，退出后返回 true（不注入任何模块）。
//
// 不创建游戏进程、不加载目标模块、不触碰用户目录。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "YuaExtractHost.h"

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

    LRESULT CALLBACK SilentWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    HWND CreateSilentWindow()
    {
        const wchar_t* className = L"YuaSecurityLifecycleWaitTestWindow";
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = SilentWndProc;
        cls.hInstance = ::GetModuleHandleW(nullptr);
        cls.lpszClassName = className;
        ::RegisterClassExW(&cls);
        return ::CreateWindowExW(0, className, L"wait-test", WS_OVERLAPPED, 0, 0, 100, 100,
                                 nullptr, nullptr, cls.hInstance, nullptr);
    }

    int RunSleepChild(int argc, wchar_t** argv)
    {
        if (argc >= 3 && _wcsicmp(argv[1], L"--sleep") == 0)
        {
            ::Sleep(static_cast<DWORD>(_wtoi(argv[2])));
            return 0;
        }
        return -1;
    }
}

int wmain(int argc, wchar_t** argv)
{
    const int childResult = RunSleepChild(argc, argv);
    if (childResult >= 0)
    {
        return childResult;
    }

    // 1) 未 signal + 短 deadline：必须超时返回 false。
    {
        HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        bool cancelled = false;
        const ULONGLONG started = ::GetTickCount64();
        const bool ready = Yua::Host::Detail::WaitForHandleResponsive(
            nullptr, event, started + 250ull, cancelled);
        const ULONGLONG elapsed = ::GetTickCount64() - started;
        Check(!ready && !cancelled, "bounded wait times out on unsignaled handle");
        Check(elapsed >= 180ull, "bounded wait actually waited near the deadline");
        ::CloseHandle(event);
    }

    // 2) 已 signal：立即返回 true。
    {
        HANDLE event = ::CreateEventW(nullptr, TRUE, TRUE, nullptr);
        bool cancelled = false;
        const bool ready = Yua::Host::Detail::WaitForHandleResponsive(
            nullptr, event, ::GetTickCount64() + 2000ull, cancelled);
        Check(ready && !cancelled, "bounded wait returns true on signaled handle");
        ::CloseHandle(event);
    }

    // 3) owner 的 WM_CLOSE 只探测不消费。
    {
        HWND window = CreateSilentWindow();
        HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(window != nullptr, "test window created");
        if (window)
        {
            ::PostMessageW(window, WM_CLOSE, 0, 0);
            bool cancelled = false;
            const bool ready = Yua::Host::Detail::WaitForHandleResponsive(
                window, event, ::GetTickCount64() + 2000ull, cancelled);
            Check(!ready && cancelled, "queued WM_CLOSE is reported as cancel");

            MSG message{};
            const bool stillQueued = ::PeekMessageW(&message, window, WM_CLOSE, WM_CLOSE,
                                                    PM_NOREMOVE) != FALSE;
            Check(stillQueued, "WM_CLOSE is preserved in the owner queue (not swallowed)");
            ::PeekMessageW(&message, window, WM_CLOSE, WM_CLOSE, PM_REMOVE);
            ::DestroyWindow(window);
        }
        ::CloseHandle(event);
    }

    // 4) WM_QUIT 也只探测，留给外层消息循环。
    {
        HANDLE event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ::PostThreadMessageW(::GetCurrentThreadId(), WM_QUIT, 7u, 0u);
        bool cancelled = false;
        const bool ready = Yua::Host::Detail::WaitForHandleResponsive(
            nullptr, event, ::GetTickCount64() + 2000ull, cancelled);
        Check(!ready && cancelled, "queued WM_QUIT is reported as cancel");
        MSG message{};
        const bool stillQueued = ::PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT,
                                                PM_NOREMOVE) != FALSE;
        Check(stillQueued, "WM_QUIT is preserved in the thread queue");
        ::PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE);
        ::CloseHandle(event);
    }

    // 5) 自建 sleep 子进程：只等待句柄，不注入。
    {
        wchar_t selfPath[MAX_PATH]{};
        ::GetModuleFileNameW(nullptr, selfPath, _countof(selfPath));
        std::wstring commandLine = L"\"" + std::wstring(selfPath) + L"\" --sleep 500";
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (::CreateProcessW(selfPath, commandLine.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        {
            bool cancelled = false;
            const bool early = Yua::Host::Detail::WaitForHandleResponsive(
                nullptr, pi.hProcess, ::GetTickCount64() + 150ull, cancelled);
            Check(!early, "sleep child is still running at the short deadline");

            ::WaitForSingleObject(pi.hProcess, 5000u);
            const bool late = Yua::Host::Detail::WaitForHandleResponsive(
                nullptr, pi.hProcess, ::GetTickCount64() + 2000ull, cancelled);
            Check(late, "sleep child is observed as exited after it finishes");

            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
        }
        else
        {
            Check(false, "create self sleep child process");
        }
    }

    std::printf("RESULT %s (%d failures)\n", g_failures == 0 ? "PASS" : "FAIL", g_failures);
    return g_failures == 0 ? 0 : 1;
}
