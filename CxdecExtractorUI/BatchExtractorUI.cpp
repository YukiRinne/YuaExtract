#include "BatchExtractorUI.h"
#include "logdir.h"
#include "utf8text.h"
#include "resource.h"

#include <CommCtrl.h>
#include <CommDlg.h>
#include <ShlObj.h>
#include <ShObjIdl.h>
#include <Shlwapi.h>
#include <strsafe.h>

#include <algorithm>
#include <cstdarg>
#include <string>
#include <vector>
#include <deque>
#include <new>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace
{
    constexpr size_t MaxUiPath = 1024u;
    constexpr UINT WM_APP_TASK_UPDATE = WM_APP + 0x101u;
    constexpr UINT WM_APP_WATCHDOG_TICK = WM_APP + 0x102u;

    // "正在解包"这一类更新的最小投递间隔。
    //
    // 解包器每个文件发两条（开始/完成），一个 48324 文件的包就是 ~10 万条消息，而
    // Windows 的 posted message 队列上限是 1 万条 —— 满了 PostMessage 直接失败。
    // 实测一次 12 包的解包共 120290 条，界面只收到 54809 条（丢 54%），丢的里面
    // 甚至包括某个包的收尾状态，那一行就永远停在"处理中"，看起来像卡死。
    // 节流到 100ms 一条后总量降到几百条，队列不可能再被打满。
    constexpr DWORD kProgressThrottleMs = 100u;

    // 现代浅色石板蓝（Slate & Cobalt）配色体系
    constexpr COLORREF kColorBgCanvas = RGB(0xF1, 0xF5, 0xF9);
    constexpr COLORREF kColorCardBg = RGB(0xFF, 0xFF, 0xFF);
    constexpr COLORREF kColorCardBorder = RGB(0xCB, 0xD5, 0xE1);
    constexpr COLORREF kColorText = RGB(0x0F, 0x17, 0x2A);
    constexpr COLORREF kColorMuted = RGB(0x64, 0x74, 0x8B);
    constexpr COLORREF kColorAccent = RGB(0x25, 0x63, 0xEB);
    constexpr COLORREF kColorAccentHover = RGB(0x1D, 0x4E, 0xD8);
    constexpr COLORREF kColorAccentPress = RGB(0x1E, 0x40, 0xAF);
    constexpr COLORREF kColorBtnBg = RGB(0xF8, 0xFA, 0xFC);
    constexpr COLORREF kColorBtnHover = RGB(0xF1, 0xF5, 0xF9);
    constexpr COLORREF kColorBtnPress = RGB(0xE2, 0xE8, 0xF0);
    constexpr COLORREF kColorBtnBorder = RGB(0xCB, 0xD5, 0xE1);
    constexpr COLORREF kColorOk = RGB(0x15, 0x80, 0x3D);
    constexpr COLORREF kColorBad = RGB(0xDC, 0x26, 0x26);
    constexpr COLORREF kColorRowStripe = RGB(0xF8, 0xFA, 0xFC);

    enum class UiTaskState
    {
        Queued,
        Preparing,
        IndexLoaded,
        Extracting,
        Completed,
        Failed
    };

    struct TaskRecord
    {
        unsigned int Id;
        unsigned int BatchId;
        std::wstring PackagePath;
        std::wstring DisplayName;
        std::wstring OutputDirectory;
        UiTaskState State;
        unsigned int Current;
        unsigned int Total;
        std::wstring Detail;
        bool Finished;
        bool Succeeded;
        ULONGLONG LastTick;
    };

    struct PendingTask
    {
        unsigned int Id;
        unsigned int BatchId;
        std::wstring PackagePath;
        std::wstring OutputDirectory;
    };

    struct TaskUpdateMessage
    {
        unsigned int TaskId;
        unsigned int State;
        unsigned int Current;
        unsigned int Total;
        // true = 这条是工作线程在解包返回后补发的收尾兜底消息（不是解包器的原始通知）。
        // 界面据此避免用兜底文案盖掉解包器给的更详细的原因。
        bool IsFallback;
        wchar_t PackagePath[MaxUiPath];
        wchar_t Detail[MaxUiPath];
    };

    struct UiContext
    {
        UI::StartupContext Startup;
        HWND Window;
        HWND TaskList;
        HWND OutputEdit;
        HWND OverallProgress;
        HWND StatusText;
        HWND NotifyPopup;
        HWND NotifySound;
        HWND WorkerInfo;
        HFONT FontUi;
        HFONT FontBold;
        HBRUSH WhiteBrush;
        RECT TopCardRect;
        RECT ListCardRect;
        CRITICAL_SECTION QueueLock;
        CRITICAL_SECTION LogLock;
        HANDLE QueueEvent;
        HANDLE StopEvent;
        HANDLE WatchdogThread;
        std::vector<HANDLE> Workers;
        std::deque<PendingTask> Queue;
        std::vector<TaskRecord> Tasks;
        std::wstring DefaultOutputDirectory;
        std::wstring ModuleDirectory;
        std::wstring UiLogPath;
        unsigned int NextTaskId;
        unsigned int ActiveBatchId;
        unsigned int NextBatchId;
        unsigned int DisplayedOverallPercent;
        LONG ActiveWorkers;
        bool CompletionNotified;
        // 上一次成功投递"正在解包"更新的时刻（GetTickCount，毫秒）。见 kProgressThrottleMs。
        LONG LastProgressPostTick;
    };

    std::wstring FormatString(const wchar_t* format, ...)
    {
        wchar_t buffer[1024];
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

    std::wstring CombinePath(const std::wstring& directory, const std::wstring& fileName)
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

    std::wstring GetModulePath(HMODULE module)
    {
        wchar_t buffer[MaxUiPath];
        DWORD length = ::GetModuleFileNameW(module, buffer, (DWORD)_countof(buffer));
        if (length == 0 || length >= _countof(buffer))
        {
            return std::wstring();
        }
        return std::wstring(buffer, length);
    }

    std::wstring GetDirectoryName(const std::wstring& fullPath)
    {
        if (fullPath.empty())
        {
            return std::wstring();
        }

        wchar_t buffer[MaxUiPath];
        StringCchCopyW(buffer, _countof(buffer), fullPath.c_str());
        if (::PathRemoveFileSpecW(buffer))
        {
            return std::wstring(buffer);
        }
        return std::wstring();
    }

    std::wstring GetWindowTextString(HWND control)
    {
        int length = ::GetWindowTextLengthW(control);
        if (length <= 0)
        {
            return std::wstring();
        }

        std::wstring text((size_t)length + 1u, L'\0');
        int copied = ::GetWindowTextW(control, text.data(), length + 1);
        if (copied <= 0)
        {
            return std::wstring();
        }
        text.resize((size_t)copied);
        return text;
    }

    void WriteUiLog(UiContext* context, const wchar_t* format, ...)
    {
        if (!context)
        {
            return;
        }

        wchar_t message[1024];
        va_list ap;
        va_start(ap, format);
        int count = _vsnwprintf_s(message, _countof(message), _TRUNCATE, format, ap);
        va_end(ap);
        if (count <= 0)
        {
            return;
        }

        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        std::wstring line = FormatString(L"%04u-%02u-%02u %02u:%02u:%02u | %s\r\n",
                                         st.wYear,
                                         st.wMonth,
                                         st.wDay,
                                         st.wHour,
                                         st.wMinute,
                                         st.wSecond,
                                         message);

        ::EnterCriticalSection(&context->LogLock);
        AppendUtf8Line(context->UiLogPath, line);
        ::LeaveCriticalSection(&context->LogLock);
    }

    const wchar_t* ToStateText(UiTaskState state)
    {
        switch (state)
        {
            case UiTaskState::Queued: return L"排队中";
            case UiTaskState::Preparing: return L"准备中";
            case UiTaskState::IndexLoaded: return L"读取索引";
            case UiTaskState::Extracting: return L"处理中";
            case UiTaskState::Completed: return L"已完成";
            case UiTaskState::Failed: return L"失败";
        }
        return L"未知";
    }

    std::wstring ToProgressText(const TaskRecord& task)
    {
        if (task.Finished)
        {
            return task.Succeeded ? L"100%" : L"完成(含失败)";
        }

        if (task.Total == 0u)
        {
            return task.State == UiTaskState::Queued ? L"等待中" : L"准备中";
        }

        unsigned int percent = task.Current >= task.Total ? 100u : (task.Current * 100u) / task.Total;
        return FormatString(L"%u%% (%u/%u)", percent, task.Current, task.Total);
    }

    int FindTaskIndex(const UiContext* context, unsigned int taskId)
    {
        for (size_t index = 0; index < context->Tasks.size(); ++index)
        {
            if (context->Tasks[index].Id == taskId)
            {
                return (int)index;
            }
        }
        return -1;
    }

    int FindListItemByTaskId(HWND listView, unsigned int taskId)
    {
        LVFINDINFOW info{};
        info.flags = LVFI_PARAM;
        info.lParam = (LPARAM)taskId;
        return ListView_FindItem(listView, -1, &info);
    }

    bool HasUnfinishedTaskInBatch(const UiContext* context, unsigned int batchId)
    {
        for (const TaskRecord& task : context->Tasks)
        {
            if (task.BatchId == batchId && !task.Finished)
            {
                return true;
            }
        }
        return false;
    }

    bool HasQueuedTaskInBatch(const UiContext* context, unsigned int batchId)
    {
        for (const PendingTask& task : context->Queue)
        {
            if (task.BatchId == batchId)
            {
                return true;
            }
        }
        return false;
    }

    unsigned int FindNextPendingBatchId(const UiContext* context)
    {
        unsigned int nextBatchId = 0u;
        for (const TaskRecord& task : context->Tasks)
        {
            if (!task.Finished && (nextBatchId == 0u || task.BatchId < nextBatchId))
            {
                nextBatchId = task.BatchId;
            }
        }
        return nextBatchId;
    }

    void AdvanceBatchIfIdle(UiContext* context)
    {
        if (!context || context->ActiveWorkers > 0)
        {
            return;
        }

        ::EnterCriticalSection(&context->QueueLock);
        bool hasCurrentQueued = HasQueuedTaskInBatch(context, context->ActiveBatchId);
        ::LeaveCriticalSection(&context->QueueLock);

        if (hasCurrentQueued || HasUnfinishedTaskInBatch(context, context->ActiveBatchId))
        {
            return;
        }

        unsigned int nextBatchId = FindNextPendingBatchId(context);
        if (nextBatchId != 0u && nextBatchId != context->ActiveBatchId)
        {
            context->ActiveBatchId = nextBatchId;
            context->DisplayedOverallPercent = 0u;
            ::SendMessageW(context->OverallProgress, PBM_SETPOS, 0, 0);
            ::SetEvent(context->QueueEvent);
            WriteUiLog(context, L"Advanced to batch #%u", nextBatchId);
        }
    }

    void RefreshTaskRow(UiContext* context, int taskIndex)
    {
        if (!context || taskIndex < 0 || taskIndex >= (int)context->Tasks.size())
        {
            return;
        }

        const TaskRecord& task = context->Tasks[(size_t)taskIndex];
        int listIndex = FindListItemByTaskId(context->TaskList, task.Id);
        if (listIndex < 0)
        {
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = ListView_GetItemCount(context->TaskList);
            item.pszText = const_cast<LPWSTR>(task.DisplayName.c_str());
            item.lParam = (LPARAM)task.Id;
            listIndex = ListView_InsertItem(context->TaskList, &item);
        }

        ListView_SetItemText(context->TaskList, listIndex, 0, const_cast<LPWSTR>(task.DisplayName.c_str()));
        ListView_SetItemText(context->TaskList, listIndex, 1, const_cast<LPWSTR>(ToStateText(task.State)));

        std::wstring progress = ToProgressText(task);
        ListView_SetItemText(context->TaskList, listIndex, 2, progress.data());
        ListView_SetItemText(context->TaskList, listIndex, 3, const_cast<LPWSTR>(task.Detail.c_str()));
    }

    bool IsSupportedPackage(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }

        DWORD attr = ::GetFileAttributesW(path.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            return false;
        }

        const wchar_t* extension = ::PathFindExtensionW(path.c_str());
        return extension != nullptr && _wcsicmp(extension, L".xp3") == 0;
    }

    std::wstring BrowseFolder(HWND owner, const std::wstring& title)
    {
        IFileDialog* dialog = nullptr;
        HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog,
                                        nullptr,
                                        CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&dialog));
        if (FAILED(hr) || !dialog)
        {
            return std::wstring();
        }

        std::wstring result;
        DWORD options = 0;
        if (SUCCEEDED(dialog->GetOptions(&options)))
        {
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        }
        dialog->SetTitle(title.c_str());

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
        return result;
    }

    std::vector<std::wstring> BrowsePackageFiles(HWND owner)
    {
        wchar_t buffer[32768]{};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = owner;
        ofn.lpstrFilter = L"XP3 Files (*.xp3)\0*.xp3\0All Files (*.*)\0*.*\0";
        ofn.lpstrFile = buffer;
        ofn.nMaxFile = (DWORD)_countof(buffer);
        ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_ALLOWMULTISELECT | OFN_HIDEREADONLY;

        std::vector<std::wstring> files;
        if (!::GetOpenFileNameW(&ofn))
        {
            return files;
        }

        const wchar_t* first = buffer;
        const wchar_t* second = first + wcslen(first) + 1;
        if (*second == L'\0')
        {
            files.emplace_back(first);
            return files;
        }

        std::wstring directory = first;
        while (*second != L'\0')
        {
            files.push_back(CombinePath(directory, second));
            second += wcslen(second) + 1;
        }
        return files;
    }

    bool PopPendingTask(UiContext* context, PendingTask& task)
    {
        bool hasTask = false;

        ::EnterCriticalSection(&context->QueueLock);
        for (auto it = context->Queue.begin(); it != context->Queue.end(); ++it)
        {
            if (it->BatchId == context->ActiveBatchId)
            {
                task = *it;
                context->Queue.erase(it);
                hasTask = true;
                break;
            }
        }

        if (context->Queue.empty())
        {
            ::ResetEvent(context->QueueEvent);
        }
        ::LeaveCriticalSection(&context->QueueLock);

        return hasTask;
    }

    void EnqueueTask(UiContext* context, const std::wstring& packagePath, const std::wstring& outputDirectory, unsigned int batchId)
    {
        TaskRecord task{};
        task.Id = context->NextTaskId++;
        task.BatchId = batchId;
        task.PackagePath = packagePath;
        task.DisplayName = ::PathFindFileNameW(packagePath.c_str());
        task.OutputDirectory = outputDirectory;
        task.State = UiTaskState::Queued;
        task.Current = 0u;
        task.Total = 0u;
        task.Detail = outputDirectory;
        task.Finished = false;
        task.Succeeded = false;
        task.LastTick = ::GetTickCount64();

        context->Tasks.push_back(task);
        RefreshTaskRow(context, (int)context->Tasks.size() - 1);

        ::EnterCriticalSection(&context->QueueLock);
        context->Queue.push_back(PendingTask{ task.Id, batchId, packagePath, outputDirectory });
        ::SetEvent(context->QueueEvent);
        ::LeaveCriticalSection(&context->QueueLock);

        context->CompletionNotified = false;
        WriteUiLog(context, L"Task queued: #%u batch=%u %s -> %s", task.Id, batchId, packagePath.c_str(), outputDirectory.c_str());
    }

    void AddPackageFiles(UiContext* context, const std::vector<std::wstring>& paths)
    {
        if (!context)
        {
            return;
        }

        std::wstring outputDirectory = GetWindowTextString(context->OutputEdit);
        if (outputDirectory.empty())
        {
            outputDirectory = context->DefaultOutputDirectory;
            ::SetWindowTextW(context->OutputEdit, outputDirectory.c_str());
        }

        unsigned int acceptedCount = 0u;
        bool hasActiveBatchWork = (context->ActiveWorkers > 0) || HasUnfinishedTaskInBatch(context, context->ActiveBatchId);
        unsigned int batchId = 0u;
        if (!hasActiveBatchWork)
        {
            if (context->ActiveBatchId == 0u)
            {
                context->ActiveBatchId = 1u;
                context->NextBatchId = 2u;
            }
            batchId = context->ActiveBatchId;
        }
        else
        {
            if (context->NextBatchId == 0u)
            {
                context->NextBatchId = context->ActiveBatchId + 1u;
            }
            batchId = context->NextBatchId++;
        }

        for (const std::wstring& path : paths)
        {
            if (!IsSupportedPackage(path))
            {
                WriteUiLog(context, L"Skip unsupported file: %s", path.c_str());
                continue;
            }

            EnqueueTask(context, path, outputDirectory, batchId);
            ++acceptedCount;
        }

        if (acceptedCount == 0u)
        {
            ::SetWindowTextW(context->StatusText, L"没有可处理的 XP3 文件。");
        }
    }

    void MaybeNotifyFinished(UiContext* context)
    {
        if (!context || context->CompletionNotified)
        {
            return;
        }

        bool hasTask = !context->Tasks.empty();
        bool hasRunning = context->ActiveWorkers > 0;
        bool hasQueued = false;

        ::EnterCriticalSection(&context->QueueLock);
        hasQueued = !context->Queue.empty();
        ::LeaveCriticalSection(&context->QueueLock);

        if (!hasTask || hasRunning || hasQueued)
        {
            return;
        }

        bool allFinished = true;
        bool allSucceeded = true;
        for (const TaskRecord& task : context->Tasks)
        {
            allFinished = allFinished && task.Finished;
            if (task.Finished)
            {
                allSucceeded = allSucceeded && task.Succeeded;
            }
        }

        if (!allFinished)
        {
            return;
        }

        context->CompletionNotified = true;

        if (::SendMessageW(context->NotifySound, BM_GETCHECK, 0, 0) == BST_CHECKED)
        {
            ::MessageBeep(allSucceeded ? MB_ICONINFORMATION : MB_ICONWARNING);
        }

        if (::SendMessageW(context->NotifyPopup, BM_GETCHECK, 0, 0) == BST_CHECKED)
        {
            ::MessageBoxW(context->Window,
                          allSucceeded ? L"所有解包任务已完成。" : L"解包队列已结束，但存在失败任务，请查看列表和日志。",
                          L"批量解包完成",
                          MB_OK | (allSucceeded ? MB_ICONINFORMATION : MB_ICONWARNING));
        }
    }

    void UpdateSummary(UiContext* context)
    {
        if (!context)
        {
            return;
        }

        unsigned int queuedCount = 0u;
        unsigned int nextBatchCount = 0u;
        unsigned int finishedCount = 0u;
        unsigned int failedCount = 0u;
        unsigned int activeBatchTaskCount = 0u;
        unsigned int activeBatchFinishedCount = 0u;
        unsigned long long progressCurrent = 0u;
        unsigned long long progressTotal = 0u;

        ::EnterCriticalSection(&context->QueueLock);
        queuedCount = (unsigned int)context->Queue.size();
        ::LeaveCriticalSection(&context->QueueLock);

        for (const TaskRecord& task : context->Tasks)
        {
            if (task.Finished)
            {
                ++finishedCount;
                if (!task.Succeeded)
                {
                    ++failedCount;
                }
            }

            if (!task.Finished && task.BatchId != context->ActiveBatchId)
            {
                ++nextBatchCount;
            }
            if (task.BatchId != context->ActiveBatchId)
            {
                continue;
            }

            ++activeBatchTaskCount;
            if (task.Finished)
            {
                ++activeBatchFinishedCount;
            }

            if (task.Total > 0u)
            {
                progressCurrent += task.Finished ? task.Total : std::min(task.Current, task.Total);
                progressTotal += task.Total;
            }
            else
            {
                progressTotal += 1u;
                if (task.Finished)
                {
                    progressCurrent += 1u;
                }
            }
        }

        unsigned int percent = 0u;
        if (progressTotal > 0u)
        {
            percent = (unsigned int)((progressCurrent * 100ull) / progressTotal);
        }
        if (activeBatchTaskCount > 0u && activeBatchFinishedCount == activeBatchTaskCount)
        {
            percent = 100u;
        }
        else if (percent < context->DisplayedOverallPercent)
        {
            percent = context->DisplayedOverallPercent;
        }
        if (percent > 100u)
        {
            percent = 100u;
        }
        context->DisplayedOverallPercent = percent;

        SendMessageW(context->OverallProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));
        SendMessageW(context->OverallProgress, PBM_SETPOS, percent, 0);

        std::wstring status = FormatString(L"总任务 %u | 当前批次 %u/%u | 下一批 %u | 运行中 %ld | 完成 %u | 失败 %u",
                                           (unsigned int)context->Tasks.size(),
                                           activeBatchFinishedCount,
                                           activeBatchTaskCount,
                                           nextBatchCount,
                                           context->ActiveWorkers,
                                           finishedCount,
                                           failedCount);
        ::SetWindowTextW(context->StatusText, status.c_str());

        std::wstring title = FormatString(L"YuaExtract - XP3 批量解包器 - 运行中 %ld / 排队 %u / 完成 %u",
                                          context->ActiveWorkers,
                                          queuedCount,
                                          finishedCount);
        ::SetWindowTextW(context->Window, title.c_str());

        MaybeNotifyFinished(context);
    }

    void ClearFinishedTasks(UiContext* context)
    {
        if (!context)
        {
            return;
        }

        context->Tasks.erase(std::remove_if(context->Tasks.begin(),
                                            context->Tasks.end(),
                                            [](const TaskRecord& task) { return task.Finished; }),
                             context->Tasks.end());

        ListView_DeleteAllItems(context->TaskList);
        for (size_t index = 0; index < context->Tasks.size(); ++index)
        {
            RefreshTaskRow(context, (int)index);
        }

        WriteUiLog(context, L"Cleared finished tasks");
    }

    void PostTaskUpdate(UiContext* context,
                        unsigned int taskId,
                        unsigned int state,
                        unsigned int current,
                        unsigned int total,
                        const wchar_t* packagePath,
                        const wchar_t* detail,
                        bool isFallback = false)
    {
        if (!context || !context->Window)
        {
            return;
        }

        // 只有"正在解包"这类高频更新才节流；准备/索引完成/完成/失败一律照发 ——
        // 状态机的关键节点绝不能因为限流而丢。
        if (state == Engine::ExtractTaskExtracting)
        {
            const DWORD now = ::GetTickCount();
            // 32 位读在 x86 上不会撕裂；两个工作线程同时挤过最多多发一条，无害。
            if ((DWORD)(now - (DWORD)context->LastProgressPostTick) < kProgressThrottleMs)
            {
                return;
            }
            context->LastProgressPostTick = (LONG)now;
        }

        TaskUpdateMessage* message = (TaskUpdateMessage*)::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TaskUpdateMessage));
        if (!message)
        {
            return;
        }

        message->TaskId = taskId;
        message->State = state;
        message->Current = current;
        message->Total = total;
        message->IsFallback = isFallback;
        ::StringCchCopyW(message->PackagePath, _countof(message->PackagePath), packagePath ? packagePath : L"");
        ::StringCchCopyW(message->Detail, _countof(message->Detail), detail ? detail : L"");

        // 唯一一处真正的丢弃点：队列满时 PostMessage 失败。
        // 高频进度丢掉无所谓；准备/索引/收尾这类关键状态则必须送到 —— 先重试几次
        // （队列满通常几毫秒就排空），仍失败才放弃。加上节流之后这条路几乎不会走到，
        // 留着是为了"界面卡在中间态"这件事从此不可能发生。
        for (int attempt = 0; ; ++attempt)
        {
            if (::PostMessageW(context->Window, WM_APP_TASK_UPDATE, 0, (LPARAM)message))
            {
                return;
            }
            if (state == Engine::ExtractTaskExtracting || attempt >= 50)
            {
                break;
            }
            ::Sleep(1u);
        }
        ::HeapFree(::GetProcessHeap(), 0, message);
    }

    DWORD WINAPI WorkerThreadProc(LPVOID parameter)
    {
        UiContext* context = (UiContext*)parameter;
        HANDLE waitHandles[2]{ context->StopEvent, context->QueueEvent };

        while (true)
        {
            PendingTask task{};
            if (!PopPendingTask(context, task))
            {
                DWORD waitResult = ::WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                if (waitResult == WAIT_OBJECT_0)
                {
                    break;
                }
                continue;
            }

            ::InterlockedIncrement(&context->ActiveWorkers);
            ::PostMessageW(context->Window, WM_APP_WATCHDOG_TICK, 0, 0);

            if (context->Startup.Api.ExtractPackageEx && context->Startup.Api.SetExtractProgressCallback)
            {
                const BOOL succeeded = context->Startup.Api.ExtractPackageEx(task.PackagePath.c_str(),
                                                                             task.OutputDirectory.c_str(),
                                                                             task.Id);

                // 收尾状态由工作线程自己补一条。
                //
                // 进度消息是 PostMessage 投递的，队列满会静默丢弃。实测一次 12 包的解包
                // 往界面塞了 120290 条、丢了 54%，其中恰好把某个包的收尾状态丢掉，
                // 那一行就永远停在"处理中 100%"（解包其实早就完成了，只是界面不知道）。
                // 高频进度可以丢，任务的收尾不能丢 —— 这里拿返回值兜底。
                // 界面收到重复的收尾状态是幂等的，而且不会用兜底文案盖掉解包器给的原因。
                PostTaskUpdate(context,
                               task.Id,
                               succeeded ? Engine::ExtractTaskCompleted : Engine::ExtractTaskFailed,
                               0u,
                               0u,
                               task.PackagePath.c_str(),
                               succeeded ? L"解包完成" : L"解包完成，但存在失败条目",
                               true);
            }
            else
            {
                PostTaskUpdate(context, task.Id, Engine::ExtractTaskFailed, 0u, 0u, task.PackagePath.c_str(), L"核心接口未初始化");
                WriteUiLog(context, L"Task failed: #%u core API unavailable", task.Id);
            }

            ::InterlockedDecrement(&context->ActiveWorkers);
            ::PostMessageW(context->Window, WM_APP_WATCHDOG_TICK, 0, 0);
        }

        return 0u;
    }

    DWORD WINAPI WatchdogThreadProc(LPVOID parameter)
    {
        UiContext* context = (UiContext*)parameter;
        while (::WaitForSingleObject(context->StopEvent, 1000u) == WAIT_TIMEOUT)
        {
            if (context->Window)
            {
                ::PostMessageW(context->Window, WM_APP_WATCHDOG_TICK, 0, 0);
            }
        }
        return 0u;
    }

    void WINAPI ExtractProgressThunk(unsigned int taskId,
                                     const wchar_t* packagePath,
                                     unsigned int state,
                                     unsigned int current,
                                     unsigned int total,
                                     const wchar_t* detail,
                                     void* context)
    {
        PostTaskUpdate((UiContext*)context, taskId, state, current, total, packagePath, detail);
    }

    HFONT CreateExtractorFont(int pointSize, bool bold)
    {
        LOGFONTW lf{};
        lf.lfHeight = -::MulDiv(pointSize, 96, 72);
        lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        ::wcscpy_s(lf.lfFaceName, L"微软雅黑");
        return ::CreateFontIndirectW(&lf);
    }

    struct ExtractorButtonSubData
    {
        WNDPROC OldProc = nullptr;
        HFONT Font = nullptr;
        HFONT FontBold = nullptr;
        bool Primary = false;
        bool Hover = false;
        bool Pressed = false;
        bool Tracking = false;
    };

    LRESULT CALLBACK ModernExtractorButtonWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        ExtractorButtonSubData* data =
            reinterpret_cast<ExtractorButtonSubData*>(::GetPropW(hwnd, L"YuaExtBtnData"));
        if (!data)
        {
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        switch (msg)
        {
            case WM_MOUSEMOVE:
            {
                if (!data->Tracking)
                {
                    TRACKMOUSEEVENT tme{};
                    tme.cbSize = sizeof(tme);
                    tme.dwFlags = TME_LEAVE;
                    tme.hwndTrack = hwnd;
                    ::TrackMouseEvent(&tme);
                    data->Tracking = true;
                }
                if (!data->Hover)
                {
                    data->Hover = true;
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                }
                break;
            }
            case WM_MOUSELEAVE:
            {
                data->Tracking = false;
                if (data->Hover)
                {
                    data->Hover = false;
                    ::InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            case WM_ERASEBKGND:
                return 1;

            case BM_SETSTATE:
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
            case WM_SETFOCUS:
            case WM_KILLFOCUS:
            case WM_ENABLE:
            case WM_SETTEXT:
            {
                if (msg == BM_SETSTATE)
                {
                    data->Pressed = (wParam != 0);
                }
                ::SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
                const LRESULT res = ::CallWindowProcW(data->OldProc, hwnd, msg, wParam, lParam);
                ::SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
                ::InvalidateRect(hwnd, nullptr, FALSE);
                return res;
            }

            case WM_PAINT:
            {
                PAINTSTRUCT ps{};
                HDC targetDc = ::BeginPaint(hwnd, &ps);
                RECT rc{};
                ::GetClientRect(hwnd, &rc);
                const int w = rc.right - rc.left;
                const int h = rc.bottom - rc.top;
                if (w > 0 && h > 0)
                {
                    HDC memDc = ::CreateCompatibleDC(targetDc);
                    HBITMAP memBmp = ::CreateCompatibleBitmap(targetDc, w, h);
                    HGDIOBJ oldBmp = ::SelectObject(memDc, memBmp);

                    HBRUSH parentBrush = ::CreateSolidBrush(kColorCardBg);
                    ::FillRect(memDc, &rc, parentBrush);
                    ::DeleteObject(parentBrush);

                    const bool enabled = ::IsWindowEnabled(hwnd) != FALSE;
                    const bool pressed = data->Pressed;
                    const bool hover = data->Hover && enabled;

                    COLORREF fill = kColorBtnBg;
                    COLORREF border = kColorBtnBorder;
                    COLORREF textColor = kColorText;
                    bool useBold = false;

                    if (data->Primary)
                    {
                        fill = pressed ? kColorAccentPress : (hover ? kColorAccentHover : kColorAccent);
                        border = fill;
                        textColor = RGB(0xFF, 0xFF, 0xFF);
                        useBold = true;
                    }
                    else
                    {
                        fill = pressed ? kColorBtnPress : (hover ? kColorBtnHover : kColorBtnBg);
                        border = hover ? RGB(0x94, 0xA3, 0xB8) : kColorBtnBorder;
                        textColor = kColorText;
                    }

                    HBRUSH fillBrush = ::CreateSolidBrush(fill);
                    HPEN borderPen = ::CreatePen(PS_SOLID, 1, border);
                    HGDIOBJ oldBrush = ::SelectObject(memDc, fillBrush);
                    HGDIOBJ oldPen = ::SelectObject(memDc, borderPen);
                    ::RoundRect(memDc, 0, 0, w, h, 6, 6);
                    ::SelectObject(memDc, oldPen);
                    ::SelectObject(memDc, oldBrush);
                    ::DeleteObject(borderPen);
                    ::DeleteObject(fillBrush);

                    wchar_t text[128]{};
                    ::GetWindowTextW(hwnd, text, _countof(text));
                    ::SetBkMode(memDc, TRANSPARENT);
                    ::SetTextColor(memDc, textColor);
                    HGDIOBJ oldFont = ::SelectObject(memDc, useBold ? data->FontBold : data->Font);
                    RECT textRc = rc;
                    if (pressed && enabled)
                    {
                        ::OffsetRect(&textRc, 0, 1);
                    }
                    ::DrawTextW(memDc, text, -1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                    ::SelectObject(memDc, oldFont);

                    ::BitBlt(targetDc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
                    ::SelectObject(memDc, oldBmp);
                    ::DeleteObject(memBmp);
                    ::DeleteDC(memDc);
                }
                ::EndPaint(hwnd, &ps);
                return 0;
            }
            case WM_NCDESTROY:
            {
                WNDPROC oldProc = data->OldProc;
                ::RemovePropW(hwnd, L"YuaExtBtnData");
                delete data;
                return ::CallWindowProcW(oldProc, hwnd, msg, wParam, lParam);
            }
            default:
                break;
        }
        return ::CallWindowProcW(data->OldProc, hwnd, msg, wParam, lParam);
    }

    void SubclassExtractorButton(HWND hwnd, bool primary, HFONT font, HFONT fontBold)
    {
        if (!hwnd)
        {
            return;
        }
        ExtractorButtonSubData* data = new (std::nothrow) ExtractorButtonSubData();
        if (!data)
        {
            return;
        }
        data->Font = font;
        data->FontBold = fontBold;
        data->Primary = primary;
        data->OldProc = reinterpret_cast<WNDPROC>(
            ::SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ModernExtractorButtonWndProc)));
        ::SetPropW(hwnd, L"YuaExtBtnData", reinterpret_cast<HANDLE>(data));
    }

    void PaintExtractorCanvas(HDC targetDc, const UiContext* context)
    {
        if (!context || !context->Window)
        {
            return;
        }
        RECT client{};
        ::GetClientRect(context->Window, &client);
        const int w = client.right - client.left;
        const int h = client.bottom - client.top;
        if (w <= 0 || h <= 0)
        {
            return;
        }

        HDC memDc = ::CreateCompatibleDC(targetDc);
        HBITMAP memBmp = ::CreateCompatibleBitmap(targetDc, w, h);
        HGDIOBJ oldBmp = ::SelectObject(memDc, memBmp);

        HBRUSH bgBrush = ::CreateSolidBrush(kColorBgCanvas);
        ::FillRect(memDc, &client, bgBrush);
        ::DeleteObject(bgBrush);

        HBRUSH cardBrush = ::CreateSolidBrush(kColorCardBg);
        HPEN borderPen = ::CreatePen(PS_SOLID, 1, kColorCardBorder);
        HGDIOBJ oldBrush = ::SelectObject(memDc, cardBrush);
        HGDIOBJ oldPen = ::SelectObject(memDc, borderPen);
        if (context->TopCardRect.right > context->TopCardRect.left)
        {
            ::RoundRect(memDc, context->TopCardRect.left, context->TopCardRect.top,
                        context->TopCardRect.right, context->TopCardRect.bottom, 8, 8);
        }
        if (context->ListCardRect.right > context->ListCardRect.left)
        {
            ::RoundRect(memDc, context->ListCardRect.left, context->ListCardRect.top,
                        context->ListCardRect.right, context->ListCardRect.bottom, 8, 8);
        }
        ::SelectObject(memDc, oldPen);
        ::SelectObject(memDc, oldBrush);
        ::DeleteObject(borderPen);
        ::DeleteObject(cardBrush);

        ::BitBlt(targetDc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
        ::SelectObject(memDc, oldBmp);
        ::DeleteObject(memBmp);
        ::DeleteDC(memDc);
    }

    void LayoutExtractorDialog(UiContext* context)
    {
        if (!context || !context->Window)
        {
            return;
        }
        RECT client{};
        ::GetClientRect(context->Window, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        if (width <= 0 || height <= 0)
        {
            return;
        }

        const int margin = 12;
        const int cardLeft = margin;
        const int cardRight = width - margin;
        const int innerLeft = cardLeft + 12;
        const int innerRight = cardRight - 12;
        const int innerWidth = innerRight - innerLeft;

        const int btnAddW = 90;
        const int btnClearW = 94;
        const int btnBrowseW = 94;
        const int btnH = 26;

        int y = margin + 10;
        ::MoveWindow(::GetDlgItem(context->Window, IDC_DropHint), innerLeft, y + 4,
                     std::max(160, innerWidth - btnAddW - btnClearW - 16), 20, TRUE);
        ::MoveWindow(::GetDlgItem(context->Window, IDC_AddFiles), innerRight - btnClearW - 8 - btnAddW, y,
                     btnAddW, btnH, TRUE);
        ::MoveWindow(::GetDlgItem(context->Window, IDC_ClearCompleted), innerRight - btnClearW, y,
                     btnClearW, btnH, TRUE);
        y += btnH + 8;

        if (HWND outputLabel = ::FindWindowExW(context->Window, nullptr, L"Static", L"输出目录"))
        {
            ::MoveWindow(outputLabel, innerLeft, y + 4, 62, 20, TRUE);
        }
        const int editLeft = innerLeft + 66;
        const int editWidth = std::max(140, innerRight - btnBrowseW - 8 - editLeft);
        ::MoveWindow(context->OutputEdit, editLeft, y + 1, editWidth, 24, TRUE);
        ::MoveWindow(::GetDlgItem(context->Window, IDC_BrowseOutput), innerRight - btnBrowseW, y,
                     btnBrowseW, btnH, TRUE);
        y += btnH + 8;

        ::MoveWindow(context->NotifyPopup, innerLeft, y, 88, 20, TRUE);
        ::MoveWindow(context->NotifySound, innerLeft + 96, y, 96, 20, TRUE);
        ::MoveWindow(context->WorkerInfo, innerRight - 130, y + 2, 130, 18, TRUE);
        y += 26;

        ::MoveWindow(context->OverallProgress, innerLeft, y, innerWidth, 14, TRUE);
        y += 20;

        ::MoveWindow(context->StatusText, innerLeft, y, innerWidth, 18, TRUE);
        y += 26;
        context->TopCardRect = RECT{ cardLeft, margin, cardRight, y };

        const int listCardTop = y + 8;
        const int listCardBottom = height - margin;
        context->ListCardRect = RECT{ cardLeft, listCardTop, cardRight, listCardBottom };
        const int listHeight = std::max(80, listCardBottom - listCardTop - 16);
        ::MoveWindow(context->TaskList, innerLeft, listCardTop + 8, innerWidth, listHeight, TRUE);
        if (context->TaskList && innerWidth > 320)
        {
            ListView_SetColumnWidth(context->TaskList, 3, innerWidth - 295 - 20);
        }

        ::RedrawWindow(context->Window, nullptr, nullptr,
                       RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    void InitializeListViewColumns(HWND listView)
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        column.pszText = const_cast<LPWSTR>(L"文件");
        column.cx = 130;
        ListView_InsertColumn(listView, 0, &column);

        column.pszText = const_cast<LPWSTR>(L"状态");
        column.cx = 75;
        column.iSubItem = 1;
        ListView_InsertColumn(listView, 1, &column);

        column.pszText = const_cast<LPWSTR>(L"进度");
        column.cx = 90;
        column.iSubItem = 2;
        ListView_InsertColumn(listView, 2, &column);

        column.pszText = const_cast<LPWSTR>(L"详细信息");
        column.cx = 220;
        column.iSubItem = 3;
        ListView_InsertColumn(listView, 3, &column);

        ListView_SetExtendedListViewStyle(listView, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    }

    unsigned int ResolveWorkerCount()
    {
        // TVP/Cxdec 运行时在并发解包下不稳定，
        // 所以只保留一个解包线程，响应性交给看门狗和 UI 线程
        return 1u;
    }

    INT_PTR CALLBACK ExtractorDialogWindProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        UiContext* context = (UiContext*)::GetWindowLongPtrW(hwnd, GWLP_USERDATA);

        switch (msg)
        {
            case WM_INITDIALOG:
            {
                context = (UiContext*)lParam;
                context->Window = hwnd;
                context->TaskList = ::GetDlgItem(hwnd, IDC_TaskList);
                context->OutputEdit = ::GetDlgItem(hwnd, IDC_OutputPath);
                context->OverallProgress = ::GetDlgItem(hwnd, IDC_OverallProgress);
                context->StatusText = ::GetDlgItem(hwnd, IDC_StatusText);
                context->NotifyPopup = ::GetDlgItem(hwnd, IDC_NotifyPopup);
                context->NotifySound = ::GetDlgItem(hwnd, IDC_NotifySound);
                context->WorkerInfo = ::GetDlgItem(hwnd, IDC_WorkerInfo);
                context->FontUi = CreateExtractorFont(9, false);
                context->FontBold = CreateExtractorFont(9, true);
                context->WhiteBrush = ::CreateSolidBrush(kColorCardBg);

                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)context);
                ::SendMessageW(context->OutputEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(6, 6));
                SubclassExtractorButton(::GetDlgItem(hwnd, IDC_AddFiles), true, context->FontUi, context->FontBold);
                SubclassExtractorButton(::GetDlgItem(hwnd, IDC_ClearCompleted), false, context->FontUi, context->FontBold);
                SubclassExtractorButton(::GetDlgItem(hwnd, IDC_BrowseOutput), false, context->FontUi, context->FontBold);

                ::SendMessageW(context->NotifyPopup, BM_SETCHECK, BST_CHECKED, 0);
                ::SendMessageW(context->NotifySound, BM_SETCHECK, BST_CHECKED, 0);
                ::SetWindowTextW(context->OutputEdit, context->DefaultOutputDirectory.c_str());
                ::SetWindowTextW(context->WorkerInfo, FormatString(L"工作线程: %u", (unsigned int)context->Workers.size()).c_str());
                ::DragAcceptFiles(hwnd, TRUE);
                InitializeListViewColumns(context->TaskList);
                LayoutExtractorDialog(context);
                UpdateSummary(context);
                return TRUE;
            }
            case WM_ERASEBKGND:
            {
                if (context)
                {
                    PaintExtractorCanvas((HDC)wParam, context);
                    return TRUE;
                }
                break;
            }
            case WM_PAINT:
            {
                if (context)
                {
                    PAINTSTRUCT ps{};
                    HDC dc = ::BeginPaint(hwnd, &ps);
                    PaintExtractorCanvas(dc, context);
                    ::EndPaint(hwnd, &ps);
                    return TRUE;
                }
                break;
            }
            case WM_SIZE:
            {
                if (context)
                {
                    LayoutExtractorDialog(context);
                    return TRUE;
                }
                break;
            }
            case WM_GETMINMAXINFO:
            {
                MINMAXINFO* info = (MINMAXINFO*)lParam;
                info->ptMinTrackSize.x = 640;
                info->ptMinTrackSize.y = 440;
                return TRUE;
            }
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORBTN:
            {
                if (context && context->WhiteBrush)
                {
                    HDC dc = (HDC)wParam;
                    HWND ctrl = (HWND)lParam;
                    if (ctrl == context->StatusText || ctrl == context->WorkerInfo ||
                        ctrl == ::GetDlgItem(hwnd, IDC_DropHint))
                    {
                        ::SetTextColor(dc, kColorMuted);
                    }
                    else
                    {
                        ::SetTextColor(dc, kColorText);
                    }
                    ::SetBkColor(dc, kColorCardBg);
                    return (INT_PTR)context->WhiteBrush;
                }
                break;
            }
            case WM_NOTIFY:
            {
                if (context)
                {
                    const NMHDR* hdr = (const NMHDR*)lParam;
                    if (hdr && hdr->idFrom == IDC_TaskList && hdr->code == NM_CUSTOMDRAW)
                    {
                        NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lParam;
                        switch (cd->nmcd.dwDrawStage)
                        {
                            case CDDS_PREPAINT:
                                ::SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, CDRF_NOTIFYITEMDRAW);
                                return TRUE;
                            case CDDS_ITEMPREPAINT:
                            {
                                const int row = (int)cd->nmcd.dwItemSpec;
                                cd->clrTextBk = (row % 2 == 1) ? kColorRowStripe : kColorCardBg;
                                cd->clrText = kColorText;
                                ::SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, CDRF_NOTIFYSUBITEMDRAW);
                                return TRUE;
                            }
                            case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
                            {
                                const int row = (int)cd->nmcd.dwItemSpec;
                                cd->clrTextBk = (row % 2 == 1) ? kColorRowStripe : kColorCardBg;
                                cd->clrText = kColorText;
                                if (cd->iSubItem == 1)
                                {
                                    const unsigned int taskId = (unsigned int)cd->nmcd.lItemlParam;
                                    const int idx = FindTaskIndex(context, taskId);
                                    if (idx >= 0)
                                    {
                                        const UiTaskState st = context->Tasks[(size_t)idx].State;
                                        if (st == UiTaskState::Completed)
                                        {
                                            cd->clrText = kColorOk;
                                        }
                                        else if (st == UiTaskState::Extracting || st == UiTaskState::Preparing ||
                                                 st == UiTaskState::IndexLoaded)
                                        {
                                            cd->clrText = kColorAccent;
                                        }
                                        else if (st == UiTaskState::Failed)
                                        {
                                            cd->clrText = kColorBad;
                                        }
                                        else
                                        {
                                            cd->clrText = kColorMuted;
                                        }
                                    }
                                }
                                ::SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, CDRF_NEWFONT);
                                return TRUE;
                            }
                            default:
                                break;
                        }
                    }
                }
                break;
            }
            case WM_DROPFILES:
            {
                HDROP drop = (HDROP)wParam;
                UINT fileCount = ::DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0u);
                std::vector<std::wstring> paths;
                paths.reserve(fileCount);

                for (UINT index = 0; index < fileCount; ++index)
                {
                    wchar_t path[MaxUiPath]{};
                    if (::DragQueryFileW(drop, index, path, (UINT)_countof(path)) > 0u)
                    {
                        paths.emplace_back(path);
                    }
                }

                ::DragFinish(drop);
                AddPackageFiles(context, paths);
                UpdateSummary(context);
                return TRUE;
            }
            case WM_COMMAND:
            {
                switch (LOWORD(wParam))
                {
                    case IDC_AddFiles:
                        AddPackageFiles(context, BrowsePackageFiles(hwnd));
                        UpdateSummary(context);
                        return TRUE;
                    case IDC_BrowseOutput:
                    {
                        std::wstring folder = BrowseFolder(hwnd, L"选择解包输出目录");
                        if (!folder.empty())
                        {
                            ::SetWindowTextW(context->OutputEdit, folder.c_str());
                        }
                        return TRUE;
                    }
                    case IDC_ClearCompleted:
                        ClearFinishedTasks(context);
                        UpdateSummary(context);
                        return TRUE;
                }
                break;
            }
            case WM_APP_TASK_UPDATE:
            {
                TaskUpdateMessage* message = (TaskUpdateMessage*)lParam;
                if (message)
                {
                    int taskIndex = FindTaskIndex(context, message->TaskId);
                    if (taskIndex >= 0)
                    {
                        TaskRecord& task = context->Tasks[(size_t)taskIndex];
                        const bool wasFinished = task.Finished;

                        // 收尾兜底消息带的是 0/0，不能拿它把已经收到的真实进度数字清掉。
                        if (message->Total > 0u)
                        {
                            task.Current = message->Current;
                            task.Total = message->Total;
                        }

                        // 兜底消息只负责"把这一行收尾"，不覆盖解包器给出的更详细的原因
                        // （失败时那条明细里带着首条失败原因，很有用）。
                        if (!message->IsFallback || !wasFinished)
                        {
                            task.Detail = message->Detail;
                        }
                        task.LastTick = ::GetTickCount64();

                        switch ((Engine::ExtractTaskState)message->State)
                        {
                            case Engine::ExtractTaskPreparing:
                                task.State = UiTaskState::Preparing;
                                break;
                            case Engine::ExtractTaskIndexLoaded:
                                task.State = UiTaskState::IndexLoaded;
                                break;
                            case Engine::ExtractTaskExtracting:
                                task.State = UiTaskState::Extracting;
                                break;
                            case Engine::ExtractTaskCompleted:
                                task.State = UiTaskState::Completed;
                                task.Finished = true;
                                task.Succeeded = true;
                                // 完成的含义就是跑满了：兜底消息没带数字时也要显示成 100%。
                                if (task.Total > 0u)
                                {
                                    task.Current = task.Total;
                                }
                                break;
                            case Engine::ExtractTaskFailed:
                                task.State = UiTaskState::Failed;
                                task.Finished = true;
                                task.Succeeded = false;
                                break;
                            default:
                                task.State = UiTaskState::Queued;
                                break;
                        }

                        RefreshTaskRow(context, taskIndex);
                        WriteUiLog(context,
                                   L"Task update: #%u state=%u current=%u total=%u detail=%s",
                                   task.Id,
                                   message->State,
                                   message->Current,
                                   message->Total,
                                   message->Detail);
                    }

                    ::HeapFree(::GetProcessHeap(), 0, message);
                }
                AdvanceBatchIfIdle(context);
                UpdateSummary(context);
                return TRUE;
            }
            case WM_APP_WATCHDOG_TICK:
                AdvanceBatchIfIdle(context);
                UpdateSummary(context);
                return TRUE;
            case WM_CLOSE:
                ::DestroyWindow(hwnd);
                return TRUE;
            case WM_DESTROY:
                ::PostQuitMessage(0);
                return TRUE;
        }
        return FALSE;
    }
}

namespace UI
{
    DWORD WINAPI RunBatchExtractorUi(LPVOID parameter)
    {
        StartupContext startup = *(StartupContext*)parameter;

        INITCOMMONCONTROLSEX initControls{};
        initControls.dwSize = sizeof(initControls);
        initControls.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
        ::InitCommonControlsEx(&initControls);

        UiContext* context = new (std::nothrow) UiContext{};
        if (!context)
        {
            return 0u;
        }

        context->Startup = startup;
        context->QueueEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        context->StopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        context->WatchdogThread = nullptr;
        context->NextTaskId = 1u;
        context->ActiveBatchId = 1u;
        context->NextBatchId = 2u;
        context->DisplayedOverallPercent = 0u;
        context->ActiveWorkers = 0;
        context->CompletionNotified = false;
        ::InitializeCriticalSection(&context->QueueLock);
        ::InitializeCriticalSection(&context->LogLock);

        context->ModuleDirectory = GetDirectoryName(GetModulePath(startup.ModuleInstance));
        // 日志统一落在 <工具根>\Log\，跟解包器/Loader 写同一个目录。
        context->UiLogPath = Log::LogFilePath(Log::ResolveLogDirectory(context->ModuleDirectory), L"ExtractorUI.log");

        std::wstring gameDirectory = GetDirectoryName(GetModulePath(::GetModuleHandleW(nullptr)));
        context->DefaultOutputDirectory = CombinePath(gameDirectory, L"Extractor_Output");

        if (context->Startup.Api.SetExtractProgressCallback)
        {
            context->Startup.Api.SetExtractProgressCallback(ExtractProgressThunk, context);
        }

        unsigned int workerCount = ResolveWorkerCount();
        for (unsigned int index = 0u; index < workerCount; ++index)
        {
            HANDLE worker = ::CreateThread(nullptr, 0u, WorkerThreadProc, context, 0u, nullptr);
            if (worker)
            {
                context->Workers.push_back(worker);
            }
        }

        context->WatchdogThread = ::CreateThread(nullptr, 0u, WatchdogThreadProc, context, 0u, nullptr);

        WriteUiLog(context, L"Batch extractor UI started with %u workers", (unsigned int)context->Workers.size());

        HWND hwnd = ::CreateDialogParamW(startup.ModuleInstance,
                                         MAKEINTRESOURCEW(IDD_MainForm),
                                         nullptr,
                                         ExtractorDialogWindProc,
                                         (LPARAM)context);
        if (!hwnd)
        {
            WriteUiLog(context, L"CreateDialogParamW failed");
            if (context->Startup.Api.SetExtractProgressCallback)
            {
                context->Startup.Api.SetExtractProgressCallback(nullptr, nullptr);
            }
            ::SetEvent(context->StopEvent);
            if (context->QueueEvent)
            {
                ::SetEvent(context->QueueEvent);
            }

            for (HANDLE worker : context->Workers)
            {
                ::WaitForSingleObject(worker, INFINITE);
                ::CloseHandle(worker);
            }
            context->Workers.clear();

            if (context->WatchdogThread)
            {
                ::WaitForSingleObject(context->WatchdogThread, INFINITE);
                ::CloseHandle(context->WatchdogThread);
            }

            if (context->QueueEvent)
            {
                ::CloseHandle(context->QueueEvent);
            }
            if (context->StopEvent)
            {
                ::CloseHandle(context->StopEvent);
            }

            ::DeleteCriticalSection(&context->LogLock);
            ::DeleteCriticalSection(&context->QueueLock);
            delete context;
            return 0u;
        }
        ::ShowWindow(hwnd, SW_NORMAL);

        MSG msg{};
        while (BOOL ret = ::GetMessageW(&msg, nullptr, 0u, 0u))
        {
            if (ret == -1)
            {
                break;
            }

            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }

        if (context->Startup.Api.SetExtractProgressCallback)
        {
            context->Startup.Api.SetExtractProgressCallback(nullptr, nullptr);
        }

        ::SetEvent(context->StopEvent);
        if (context->QueueEvent)
        {
            ::SetEvent(context->QueueEvent);
        }

        for (HANDLE worker : context->Workers)
        {
            ::WaitForSingleObject(worker, INFINITE);
            ::CloseHandle(worker);
        }
        context->Workers.clear();

        if (context->WatchdogThread)
        {
            ::WaitForSingleObject(context->WatchdogThread, INFINITE);
            ::CloseHandle(context->WatchdogThread);
        }

        WriteUiLog(context, L"Batch extractor UI stopped");

        if (context->QueueEvent)
        {
            ::CloseHandle(context->QueueEvent);
        }
        if (context->StopEvent)
        {
            ::CloseHandle(context->StopEvent);
        }
        if (context->FontUi)
        {
            ::DeleteObject(context->FontUi);
        }
        if (context->FontBold)
        {
            ::DeleteObject(context->FontBold);
        }
        if (context->WhiteBrush)
        {
            ::DeleteObject(context->WhiteBrush);
        }

        ::DeleteCriticalSection(&context->LogLock);
        ::DeleteCriticalSection(&context->QueueLock);
        delete context;
        return 0u;
    }
}
