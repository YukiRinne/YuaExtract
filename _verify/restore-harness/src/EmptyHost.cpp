// 验证用「空宿主进程」（不是产品的一部分，只在 _verify\restore-harness 下构建）。
//
// 用途：在不启动真实游戏的前提下，把注入型 DLL（CxdecStringDumper.dll）LoadLibrary 进来，
// 让它的 DllMain / 初始化路径真的跑一遍，从而观察：
//   - 它创建了哪些顶层窗口（类名、标题）
//   - 它写了哪些日志（RuntimeHashRestore.log 等）
//   - 环境变量（CXDEC_HASH_COLLECT_ONLY 等）对上述行为的影响
//
// 为什么必须是一个独立 EXE（而不是像 _verify\harness 那样用 PowerShell 当宿主）：
//   1) 32 位 DLL 只能进 32 位进程，pwsh 是 64 位；
//   2) 环境变量要在 LoadLibrary **之前**设好，独立进程最干净；
//   3) 宿主的窗口要和 DLL 创建的窗口区分开（本进程有 kHostClassName 这个已知窗口），
//      否则光看 PID 分不清哪个窗口是 DLL 建的。
//
// 用法：
//   EmptyHost.exe --dll <DLL 路径> [--status <状态文件路径>] [--title <窗口标题>]
//                 [--lifetime <毫秒>]
//
// 退出方式（任意一种）：
//   - 外部结束本进程（Stop-Process）：默认观测方式，不会走到 DLL 卸载路径；
//   - 给宿主窗口发 WM_CLOSE；
//   - --lifetime 到点（兜底，默认 60000 毫秒）。
//
// 状态文件（UTF-8，key=value 每行一条）在 LoadLibrary 之后立刻写出，
// 里面有 load=ok/fail 与 error=<Win32 错误码>，验证脚本靠它区分
// "DLL 没装进来" 和 "DLL 装进来了但行为不对"。

#include <Windows.h>

#include <string>
#include <vector>

namespace
{
    constexpr wchar_t kHostClassName[] = L"YuaEmptyHostWindow";
    constexpr wchar_t kHostWindowTitle[] = L"YuaExtract 验证用空宿主";
    constexpr UINT_PTR kLifetimeTimerId = 1u;

    struct Options
    {
        std::wstring dllPath;
        std::wstring statusPath;
        std::wstring title = kHostWindowTitle;
        unsigned int lifetimeMs = 60000u;
    };

    std::vector<std::wstring> SplitCommandLine()
    {
        std::vector<std::wstring> arguments;
        int count = 0;
        LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &count);
        if (argv == nullptr)
        {
            return arguments;
        }
        for (int index = 1; index < count; ++index)  // 跳过自身路径
        {
            arguments.emplace_back(argv[index]);
        }
        ::LocalFree(argv);
        return arguments;
    }

    bool ParseOptions(const std::vector<std::wstring>& arguments, Options& options)
    {
        for (size_t index = 0; index < arguments.size(); ++index)
        {
            const std::wstring& name = arguments[index];
            const bool hasValue = index + 1u < arguments.size();
            if (name == L"--dll" && hasValue)
            {
                options.dllPath = arguments[++index];
            }
            else if (name == L"--status" && hasValue)
            {
                options.statusPath = arguments[++index];
            }
            else if (name == L"--title" && hasValue)
            {
                options.title = arguments[++index];
            }
            else if (name == L"--lifetime" && hasValue)
            {
                options.lifetimeMs = static_cast<unsigned int>(_wtoi(arguments[++index].c_str()));
            }
        }
        return !options.dllPath.empty();
    }

    void AppendUtf8(std::string& text, const std::wstring& line)
    {
        if (line.empty())
        {
            text += "\r\n";
            return;
        }
        const int need = ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                              nullptr, 0, nullptr, nullptr);
        if (need <= 0)
        {
            return;
        }
        const size_t offset = text.size();
        text.resize(offset + static_cast<size_t>(need));
        ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), &text[offset], need,
                              nullptr, nullptr);
        text += "\r\n";
    }

    bool WriteStatusFile(const Options& options, const std::vector<std::pair<std::wstring, std::wstring>>& pairs)
    {
        if (options.statusPath.empty())
        {
            return false;
        }

        std::string text;
        for (const auto& pair : pairs)
        {
            AppendUtf8(text, pair.first + L"=" + pair.second);
        }

        HANDLE file = ::CreateFileW(options.statusPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        DWORD written = 0;
        const BOOL ok = ::WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        ::CloseHandle(file);
        return ok != FALSE;
    }

    std::wstring FormatNumber(unsigned long long value)
    {
        wchar_t buffer[32]{};
        ::swprintf_s(buffer, L"%llu", value);
        return buffer;
    }

    struct HostState
    {
        ULONGLONG startedTick = 0;
        unsigned int lifetimeMs = 60000u;
    };

    LRESULT CALLBACK HostWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        HostState* state = reinterpret_cast<HostState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (message)
        {
            case WM_CREATE:
            {
                CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
                state = static_cast<HostState*>(create->lpCreateParams);
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
                ::SetTimer(hwnd, kLifetimeTimerId, 250u, nullptr);
                return 0;
            }
            case WM_TIMER:
            {
                if (state != nullptr && state->lifetimeMs > 0u &&
                    ::GetTickCount64() - state->startedTick >= state->lifetimeMs)
                {
                    ::DestroyWindow(hwnd);
                }
                return 0;
            }
            case WM_CLOSE:
                ::DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                ::KillTimer(hwnd, kLifetimeTimerId);
                ::PostQuitMessage(0);
                return 0;
            default:
                break;
        }
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, LPWSTR commandLine, int showCommand)
{
    UNREFERENCED_PARAMETER(previous);
    UNREFERENCED_PARAMETER(commandLine);
    UNREFERENCED_PARAMETER(showCommand);

    Options options;
    if (!ParseOptions(SplitCommandLine(), options))
    {
        ::MessageBoxW(nullptr, L"用法：EmptyHost.exe --dll <DLL 路径> [--status <文件>] [--lifetime <毫秒>]",
                      kHostWindowTitle, MB_ICONERROR | MB_OK);
        return 2;
    }

    // 先把宿主窗口建起来再 LoadLibrary：这样 DLL 初始化里如果做了什么
    // 依赖"当前有窗口/消息队列"的事，行为跟真实宿主更接近。
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = HostWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kHostClassName;
    if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        return 3;
    }

    HostState state;
    state.startedTick = ::GetTickCount64();
    state.lifetimeMs = options.lifetimeMs;

    HWND hwnd = ::CreateWindowExW(0, kHostClassName, options.title.c_str(),
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT,
                                  CW_USEDEFAULT, 620, 160, nullptr, nullptr, instance, &state);
    if (hwnd == nullptr)
    {
        return 4;
    }

    // 窗口里放两行说明：人工盯屏时一眼能看出这是哪个用例。
    ::CreateWindowW(L"STATIC", L"（验证用空宿主，不是产品）", WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 14, 580, 20,
                    hwnd, nullptr, instance, nullptr);
    ::CreateWindowW(L"STATIC", options.dllPath.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS, 16, 40,
                    580, 20, hwnd, nullptr, instance, nullptr);
    HWND statusLine = ::CreateWindowW(L"STATIC", L"正在加载 DLL……", WS_CHILD | WS_VISIBLE | SS_LEFT, 16, 66, 580,
                                      20, hwnd, nullptr, instance, nullptr);
    ::ShowWindow(hwnd, SW_SHOW);
    ::UpdateWindow(hwnd);

    ::SetLastError(ERROR_SUCCESS);
    HMODULE module = ::LoadLibraryW(options.dllPath.c_str());
    const DWORD loadError = module != nullptr ? 0u : ::GetLastError();

    std::wstring summary = module != nullptr ? L"DLL 加载成功（可被外部结束）"
                                             : L"DLL 加载失败，错误码 " + FormatNumber(loadError);
    ::SetWindowTextW(statusLine, summary.c_str());

    WriteStatusFile(options,
                    { { L"pid", FormatNumber(::GetCurrentProcessId()) },
                      { L"dll", options.dllPath },
                      { L"load", module != nullptr ? L"ok" : L"fail" },
                      { L"error", FormatNumber(loadError) },
                      { L"lifetimeMs", FormatNumber(options.lifetimeMs) } });

    MSG message{};
    while (::GetMessageW(&message, nullptr, 0u, 0u) > 0)
    {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    // 注意：这里**故意不** FreeLibrary。DLL 自己的 UI 线程还在跑，
    // 卸载会在 DllMain detach 里撞上它；验证也不需要走卸载路径
    // （真实宿主的最终形态就是进程退出）。正常路径是外部结束本进程。
    UNREFERENCED_PARAMETER(module);
    return 0;
}

#pragma comment(linker, "/ENTRY:wWinMainCRTStartup")
