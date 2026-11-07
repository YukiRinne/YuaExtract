#include <windows.h>
#include <commctrl.h>
#include <detours.h>
#include "detour_section.h"
#include <ShObjIdl.h>
#include <ShlObj.h>
#include <algorithm>
#include <cstdarg>
#include <cwctype>
#include <string>
#include <unordered_set>
#include <vector>

#include "loaderipc.h"
#include "diag.h"
#include "path.h"
#include "util.h"
#include "directory.h"
#include "encoding.h"
#include "log.h"
#include "logdir.h"
#include "resource.h"
#include "SteamApiSwap.h"
#include "TryExport.h"
#include "YuaExtractHost.h"
#include "YuaExtractMainWindow.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/MERGE:\".detourd=.data\"")
#pragma comment(linker, "/MERGE:\".detourc=.rdata\"")

#ifdef _UNICODE
#if defined _M_IX86
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='x86' publicKeyToken='6595b64144ccf1df' language='*'\"")
#elif defined _M_X64
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='amd64' publicKeyToken='6595b64144ccf1df' language='*'\"")
#else
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif
#endif

static std::wstring g_LoaderFullPath;
static std::wstring g_LoaderCurrentDirectory;
static std::wstring g_KrkrExeFullPath;
static std::wstring g_KrkrExeDirectory;

// 原版功能窗口（IDD_MainForm）现在由 YuaExtract 主界面按需打开，不再自己撑起整个进程。
// 只有它确实是"主窗口"时（老用法）才在关闭时结束消息循环，否则关掉它会把整个程序带走。
static bool g_LegacyDialogOwnsApp = false;
// 原版功能窗口句柄。主界面按需打开 / 复用，RunLegacyAction 也用它当命令目标。
static HWND g_LegacyWindow = nullptr;
// WebView2 桥的日志镜像。默认空 = 只写文件，行为与以前完全一致。
static Yua::Host::LogSink g_LogSink = nullptr;

namespace
{
    constexpr wchar_t RuntimeHashTargetDirectoryEnvName[] = L"CXDEC_RUNTIME_HASH_TARGET_DIR";
    // 步骤2 的"只收集"开关：注入 CxdecStringDumper.dll 时置 1，
    // 那个模块就只把 Hash 映射写成文件，不弹自己的恢复界面、不改游戏资源。
    // 名字是和 CxdecStringDumper 侧约定死的，别改。
    constexpr wchar_t HashCollectOnlyEnvName[] = L"CXDEC_HASH_COLLECT_ONLY";
    constexpr wchar_t HashCrackOutputDirectoryEnvName[] = L"CXDEC_HASH_CRACK_OUTPUT_DIR";
    constexpr wchar_t HashCrackDirsFileEnvName[] = L"CXDEC_HASH_CRACK_DIRS_FILE";
    constexpr wchar_t HashCrackFilesFileEnvName[] = L"CXDEC_HASH_CRACK_FILES_FILE";
    constexpr wchar_t HashCrackPureHashDirectoryEnvName[] = L"CXDEC_HASH_CRACK_PURE_HASH_DIR";
    constexpr wchar_t HashCrackSupplementalMapEnvName[] = L"CXDEC_HASH_CRACK_SUPPLEMENTAL_MAP";
    constexpr wchar_t HashCrackSuppressRestoreUiEnvName[] = L"CXDEC_HASH_CRACK_SUPPRESS_RESTORE_UI";
    constexpr wchar_t HookHashDialogClassName[] = L"CxdecHookHashRestorePrepareWindow";
    constexpr int IDC_HOOK_PURE_EDIT = 3101;
    constexpr int IDC_HOOK_PURE_BROWSE = 3102;
    constexpr int IDC_HOOK_OUTPUT_EDIT = 3103;
    constexpr int IDC_HOOK_OUTPUT_BROWSE = 3104;
    constexpr int IDC_HOOK_SUPPLEMENT_EDIT = 3105;
    constexpr int IDC_HOOK_SUPPLEMENT_BROWSE = 3106;
    constexpr int IDC_HOOK_DIRS_EDIT = 3107;
    constexpr int IDC_HOOK_DIRS_BROWSE = 3108;
    constexpr int IDC_HOOK_FILES_EDIT = 3109;
    constexpr int IDC_HOOK_FILES_BROWSE = 3110;
    constexpr int IDC_HOOK_MAKE_CANDIDATE = 3111;
    constexpr int IDC_HOOK_RESCAN = 3112;
    constexpr int IDC_HOOK_SUMMARY = 3113;
    constexpr int IDC_HOOK_START = 3114;
    constexpr int IDC_HOOK_HINT_BASE = 3200;
    constexpr int IDC_HOOK_HINT_COUNT = 8;

    struct HookHashRestoreLaunchOptions
    {
        std::wstring PureHashDirectory;
        std::wstring OutputDirectory;
        std::wstring SupplementalMapPath;
        std::wstring DirsPath;
        std::wstring FilesPath;
    };

    std::wstring BrowseFolder(HWND owner, const wchar_t* title);

    std::wstring CombinePathLocal(const std::wstring& directory, const std::wstring& fileName)
    {
        if (directory.empty())
        {
            return fileName;
        }
        if (directory.back() == L'\\' || directory.back() == L'/')
        {
            return directory + fileName;
        }
        return directory + L'\\' + fileName;
    }

    std::wstring GetFileNameLocal(const std::wstring& path)
    {
        size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? path : path.substr(slash + 1u);
    }

    std::wstring GetParentDirectoryLocal(const std::wstring& path)
    {
        size_t slash = path.find_last_of(L"\\/");
        return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
    }

    bool SamePathText(const std::wstring& left, const std::wstring& right)
    {
        return !left.empty() && !right.empty() && _wcsicmp(left.c_str(), right.c_str()) == 0;
    }

    bool FileExistsLocal(const std::wstring& path)
    {
        DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    bool DirectoryExistsLocal(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }
        DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    std::wstring GetModuleDllPath(const std::wstring& dllFileName)
    {
        std::wstring modulePath = Path::Combine(Path::Combine(g_LoaderCurrentDirectory, L"CxdecExtractordll"), dllFileName);
        if (FileExistsLocal(modulePath))
        {
            return modulePath;
        }

        std::wstring fallbackPath = Path::Combine(g_LoaderCurrentDirectory, dllFileName);
        if (FileExistsLocal(fallbackPath))
        {
            return fallbackPath;
        }

        return modulePath;
    }

    std::wstring FormatString(const wchar_t* format, ...)
    {
        wchar_t buffer[2048]{};
        va_list ap;
        va_start(ap, format);
        int count = _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, ap);
        va_end(ap);
        if (count <= 0)
        {
            return std::wstring();
        }
        return std::wstring(buffer, count);
    }

    bool FindLatestFile(const std::wstring& directory, const std::wstring& pattern, std::wstring& latestPath, FILETIME& latestTime)
    {
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(CombinePathLocal(directory, pattern).c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        bool found = false;
        do
        {
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                continue;
            }
            std::wstring name = data.cFileName;
            std::transform(name.begin(), name.end(), name.begin(), [](wchar_t ch) { return (wchar_t)towlower(ch); });
            if (name.find(L"_match") != std::wstring::npos || name.find(L"_tmp") != std::wstring::npos)
            {
                continue;
            }

            if (!found || ::CompareFileTime(&data.ftLastWriteTime, &latestTime) > 0)
            {
                found = true;
                latestTime = data.ftLastWriteTime;
                latestPath = CombinePathLocal(directory, data.cFileName);
            }
        } while (::FindNextFileW(find, &data));

        ::FindClose(find);
        return found;
    }

    unsigned int CountTextLines(const std::wstring& path)
    {
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return 0u;
        }

        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 64ll * 1024ll * 1024ll)
        {
            ::CloseHandle(file);
            return 0u;
        }

        std::string bytes((size_t)size.QuadPart, '\0');
        DWORD read = 0u;
        BOOL ok = ::ReadFile(file, bytes.data(), (DWORD)bytes.size(), &read, nullptr);
        ::CloseHandle(file);
        if (!ok || read != bytes.size())
        {
            return 0u;
        }

        unsigned int lines = 0u;
        for (char ch : bytes)
        {
            if (ch == '\n')
            {
                ++lines;
            }
        }
        if (!bytes.empty() && bytes.back() != '\n')
        {
            ++lines;
        }
        return lines;
    }

    std::wstring MakeRelativePathLocal(const std::wstring& root, const std::wstring& path)
    {
        if (path.length() <= root.length())
        {
            return std::wstring();
        }

        size_t start = root.length();
        if (path[start] == L'\\' || path[start] == L'/')
        {
            ++start;
        }
        return path.substr(start);
    }

    void CollectCandidateNames(const std::wstring& root,
                               const std::wstring& directory,
                               std::unordered_set<std::wstring>& directoryNames,
                               std::unordered_set<std::wstring>& fileNames)
    {
        WIN32_FIND_DATAW data{};
        HANDLE find = ::FindFirstFileW(CombinePathLocal(directory, L"*").c_str(), &data);
        if (find == INVALID_HANDLE_VALUE)
        {
            return;
        }

        do
        {
            if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
            {
                continue;
            }
            if (_wcsicmp(data.cFileName, L"ExtractLog") == 0 || _wcsicmp(data.cFileName, L"Extractor_Log") == 0)
            {
                continue;
            }

            std::wstring fullPath = CombinePathLocal(directory, data.cFileName);
            std::wstring relative = MakeRelativePathLocal(root, fullPath);
            std::replace(relative.begin(), relative.end(), L'\\', L'/');
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                if (!relative.empty())
                {
                    directoryNames.insert(relative);
                }
                CollectCandidateNames(root, fullPath, directoryNames, fileNames);
            }
            else if (!relative.empty())
            {
                fileNames.insert(relative);
                size_t slash = relative.find_last_of(L'/');
                fileNames.insert(slash == std::wstring::npos ? relative : relative.substr(slash + 1u));
            }
        } while (::FindNextFileW(find, &data));

        ::FindClose(find);
    }

    std::vector<std::wstring> SortedCandidateLines(const std::unordered_set<std::wstring>& values)
    {
        std::vector<std::wstring> lines(values.begin(), values.end());
        std::sort(lines.begin(), lines.end(), [](const std::wstring& left, const std::wstring& right)
        {
            return _wcsicmp(left.c_str(), right.c_str()) < 0;
        });
        return lines;
    }

    bool WriteUtf16LinesLocal(const std::wstring& path, const std::vector<std::wstring>& lines)
    {
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }

        WORD bom = 0xFEFF;
        DWORD written = 0u;
        ::WriteFile(file, &bom, sizeof(bom), &written, nullptr);
        for (const std::wstring& line : lines)
        {
            ::WriteFile(file, line.c_str(), (DWORD)(line.length() * sizeof(wchar_t)), &written, nullptr);
            ::WriteFile(file, L"\r\n", 4u, &written, nullptr);
        }
        ::CloseHandle(file);
        return true;
    }

    std::wstring GetTimestampStringLocal()
    {
        SYSTEMTIME time{};
        ::GetLocalTime(&time);
        return FormatString(L"%04u_%02u_%02u_%02u_%02u_%02u", time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    }

    bool MakeCandidateLists(HWND owner, HookHashRestoreLaunchOptions& options)
    {
        std::wstring sourceDirectory = BrowseFolder(owner, L"选择用于制作候选lst的明文资源目录");
        if (sourceDirectory.empty())
        {
            return false;
        }
        if (options.OutputDirectory.empty())
        {
            options.OutputDirectory = CombinePathLocal(g_KrkrExeDirectory, L"StringHashDumper_Output");
        }
        ::SHCreateDirectoryExW(owner, options.OutputDirectory.c_str(), nullptr);

        std::unordered_set<std::wstring> directoryNames;
        std::unordered_set<std::wstring> fileNames;
        directoryNames.insert(L"/");
        CollectCandidateNames(sourceDirectory, sourceDirectory, directoryNames, fileNames);

        std::wstring stamp = GetTimestampStringLocal();
        options.DirsPath = CombinePathLocal(options.OutputDirectory, L"dirs_" + stamp + L".txt");
        options.FilesPath = CombinePathLocal(options.OutputDirectory, L"files_" + stamp + L".txt");
        return WriteUtf16LinesLocal(options.DirsPath, SortedCandidateLines(directoryNames))
            && WriteUtf16LinesLocal(options.FilesPath, SortedCandidateLines(fileNames));
    }

    void ScanLatestHookCandidates(HookHashRestoreLaunchOptions& options)
    {
        if (options.OutputDirectory.empty())
        {
            options.OutputDirectory = CombinePathLocal(g_KrkrExeDirectory, L"StringHashDumper_Output");
        }

        FILETIME latestDirsTime{};
        FILETIME latestFilesTime{};
        FindLatestFile(options.OutputDirectory, L"dirs_*.txt", options.DirsPath, latestDirsTime);
        FindLatestFile(options.OutputDirectory, L"files_*.txt", options.FilesPath, latestFilesTime);
    }

    void NormalizeHookDirectories(HookHashRestoreLaunchOptions& options)
    {
        if (options.OutputDirectory.empty())
        {
            options.OutputDirectory = CombinePathLocal(g_KrkrExeDirectory, L"StringHashDumper_Output");
        }

        bool pureLooksLikeHashOutput = _wcsicmp(GetFileNameLocal(options.PureHashDirectory).c_str(), L"StringHashDumper_Output") == 0;
        if (options.PureHashDirectory.empty() || SamePathText(options.PureHashDirectory, options.OutputDirectory) || pureLooksLikeHashOutput)
        {
            std::wstring parent = GetParentDirectoryLocal(options.OutputDirectory);
            options.PureHashDirectory = CombinePathLocal(parent.empty() ? g_KrkrExeDirectory : parent, L"Extractor_Output");
        }
    }

    std::wstring BrowseFolder(HWND owner, const wchar_t* title)
    {
        std::wstring result;
        HRESULT coInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

        IFileDialog* dialog = nullptr;
        HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(hr) && dialog)
        {
            DWORD options = 0u;
            if (SUCCEEDED(dialog->GetOptions(&options)))
            {
                dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
            }
            dialog->SetTitle(title);

            if (SUCCEEDED(dialog->Show(owner)))
            {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)) && item)
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                    {
                        result = path;
                        ::CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }

        if (SUCCEEDED(coInit))
        {
            ::CoUninitialize();
        }
        return result;
    }

    // 统一的文件选择框：save=true 走"另存为"，否则走"打开"。
    // defaultDirectory / defaultName 只决定弹出来时的初始位置，用户可以随便改。
    std::wstring BrowseFileDialog(HWND owner, const wchar_t* title, bool save,
                                  const wchar_t* filterName, const wchar_t* filterSpec,
                                  const wchar_t* defaultDirectory, const wchar_t* defaultName)
    {
        std::wstring result;
        HRESULT coInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

        IFileDialog* dialog = nullptr;
        HRESULT hr = ::CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog,
                                        nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        if (SUCCEEDED(hr) && dialog)
        {
            DWORD options = 0u;
            if (SUCCEEDED(dialog->GetOptions(&options)))
            {
                options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
                options |= save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST;
                dialog->SetOptions(options);
            }
            dialog->SetTitle(title);

            if (filterName != nullptr && filterSpec != nullptr)
            {
                COMDLG_FILTERSPEC filters[] =
                {
                    { filterName, filterSpec },
                    { L"所有文件", L"*.*" }
                };
                dialog->SetFileTypes(_countof(filters), filters);
            }

            if (defaultDirectory != nullptr && defaultDirectory[0] != L'\0')
            {
                IShellItem* folder = nullptr;
                if (SUCCEEDED(::SHCreateItemFromParsingName(defaultDirectory, nullptr, IID_PPV_ARGS(&folder))) && folder)
                {
                    dialog->SetFolder(folder);
                    folder->Release();
                }
            }
            if (defaultName != nullptr && defaultName[0] != L'\0')
            {
                dialog->SetFileName(defaultName);
            }

            if (SUCCEEDED(dialog->Show(owner)))
            {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)) && item)
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                    {
                        result = path;
                        ::CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }

        if (SUCCEEDED(coInit))
        {
            ::CoUninitialize();
        }
        return result;
    }

    std::wstring BrowseTextFile(HWND owner, const wchar_t* title)
    {
        return BrowseFileDialog(owner, title, false, L"候选文本", L"*.txt", nullptr, nullptr);
    }

    std::wstring BrowseExeFile(HWND owner, const wchar_t* title)
    {
        return BrowseFileDialog(owner, title, false, L"游戏主程序", L"*.exe", nullptr, nullptr);
    }

    // 默认落在 <游戏目录>\patch.xp3（国际版约定），用户可以直接改。
    std::wstring BrowseSaveXp3File(HWND owner, const wchar_t* title, const std::wstring& defaultPath)
    {
        const std::wstring defaultDirectory = GetParentDirectoryLocal(defaultPath);
        const std::wstring defaultName = GetFileNameLocal(defaultPath);
        return BrowseFileDialog(owner, title, true, L"XP3 封包", L"*.xp3",
                                defaultDirectory.c_str(), defaultName.c_str());
    }

    std::wstring GetWindowTextString(HWND hwnd)
    {
        int length = ::GetWindowTextLengthW(hwnd);
        if (length <= 0)
        {
            return std::wstring();
        }

        std::wstring text((size_t)length + 1u, L'\0');
        int copied = ::GetWindowTextW(hwnd, text.data(), length + 1);
        if (copied <= 0)
        {
            return std::wstring();
        }
        text.resize((size_t)copied);
        return text;
    }

    HFONT CreateLoaderUiFont(HWND hwnd, int pointSize = 9, bool bold = false)
    {
        HDC dc = ::GetDC(hwnd);
        int dpiY = dc ? ::GetDeviceCaps(dc, LOGPIXELSY) : 96;
        if (dc)
        {
            ::ReleaseDC(hwnd, dc);
        }
        return ::CreateFontW(-::MulDiv(pointSize, dpiY, 72), 0, 0, 0, bold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
    }

    BOOL CALLBACK ApplyLoaderFontToChild(HWND child, LPARAM font)
    {
        ::SendMessageW(child, WM_SETFONT, (WPARAM)font, TRUE);
        return TRUE;
    }

    std::wstring BuildHookStatus(const HookHashRestoreLaunchOptions& options)
    {
        std::wstring missing;
        auto addMissing = [&missing](const wchar_t* name)
        {
            if (!missing.empty())
            {
                missing += L"、";
            }
            missing += name;
        };

        if (options.PureHashDirectory.empty())
        {
            addMissing(L"纯Hash目录");
        }
        if (options.OutputDirectory.empty())
        {
            addMissing(L"Hash输出目录");
        }
        if (options.DirsPath.empty() && options.FilesPath.empty())
        {
            addMissing(L"候选表");
        }

        std::wstring status;
        if (missing.empty())
        {
            status = L"状态：就绪，可以开始撞库。";
        }
        else
        {
            status = L"状态：还缺 " + missing + L"，补齐后才能开始。";
        }

        status += L"\r\n游戏主程序：";
        status += g_KrkrExeFullPath;

        if (!options.DirsPath.empty() || !options.FilesPath.empty())
        {
            status += L"\r\n候选表：目录 ";
            status += options.DirsPath.empty() ? std::wstring(L"未选择")
                                               : FormatString(L"%u 项", CountTextLines(options.DirsPath));
            status += L"  /  文件 ";
            status += options.FilesPath.empty() ? std::wstring(L"未选择")
                                                : FormatString(L"%u 项", CountTextLines(options.FilesPath));
        }

        if (!options.SupplementalMapPath.empty())
        {
            status += L"\r\n补充映射：";
            status += GetFileNameLocal(options.SupplementalMapPath);
        }

        return status;
    }

    struct HookDialogContext
    {
        HookHashRestoreLaunchOptions Options;
        bool Accepted;
        bool StatusReady;
        HFONT Font;
        HFONT BoldFont;
        HWND StepLabels[4];
    };

    void RefreshHookDialog(HWND hwnd, HookDialogContext* context)
    {
        if (!context)
        {
            return;
        }
        NormalizeHookDirectories(context->Options);
        ::SetWindowTextW(::GetDlgItem(hwnd, IDC_HOOK_PURE_EDIT), context->Options.PureHashDirectory.c_str());
        ::SetWindowTextW(::GetDlgItem(hwnd, IDC_HOOK_OUTPUT_EDIT), context->Options.OutputDirectory.c_str());
        ::SetWindowTextW(::GetDlgItem(hwnd, IDC_HOOK_SUPPLEMENT_EDIT), context->Options.SupplementalMapPath.c_str());
        ::SetWindowTextW(::GetDlgItem(hwnd, IDC_HOOK_DIRS_EDIT), context->Options.DirsPath.c_str());
        ::SetWindowTextW(::GetDlgItem(hwnd, IDC_HOOK_FILES_EDIT), context->Options.FilesPath.c_str());

        context->StatusReady = !context->Options.PureHashDirectory.empty()
                            && !context->Options.OutputDirectory.empty()
                            && (!context->Options.DirsPath.empty() || !context->Options.FilesPath.empty());
        ::SetWindowTextW(::GetDlgItem(hwnd, IDC_HOOK_SUMMARY), BuildHookStatus(context->Options).c_str());
        ::InvalidateRect(::GetDlgItem(hwnd, IDC_HOOK_SUMMARY), nullptr, TRUE);
    }

    LRESULT CALLBACK HookHashDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        HookDialogContext* context = (HookDialogContext*)::GetWindowLongPtrW(hwnd, GWLP_USERDATA);
        switch (message)
        {
            case WM_CREATE:
            {
                CREATESTRUCTW* create = (CREATESTRUCTW*)lParam;
                context = (HookDialogContext*)create->lpCreateParams;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)context);
                context->Font = CreateLoaderUiFont(hwnd, 9, false);
                context->BoldFont = CreateLoaderUiFont(hwnd, 10, true);
                for (HWND& label : context->StepLabels)
                {
                    label = nullptr;
                }

                const int kEditX = 222;
                const int kEditW = 524;
                const int kBrowseX = 754;
                const int kBrowseW = 94;

                auto MakeStepLabel = [&](int y, const wchar_t* text) -> HWND
                {
                    return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         16, y, 200, 20, hwnd, nullptr, nullptr, nullptr);
                };
                auto MakeHint = [&](int y, int index, const wchar_t* text)
                {
                    CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  kEditX, y, 630, 17, hwnd, (HMENU)(INT_PTR)(IDC_HOOK_HINT_BASE + index), nullptr, nullptr);
                };
                auto MakeEdit = [&](int x, int y, int width, int id)
                {
                    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    x, y, width, 24, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
                };
                auto MakeBrowse = [&](int y, int id)
                {
                    CreateWindowW(L"BUTTON", L"浏览", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  kBrowseX, y, kBrowseW, 26, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
                };

                // 第 1 步：要恢复的纯 Hash 解包目录
                context->StepLabels[0] = MakeStepLabel(16, L"第 1 步 · 纯Hash目录（必填）");
                MakeEdit(kEditX, 13, kEditW, IDC_HOOK_PURE_EDIT);
                MakeBrowse(12, IDC_HOOK_PURE_BROWSE);
                MakeHint(43, 0, L"要恢复的纯Hash解包目录，通常是游戏目录下的 Extractor_Output");

                // 第 2 步：撞库结果写入位置
                context->StepLabels[1] = MakeStepLabel(76, L"第 2 步 · Hash输出目录（必填）");
                MakeEdit(kEditX, 73, kEditW, IDC_HOOK_OUTPUT_EDIT);
                MakeBrowse(72, IDC_HOOK_OUTPUT_BROWSE);
                MakeHint(103, 1, L"撞库结果 HashRestore_RecoveredNames.lst 的写入目录");

                // 第 3 步：候选表（按钮与候选字段放在一起）
                context->StepLabels[2] = MakeStepLabel(136, L"第 3 步 · 候选表（必填）");
                CreateWindowW(L"STATIC", L"候选目录表", WS_CHILD | WS_VISIBLE | SS_LEFT,
                              36, 168, 100, 20, hwnd, nullptr, nullptr, nullptr);
                MakeEdit(146, 165, 600, IDC_HOOK_DIRS_EDIT);
                MakeBrowse(164, IDC_HOOK_DIRS_BROWSE);
                CreateWindowW(L"STATIC", L"候选文件表", WS_CHILD | WS_VISIBLE | SS_LEFT,
                              36, 200, 100, 20, hwnd, nullptr, nullptr, nullptr);
                MakeEdit(146, 197, 600, IDC_HOOK_FILES_EDIT);
                MakeBrowse(196, IDC_HOOK_FILES_BROWSE);
                CreateWindowW(L"BUTTON", L"从明文资源目录制作候选lst", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              146, 232, 200, 28, hwnd, (HMENU)IDC_HOOK_MAKE_CANDIDATE, nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"扫描最新候选", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              356, 232, 140, 28, hwnd, (HMENU)IDC_HOOK_RESCAN, nullptr, nullptr);
                MakeHint(268, 2, L"没有候选表时先用左侧按钮生成；目录表和文件表至少要有一个");

                // 第 4 步：可选补充映射
                context->StepLabels[3] = MakeStepLabel(302, L"第 4 步 · 补充lst映射（可选）");
                MakeEdit(kEditX, 299, kEditW, IDC_HOOK_SUPPLEMENT_EDIT);
                MakeBrowse(298, IDC_HOOK_SUPPLEMENT_BROWSE);
                MakeHint(329, 3, L"已有的映射文件，会合并进最终结果；没有可留空");

                // 底部状态：只显示校验结果与游戏主程序，不重复上面的输入
                CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                              16, 360, 832, 62, hwnd, (HMENU)IDC_HOOK_SUMMARY, nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"开始撞库", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                              650, 434, 110, 32, hwnd, (HMENU)IDC_HOOK_START, nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              768, 434, 90, 32, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);

                ::SendMessageW(hwnd, WM_SETFONT, (WPARAM)context->Font, TRUE);
                ::EnumChildWindows(hwnd, ApplyLoaderFontToChild, (LPARAM)context->Font);
                for (HWND label : context->StepLabels)
                {
                    if (label)
                    {
                        ::SendMessageW(label, WM_SETFONT, (WPARAM)context->BoldFont, TRUE);
                    }
                }
                RefreshHookDialog(hwnd, context);
                return 0;
            }
            case WM_CTLCOLORSTATIC:
            {
                if (!context)
                {
                    break;
                }
                const int controlId = ::GetDlgCtrlID((HWND)lParam);
                HDC dc = (HDC)wParam;
                ::SetBkMode(dc, TRANSPARENT);
                if (controlId == IDC_HOOK_SUMMARY)
                {
                    ::SetTextColor(dc, context->StatusReady ? RGB(0, 110, 40) : RGB(178, 78, 0));
                    return (LRESULT)::GetSysColorBrush(COLOR_WINDOW);
                }
                if (controlId >= IDC_HOOK_HINT_BASE && controlId < IDC_HOOK_HINT_BASE + IDC_HOOK_HINT_COUNT)
                {
                    ::SetTextColor(dc, RGB(110, 110, 110));
                    return (LRESULT)::GetSysColorBrush(COLOR_WINDOW);
                }
                break;
            }
            case WM_COMMAND:
                if (!context)
                {
                    break;
                }
                if (LOWORD(wParam) == IDC_HOOK_PURE_BROWSE)
                {
                    std::wstring folder = BrowseFolder(hwnd, L"选择需要恢复的纯Hash目录");
                    if (!folder.empty())
                    {
                        if (SamePathText(folder, GetWindowTextString(::GetDlgItem(hwnd, IDC_HOOK_OUTPUT_EDIT))))
                        {
                            ::MessageBoxW(hwnd, L"纯Hash目录不能和 Hash 输出目录相同。纯Hash目录应选择 Extractor_Output。", L"Cxdec Hook撞库恢复Hash映射", MB_OK | MB_ICONWARNING);
                            return 0;
                        }
                        context->Options.PureHashDirectory = folder;
                        RefreshHookDialog(hwnd, context);
                    }
                    return 0;
                }
                if (LOWORD(wParam) == IDC_HOOK_OUTPUT_BROWSE)
                {
                    std::wstring folder = BrowseFolder(hwnd, L"选择 Hash 输出目录");
                    if (!folder.empty())
                    {
                        context->Options.OutputDirectory = folder;
                        context->Options.DirsPath.clear();
                        context->Options.FilesPath.clear();
                        ScanLatestHookCandidates(context->Options);
                        RefreshHookDialog(hwnd, context);
                    }
                    return 0;
                }
                if (LOWORD(wParam) == IDC_HOOK_SUPPLEMENT_BROWSE)
                {
                    std::wstring file = BrowseTextFile(hwnd, L"选择补充lst映射");
                    if (!file.empty())
                    {
                        context->Options.SupplementalMapPath = file;
                        RefreshHookDialog(hwnd, context);
                    }
                    return 0;
                }
                if (LOWORD(wParam) == IDC_HOOK_DIRS_BROWSE)
                {
                    std::wstring file = BrowseTextFile(hwnd, L"选择候选目录表");
                    if (!file.empty())
                    {
                        context->Options.DirsPath = file;
                        RefreshHookDialog(hwnd, context);
                    }
                    return 0;
                }
                if (LOWORD(wParam) == IDC_HOOK_FILES_BROWSE)
                {
                    std::wstring file = BrowseTextFile(hwnd, L"选择候选文件表");
                    if (!file.empty())
                    {
                        context->Options.FilesPath = file;
                        RefreshHookDialog(hwnd, context);
                    }
                    return 0;
                }
                if (LOWORD(wParam) == IDC_HOOK_MAKE_CANDIDATE)
                {
                    context->Options.PureHashDirectory = GetWindowTextString(::GetDlgItem(hwnd, IDC_HOOK_PURE_EDIT));
                    context->Options.OutputDirectory = GetWindowTextString(::GetDlgItem(hwnd, IDC_HOOK_OUTPUT_EDIT));
                    context->Options.SupplementalMapPath = GetWindowTextString(::GetDlgItem(hwnd, IDC_HOOK_SUPPLEMENT_EDIT));
                    bool ok = MakeCandidateLists(hwnd, context->Options);
                    RefreshHookDialog(hwnd, context);
                    ::MessageBoxW(hwnd,
                                  ok ? L"候选lst制作完成。" : L"候选lst制作失败或已取消。",
                                  L"Cxdec Hook撞库恢复Hash映射",
                                  ok ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
                    return 0;
                }
                if (LOWORD(wParam) == IDC_HOOK_RESCAN)
                {
                    context->Options.PureHashDirectory = GetWindowTextString(::GetDlgItem(hwnd, IDC_HOOK_PURE_EDIT));
                    context->Options.OutputDirectory = GetWindowTextString(::GetDlgItem(hwnd, IDC_HOOK_OUTPUT_EDIT));
                    context->Options.SupplementalMapPath = GetWindowTextString(::GetDlgItem(hwnd, IDC_HOOK_SUPPLEMENT_EDIT));
                    context->Options.DirsPath.clear();
                    context->Options.FilesPath.clear();
                    ScanLatestHookCandidates(context->Options);
                    RefreshHookDialog(hwnd, context);
                    return 0;
                }
                if (LOWORD(wParam) == IDC_HOOK_START)
                {
                    context->Options.PureHashDirectory = GetWindowTextString(::GetDlgItem(hwnd, IDC_HOOK_PURE_EDIT));
                    context->Options.OutputDirectory = GetWindowTextString(::GetDlgItem(hwnd, IDC_HOOK_OUTPUT_EDIT));
                    context->Options.SupplementalMapPath = GetWindowTextString(::GetDlgItem(hwnd, IDC_HOOK_SUPPLEMENT_EDIT));
                    context->Options.DirsPath = GetWindowTextString(::GetDlgItem(hwnd, IDC_HOOK_DIRS_EDIT));
                    context->Options.FilesPath = GetWindowTextString(::GetDlgItem(hwnd, IDC_HOOK_FILES_EDIT));
                    NormalizeHookDirectories(context->Options);
                    if (SamePathText(context->Options.PureHashDirectory, context->Options.OutputDirectory))
                    {
                        ::MessageBoxW(hwnd, L"纯Hash目录不能和 Hash 输出目录相同。纯Hash目录应选择 Extractor_Output。", L"Cxdec Hook撞库恢复Hash映射", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    if (context->Options.DirsPath.empty() && context->Options.FilesPath.empty())
                    {
                        int choice = ::MessageBoxW(hwnd,
                                                   L"还没有候选表。\r\n\r\n可以点击“从明文资源目录制作候选lst”生成，也可以点击“扫描最新候选”，或者手动浏览选择 dirs/files txt。\r\n\r\n现在要制作候选lst吗？",
                                                   L"Cxdec Hook撞库恢复Hash映射",
                                                   MB_YESNO | MB_ICONQUESTION);
                        if (choice == IDYES)
                        {
                            bool ok = MakeCandidateLists(hwnd, context->Options);
                            RefreshHookDialog(hwnd, context);
                            if (!ok)
                            {
                                return 0;
                            }
                        }
                        else
                        {
                            return 0;
                        }
                    }
                    context->Accepted = true;
                    ::DestroyWindow(hwnd);
                    return 0;
                }
                if (LOWORD(wParam) == IDCANCEL)
                {
                    ::DestroyWindow(hwnd);
                    return 0;
                }
                break;
            case WM_CLOSE:
                ::DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                if (context)
                {
                    if (context->Font)
                    {
                        ::DeleteObject(context->Font);
                        context->Font = nullptr;
                    }
                    if (context->BoldFont)
                    {
                        ::DeleteObject(context->BoldFont);
                        context->BoldFont = nullptr;
                    }
                }
                return 0;
        }
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool ShowHookHashRestoreLaunchDialog(HWND owner, HookHashRestoreLaunchOptions& options)
    {
        options.PureHashDirectory = CombinePathLocal(g_KrkrExeDirectory, L"Extractor_Output");
        options.OutputDirectory = CombinePathLocal(g_KrkrExeDirectory, L"StringHashDumper_Output");
        NormalizeHookDirectories(options);
        ScanLatestHookCandidates(options);

        HookDialogContext context{};
        context.Options = options;

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = HookHashDialogProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        windowClass.lpszClassName = HookHashDialogClassName;
        ::RegisterClassExW(&windowClass);

        HWND hwnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME,
                                      HookHashDialogClassName,
                                      L"Cxdec Hook撞库恢复Hash映射",
                                      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                      CW_USEDEFAULT,
                                      CW_USEDEFAULT,
                                      880,
                                      530,
                                      owner,
                                      nullptr,
                                      windowClass.hInstance,
                                      &context);
        if (!hwnd)
        {
            return false;
        }

        ::EnableWindow(owner, FALSE);
        ::ShowWindow(hwnd, SW_SHOW);
        ::UpdateWindow(hwnd);

        MSG msg{};
        while (::IsWindow(hwnd) && ::GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::EnableWindow(owner, TRUE);
        ::SetForegroundWindow(owner);

        options = context.Options;
        return context.Accepted;
    }

    // ---------- 封包（目录 -> XP3）----------

    constexpr wchar_t RepackDialogClassName[] = L"CxdecRepackWindow";
    constexpr int IDC_REPACK_INPUT_EDIT = 3301;
    constexpr int IDC_REPACK_INPUT_BROWSE = 3302;
    constexpr int IDC_REPACK_EXE_EDIT = 3303;
    constexpr int IDC_REPACK_EXE_BROWSE = 3304;
    constexpr int IDC_REPACK_OUTPUT_EDIT = 3305;
    constexpr int IDC_REPACK_OUTPUT_BROWSE = 3306;
    constexpr int IDC_REPACK_KEYS_EDIT = 3307;
    constexpr int IDC_REPACK_KEYS_BROWSE = 3308;
    constexpr int IDC_REPACK_RESCRAMBLE = 3309;
    constexpr int IDC_REPACK_STATUS = 3310;
    constexpr int IDC_REPACK_START = 3311;
    constexpr int IDC_REPACK_SALT_EDIT = 3312;
    constexpr int IDC_REPACK_HINT_BASE = 3400;
    constexpr int IDC_REPACK_HINT_COUNT = 8;
    constexpr UINT RepackDoneMessage = WM_APP + 1;

    typedef int(__stdcall* RepackSniffFn)(const wchar_t*, int*, char*, int, char*, int);
    typedef int(__stdcall* RepackPackFn)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*,
                                         const wchar_t*, int, int, char*, int, char*, int);
    typedef int(__stdcall* RepackPackExFn)(const wchar_t*, const wchar_t*, const wchar_t*, const wchar_t*,
                                           const wchar_t*, int, int, DWORD, char*, int, char*, int);
    typedef unsigned int(__stdcall* RepackNextRevisionFn)(const wchar_t*);

    struct RepackerApi
    {
        HMODULE Module;
        RepackSniffFn Sniff;
        RepackPackFn Pack;
        RepackPackExFn PackEx;
        RepackNextRevisionFn NextRevision;
    };

    // 只加载一次，之后复用同一份函数指针。
    // 调用方（界面线程）总是在起后台线程之前先调一次，所以不存在并发初始化。
    const RepackerApi* LoadRepackerApi(std::wstring& errorOut)
    {
        static RepackerApi api{};
        static bool tried = false;
        static std::wstring loadError;

        if (!tried)
        {
            tried = true;

            const std::wstring dllPath = GetModuleDllPath(L"CxdecRepacker.dll");
            api.Module = ::LoadLibraryW(dllPath.c_str());
            if (api.Module == nullptr)
            {
                loadError = FormatString(L"无法加载封包模块：\r\n%s", dllPath.c_str());
            }
            else
            {
                api.Sniff = (RepackSniffFn)::GetProcAddress(api.Module, "SniffInputDir");
                api.Pack = (RepackPackFn)::GetProcAddress(api.Module, "Repack");
                api.PackEx = (RepackPackExFn)::GetProcAddress(api.Module, "RepackEx");
                api.NextRevision = (RepackNextRevisionFn)::GetProcAddress(api.Module, "NextPatchRevision");
                if (api.Sniff == nullptr || api.Pack == nullptr)
                {
                    loadError = FormatString(L"封包模块缺少导出接口：\r\n%s", dllPath.c_str());
                }
            }
        }

        if (!loadError.empty())
        {
            errorOut = loadError;
            return nullptr;
        }
        return &api;
    }

    // 导出接口回的是 ANSI
    std::wstring AnsiBufferToString(const char* text)
    {
        if (text == nullptr || text[0] == '\0')
        {
            return std::wstring();
        }
        return Encoding::AnsiToUnicode(std::string(text), Encoding::ACP);
    }

    struct RepackOptions
    {
        std::wstring InputDirectory;
        std::wstring OutputXp3;
        std::wstring ExePath;
        std::wstring KeysRoot;
        // 盐（pathHash/fileHash 的额外输入串）。留空就用参数里带的那个。
        std::wstring MediaName;
        bool Rescramble;
        // 是否允许为了取参数执行游戏内嵌原生代码。默认 false，只有界面在
        // 用户逐次确认后才置 true。
        bool AllowNative = false;
    };

    // 输出默认名：<游戏目录>\patch.xp3。
    //
    // 补丁文件名按发行版不同：**国际版（Hikari Field 等）只认 `patch.xp3`**，
    // 日版那批新框架才要求带修订号的 `patch@r<N>.xp3`（修订号还得压过游戏自己的
    // build 号，否则引擎当它比本体旧直接跳过）。默认按国际版给，日版手改成 @r<N> 即可。
    std::wstring DefaultPatchOutputPath(const std::wstring& exePath)
    {
        const std::wstring gameDirectory = GetParentDirectoryLocal(exePath);
        if (gameDirectory.empty())
        {
            return std::wstring();
        }
        return FormatString(L"%s\\patch.xp3", gameDirectory.c_str());
    }

    struct RepackDialogContext
    {
        RepackOptions Options;
        bool Ready;
        bool Busy;
        HFONT Font;
        HFONT BoldFont;
        HWND StepLabels[4];
    };

    std::wstring BuildRepackStatus(HWND hwnd, RepackDialogContext* context)
    {
        context->Options.InputDirectory = GetWindowTextString(::GetDlgItem(hwnd, IDC_REPACK_INPUT_EDIT));
        context->Options.OutputXp3 = GetWindowTextString(::GetDlgItem(hwnd, IDC_REPACK_OUTPUT_EDIT));
        context->Options.ExePath = GetWindowTextString(::GetDlgItem(hwnd, IDC_REPACK_EXE_EDIT));
        context->Options.KeysRoot = GetWindowTextString(::GetDlgItem(hwnd, IDC_REPACK_KEYS_EDIT));
        context->Options.MediaName = GetWindowTextString(::GetDlgItem(hwnd, IDC_REPACK_SALT_EDIT));
        context->Options.Rescramble = ::IsDlgButtonChecked(hwnd, IDC_REPACK_RESCRAMBLE) == BST_CHECKED;
        context->Ready = false;

        if (context->Options.InputDirectory.empty())
        {
            return std::wstring(L"状态：先选要封包的目录。");
        }

        std::wstring loadError;
        const RepackerApi* api = LoadRepackerApi(loadError);
        if (api == nullptr)
        {
            return L"状态：" + loadError;
        }

        std::wstring status;
        int mode = -1;
        char detail[1024]{};
        char error[1024]{};
        if (api->Sniff(context->Options.InputDirectory.c_str(), &mode, detail, sizeof(detail), error, sizeof(error)))
        {
            context->Ready = !context->Options.OutputXp3.empty();
            status = L"状态：可以封包。\r\n目录形态：";
            status += AnsiBufferToString(detail);
        }
        else
        {
            status = L"状态：这个目录还封不了 —— ";
            status += AnsiBufferToString(error);
        }

        status += L"\r\n输出：";
        status += context->Options.OutputXp3.empty() ? std::wstring(L"还没填（必填）") : context->Options.OutputXp3;
        status += L"\r\n参数：";
        status += context->Options.ExePath.empty()
                      ? std::wstring(L"未指定游戏主程序，会用内置参数")
                      : GetFileNameLocal(context->Options.ExePath);
        if (!context->Options.KeysRoot.empty())
        {
            status += L"\r\n参数仓库：";
            status += context->Options.KeysRoot;
        }
        // 盐错了不报错也不崩，游戏只是当这个包不存在，所以这里必须露出来
        status += L"\r\n盐：";
        status += context->Options.MediaName.empty()
                      ? std::wstring(L"用参数里带的（一般就是 xp3hnp）")
                      : context->Options.MediaName;
        return status;
    }

    void RefreshRepackDialog(HWND hwnd, RepackDialogContext* context)
    {
        if (context == nullptr)
        {
            return;
        }
        ::SetWindowTextW(::GetDlgItem(hwnd, IDC_REPACK_STATUS), BuildRepackStatus(hwnd, context).c_str());
        ::InvalidateRect(::GetDlgItem(hwnd, IDC_REPACK_STATUS), nullptr, TRUE);
    }

    struct RepackJob
    {
        HWND Owner;
        std::wstring InputDirectory;
        std::wstring OutputXp3;
        std::wstring ExePath;
        std::wstring KeysRoot;
        std::wstring MediaName;
        bool Rescramble;
        bool AllowNative = false;
    };
    // 连一个错误串都构造不出来时（内存耗尽）也不能让它再抛一次——
    // 退化成一条不带消息的完成通知，界面那边对 payload == nullptr 有处理，
    // 至少别让对话框一直等下去。
    std::wstring* MakePayloadQuietly(const wchar_t* prefix, const std::string& detail)
    {
        try
        {
            std::wstring text(prefix);
            text += ExportGuard::WideFromUtf8(detail);
            return new std::wstring(std::move(text));
        }
        catch (...)
        {
            return nullptr;
        }
    }

    // 打包可能很久，放后台线程跑；结果通过 RepackDoneMessage 回发，
    // 由窗口负责释放那个 string。
    DWORD WINAPI RepackThreadProc(LPVOID parameter)
    {
        RepackJob* job = (RepackJob*)parameter;

        bool ok = false;
        std::wstring message;
        std::wstring* payload = nullptr;

        // 从 CreateThread 起的线程里漏出去的 C++ 异常会直接送掉整个进程
        // （terminate -> abort -> __fastfail），宿主连错误框都来不及弹。
        // 所以连"构造结果、回发消息"都一起兜住：最需要这套兜底的恰恰是内存耗尽，
        // 而那条路上 new 一个 string 本身就会抛。
        try
        {
            std::wstring loadError;
            const RepackerApi* api = LoadRepackerApi(loadError);
            if (api == nullptr)
            {
                message = loadError;
            }
            else
            {
                // 先嗅探一遍：形态不对就别白打一遍包
                int mode = -1;
                char detail[1024]{};
                char sniffError[2048]{};
                if (!api->Sniff(job->InputDirectory.c_str(), &mode, detail, sizeof(detail),
                                sniffError, sizeof(sniffError)))
                {
                    message = L"封包失败：" + AnsiBufferToString(sniffError);
                }
                else
                {
                    char result[4096]{};
                    char packError[2048]{};
                    const wchar_t* exePath = job->ExePath.empty() ? nullptr : job->ExePath.c_str();
                    const wchar_t* keysRoot = job->KeysRoot.empty() ? nullptr : job->KeysRoot.c_str();
                    const wchar_t* mediaName = job->MediaName.empty() ? nullptr : job->MediaName.c_str();
                    const DWORD flags = job->AllowNative ? 1u : 0u;
                    BOOL packed = FALSE;
                    if (api->PackEx != nullptr)
                    {
                        packed = api->PackEx(job->InputDirectory.c_str(), job->OutputXp3.c_str(), exePath,
                                             keysRoot, mediaName, -1, job->Rescramble ? 1 : 0, flags,
                                             result, sizeof(result), packError, sizeof(packError));
                    }
                    else if (!job->AllowNative)
                    {
                        // 旧模块没有 RepackEx：只允许安全默认（flags=0）路径。
                        packed = api->Pack(job->InputDirectory.c_str(), job->OutputXp3.c_str(), exePath,
                                           keysRoot, mediaName, -1, job->Rescramble ? 1 : 0, result,
                                           sizeof(result), packError, sizeof(packError));
                    }
                    else
                    {
                        strcpy_s(packError, "封包模块版本过旧（缺少 RepackEx），不能执行原生参数派生。");
                    }
                    if (packed)
                    {
                        ok = true;
                        message = L"封包完成。\r\n\r\n";
                        message += AnsiBufferToString(result);
                        message += L"\r\n\r\n输出：" + job->OutputXp3;
                    }
                    else
                    {
                        message = L"封包失败：" + AnsiBufferToString(packError);
                    }
                }
            }

            payload = new std::wstring(message);
        }
        catch (const std::exception& e)
        {
            ok = false;
            payload = MakePayloadQuietly(L"封包时发生内部错误：", e.what());
        }
        catch (...)
        {
            ok = false;
            payload = MakePayloadQuietly(L"封包时发生内部错误：", "未知异常");
        }

        if (::PostMessageW(job->Owner, RepackDoneMessage, ok ? 1u : 0u, (LPARAM)payload) == FALSE)
        {
            delete payload;
        }
        delete job;
        return 0;
    }

    LRESULT CALLBACK RepackDialogProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        RepackDialogContext* context = (RepackDialogContext*)::GetWindowLongPtrW(hwnd, GWLP_USERDATA);

        if (message == RepackDoneMessage)
        {
            std::wstring* payload = (std::wstring*)lParam;
            if (context != nullptr)
            {
                context->Busy = false;
                ::SetWindowTextW(hwnd, L"Cxdec 封包（目录 → XP3）");
                ::EnableWindow(::GetDlgItem(hwnd, IDC_REPACK_START), TRUE);
            }
            if (payload != nullptr)
            {
                ::SetWindowTextW(::GetDlgItem(hwnd, IDC_REPACK_STATUS), payload->c_str());
                ::MessageBoxW(hwnd, payload->c_str(), L"Cxdec 封包",
                              wParam == 0u ? (MB_OK | MB_ICONERROR) : (MB_OK | MB_ICONINFORMATION));
                delete payload;
            }
            return 0;
        }

        switch (message)
        {
            case WM_CREATE:
            {
                CREATESTRUCTW* create = (CREATESTRUCTW*)lParam;
                context = (RepackDialogContext*)create->lpCreateParams;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)context);
                context->Font = CreateLoaderUiFont(hwnd, 9, false);
                context->BoldFont = CreateLoaderUiFont(hwnd, 10, true);
                for (HWND& label : context->StepLabels)
                {
                    label = nullptr;
                }

                const int kEditX = 222;
                const int kEditW = 524;
                const int kBrowseX = 754;
                const int kBrowseW = 94;

                auto MakeStepLabel = [&](int y, const wchar_t* text) -> HWND
                {
                    return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                         16, y, 200, 20, hwnd, nullptr, nullptr, nullptr);
                };
                auto MakeHint = [&](int y, int index, const wchar_t* text)
                {
                    CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                  kEditX, y, 630, 17, hwnd, (HMENU)(INT_PTR)(IDC_REPACK_HINT_BASE + index), nullptr, nullptr);
                };
                auto MakeEdit = [&](int x, int y, int width, int id)
                {
                    CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                    x, y, width, 24, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
                };
                auto MakeBrowse = [&](int y, int id)
                {
                    CreateWindowW(L"BUTTON", L"浏览", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                  kBrowseX, y, kBrowseW, 26, hwnd, (HMENU)(INT_PTR)id, nullptr, nullptr);
                };

                // 第 1 步：要封包的资源目录
                context->StepLabels[0] = MakeStepLabel(16, L"第 1 步 · 要封包的目录（必填）");
                MakeEdit(kEditX, 13, kEditW, IDC_REPACK_INPUT_EDIT);
                MakeBrowse(12, IDC_REPACK_INPUT_BROWSE);
                MakeHint(43, 0, L"解包出来的资源目录；单域 / 多域 / 平铺三种形态会自动判定");

                // 第 2 步：游戏主程序，用来从参数仓库取这套参数
                context->StepLabels[1] = MakeStepLabel(76, L"第 2 步 · 游戏主程序（可选）");
                MakeEdit(kEditX, 73, kEditW, IDC_REPACK_EXE_EDIT);
                MakeBrowse(72, IDC_REPACK_EXE_BROWSE);
                MakeHint(103, 1, L"用来取这套游戏的参数；不填就用内置参数，目标游戏不是它的话结果不会对");

                // 第 3 步：输出 XP3
                context->StepLabels[2] = MakeStepLabel(136, L"第 3 步 · 输出 XP3（必填）");
                MakeEdit(kEditX, 133, kEditW, IDC_REPACK_OUTPUT_EDIT);
                MakeBrowse(132, IDC_REPACK_OUTPUT_BROWSE);
                MakeHint(163, 2, L"默认写到游戏目录下的 patch.xp3；日版那批要改成 patch@rN.xp3");

                // 第 4 步：参数仓库目录，留空走封包模块自己的默认
                context->StepLabels[3] = MakeStepLabel(196, L"第 4 步 · 参数仓库（可选）");
                MakeEdit(kEditX, 193, kEditW, IDC_REPACK_KEYS_EDIT);
                MakeBrowse(192, IDC_REPACK_KEYS_BROWSE);
                MakeHint(223, 3, L"留空表示用封包模块默认的 keys 目录；参数按 EXE 内容摘要存放，可以跨游戏共用");

                // 盐：一般不填。派生的那套不对（非默认 mediaName 的游戏）时才手动指定。
                // 它不是完整的一步，所以不占 StepLabels，跟下面的勾选框一样用普通字号。
                CreateWindowW(L"STATIC", L"盐 / mediaName（可留空）", WS_CHILD | WS_VISIBLE | SS_LEFT,
                              16, 252, 200, 20, hwnd, nullptr, nullptr, nullptr);
                MakeEdit(kEditX, 249, 240, IDC_REPACK_SALT_EDIT);
                CreateWindowW(L"STATIC", L"留空就用参数里带的（一般就是 xp3hnp）", WS_CHILD | WS_VISIBLE | SS_LEFT,
                              kEditX + 252, 252, 380, 17, hwnd,
                              (HMENU)(INT_PTR)(IDC_REPACK_HINT_BASE + 5), nullptr, nullptr);

                CreateWindowW(L"BUTTON", L"把干净文本重新加扰（补丁包通常要勾）", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                              kEditX, 300, kEditW, 22, hwnd, (HMENU)(INT_PTR)IDC_REPACK_RESCRAMBLE, nullptr, nullptr);
                MakeHint(325, 4, L"只对 FF FE 开头的干净文本生效，其它文件原样打进去");

                CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                              16, 352, 832, 96, hwnd, (HMENU)(INT_PTR)IDC_REPACK_STATUS, nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"开始封包", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                              650, 456, 110, 32, hwnd, (HMENU)(INT_PTR)IDC_REPACK_START, nullptr, nullptr);
                CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              768, 456, 90, 32, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);

                ::SetWindowTextW(::GetDlgItem(hwnd, IDC_REPACK_INPUT_EDIT), context->Options.InputDirectory.c_str());
                ::SetWindowTextW(::GetDlgItem(hwnd, IDC_REPACK_EXE_EDIT), context->Options.ExePath.c_str());
                ::SetWindowTextW(::GetDlgItem(hwnd, IDC_REPACK_OUTPUT_EDIT), context->Options.OutputXp3.c_str());
                ::SetWindowTextW(::GetDlgItem(hwnd, IDC_REPACK_KEYS_EDIT), context->Options.KeysRoot.c_str());
                ::SetWindowTextW(::GetDlgItem(hwnd, IDC_REPACK_SALT_EDIT), context->Options.MediaName.c_str());
                ::CheckDlgButton(hwnd, IDC_REPACK_RESCRAMBLE, context->Options.Rescramble ? BST_CHECKED : BST_UNCHECKED);

                ::SendMessageW(hwnd, WM_SETFONT, (WPARAM)context->Font, TRUE);
                ::EnumChildWindows(hwnd, ApplyLoaderFontToChild, (LPARAM)context->Font);
                for (HWND label : context->StepLabels)
                {
                    if (label)
                    {
                        ::SendMessageW(label, WM_SETFONT, (WPARAM)context->BoldFont, TRUE);
                    }
                }
                RefreshRepackDialog(hwnd, context);
                return 0;
            }
            case WM_CTLCOLORSTATIC:
            {
                if (!context)
                {
                    break;
                }
                const int controlId = ::GetDlgCtrlID((HWND)lParam);
                HDC dc = (HDC)wParam;
                ::SetBkMode(dc, TRANSPARENT);
                if (controlId == IDC_REPACK_STATUS)
                {
                    ::SetTextColor(dc, context->Ready ? RGB(0, 110, 40) : RGB(178, 78, 0));
                    return (LRESULT)::GetSysColorBrush(COLOR_WINDOW);
                }
                if (controlId >= IDC_REPACK_HINT_BASE && controlId < IDC_REPACK_HINT_BASE + IDC_REPACK_HINT_COUNT)
                {
                    ::SetTextColor(dc, RGB(110, 110, 110));
                    return (LRESULT)::GetSysColorBrush(COLOR_WINDOW);
                }
                break;
            }
            case WM_COMMAND:
                if (!context)
                {
                    break;
                }
                switch (LOWORD(wParam))
                {
                    case IDC_REPACK_INPUT_BROWSE:
                    {
                        std::wstring folder = BrowseFolder(hwnd, L"选择要封包的资源目录");
                        if (!folder.empty())
                        {
                            ::SetWindowTextW(::GetDlgItem(hwnd, IDC_REPACK_INPUT_EDIT), folder.c_str());
                            RefreshRepackDialog(hwnd, context);
                        }
                        return 0;
                    }
                    case IDC_REPACK_EXE_BROWSE:
                    {
                        std::wstring exe = BrowseExeFile(hwnd, L"选择游戏主程序");
                        if (!exe.empty())
                        {
                            ::SetWindowTextW(::GetDlgItem(hwnd, IDC_REPACK_EXE_EDIT), exe.c_str());
                            // 输出还是空的就顺手填上默认补丁包名
                            if (GetWindowTextString(::GetDlgItem(hwnd, IDC_REPACK_OUTPUT_EDIT)).empty())
                            {
                                const std::wstring defaultOutput = DefaultPatchOutputPath(exe);
                                ::SetWindowTextW(::GetDlgItem(hwnd, IDC_REPACK_OUTPUT_EDIT), defaultOutput.c_str());
                            }
                            RefreshRepackDialog(hwnd, context);
                        }
                        return 0;
                    }
                    case IDC_REPACK_OUTPUT_BROWSE:
                    {
                        const std::wstring current = GetWindowTextString(::GetDlgItem(hwnd, IDC_REPACK_OUTPUT_EDIT));
                        const std::wstring fallback = current.empty()
                                                          ? DefaultPatchOutputPath(GetWindowTextString(::GetDlgItem(hwnd, IDC_REPACK_EXE_EDIT)))
                                                          : current;
                        std::wstring target = BrowseSaveXp3File(hwnd, L"选择输出 XP3", fallback);
                        if (target.empty())
                        {
                            return 0;
                        }
                        // 用户只输了名字、没写扩展名时补上
                        if (target.size() < 4u || _wcsicmp(target.c_str() + target.size() - 4u, L".xp3") != 0)
                        {
                            target += L".xp3";
                        }
                        ::SetWindowTextW(::GetDlgItem(hwnd, IDC_REPACK_OUTPUT_EDIT), target.c_str());
                        RefreshRepackDialog(hwnd, context);
                        return 0;
                    }
                    case IDC_REPACK_KEYS_BROWSE:
                    {
                        std::wstring folder = BrowseFolder(hwnd, L"选择参数仓库目录");
                        if (!folder.empty())
                        {
                            ::SetWindowTextW(::GetDlgItem(hwnd, IDC_REPACK_KEYS_EDIT), folder.c_str());
                            RefreshRepackDialog(hwnd, context);
                        }
                        return 0;
                    }
                    case IDC_REPACK_RESCRAMBLE:
                        RefreshRepackDialog(hwnd, context);
                        return 0;
                    case IDC_REPACK_INPUT_EDIT:
                    case IDC_REPACK_EXE_EDIT:
                    case IDC_REPACK_OUTPUT_EDIT:
                    case IDC_REPACK_KEYS_EDIT:
                    case IDC_REPACK_SALT_EDIT:
                        // 只认失焦：刷新要扫一遍目录，不能跟着每次按键跑
                        if (HIWORD(wParam) == EN_KILLFOCUS)
                        {
                            RefreshRepackDialog(hwnd, context);
                        }
                        return 0;
                    case IDC_REPACK_START:
                    {
                        if (context->Busy)
                        {
                            return 0;
                        }
                        RefreshRepackDialog(hwnd, context);
                        if (context->Options.InputDirectory.empty())
                        {
                            ::MessageBoxW(hwnd, L"先选要封包的目录。", L"Cxdec 封包", MB_OK | MB_ICONWARNING);
                            return 0;
                        }
                        if (context->Options.OutputXp3.empty())
                        {
                            ::MessageBoxW(hwnd, L"先填输出 XP3 的路径。", L"Cxdec 封包", MB_OK | MB_ICONWARNING);
                            return 0;
                        }
                        if (!context->Ready)
                        {
                            ::MessageBoxW(hwnd, L"这个目录还判定不出形态，先按上面的提示改一改。", L"Cxdec 封包", MB_OK | MB_ICONWARNING);
                            return 0;
                        }

                        bool allowNative = false;
                        if (!context->Options.ExePath.empty())
                        {
                            // 仓库里已有该 EXE 参数、或游戏目录旁已有生成的 hxv4p 时，
                            // 这一步不会执行原生代码；否则会调 CxdecKeyStatic 现场派生，
                            // 那会把游戏内嵌 bootstrap DLL 载入本进程执行 —— 必须逐次确认。
                            context->Busy = true;
                            const std::wstring nativeQuestion =
                                L"本次封包指定了游戏 EXE：\r\n" + context->Options.ExePath +
                                L"\r\n\r\n如果参数仓库里没有它的参数、游戏目录旁也没有生成的 hxv4p，"
                                L"工具会现场派生参数 —— 这一步会加载游戏内嵌的 bootstrap DLL，"
                                L"并在本工具进程里执行其中的原生代码。\r\n\r\n"
                                L"· 工具进程拥有当前用户权限；原生代码出错可能使工具崩溃。\r\n"
                                L"· 请只在你合法拥有、且信任来源的游戏副本上执行。\r\n"
                                L"· 这不是沙箱。\r\n\r\n"
                                L"点「是」= 允许在需要时执行（推荐给可信副本）；\r\n"
                                L"点「否」= 只走安全路径：已有参数可继续，缺参数则明确失败并说明需要授权。";
                            allowNative = ::MessageBoxW(hwnd, nativeQuestion.c_str(),
                                                        L"确认是否允许执行游戏内嵌原生代码",
                                                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
                            context->Busy = false;
                        }

                        RepackJob* job = new RepackJob{};
                        job->Owner = hwnd;
                        job->InputDirectory = context->Options.InputDirectory;
                        job->OutputXp3 = context->Options.OutputXp3;
                        job->ExePath = context->Options.ExePath;
                        job->KeysRoot = context->Options.KeysRoot;
                        job->MediaName = context->Options.MediaName;
                        job->Rescramble = context->Options.Rescramble;
                        job->AllowNative = allowNative;

                        DWORD threadId = 0u;
                        HANDLE thread = ::CreateThread(nullptr, 0u, RepackThreadProc, job, 0u, &threadId);
                        if (thread == nullptr)
                        {
                            delete job;
                            ::MessageBoxW(hwnd, L"起不了后台线程。", L"Cxdec 封包", MB_OK | MB_ICONERROR);
                            return 0;
                        }
                        ::CloseHandle(thread);

                        context->Busy = true;
                        ::EnableWindow(::GetDlgItem(hwnd, IDC_REPACK_START), FALSE);
                        ::SetWindowTextW(::GetDlgItem(hwnd, IDC_REPACK_STATUS), L"状态：正在封包，请稍候……");
                        ::SetWindowTextW(hwnd, L"Cxdec 封包（目录 → XP3）- 正在封包");
                        return 0;
                    }
                    case IDCANCEL:
                        if (context != nullptr && context->Busy)
                        {
                            // 与 WM_CLOSE 一致：封包线程还在写盘时不允许绕过去销毁窗口。
                            ::MessageBoxW(hwnd, L"正在封包，等它写完再关。", L"Cxdec 封包",
                                          MB_OK | MB_ICONINFORMATION);
                            return 0;
                        }
                        ::DestroyWindow(hwnd);
                        return 0;
                }
                break;
            case WM_CLOSE:
                if (context != nullptr && context->Busy)
                {
                    ::MessageBoxW(hwnd, L"正在封包，等它写完再关。", L"Cxdec 封包", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                ::DestroyWindow(hwnd);
                return 0;
            case WM_DESTROY:
                if (context)
                {
                    if (context->Font)
                    {
                        ::DeleteObject(context->Font);
                        context->Font = nullptr;
                    }
                    if (context->BoldFont)
                    {
                        ::DeleteObject(context->BoldFont);
                        context->BoldFont = nullptr;
                    }
                }
                return 0;
        }
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    void ShowRepackDialog(HWND owner)
    {
        std::wstring loadError;
        if (LoadRepackerApi(loadError) == nullptr)
        {
            ::MessageBoxW(owner, loadError.c_str(), L"Cxdec 封包", MB_OK | MB_ICONERROR);
            return;
        }

        RepackDialogContext context{};
        context.Ready = false;
        context.Busy = false;
        context.Font = nullptr;
        context.BoldFont = nullptr;
        for (HWND& label : context.StepLabels)
        {
            label = nullptr;
        }

        // Loader 启动时就拿到了游戏主程序，直接带上，省得再选一次
        context.Options.ExePath = g_KrkrExeFullPath;
        const std::wstring extractOutput = CombinePathLocal(g_KrkrExeDirectory, L"Extractor_Output");
        if (DirectoryExistsLocal(extractOutput))
        {
            context.Options.InputDirectory = extractOutput;
        }
        context.Options.OutputXp3 = DefaultPatchOutputPath(context.Options.ExePath);
        context.Options.Rescramble = true;

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.lpfnWndProc = RepackDialogProc;
        windowClass.hInstance = ::GetModuleHandleW(nullptr);
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        windowClass.lpszClassName = RepackDialogClassName;
        ::RegisterClassExW(&windowClass);

        // 控件坐标是**客户区**坐标系，而 CreateWindowExW 收的是窗口尺寸。
        // 非客户区高度随主题 / DPI / 标题栏字体变，本机实测竖向就要吃掉 39px
        //（SM_CYCAPTION 23 + 边框 16），所以不能拿固定值去凑——按需要的客户区反推。
        const int kRepackClientWidth = 880;
        const int kRepackClientHeight = 504;  // 最下面那排按钮底边 488 + 余量
        RECT windowRect = {0, 0, kRepackClientWidth, kRepackClientHeight};
        ::AdjustWindowRectEx(&windowRect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                             WS_EX_DLGMODALFRAME);

        HWND hwnd = ::CreateWindowExW(WS_EX_DLGMODALFRAME,
                                      RepackDialogClassName,
                                      L"Cxdec 封包（目录 → XP3）",
                                      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                      CW_USEDEFAULT,
                                      CW_USEDEFAULT,
                                      windowRect.right - windowRect.left,
                                      windowRect.bottom - windowRect.top,
                                      owner,
                                      nullptr,
                                      windowClass.hInstance,
                                      &context);
        if (!hwnd)
        {
            return;
        }

        ::EnableWindow(owner, FALSE);
        ::ShowWindow(hwnd, SW_SHOW);
        ::UpdateWindow(hwnd);

        MSG msg{};
        while (::IsWindow(hwnd) && ::GetMessageW(&msg, nullptr, 0, 0) > 0)
        {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        ::EnableWindow(owner, TRUE);
        ::SetForegroundWindow(owner);
    }

    void SetLoaderWindowHandleEnv(HWND hwnd)
    {
        // KeyDumper 运行在目标进程里，不能直接持有 loader HWND。
        // 这里通过环境变量传句柄，方便跨进程回发进度消息。
        std::wstring value = std::to_wstring((unsigned long long)(ULONG_PTR)hwnd);
        ::SetEnvironmentVariableW(LoaderIpc::LoaderWindowHandleEnvName, value.c_str());
    }

    void ClearLoaderWindowHandleEnv()
    {
        ::SetEnvironmentVariableW(LoaderIpc::LoaderWindowHandleEnvName, nullptr);
    }

    void SetProgressPercentText(HWND hwnd, unsigned int percent)
    {
        wchar_t text[16]{};
        wsprintfW(text, L"%u%%", percent);
        ::SetWindowTextW(::GetDlgItem(hwnd, IDC_KeyProgressText), text);
    }

    void ShowKeyProgressControls(HWND hwnd, bool visible)
    {
        int showMode = visible ? SW_SHOW : SW_HIDE;
        ::ShowWindow(::GetDlgItem(hwnd, IDC_KeyProgress), showMode);
        ::ShowWindow(::GetDlgItem(hwnd, IDC_KeyProgressText), showMode);
        ::ShowWindow(::GetDlgItem(hwnd, IDC_KeyProgressLabel), showMode);
    }

    void InitializeKeyProgressControls(HWND hwnd)
    {
        HWND progressBar = ::GetDlgItem(hwnd, IDC_KeyProgress);
        if (progressBar)
        {
            ::SendMessageW(progressBar, PBM_SETRANGE, 0u, MAKELPARAM(0, 100));
            ::SendMessageW(progressBar, PBM_SETPOS, 0u, 0u);
        }

        ::SetWindowTextW(::GetDlgItem(hwnd, IDC_KeyProgressLabel), L"提取进度");
        SetProgressPercentText(hwnd, 0u);
    }

    void UpdateKeyProgressUi(HWND hwnd, unsigned int percent, const wchar_t* labelText)
    {
        if (percent > 100u)
        {
            percent = 100u;
        }

        HWND progressBar = ::GetDlgItem(hwnd, IDC_KeyProgress);
        if (progressBar)
        {
            ::SendMessageW(progressBar, PBM_SETPOS, (WPARAM)percent, 0u);
        }

        if (labelText)
        {
            ::SetWindowTextW(::GetDlgItem(hwnd, IDC_KeyProgressLabel), labelText);
        }

        SetProgressPercentText(hwnd, percent);
    }

    // 静态 Key 提取会加载游戏内嵌的 bootstrap DLL 并在本进程里执行其中的原生函数。
    // 这不是沙箱：必须逐次、显式地取得用户同意，默认路径绝不执行。
    void LoaderLog(const wchar_t* text); // 定义在下方日志初始化之后
    bool RunStaticKeyExtraction(HWND hwnd)
    {
        if (g_KrkrExeFullPath.empty())
        {
            ::MessageBoxW(hwnd, L"未指定游戏 EXE。请先在 YuaExtract 主界面里选择目标游戏主程序。", L"YuaExtract", MB_OK | MB_ICONERROR);
            return false;
        }

        std::wstring keyStaticDll = GetModuleDllPath(L"CxdecKeyStatic.dll");
        std::wstring staticOutput = CombinePathLocal(g_KrkrExeDirectory, L"ExtractKey_Output\\Static");
        const std::wstring warning =
            L"即将从目标游戏内嵌资源中提取静态 Key。\r\n\r\n"
            L"目标 EXE：\r\n" + g_KrkrExeFullPath + L"\r\n\r\n"
            L"输出目录：\r\n" + staticOutput + L"\r\n\r\n"
            L"这一步会加载游戏内嵌的 bootstrap DLL，并在本工具进程里执行其中的原生代码。\r\n"
            L"· 工具进程拥有当前用户权限；原生代码出错可能使工具崩溃。\r\n"
            L"· 请只在你合法拥有、且信任来源的游戏副本上执行。\r\n"
            L"· 这不是沙箱：工具无法隔离这段原生代码。\r\n\r\n"
            L"点「是」= 确认并执行；点「否」= 什么都不做（不会创建输出或临时 DLL）。";
        if (::MessageBoxW(hwnd, warning.c_str(), L"确认执行游戏内嵌原生代码",
                          MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        {
            LoaderLog(L"[Loader] Static key extraction cancelled by user (native code not executed)");
            return false;
        }

        HMODULE hKeyStatic = ::LoadLibraryW(keyStaticDll.c_str());
        if (!hKeyStatic)
        {
            ::MessageBoxW(hwnd, FormatString(L"无法加载静态提取模块：\r\n%s", keyStaticDll.c_str()).c_str(), L"YuaExtract", MB_OK | MB_ICONERROR);
            return false;
        }

        typedef BOOL(__stdcall * tExtractKeyEx)(const wchar_t*, const wchar_t*, DWORD, char*, int);
        auto extractKeyEx = (tExtractKeyEx)::GetProcAddress(hKeyStatic, "ExtractKeyEx");
        if (!extractKeyEx)
        {
            // 旧 ABI 的 ExtractKey 默认拒绝原生执行，不能在这里隐式回退。
            ::FreeLibrary(hKeyStatic);
            ::MessageBoxW(hwnd, L"静态提取模块版本过旧（缺少 ExtractKeyEx）。请使用与本程序配套的模块。",
                          L"YuaExtract", MB_OK | MB_ICONERROR);
            return false;
        }

        char errorBuf[2048]{};
        const BOOL ok = extractKeyEx(g_KrkrExeFullPath.c_str(), staticOutput.c_str(), 1u /*allow trusted native*/,
                                     errorBuf, sizeof(errorBuf));
        ::FreeLibrary(hKeyStatic);

        if (ok)
        {
            ::MessageBoxW(hwnd, FormatString(L"静态提取完成。\r\n输出目录：%s", staticOutput.c_str()).c_str(), L"YuaExtract", MB_OK | MB_ICONINFORMATION);
            return true;
        }
        ::MessageBoxA(hwnd, errorBuf[0] ? errorBuf : "Unknown error", "YuaExtract", MB_OK | MB_ICONERROR);
        return false;
    }

    // ---------- 目标 EXE 导入 ----------

    // 加载器日志。
    //
    // 落点统一到 <工具根>\Log\（就是 loader.exe 旁边的 Log 目录），跟解包器、
    // 各 Dumper、UI 写同一处：用户报问题时只要看这一个地方。工具根写不了
    // （装在 Program Files 之类）时由 logdir 退到 %LOCALAPPDATA%\KrkrExtract\Log\，
    // 并把最终位置与原因写在日志开头。
    //
    // 以前是"每写一行就 _wfopen_s 开一次、ANSI 文本模式、没有时间戳"，而且打开失败
    // 完全静默 —— 用户报"日志是空的"时分不清是没出事还是压根没写进去。现在走
    // Common 的 Log::Logger：UTF-8、带锁、追加、带时间戳，并且能查询打开/写入是否成功。
    Log::Logger& LoaderLogger()
    {
        static Log::Logger logger;
        static bool initialized = false;

        if (!initialized)
        {
            initialized = true;

            std::wstring loaderDirectory = Path::GetDirectoryName(g_LoaderFullPath);
            if (loaderDirectory.empty())
            {
                wchar_t self[MAX_PATH]{};
                ::GetModuleFileNameW(nullptr, self, _countof(self));
                loaderDirectory = Path::GetDirectoryName(self);
            }

            const Log::LogDirectory directory = Log::ResolveLogDirectory(loaderDirectory);
            const std::wstring path = Log::LogFilePath(directory, Yua::Host::kLogFileName);
            logger.Open(path.c_str());

            if (logger.IsOpen())
            {
                logger.WriteLine(L"[Loader] log file: %s", path.c_str());

                // 换了地方就得说明白：用户按"工具目录"的老习惯去找会扑空。
                if (directory.fallback)
                {
                    logger.WriteLine(L"[Loader] 主日志目录不可用，已改用：%s", directory.path.c_str());
                    logger.WriteLine(L"[Loader] 主位置：%s（建目录失败，code=%lu）",
                                     directory.primaryPath.c_str(),
                                     static_cast<unsigned long>(directory.primaryError));
                }
            }
            else
            {
                ::OutputDebugStringW(Win32Error::Describe(logger.LastOpenError()).c_str());
            }
        }

        return logger;
    }

    void LoaderLogLevel(Log::Level level, const wchar_t* text)
    {
        ::OutputDebugStringW(text);

        if (LoaderLogger().IsOpen())
        {
            LoaderLogger().WriteLineLevel(level, L"%s", text);
        }
    }

    void LoaderLog(const wchar_t* text)
    {
        LoaderLogLevel(Log::Level::Info, text);
    }

    // 失败点统一走这个：把 Win32 错误码和路径一起写出来。
    //
    // 以前多处只写 "[Loader] FAIL: 某某"，看日志的人无从判断原因（权限？被占用？
    // 路径不存在？）。**必须在失败那次调用之后立刻调用**，中间不要再插别的 API。
    void LoaderLogFailure(const wchar_t* what, const std::wstring& path)
    {
        LoaderLogLevel(Log::Level::Error,
                       Win32Error::FailureLine(what, path, Win32Error::Capture()).c_str());
    }

    // 有界等待一个句柄（remote thread / 预检进程），deadline 覆盖调用方整个阶段。
    // 实现在 YuaExtractHost.h 的 Detail:: 下：headless 测试直接编入同一份实现，
    // 避免"测试测的是另一套等待循环"。
    bool WaitForHandleResponsive(HWND owner, HANDLE handle, ULONGLONG deadline, bool& cancelRequested)
    {
        return Yua::Host::Detail::WaitForHandleResponsive(owner, handle, deadline, cancelRequested);
    }

    bool WaitForOwnedProcessResponsive(HWND owner, HANDLE process, DWORD totalTimeoutMs)
    {
        bool cancelled = false;
        return WaitForHandleResponsive(owner, process, ::GetTickCount64() + totalTimeoutMs, cancelled);
    }

    // 检测并处理 SteamStub 保护壳。
    // 返回真实状态；失败时 error 写明原因。调用方据此决定继续 / 中止，不能把失败当"继续"。
    Yua::Host::PrepareStatus RunSteamStubPrecheck(HWND owner, const std::wstring& exePath,
                                                  std::wstring& error)
    {
        error.clear();
        if (exePath.empty())
        {
            error = L"目标 EXE 为空。";
            return Yua::Host::PrepareStatus::Failed;
        }

        std::wstring loaderDir = Path::GetDirectoryName(g_LoaderFullPath) + L"\\";
        const std::wstring unpackerPath = loaderDir + L"CxdecExtractordll\\CxdecPeUnpacker.dll";
        const std::wstring antiMalformDll = loaderDir + L"CxdecExtractordll\\CxdecAntiMalform.dll";
        HMODULE hUnp = ::LoadLibraryW(unpackerPath.c_str());
        if (!hUnp)
        {
            LoaderLogFailure(L"LoadLibrary CxdecPeUnpacker.dll", unpackerPath);
            error = L"无法加载脱壳模块：" + unpackerPath;
            return Yua::Host::PrepareStatus::Failed;
        }
        struct UnpackerGuard
        {
            HMODULE Module;
            ~UnpackerGuard() { if (Module) ::FreeLibrary(Module); }
        } unpackerGuard{ hUnp };

        auto D = (bool(*)(const wchar_t*))::GetProcAddress(hUnp, "CxdecPeUnpacker_Detect");
        auto P = (bool(*)(const wchar_t*, const wchar_t*))::GetProcAddress(hUnp, "CxdecPeUnpacker_Process");
        auto lastErr = (const wchar_t * (*)())::GetProcAddress(hUnp, "CxdecPeUnpacker_LastError");
        if (D == nullptr || P == nullptr)
        {
            // 默认负例：导出缺失必须在写 steam / 创建进程之前失败。
            error = L"脱壳模块缺少 CxdecPeUnpacker_Detect / Process 导出，已中止（未改动任何游戏文件）。";
            return Yua::Host::PrepareStatus::Failed;
        }
        if (::GetFileAttributesW(antiMalformDll.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            error = L"缺少反篡改注入模块，已中止（未改动任何游戏文件）：" + antiMalformDll;
            return Yua::Host::PrepareStatus::Failed;
        }
        // 只按数据文件映射，不执行 DllMain：确认它确实是一份可被系统 LoadLibrary 的 PE。
        // 这一步必须在任何 steam_api / 工作副本改动之前完成。
        {
            HMODULE probe = ::LoadLibraryExW(antiMalformDll.c_str(), nullptr,
                                             LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
            if (probe == nullptr)
            {
                error = L"反篡改注入模块无法映射（不是有效的 PE DLL），已中止（未改动任何游戏文件）：" +
                        antiMalformDll;
                return Yua::Host::PrepareStatus::Failed;
            }
            ::FreeLibrary(probe);
        }

        auto LogLastError = [&](const wchar_t* prefix)
        {
            if (lastErr == nullptr)
            {
                return;
            }
            const wchar_t* why = lastErr();
            if (why != nullptr && *why != L'\0')
            {
                LoaderLog(FormatString(L"%s%s", prefix, why).c_str());
            }
        };

        LoaderLog(L"[Loader] Checking SteamStub...");
        const bool packed = (D && P && D(exePath.c_str()));

        if (!packed)
        {
            LogLastError(L"[Loader] detect: ");
            LoaderLog(L"[Loader] Not packed - will still inject and patch");
        }
        else
        {
            LoaderLog(L"[Loader] SteamStub detected");
            if (IDYES != ::MessageBoxW(owner,
                                       L"检测到 SteamStub 保护壳，需要脱壳处理。\n\n是 - 脱壳并打补丁\n否 - 退出",
                                       L"检测到保护壳", MB_YESNO | MB_ICONQUESTION))
            {
                LoaderLog(L"[Loader] User cancelled");
                return Yua::Host::PrepareStatus::Cancelled;
            }
            LoaderLog(L"[Loader] User confirmed");
        }

        std::wstring gameDir = Path::GetDirectoryName(exePath) + L"\\";
        std::wstring stem = exePath;
        size_t dot = stem.rfind(L'.');
        if (dot != std::wstring::npos)
        {
            stem = stem.substr(0, dot);
        }
        std::wstring unpacked = stem + L"_unp.exe";
        std::wstring api = gameDir + L"steam_api.dll";
        std::wstring crackedApi = loaderDir + L"CxdecExtractordll\\steamapi_cra\\steam_api.dll";
        std::wstring crackExe = stem + L"_crack.exe";

        // 只有「游戏目录本来就有 steam_api.dll」**且**「破解版 dll 存在」时才替换。
        const bool haveCracked = (::GetFileAttributesW(crackedApi.c_str()) != INVALID_FILE_ATTRIBUTES);
        const bool haveGameApi = (::GetFileAttributesW(api.c_str()) != INVALID_FILE_ATTRIBUTES);

        // 记录本次开始前 _crack.exe 的状态：只有本次真的写/改过它，才能报 Patched。
        WIN32_FILE_ATTRIBUTE_DATA crackedBefore{};
        const bool hadCracked = ::GetFileAttributesExW(crackExe.c_str(), GetFileExInfoStandard, &crackedBefore) != FALSE;

        // 可测试的替换事务：唯一 backup、CREATE_NEW 唯一 staging、MoveFileEx 原子替换；
        // 析构时未 Commit 则回滚，回滚失败保留唯一 backup 并如实写 error。
        Yua::Host::SteamSwap::Transaction steamSwap;
        auto failAfterSwap = [&](const std::wstring& reason) -> Yua::Host::PrepareStatus
        {
            std::wstring rollbackError;
            steamSwap.Rollback(rollbackError);
            error = reason;
            if (!rollbackError.empty())
            {
                error += L" " + rollbackError;
            }
            LoaderLog(FormatString(L"[Loader] %s %s", error.c_str(), rollbackError.c_str()).c_str());
            return Yua::Host::PrepareStatus::Failed;
        };
        // 用户取消（关窗 / WM_QUIT）不是"检测失败"：回滚后按 Cancelled 上报，
        // 界面不会弹一个不必要的错误框。
        auto cancelAfterSwap = [&](const std::wstring& reason) -> Yua::Host::PrepareStatus
        {
            std::wstring rollbackError;
            steamSwap.Rollback(rollbackError);
            error = reason;
            if (!rollbackError.empty())
            {
                error += L" " + rollbackError;
                LoaderLog(FormatString(L"[Loader] %s %s", error.c_str(), rollbackError.c_str()).c_str());
                return Yua::Host::PrepareStatus::Failed;
            }
            LoaderLog(FormatString(L"[Loader] %s", error.c_str()).c_str());
            return Yua::Host::PrepareStatus::Cancelled;
        };

        if (haveCracked && haveGameApi)
        {
            const std::wstring backupHint = api + L".yua-bak-<本次PID/时间戳>";
            const std::wstring question =
                L"本流程会临时替换游戏目录里的 steam_api.dll，以便完成反篡改补丁。\r\n\r\n"
                L"将被修改：\r\n" + api + L"\r\n\r\n"
                L"原始文件会先完整移到一个**本次独有的备份**（形如 " + backupHint +
                L"），替换文件先写入唯一 staging 文件，再原子改名；后续任一步失败会尝试回滚。\r\n\r\n"
                L"确认继续？";
            if (::MessageBoxW(owner, question.c_str(), L"确认替换 steam_api.dll",
                              MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            {
                LoaderLog(L"[Loader] User declined steam_api.dll replacement");
            }
            else
            {
                std::wstring transactionError;
                if (!steamSwap.Begin(api, crackedApi, transactionError))
                {
                    error = L"准备 steam_api.dll 替换事务失败：" + transactionError;
                    return Yua::Host::PrepareStatus::Failed;
                }
                LoaderLog(L"[Loader] steam_api.dll backed up to a unique current backup");
            }
        }
        else
        {
            LoaderLogLevel(Log::Level::Warn,
                           haveGameApi ? L"[Loader] WARNING: cracked steam_api.dll missing, skip swap"
                                       : L"[Loader] no steam_api.dll in game dir, skip swap");
        }

        if (packed)
        {
            LoaderLog(L"[Loader] Unpacking...");
            // 先清掉可能残留的旧工作副本：脱壳函数即便返回 true，也不能让上一次的
            // 旧文件冒充本次产物。
            if (::GetFileAttributesW(unpacked.c_str()) != INVALID_FILE_ATTRIBUTES &&
                !::DeleteFileW(unpacked.c_str()))
            {
                LoaderLogFailure(L"DeleteFile 旧脱壳工作副本", unpacked);
                return failAfterSwap(L"旧工作副本删不掉，无法安全生成新副本。");
            }
            if (!P(exePath.c_str(), unpacked.c_str()))
            {
                LoaderLogLevel(Log::Level::Error, L"[Loader] FAIL: unpack");
                LogLastError(L"[Loader] unpack: ");
                return failAfterSwap(L"脱壳失败，未产出工作副本。");
            }
        }
        else
        {
            LoaderLog(L"[Loader] Copying working copy...");
            if (::GetFileAttributesW(unpacked.c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                if (!::DeleteFileW(unpacked.c_str()))
                {
                    LoaderLogFailure(L"DeleteFile 旧工作副本", unpacked);
                    return failAfterSwap(L"旧工作副本删不掉，无法安全复制新副本。");
                }
            }
            if (!::CopyFileW(exePath.c_str(), unpacked.c_str(), FALSE))
            {
                LoaderLogFailure(L"CopyFile 工作副本", unpacked);
                return failAfterSwap(L"复制工作副本失败。");
            }
        }
        if (::GetFileAttributesW(unpacked.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            LoaderLog(L"[Loader] FAIL: working copy missing after unpack/copy");
            return failAfterSwap(L"没有生成本次补丁工作副本，已中止。");
        }

        LoaderLog(L"[Loader] Injecting...");
        std::wstring dll = antiMalformDll;
        std::wstring cmd = L"\"" + unpacked + L"\"";
        std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
        cmdline.push_back(L'\0');

        // 预检总 deadline：remote thread 加载 + 主进程执行共用一个 60s 预算。
        const ULONGLONG deadline = ::GetTickCount64() + 60000ull;
        bool cancelRequested = false;

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!::CreateProcessW(unpacked.c_str(), cmdline.data(), nullptr, nullptr, FALSE,
                              CREATE_SUSPENDED, nullptr, gameDir.c_str(), &si, &pi))
        {
            LoaderLogFailure(L"CreateProcess", unpacked);
            ::DeleteFileW(unpacked.c_str());
            return failAfterSwap(L"创建补丁工作进程失败。");
        }
        LoaderLog(L"[Loader] Process created suspended");

        struct ProcessGuard
        {
            PROCESS_INFORMATION* Info;
            ~ProcessGuard()
            {
                if (Info->hThread)
                {
                    ::CloseHandle(Info->hThread);
                    Info->hThread = nullptr;
                }
                if (Info->hProcess)
                {
                    ::CloseHandle(Info->hProcess);
                    Info->hProcess = nullptr;
                }
            }
        } processGuard{ &pi };

        auto terminateOwnedChildAndWait = [&]()
        {
            if (pi.hProcess)
            {
                ::TerminateProcess(pi.hProcess, 1u);
                ::WaitForSingleObject(pi.hProcess, 5000u);
            }
        };

        uint8_t detourData[0x678] = {};
        *(uint32_t*)detourData = 0x678;
        if (CreateDetourSection(pi.hProcess, detourData, sizeof(detourData)) == nullptr)
        {
            // 远端 .detour 载荷写不进去，后续注入注定失败：立刻停掉本次自建 child，
            // 不继续分配/写注入参数，也不把它报成成功。
            LoaderLog(L"[Loader] FAIL: CreateDetourSection");
            terminateOwnedChildAndWait();
            ::DeleteFileW(unpacked.c_str());
            return failAfterSwap(L"无法在补丁工作进程中写入 .detour 节，已中止。");
        }

        bool injectionFailed = false;
        std::wstring injectionError;
        SIZE_T bytes = (dll.length() + 1) * sizeof(wchar_t);
        LPVOID remote = ::VirtualAllocEx(pi.hProcess, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        HANDLE remoteThread = nullptr;
        bool remoteThreadCreated = false;
        bool remoteThreadExited = false;
        if (remote)
        {
            if (::WriteProcessMemory(pi.hProcess, remote, dll.c_str(), bytes, nullptr))
            {
                auto* loadLibrary = (LPTHREAD_START_ROUTINE)::GetProcAddress(
                    ::GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
                remoteThread = loadLibrary != nullptr
                    ? ::CreateRemoteThread(pi.hProcess, nullptr, 0, loadLibrary, remote, 0, nullptr)
                    : nullptr;
                remoteThreadCreated = remoteThread != nullptr;
                if (remoteThread)
                {
                    // remote 必须在等待结束、确认线程退出后才允许释放参数内存。
                    if (!WaitForHandleResponsive(owner, remoteThread, deadline, cancelRequested))
                    {
                        injectionFailed = true;
                        injectionError = cancelRequested ? L"用户请求取消预检"
                                                         : L"等待远程加载线程超时";
                    }
                    else
                    {
                        remoteThreadExited = true;
                        DWORD moduleHandle = 0;
                        if (!::GetExitCodeThread(remoteThread, &moduleHandle) || moduleHandle == 0)
                        {
                            injectionFailed = true;
                            injectionError = L"注入模块没有成功加载（LoadLibraryW 返回空）";
                        }
                    }
                }
                else
                {
                    LoaderLogFailure(L"CreateRemoteThread 加载注入模块", dll);
                    injectionFailed = true;
                    injectionError = L"创建远程线程失败";
                }
            }
            else
            {
                LoaderLogFailure(L"WriteProcessMemory 写入注入参数", dll);
                injectionFailed = true;
                injectionError = L"写入注入参数失败";
            }
        }
        else
        {
            LoaderLogFailure(L"VirtualAllocEx 分配注入参数", dll);
            injectionFailed = true;
            injectionError = L"分配注入参数内存失败";
        }

        if (injectionFailed)
        {
            terminateOwnedChildAndWait();
            if (remoteThread)
            {
                if (::WaitForSingleObject(remoteThread, 5000u) == WAIT_OBJECT_0)
                {
                    remoteThreadExited = true;
                }
                ::CloseHandle(remoteThread);
                remoteThread = nullptr;
            }
            // 远程线程尚未确认退出时绝不释放它的参数内存（进程已终止也按最保守处理）。
            if (remote && (!remoteThreadCreated || remoteThreadExited))
            {
                ::VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
            }
            ::DeleteFileW(unpacked.c_str());
            if (cancelRequested)
            {
                return cancelAfterSwap(L"用户请求取消预检，已终止本次工作副本。");
            }
            return failAfterSwap(L"注入反篡改模块失败；已终止本次预检工作进程：" + injectionError);
        }
        if (remoteThread)
        {
            remoteThreadExited = true; // 上面的成功等待已确认线程退出
            ::CloseHandle(remoteThread);
            remoteThread = nullptr;
        }
        if (remote)
        {
            ::VirtualFreeEx(pi.hProcess, remote, 0, MEM_RELEASE);
        }

        if (::ResumeThread(pi.hThread) == static_cast<DWORD>(-1))
        {
            LoaderLogFailure(L"ResumeThread", unpacked);
            terminateOwnedChildAndWait();
            ::DeleteFileW(unpacked.c_str());
            return failAfterSwap(L"恢复预检工作进程失败。");
        }
        LoaderLog(L"[Loader] Process resumed");

        if (!WaitForHandleResponsive(owner, pi.hProcess, deadline, cancelRequested))
        {
            LoaderLog(L"[Loader] Precheck timed out or cancelled; terminating its own working copy");
            terminateOwnedChildAndWait();
            ::DeleteFileW(unpacked.c_str());
            if (cancelRequested)
            {
                return cancelAfterSwap(L"用户请求取消预检，已终止本次工作副本。");
            }
            return failAfterSwap(L"反篡改预检超过 60 秒未结束，已终止本次工作副本。");
        }

        DWORD exitCode = 0;
        if (!::GetExitCodeProcess(pi.hProcess, &exitCode))
        {
            const DWORD code = ::GetLastError();
            terminateOwnedChildAndWait();
            ::DeleteFileW(unpacked.c_str());
            return failAfterSwap(L"读取预检进程退出码失败（错误码 " + std::to_wstring(code) + L"）。");
        }
        ::CloseHandle(pi.hProcess);
        pi.hProcess = nullptr;
        ::DeleteFileW(unpacked.c_str());

        if (exitCode == 2)
        {
            LoaderLog(L"[Loader] Nothing to patch");
            std::wstring rollbackError;
            if (!steamSwap.Rollback(rollbackError))
            {
                error = L"无需补丁，但恢复原 steam_api.dll 失败：" + rollbackError;
                return Yua::Host::PrepareStatus::Failed;
            }
            return Yua::Host::PrepareStatus::Ready;
        }
        if (exitCode == 3)
        {
            LoaderLog(L"[Loader] Patch could not be applied");
            return failAfterSwap(L"检测到需要补丁，但补丁没有应用成功（详见日志）。");
        }
        if (exitCode != 0)
        {
            return failAfterSwap(L"预检进程异常退出（退出码 " + std::to_wstring(exitCode) + L"），不视为成功。");
        }

        WIN32_FILE_ATTRIBUTE_DATA crackedAfter{};
        const bool hasCrackedNow =
            ::GetFileAttributesExW(crackExe.c_str(), GetFileExInfoStandard, &crackedAfter) != FALSE;
        const bool crackedIsFresh =
            hasCrackedNow &&
            (!hadCracked ||
             ::CompareFileTime(&crackedAfter.ftLastWriteTime, &crackedBefore.ftLastWriteTime) > 0);
        if (!crackedIsFresh)
        {
            // 旧 _crack.exe 仍然在盘上，不能拿它当本次成功。
            LoaderLog(L"[Loader] No fresh _crack.exe produced; not treating as success");
            return failAfterSwap(L"补丁流程结束，但本次没有产出新的 _crack.exe；不会沿用旧文件。");
        }

        // 本次确实产出了新 _crack.exe：保留备份（原 steam_api.dll 仍可由用户找回）。
        steamSwap.Commit();
        LoaderLog(FormatString(L"[Loader] Anti-malform patch done, cracked exe: %s", crackExe.c_str()).c_str());
        return Yua::Host::PrepareStatus::Patched;
    }

    std::wstring BrowseForExe(HWND owner)
    {
        std::wstring result;
        HRESULT coInit = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

        IFileOpenDialog* dialog = nullptr;
        if (SUCCEEDED(::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) && dialog)
        {
            DWORD options = 0u;
            if (SUCCEEDED(dialog->GetOptions(&options)))
            {
                dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_FILEMUSTEXIST);
            }
            dialog->SetTitle(L"选择游戏主程序");

            COMDLG_FILTERSPEC filters[] =
            {
                { L"可执行文件", L"*.exe" },
                { L"所有文件", L"*.*" }
            };
            dialog->SetFileTypes(_countof(filters), filters);

            if (SUCCEEDED(dialog->Show(owner)))
            {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item)) && item)
                {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
                    {
                        result = path;
                        ::CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }

        if (SUCCEEDED(coInit))
        {
            ::CoUninitialize();
        }
        return result;
    }

    // 子控件默认不接收 WM_DROPFILES，会把拖放挡掉。
    // 让每个子控件也接受拖放并原样转交给对话框，实现"窗口任意位置都能放下"。
    LRESULT CALLBACK ForwardDropToParent(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                         UINT_PTR /*idSubclass*/, DWORD_PTR refData)
    {
        if (msg == WM_DROPFILES)
        {
            ::SendMessageW((HWND)refData, WM_DROPFILES, wParam, lParam);
            return 0;
        }
        return ::DefSubclassProc(hwnd, msg, wParam, lParam);
    }

    void EnableDropOnWindowAndChildren(HWND hwnd)
    {
        ::DragAcceptFiles(hwnd, TRUE);
        for (HWND child = ::GetWindow(hwnd, GW_CHILD); child; child = ::GetWindow(child, GW_HWNDNEXT))
        {
            ::DragAcceptFiles(child, TRUE);
            ::SetWindowSubclass(child, ForwardDropToParent, 0u, (DWORD_PTR)hwnd);
        }
    }

    // 校验拖入/选中的路径能不能当游戏主程序用。
    bool IsUsableGameExe(HWND owner, const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }

        DWORD attributes = ::GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY))
        {
            ::MessageBoxW(owner, FormatString(L"无法读取该文件：\r\n%s", path.c_str()).c_str(),
                          L"YuaExtract", MB_OK | MB_ICONERROR);
            return false;
        }

        if (_wcsicmp(Path::GetExtension(path).c_str(), L".exe") != 0)
        {
            ::MessageBoxW(owner, L"请拖入游戏主程序（.exe 文件）。",
                          L"YuaExtract", MB_OK | MB_ICONWARNING);
            return false;
        }

        if (!g_LoaderFullPath.empty() && _wcsicmp(path.c_str(), g_LoaderFullPath.c_str()) == 0)
        {
            ::MessageBoxW(owner, L"这是启动器自身，请选择游戏主程序。",
                          L"YuaExtract", MB_OK | MB_ICONWARNING);
            return false;
        }

        return true;
    }

    // 起进程 + 注入一个模块 DLL。
    //
    // 三条入口都走这一段（新主界面的步骤1、步骤2，以及原版功能窗口），
    // 免得以后"只在一处修了注入"。用 CREATE_SUSPENDED + CreateRemoteThread 而不是
    // DetourCreateProcessWithDllW，是为了避开 ANSI 编码在日文路径下损坏 DLL 路径的老问题。
    //
    // 注入失败时**仍然让游戏跑起来**（与上游一致）：游戏本身没问题，失败的只是"模块没进去"，
    // 怎么提示交给调用方。返回 false 时 errorOut 是给用户看的原因，细节在日志里。
    bool StartGameAndInjectDll(HWND owner, const std::wstring& injectDllFullPath, std::wstring& errorOut)
    {
        errorOut.clear();

        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};

        if (!CreateProcessW(g_KrkrExeFullPath.c_str(), NULL, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL,
                            g_KrkrExeDirectory.c_str(), &si, &pi))
        {
            LoaderLogFailure(L"CreateProcess", g_KrkrExeFullPath);
            errorOut = FormatString(L"创建进程失败。\r\n\r\n游戏：%s\r\n工作目录：%s\r\n\r\n详见日志。",
                                    g_KrkrExeFullPath.c_str(), g_KrkrExeDirectory.c_str());
            return false;
        }

        // 只终止**本函数刚创建的这个**阻塞中的子进程；绝不碰用户已有的游戏进程。
        auto abandonChild = [&pi](const wchar_t* reason)
        {
            // 只终止本次新建的挂起 child，并等它真正退出后才收句柄。
            ::TerminateProcess(pi.hProcess, 1u);
            ::WaitForSingleObject(pi.hProcess, 5000u);
            ::CloseHandle(pi.hThread);
            ::CloseHandle(pi.hProcess);
            pi.hThread = nullptr;
            pi.hProcess = nullptr;
            LoaderLog(reason);
        };

        bool injected = false;
        std::wstring failureDetail;
        const SIZE_T pathBytes = (injectDllFullPath.length() + 1) * sizeof(wchar_t);
        LPVOID pRemote = ::VirtualAllocEx(pi.hProcess, NULL, pathBytes, MEM_COMMIT | MEM_RESERVE,
                                          PAGE_READWRITE);
        bool remoteMemorySafeToFree = false;
        if (pRemote)
        {
            SIZE_T written = 0;
            if (::WriteProcessMemory(pi.hProcess, pRemote, injectDllFullPath.c_str(), pathBytes, &written) &&
                written == pathBytes)
            {
                auto* loadLibrary = (LPTHREAD_START_ROUTINE)::GetProcAddress(
                    ::GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
                HANDLE remote = loadLibrary != nullptr
                                    ? ::CreateRemoteThread(pi.hProcess, NULL, 0, loadLibrary, pRemote, 0, NULL)
                                    : nullptr;
                if (remote)
                {
                    bool cancelRequested = false;
                    const bool loaded = WaitForHandleResponsive(
                        owner, remote, ::GetTickCount64() + 30000ull, cancelRequested);
                    bool remoteExited = loaded;
                    if (!loaded)
                    {
                        // remote 未退出前不释放参数内存：先终止本次 child 并等线程退出。
                        ::TerminateProcess(pi.hProcess, 1u);
                        ::WaitForSingleObject(pi.hProcess, 5000u);
                        remoteExited = ::WaitForSingleObject(remote, 5000u) == WAIT_OBJECT_0;
                    }
                    DWORD moduleHandle = 0;
                    const bool gotExit = loaded && ::GetExitCodeThread(remote, &moduleHandle) != FALSE;
                    ::CloseHandle(remote);
                    // 只有确认远程线程已经退出，才允许释放它正在读取的参数内存。
                    remoteMemorySafeToFree = remoteExited;
                    if (loaded && gotExit && moduleHandle != 0)
                    {
                        injected = true;
                    }
                    else
                    {
                        LoaderLog(L"[Loader] FAIL: injected module did not load (null/exit or timeout)");
                        failureDetail = cancelRequested
                                            ? L"用户请求取消启动"
                                            : L"注入模块没有成功加载（LoadLibraryW 返回空或等待超时）";
                    }
                }
                else
                {
                    // CreateRemoteThread 没成功 = 没有线程在读参数内存，可以安全释放。
                    remoteMemorySafeToFree = true;
                    LoaderLogFailure(L"CreateRemoteThread 加载注入模块", injectDllFullPath);
                    failureDetail = L"创建远程线程失败";
                }
            }
            else
            {
                remoteMemorySafeToFree = true;
                LoaderLogFailure(L"WriteProcessMemory 写入注入参数", injectDllFullPath);
                failureDetail = L"写入注入参数失败";
            }
            if (remoteMemorySafeToFree)
            {
                ::VirtualFreeEx(pi.hProcess, pRemote, 0, MEM_RELEASE);
            }
            else
            {
                // 进程已被终止但远程线程未在窗口内退出：宁可泄漏这块远端内存，
                // 也不能释放线程还在使用的参数（进程退出时系统会整体回收）。
                LoaderLog(L"[Loader] remote thread did not exit; skip VirtualFreeEx on its parameter block");
            }
        }
        else
        {
            LoaderLogFailure(L"VirtualAllocEx 分配注入参数", injectDllFullPath);
            failureDetail = L"分配注入参数内存失败";
        }

        if (!injected)
        {
            abandonChild(L"[Loader] Injection failed; terminated the suspended child we created");
            errorOut = L"游戏进程未被启动：模块注入失败。\r\n\r\n模块：" + injectDllFullPath +
                       L"\r\n原因：" + (failureDetail.empty() ? std::wstring(L"详见日志") : failureDetail);
            return false;
        }

        if (::ResumeThread(pi.hThread) == static_cast<DWORD>(-1))
        {
            LoaderLogFailure(L"ResumeThread", g_KrkrExeFullPath);
            abandonChild(L"[Loader] ResumeThread failed; terminated the suspended child we created");
            errorOut = L"游戏进程未被启动：恢复主线程失败。\r\n\r\n详见日志。";
            return false;
        }
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return true;
    }
}

INT_PTR CALLBACK LoaderDialogWindProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == LoaderIpc::ProgressMessage())
    {
        // 目标进程用 RegisterWindowMessage 回传百分比，loader 只负责显示。
        ShowKeyProgressControls(hwnd, true);
        UpdateKeyProgressUi(hwnd, (unsigned int)wParam, lParam == 1 ? L"撞库进度" : L"提取进度");
        return TRUE;
    }

    if (msg == LoaderIpc::CompletedMessage())
    {
        ShowKeyProgressControls(hwnd, true);
        UpdateKeyProgressUi(hwnd, 100u, lParam == 1 ? L"撞库完成" : L"提取完成");
        ::MessageBoxW(hwnd,
                      lParam == 1 ? L"撞库完成，恢复映射表已写入 Hash 输出目录。" : L"提取完成，请查看目录。",
                      L"YuaExtract",
                      MB_OK | MB_ICONINFORMATION);
        ::PostMessageW(hwnd, WM_CLOSE, 0u, 0u);
        return TRUE;
    }

    switch (msg)
    {
        case WM_INITDIALOG:
        {
            InitializeKeyProgressControls(hwnd);
            ShowKeyProgressControls(hwnd, false);
            return TRUE;
        }
        case WM_COMMAND:
        {
            std::wstring injectDllFileName;
            std::wstring runtimeHashTargetDirectory;
            HookHashRestoreLaunchOptions hookHashOptions;
            bool hasHookHashOptions = false;
            bool shouldCloseLoaderAfterLaunch = true;

            switch (LOWORD(wParam))
            {
                case IDC_Extractor:
                    injectDllFileName = L"CxdecExtractorUI.dll";
                    break;
                case IDC_StringDumper:
                    runtimeHashTargetDirectory = BrowseFolder(hwnd, L"选择需要恢复的纯Hash目录");
                    if (runtimeHashTargetDirectory.empty())
                    {
                        return TRUE;
                    }
                    ::SetEnvironmentVariableW(RuntimeHashTargetDirectoryEnvName, runtimeHashTargetDirectory.c_str());
                    injectDllFileName = L"CxdecStringDumper.dll";
                    break;
                case IDC_KeyStatic:
                    // 只做静态提取；运行时动态提取已废弃，不再注入 CxdecKeyDumper.dll。
                    RunStaticKeyExtraction(hwnd);
                    break;
                case IDC_Repack:
                    // 封包只在本进程里跑，不进游戏，所以不设 injectDllFileName。
                    ShowRepackDialog(hwnd);
                    break;
                case IDC_HashRestore:
                    if (!ShowHookHashRestoreLaunchDialog(hwnd, hookHashOptions))
                    {
                        return TRUE;
                    }
                    hasHookHashOptions = true;
                    injectDllFileName = L"CxdecHashRestore.dll";
                    shouldCloseLoaderAfterLaunch = false;
                    break;
            }

            if (!injectDllFileName.empty())
            {
                std::wstring injectDllFullPath = GetModuleDllPath(injectDllFileName);
                if (!FileExistsLocal(injectDllFullPath))
                {
                    ::MessageBoxW(hwnd,
                                  FormatString(L"找不到模块 DLL：\r\n%s\r\n\r\n请确认发布结构为：\r\nYuaExtract.exe\r\nCxdecExtractordll\\%s",
                                               injectDllFullPath.c_str(),
                                               injectDllFileName.c_str()).c_str(),
                                  L"YuaExtract",
                                  MB_OK | MB_ICONERROR);
                    return TRUE;
                }
                // 注入原语统一在 StartGameAndInjectDll 里，新主界面走的是同一段。
                if (!shouldCloseLoaderAfterLaunch)
                {
                    SetLoaderWindowHandleEnv(hwnd);
                }
                if (hasHookHashOptions)
                {
                    ::SetEnvironmentVariableW(HashCrackOutputDirectoryEnvName, hookHashOptions.OutputDirectory.c_str());
                    ::SetEnvironmentVariableW(HashCrackDirsFileEnvName, hookHashOptions.DirsPath.empty() ? nullptr : hookHashOptions.DirsPath.c_str());
                    ::SetEnvironmentVariableW(HashCrackFilesFileEnvName, hookHashOptions.FilesPath.empty() ? nullptr : hookHashOptions.FilesPath.c_str());
                    ::SetEnvironmentVariableW(HashCrackPureHashDirectoryEnvName, hookHashOptions.PureHashDirectory.empty() ? nullptr : hookHashOptions.PureHashDirectory.c_str());
                    ::SetEnvironmentVariableW(HashCrackSupplementalMapEnvName, hookHashOptions.SupplementalMapPath.empty() ? nullptr : hookHashOptions.SupplementalMapPath.c_str());
                    ::SetEnvironmentVariableW(HashCrackSuppressRestoreUiEnvName, L"1");
                }

                std::wstring launchError;
                if (StartGameAndInjectDll(hwnd, injectDllFullPath, launchError))
                {
                    if (!runtimeHashTargetDirectory.empty())
                    {
                        ::SetEnvironmentVariableW(RuntimeHashTargetDirectoryEnvName, nullptr);
                    }
                    if (hasHookHashOptions)
                    {
                        ::SetEnvironmentVariableW(HashCrackOutputDirectoryEnvName, nullptr);
                        ::SetEnvironmentVariableW(HashCrackDirsFileEnvName, nullptr);
                        ::SetEnvironmentVariableW(HashCrackFilesFileEnvName, nullptr);
                        ::SetEnvironmentVariableW(HashCrackPureHashDirectoryEnvName, nullptr);
                        ::SetEnvironmentVariableW(HashCrackSupplementalMapEnvName, nullptr);
                        ::SetEnvironmentVariableW(HashCrackSuppressRestoreUiEnvName, nullptr);
                    }

                    if (shouldCloseLoaderAfterLaunch)
                    {
                        ::PostMessageW(hwnd, WM_CLOSE, 0u, 0u);
                    }
                    else
                    {
                        // 异步模式下禁止重复点击，避免多个目标进程同时回报到同一个 loader。
                        ::EnableWindow(::GetDlgItem(hwnd, IDC_Extractor), FALSE);
                        ::EnableWindow(::GetDlgItem(hwnd, IDC_StringDumper), FALSE);
                        ::EnableWindow(::GetDlgItem(hwnd, IDC_HashRestore), FALSE);
                        ShowKeyProgressControls(hwnd, true);
                        UpdateKeyProgressUi(hwnd, 0u, hasHookHashOptions ? L"等待撞库开始" : L"等待提取开始");
                        ::SetWindowTextW(hwnd, hasHookHashOptions ? L"YuaExtract - 等待Hook撞库完成" : L"YuaExtract - 等待Key提取完成");
                    }
                }
                else
                {
                    if (!runtimeHashTargetDirectory.empty())
                    {
                        ::SetEnvironmentVariableW(RuntimeHashTargetDirectoryEnvName, nullptr);
                    }
                    if (hasHookHashOptions)
                    {
                        ::SetEnvironmentVariableW(HashCrackOutputDirectoryEnvName, nullptr);
                        ::SetEnvironmentVariableW(HashCrackDirsFileEnvName, nullptr);
                        ::SetEnvironmentVariableW(HashCrackFilesFileEnvName, nullptr);
                        ::SetEnvironmentVariableW(HashCrackPureHashDirectoryEnvName, nullptr);
                        ::SetEnvironmentVariableW(HashCrackSupplementalMapEnvName, nullptr);
                        ::SetEnvironmentVariableW(HashCrackSuppressRestoreUiEnvName, nullptr);
                    }
                    if (!shouldCloseLoaderAfterLaunch)
                    {
                        ClearLoaderWindowHandleEnv();
                    }
                    ::MessageBoxW(hwnd, launchError.c_str(), L"错误", MB_OK | MB_ICONERROR);
                }
            }
            return TRUE;
        }
        case WM_CLOSE:
        {
            ::DestroyWindow(hwnd);
            return TRUE;
        }
        case WM_DESTROY:
        {
            ClearLoaderWindowHandleEnv();
            if (hwnd == g_LegacyWindow)
            {
                // 句柄失效后 RunLegacyAction 会重建窗口，不能留着一个“看起来还开着”的旧值。
                g_LegacyWindow = nullptr;
            }
            // 由 YuaExtract 主界面按需打开时，关掉这个窗口只是关掉一个辅助窗口，
            // 不能把整个消息循环带走（否则主界面会跟着消失）。
            if (g_LegacyDialogOwnsApp)
            {
                ::PostQuitMessage(0);
            }
            return TRUE;
        }
    }

    return FALSE;
}

// ============================================================================
// YuaExtract 主界面用的桥。实现在本文件（而不是单独一个 cpp）的原因：
// 上面那些 helper 都在本文件的匿名命名空间里，只有这里够得着。
// 这一层刻意做得很薄 —— 只转发，不掺业务逻辑，方便日后上游更新时对照。
// ============================================================================
namespace Yua
{
namespace Host
{
    std::wstring LoaderDirectory()
    {
        return g_LoaderCurrentDirectory;
    }

    std::wstring LogDirectory()
    {
        return Log::ResolveLogDirectory(g_LoaderCurrentDirectory).path;
    }

    std::wstring ResolveModulePath(const std::wstring& dllFileName)
    {
        return GetModuleDllPath(dllFileName);
    }

    bool ModuleExists(const std::wstring& dllFileName)
    {
        return FileExistsLocal(GetModuleDllPath(dllFileName));
    }

    std::wstring ModuleMissingHint(const std::wstring& dllFileName)
    {
        return FormatString(
            L"找不到模块 DLL：\r\n%s\r\n\r\n请确认发布结构为：\r\nYuaExtract.exe\r\nCxdecExtractordll\\%s",
            GetModuleDllPath(dllFileName).c_str(), dllFileName.c_str());
    }

    std::wstring TargetExe()
    {
        return g_KrkrExeFullPath;
    }

    std::wstring TargetDirectory()
    {
        return g_KrkrExeDirectory;
    }

    void SetTargetExe(const std::wstring& exePath)
    {
        g_KrkrExeFullPath = exePath;
        g_KrkrExeDirectory = exePath.empty() ? std::wstring() : Path::GetDirectoryName(exePath);
    }

    bool ValidateGameExe(HWND owner, const std::wstring& path)
    {
        return IsUsableGameExe(owner, path);
    }

    bool PickGameExe(HWND owner, std::wstring& chosen)
    {
        const std::wstring path = BrowseForExe(owner);
        if (path.empty() || !IsUsableGameExe(owner, path))
        {
            return false;
        }
        chosen = path;
        return true;
    }

    std::wstring BrowseForFolder(HWND owner, const wchar_t* title)
    {
        return BrowseFolder(owner, title);
    }

    PrepareStatus PrepareTargetExe(HWND owner, const std::wstring& exePath, std::wstring& error)
    {
        // 预检内部有 Steam API 替换事务（RAII 回滚）。任何 std::bad_alloc 之类从
        // 事务之后抛出时，栈展开会先执行 Transaction 析构完成回滚；这里再把异常
        // 变成明确的 Failed，绝不让它穿进窗口过程。
        try
        {
            return RunSteamStubPrecheck(owner, exePath, error);
        }
        catch (const std::exception& exception)
        {
            error = L"反篡改预检流程异常，已中止（替换事务按 RAII 回滚）：";
            error += Encoding::AnsiToUnicode(std::string(exception.what()), Encoding::ACP);
            return PrepareStatus::Failed;
        }
        catch (...)
        {
            error = L"反篡改预检流程发生未知异常，已中止（替换事务按 RAII 回滚）。";
            return PrepareStatus::Failed;
        }
    }

    std::wstring CrackedExePath(const std::wstring& exePath)
    {
        if (exePath.empty())
        {
            return std::wstring();
        }

        // 与 CxdecAntiMalform 的命名口径保持一致：工作副本是 <stem>_unp.exe，
        // 它在那里把 "_unp" 摘掉再拼 "_crack.exe"。所以成品就落在原 exe 旁边。
        std::wstring stem = exePath;
        const size_t slash = stem.find_last_of(L"\\/");
        const size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash))
        {
            stem = stem.substr(0, dot);
        }
        if (stem.length() > 4 && _wcsicmp(stem.c_str() + stem.length() - 4, L"_unp") == 0)
        {
            stem = stem.substr(0, stem.length() - 4);
        }

        const std::wstring crack = stem + L"_crack.exe";
        if (::GetFileAttributesW(crack.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            return std::wstring();
        }
        return crack;
    }

    bool LaunchGameWithModule(HWND owner, const std::wstring& dllFileName,
                              const std::wstring& runtimeHashTargetDirectory, std::wstring& error)
    {
        UNREFERENCED_PARAMETER(owner);

        if (g_KrkrExeFullPath.empty())
        {
            error = L"还没有选择目标游戏主程序。";
            return false;
        }

        const std::wstring dllPath = GetModuleDllPath(dllFileName);
        if (!FileExistsLocal(dllPath))
        {
            error = ModuleMissingHint(dllFileName);
            return false;
        }

        // 步骤2（收集 Hash）走的是"只收集"模式：注入 StringDumper 是为了拿映射表，
        // 不是为了让它弹自己的恢复界面去改游戏资源。只在这个模块上设开关 ——
        // 原版功能窗口自己注入同一份 DLL 时仍要保留完整的恢复界面。
        const bool collectOnly = _wcsicmp(dllFileName.c_str(), L"CxdecStringDumper.dll") == 0;

        if (!runtimeHashTargetDirectory.empty())
        {
            ::SetEnvironmentVariableW(RuntimeHashTargetDirectoryEnvName,
                                      runtimeHashTargetDirectory.c_str());
        }
        if (collectOnly)
        {
            ::SetEnvironmentVariableW(HashCollectOnlyEnvName, L"1");
        }

        LoaderLog(FormatString(L"[YuaExtract] 启动游戏并注入 %s%s", dllFileName.c_str(),
                               collectOnly ? L"（只收集 Hash）" : L"")
                      .c_str());

        std::wstring failure;
        const bool injected = StartGameAndInjectDll(owner, dllPath, failure);

        // 环境变量是给子进程继承用的，注入完成就得清掉：不清的话后面从本进程再起的
        // 任何子进程都会继承这个开关（原版功能窗口的"运行时恢复"就会变成只收集）。
        if (collectOnly)
        {
            ::SetEnvironmentVariableW(HashCollectOnlyEnvName, nullptr);
        }
        if (!runtimeHashTargetDirectory.empty())
        {
            ::SetEnvironmentVariableW(RuntimeHashTargetDirectoryEnvName, nullptr);
        }

        error = failure;
        return injected;
    }

    void ShowLegacyFunctionsWindow(HWND owner)
    {
        if (g_LegacyWindow != nullptr && ::IsWindow(g_LegacyWindow))
        {
            ::SetForegroundWindow(g_LegacyWindow);
            return;
        }

        // 这个窗口不再是"主窗口"：关闭它不能结束整个进程。
        g_LegacyDialogOwnsApp = false;
        g_LegacyWindow = ::CreateDialogParamW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_MainForm),
                                              owner, LoaderDialogWindProc, 0u);
        if (g_LegacyWindow == nullptr)
        {
            ::MessageBoxW(owner, L"打开原版功能窗口失败。", kProductName, MB_OK | MB_ICONERROR);
            return;
        }
        ::ShowWindow(g_LegacyWindow, SW_SHOWNORMAL);
    }

    bool RunLegacyAction(HWND owner, const std::wstring& action, std::wstring& errorOut)
    {
        int command = 0;
        if (action == L"extractor")
        {
            command = IDC_Extractor;
        }
        else if (action == L"dumper")
        {
            command = IDC_StringDumper;
        }
        else if (action == L"hashRestore")
        {
            command = IDC_HashRestore;
        }
        else if (action == L"keyStatic")
        {
            command = IDC_KeyStatic;
        }
        else if (action == L"repack")
        {
            command = IDC_Repack;
        }
        else
        {
            errorOut = L"未知的原版功能：" + action;
            return false;
        }

        // 原版动作全部挂在 IDD_MainForm 的按钮上。这里不抄一份实现，
        // 而是复用同一个对话框与同一批 WM_COMMAND 处理器：行为与用户手点完全一致。
        if (g_LegacyWindow == nullptr || !::IsWindow(g_LegacyWindow))
        {
            ShowLegacyFunctionsWindow(owner);
        }
        if (g_LegacyWindow == nullptr)
        {
            errorOut = L"原版功能窗口没有建起来。";
            return false;
        }
        g_LegacyDialogOwnsApp = false;
        ::SetForegroundWindow(g_LegacyWindow);
        // BN_CLICKED + 按钮句柄：LoaderDialogWindProc 只看 LOWORD(wParam)。
        ::PostMessageW(g_LegacyWindow, WM_COMMAND, MAKEWPARAM(command, BN_CLICKED),
                       reinterpret_cast<LPARAM>(::GetDlgItem(g_LegacyWindow, command)));
        return true;
    }

    bool RunStaticKeyExtractionForTarget(HWND owner)
    {
        return RunStaticKeyExtraction(owner);
    }

    void EnableDropOnWindow(HWND hwnd)
    {
        EnableDropOnWindowAndChildren(hwnd);
    }

    void LogLine(Log::Level level, const std::wstring& text)
    {
        LoaderLogLevel(level, text.c_str());
        if (g_LogSink != nullptr)
        {
            // 镜像给界面（WebView2 桥）。可能来自 worker 线程，接收方自行保证线程安全。
            g_LogSink(level, text);
        }
    }

    void SetLogSink(LogSink sink)
    {
        g_LogSink = sink;
    }

    std::wstring Format(const wchar_t* format, ...)
    {
        wchar_t buffer[2048]{};
        va_list arguments;
        va_start(arguments, format);
        const int count = _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, arguments);
        va_end(arguments);
        if (count <= 0)
        {
            return std::wstring();
        }
        return std::wstring(buffer, static_cast<size_t>(count));
    }

    bool Confirm(HWND owner, const std::wstring& text, const std::wstring& caption)
    {
        return ::MessageBoxW(owner, text.c_str(), caption.c_str(), MB_YESNO | MB_ICONQUESTION) == IDYES;
    }

    void ShowInfo(HWND owner, const std::wstring& text)
    {
        ::MessageBoxW(owner, text.c_str(), kProductName, MB_OK | MB_ICONINFORMATION);
    }

    void ShowError(HWND owner, const std::wstring& text)
    {
        ::MessageBoxW(owner, text.c_str(), kProductName, MB_OK | MB_ICONERROR);
    }

    bool OpenInExplorer(HWND owner, const std::wstring& path)
    {
        if (path.empty() || ::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            ShowError(owner, L"目录不存在：\r\n" + path);
            return false;
        }
        const HINSTANCE result = ::ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(result) > 32;
    }

    bool OpenWithShell(HWND owner, const std::wstring& path)
    {
        if (path.empty() || ::GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            ShowError(owner, L"文件不存在：\r\n" + path);
            return false;
        }
        const HINSTANCE result = ::ShellExecuteW(owner, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(result) > 32;
    }
}
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nShowCmd)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(nShowCmd);

    INITCOMMONCONTROLSEX commonControls{ sizeof(commonControls), ICC_PROGRESS_CLASS };
    ::InitCommonControlsEx(&commonControls);

    std::wstring loaderFullPath = Util::GetAppPathW();
    std::wstring loaderCurrentDirectory = Path::GetDirectoryName(loaderFullPath);
    std::wstring krkrExeFullPath;
    std::wstring krkrExeDirectory;

    {
        int argc = 0;
        LPWSTR* argv = ::CommandLineToArgvW(lpCmdLine, &argc);
        if (argc)
        {
            // 只关心第一个参数；空命令行时 CommandLineToArgvW 会返回自身路径，
            // 那种情况必须当成没有参数，否则会往 loader 自己进程里注入。
            std::wstring candidate = argv[0];

            // 工具自身路径里带空格时（例如本仓库所在的 "E:\Harness&Agent Project\..."），
            // 没加引号的命令行会被拆成好几段，argv[0] 只剩前半截
            // （实测拿到 "E:\Harness&Agent"）。上游只是拿它跟自身路径比一比，
            // 比不过就当游戏 exe 收下，于是界面上会显示一个根本不存在的目标。
            //
            // 这里加一道硬校验：**必须是一个真实存在的 .exe** 才认。
            const DWORD attributes = candidate.empty() ? INVALID_FILE_ATTRIBUTES
                                                       : ::GetFileAttributesW(candidate.c_str());
            const bool looksLikeGameExe =
                attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 &&
                _wcsicmp(Path::GetExtension(candidate).c_str(), L".exe") == 0;

            if (looksLikeGameExe && _wcsicmp(candidate.c_str(), loaderFullPath.c_str()) != 0)
            {
                krkrExeFullPath = candidate;
                krkrExeDirectory = Path::GetDirectoryName(krkrExeFullPath);
            }
        }
        ::LocalFree(argv);
    }

    g_LoaderFullPath = loaderFullPath;
    g_LoaderCurrentDirectory = loaderCurrentDirectory;
    g_KrkrExeFullPath = krkrExeFullPath;
    g_KrkrExeDirectory = krkrExeDirectory;

    // 目标 exe 现在在 YuaExtract 主界面里选择（首页「游戏的exe程序 → 选择」）。
    // 仍然保留两种老的便利入口：把 exe 拖到界面上、或把 exe 拖到本程序图标上（走命令行参数）。
    // 双击启动时不再弹那个独立的拖放窗口 —— 主界面本身就是入口。

    // 启动就把环境落进日志。排查"某些机器上解包失败"时，最先要排除的就是环境差异：
    // 路径过长、盘符是网络盘、剩余空间不足、系统没开长路径支持、代码页不对、目录不可写。
    // 游戏目录和工具目录各记一份 —— 写失败的目录通常是游戏目录。
    {
        LoaderLogLevel(Log::Level::Info, L"[env] tool dir");
        for (const std::wstring& line : Diag::SnapshotLines(g_LoaderCurrentDirectory))
        {
            LoaderLogLevel(Log::Level::Info, line.c_str());
        }

        if (!krkrExeDirectory.empty())
        {
            LoaderLogLevel(Log::Level::Info, L"[env] game dir");
            for (const std::wstring& line : Diag::SnapshotLines(krkrExeDirectory))
            {
                LoaderLogLevel(Log::Level::Info, line.c_str());
            }
        }
    }

    // 带壳游戏的脱壳处理不在启动时做了：那时可能还没有目标。
    // 选定目标后由 Yua::Host::PrepareTargetExe 触发（见 YuaExtractMainWindow.cpp 的 ApplyTarget）。
    return Yua::RunMainWindow(hInstance, krkrExeFullPath);
}
