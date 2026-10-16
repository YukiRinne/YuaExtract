// YuaExtract 步骤3「资源名还原」窗口实现。
//
// 结构照 CxdecExtractorUI/BatchExtractorUI.cpp（"按队列 + N 工作线程 + 任务列表 +
// 详细日志"的现成实现）来，但有两点关键差别：
//
// 1) 那个跑在被注入的游戏进程里，本窗口跑在 YuaExtract 进程内，所以没有"宿主接口
//    没准备好"这种中间态，任务体就是一个纯函数 RestoreSinglePackage。
// 2) 它把"丢消息"修在 PostTaskUpdate 里（关键状态重试投递）；这里更进一步：
//    任务的终态由工作线程拿到 RestoreSinglePackage 的返回值后直接写进任务记录，
//    进度消息只是"让界面早点知道"。即便一条 PostMessage 都没送到，任务也不会永远
//    停在"处理中 100%" —— 界面重画时读到的仍然是已经写好的终态。

#include "YuaRestoreWindow.h"

#include "HashNameRestorer.h"
#include "YuaExtractHost.h"
#include "YuaExtractWorkspace.h"

#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>

#include <algorithm>
#include <cstdarg>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace Yua
{
namespace Restore
{
namespace
{
    // ---------- 控件编号 ----------

    enum : int
    {
        IDC_RS_OUTPUT_LABEL = 5001,
        IDC_RS_OUTPUT_EDIT = 5002,
        IDC_RS_WORKER_LABEL = 5003,
        IDC_RS_WORKER_EDIT = 5004,
        IDC_RS_STATUS = 5005,
        IDC_RS_LOG = 5006,
        IDC_RS_PROGRESS = 5007,
        IDC_RS_LIST = 5008,
        IDC_RS_KEEP_CHECK = 5009,
        IDC_RS_INFER_CHECK = 5010,
        IDC_RS_START = 5011,
        IDC_RS_OPEN_DIR = 5012,
        IDC_RS_BACK = 5013,
        IDC_RS_LOG_LABEL = 5014,
        IDC_RS_LIST_LABEL = 5015,
    };

    // 工作线程 → 界面线程的消息。lParam 都是 HeapAlloc 出来的，界面线程负责释放。
    constexpr UINT kMsgTaskUpdate = WM_APP + 0x501;  // TaskUpdateMessage*
    constexpr UINT kMsgRunFinished = WM_APP + 0x502; // 无载荷，界面去查 FinishIssued
    constexpr UINT kMsgWatchdog = WM_APP + 0x503;    // 兜底定时器：没有在跑的任务就收尾

    // 挖掘线程 → 界面线程的消息。挖掘线程**绝不允许**碰窗口 API（LogLine / SetWindowTextW /
    // 列表都是界面线程的东西），进度和结论只能靠这两条消息搬回来。
    // lParam 都是堆上的载荷，界面线程接管并释放；PostMessage 失败时由发消息的一方释放。
    constexpr UINT kMsgSupplementProgress = WM_APP + 0x504; // SupplementProgressMessage*
    constexpr UINT kMsgSupplementDone = WM_APP + 0x505;     // SupplementOutcome*

    constexpr UINT_PTR kWatchdogTimerId = 2;
    constexpr UINT kWatchdogIntervalMs = 700;

    // "正在还原"这一类更新的最小投递间隔。
    //
    // 还原一个封包里几万个文件会调几万次进度回调，而 Windows 的 posted message 队列
    // 上限是一万条，满了 PostMessage 直接失败。CxdecExtractorUI 的实测数据：一次 12 包
    // 的解包共投递 120290 条、界面只收到 54809 条（丢 54%），丢的里面甚至包括某个包的
    // 收尾状态，那一行就永远停在"处理中"。节流到 100ms 一条后总量降到几百条，
    // 队列不可能再被打满。节流只作用于"还原中"的高频进度，关键状态一律照发。
    constexpr DWORD kProgressThrottleMs = 100u;

    // 挖掘进度的最小刷屏间隔。内核的进度回调是按"每 64 个文件 / 每 65536 个候选"来的，
    // 一个包几十万个文件时回调并不稀疏，每次都写日志会把界面线程的时间全花在刷日志上。
    // 节流状态放在**界面线程**：挖掘线程只负责把三个计数投出去，不持有任何界面状态。
    constexpr DWORD kSupplementLogIntervalMs = 1000u;

    constexpr unsigned int kMinWorkers = 1u;
    constexpr unsigned int kMaxWorkers = 32u;
    constexpr unsigned int kDefaultWorkers = 4u;

    const wchar_t* kWindowTitle = L"YuaExtract 资源名还原";

    constexpr int kMargin = 10;
    constexpr int kRowHeight = 24;
    constexpr int kRowGap = 8;
    constexpr int kOutputLabelWidth = 76;
    constexpr int kWorkerLabelWidth = 60;
    constexpr int kWorkerEditWidth = 70;
    constexpr int kSplitTitleHeight = 22;
    constexpr int kButtonWidth = 110;
    constexpr int kButtonHeight = 28;
    constexpr int kCheckHeight = 22;
    constexpr int kProgressHeight = 8;
    constexpr int kMinLogHeight = 80;
    constexpr int kMinListHeight = 90;

    constexpr COLORREF kColorMuted = RGB(0x66, 0x66, 0x66);
    constexpr COLORREF kColorLogBack = RGB(0xF2, 0xF2, 0xF2);

    // 一次任务更新的载荷。字段全部是值传递，界面线程不碰工作线程的内存。
    struct TaskUpdateMessage
    {
        unsigned int TaskId;
        unsigned int State;
        unsigned int Step;
        unsigned int Percent;
        wchar_t Entry[256];
        wchar_t Info[512];
    };

    // 一次挖掘进度通知的载荷：只搬三个计数。
    // 界面线程收到后自己按 kSupplementLogIntervalMs 节流再写日志/状态栏 ——
    // 节流是界面的事，挖掘线程不该知道日志长什么样。
    struct SupplementProgressMessage
    {
        unsigned int ScannedFiles;
        unsigned int Candidates;
        unsigned int Hits;
    };

    struct TaskRecord
    {
        unsigned int Id = 0;
        std::wstring PackageName;
        std::wstring PackageDirectory;
        bool RootLevel = false; // true = 解包目录根下的散文件（只扫本层）
        unsigned int FileCount = 0;
        unsigned long long Bytes = 0;

        // 下面这些字段由工作线程写、界面线程读，一律在 QueueLock 保护下访问。
        RestoreTaskState State = kStateQueued;
        RestoreProgressStep Step = RestoreProgressStep::Queued;
        unsigned int Percent = 0;   // 封包内进度（台阶口径见 ComputeTaskProgressPercent）
        std::wstring CurrentEntry;  // "当前条目"列
        std::wstring Info;          // "详细信息"列
        bool Finished = false;
        RestorePackageResult Result; // 返回值原样存着，写报告时直接汇总
        DWORD ElapsedMs = 0;
    };

    struct UiContext
    {
        HINSTANCE Instance = nullptr;
        HWND Window = nullptr;
        HWND Owner = nullptr;

        HWND OutputLabel = nullptr;
        HWND OutputEdit = nullptr;
        HWND WorkerLabel = nullptr;
        HWND WorkerEdit = nullptr;
        HWND StatusText = nullptr;
        HWND LogLabel = nullptr;
        HWND LogView = nullptr;
        HWND ListLabel = nullptr;
        HWND ProgressBar = nullptr;
        HWND TaskList = nullptr;
        HWND KeepCheck = nullptr;  // 复选框一：保留未还原文件名的文件（保留时按文件头补后缀）
        HWND InferCheck = nullptr; // 复选框二：补充还原（挖掘候选名 + 哈希验证）
        HWND StartButton = nullptr;
        HWND OpenDirButton = nullptr;
        HWND BackButton = nullptr;

        HFONT FontUi = nullptr;
        HFONT FontLog = nullptr;
        HBRUSH LogBrush = nullptr;

        std::wstring GameDirectory;
        std::wstring SourceDirectory;
        std::wstring HashDirectory;
        std::wstring LogDirectory;
        std::wstring OutputDirectory;
        // 补充还原挖到的名字落到哪儿。工具维护的一份映射，与映射目录下其它 *.lst 一起被读取。
        std::wstring MinedListPath;

        HashMapTables Tables;
        unsigned int DirectoryMapEntries = 0;
        unsigned int FileNameMapEntries = 0;
        // 补充还原的结论，写完报告就交给 FinishRun（报告里要如实写出挖掘数据）。
        SupplementOutcome Supplement;

        // ---- 两遍式还原（勾了补充还原时）----
        // 用户要求的顺序：先用步骤2 收集的映射做一遍**普通还原**，再跑挖掘补充，
        // 最后把新解析出来的文件补进输出目录。挖掘必须排在普通还原之后 ——
        // 它是"补漏"，普通还原的基线要先行。
        //
        // Pass 1：普通还原（挖掘不跑）。全部任务到终态后：
        //   - 不勾补充还原 → 直接收尾（FinishRun）
        //   - 勾了补充还原 → 起挖掘线程 → （挖完经 kMsgSupplementDone 回来）重置任务表 → Pass 2
        // Pass 2：只补第一遍没交付的（alreadyDelivered 跳过）→ 收尾。
        //
        // 第一遍的结果存在 FirstPassResults，第二遍的结果存 Tasks，写报告时合并 ——
        // 报告要同时呈现"普通还原多少、补充还原新增多少"，而不是只写第二遍的数字。
        bool SupplementPending = false;  // true = Pass 1 结束后要跑挖掘再来 Pass 2
        unsigned int PassNumber = 1;     // 当前是第几遍（1 = 普通还原遍）
        std::vector<RestorePackageResult> FirstPassResults; // Pass 1 各封包结果（报告合并用）
        std::set<std::wstring> FirstPassDelivered;          // Pass 1 已交付的目标（Pass 2 跳过用）
        // **防重入闸门**：语义是"挖掘线程正在跑"（true 期间共享映射表在被改写）。
        // 挖完在 kMsgSupplementDone 里清、窗口销毁时在 join 之后清；OnStart 也拿它拒重入。
        // watchdog 每次进来都必须先看它：挖掘期间第一遍的任务表还全是终态，不挡住就会把
        // "第一遍已完成"再判一次 —— 收尾被提前触发（StopEvent 置位 → 挖掘被中止 →
        // 工作线程被停 → 第二遍任务永远排队），这正是用户实测卡住的成因。
        bool InSupplementMining = false;
        // 挖掘线程句柄。kMsgSupplementDone 的处理分支 join 一次就收掉；WM_DESTROY 里兜底
        // 再 join（窗口在挖掘中途被关掉时走这条）。**两处都必须等它退出来**才能释放 context ——
        // 它正在用 context 里的映射表和目录字符串。
        HANDLE MiningThread = nullptr;
        // 第二遍已启动过（防止同一遍被重复启动 / 同一轮里挖两次）。
        bool Pass2Started = false;
        // 挖掘进度的节流状态（界面线程侧）：上一次写日志的时刻与当时的候选数。
        DWORD LastSupplementLogTick = 0;
        unsigned int LastSupplementCandidates = 0u;

        CRITICAL_SECTION QueueLock{};
        std::vector<TaskRecord> Tasks;
        std::vector<unsigned int> Queue;
        HANDLE QueueEvent = nullptr;
        HANDLE StopEvent = nullptr;
        std::vector<HANDLE> Workers;
        unsigned int ActiveWorkers = 0;
        unsigned int NextTaskId = 1;

        bool Running = false;
        bool FinishIssued = false;
        bool Closed = false;
        // 本次运行勾没勾「保留未还原文件名的文件」。收尾时的摘要要看它才知道
        // "未还原的那些到底是被丢掉了还是被保留了"，没必要去读复选框（那时它可能已经变了）。
        bool KeepUnrestored = false;
        int LastAggregatePercent = -1;
        DWORD LastProgressPostTick = 0;
        std::wstring LastLogLine;
    };

    UiContext* g_Context = nullptr;

    // ---------- 小工具 ----------

    std::wstring FormatString(const wchar_t* format, ...)
    {
        wchar_t buffer[1024]{};
        va_list arguments;
        va_start(arguments, format);
        const int count = _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, arguments);
        va_end(arguments);
        return count > 0 ? std::wstring(buffer, static_cast<size_t>(count)) : std::wstring();
    }

    std::wstring GetControlText(HWND control)
    {
        const int length = ::GetWindowTextLengthW(control);
        if (length <= 0)
        {
            return std::wstring();
        }
        std::wstring text(static_cast<size_t>(length) + 1u, L'\0');
        const int copied = ::GetWindowTextW(control, &text[0], length + 1);
        if (copied <= 0)
        {
            return std::wstring();
        }
        text.resize(static_cast<size_t>(copied));
        return text;
    }

    std::wstring TrimTrailingSeparators(std::wstring path)
    {
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        {
            path.pop_back();
        }
        return path;
    }

    std::wstring ToLowerString(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(),
                       [](wchar_t ch) { return static_cast<wchar_t>(::towlower(ch)); });
        return value;
    }

    bool StartsWithNoCase(const std::wstring& text, const std::wstring& prefix)
    {
        if (text.length() < prefix.length() || prefix.empty())
        {
            return false;
        }
        return _wcsnicmp(text.c_str(), prefix.c_str(), prefix.length()) == 0;
    }

    bool DirectoryExistsAt(const std::wstring& path)
    {
        if (path.empty())
        {
            return false;
        }
        const DWORD attributes = ::GetFileAttributesW(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    const wchar_t* ToStateText(RestoreTaskState state)
    {
        switch (state)
        {
            case kStateQueued: return L"排队中";
            case kStateRunning: return L"处理中";
            case kStateCompleted: return L"已完成";
            case kStateFailed: return L"失败";
        }
        return L"未知";
    }

    // ---------- 日志 ----------

    // 日志控件是只读 Edit，追加走"选中末尾 + 替换"。
    // 用 WM_SETTEXT 每次重设全文在几万行时会明显卡顿，而这个窗口的日志量不小。
    void AppendLogRaw(UiContext* context, const std::wstring& text)
    {
        if (!context || !context->LogView || text.empty())
        {
            return;
        }
        const std::wstring line = text + L"\r\n";
        const int length = ::GetWindowTextLengthW(context->LogView);
        ::SendMessageW(context->LogView, EM_SETSEL, static_cast<WPARAM>(length),
                       static_cast<LPARAM>(length));
        ::SendMessageW(context->LogView, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(line.c_str()));
        ::SendMessageW(context->LogView, EM_SCROLLCARET, 0, 0);
    }

    // skipDuplicate：完全一样的一行不再刷屏（兜底消息会把同一状态重发一次）。
    void LogLine(UiContext* context, bool skipDuplicate, const wchar_t* format, ...)
    {
        wchar_t buffer[1024]{};
        va_list arguments;
        va_start(arguments, format);
        const int count = _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, arguments);
        va_end(arguments);
        if (count <= 0)
        {
            return;
        }

        SYSTEMTIME time{};
        ::GetLocalTime(&time);
        const std::wstring line =
            FormatString(L"%02u:%02u:%02u  %s", time.wHour, time.wMinute, time.wSecond, buffer);

        if (skipDuplicate && context)
        {
            if (context->LastLogLine == line)
            {
                return;
            }
            context->LastLogLine = line;
        }
        AppendLogRaw(context, line);

        // 同时写进工具日志：用户点「返回主程序」之后这条线索不能跟着窗口一起消失。
        if (context && !context->LogDirectory.empty())
        {
            Host::LogLine(Log::Level::Info, std::wstring(L"[YuaExtract] 还原：") + buffer);
        }
    }

    // ---------- 任务表 ----------

    int FindTaskIndexLocked(UiContext* context, unsigned int taskId)
    {
        for (size_t index = 0; index < context->Tasks.size(); ++index)
        {
            if (context->Tasks[index].Id == taskId)
            {
                return static_cast<int>(index);
            }
        }
        return -1;
    }

    int FindListRowByTaskId(HWND listView, unsigned int taskId)
    {
        LVFINDINFOW info{};
        info.flags = LVFI_PARAM;
        info.lParam = static_cast<LPARAM>(taskId);
        return ListView_FindItem(listView, -1, &info);
    }

    std::wstring ToProgressText(const TaskRecord& task)
    {
        if (task.Finished)
        {
            return task.State == kStateCompleted ? std::wstring(L"100%") : std::wstring(L"已停止");
        }
        if (task.State == kStateQueued)
        {
            return std::wstring(L"等待中");
        }
        if (task.Step == RestoreProgressStep::Finishing)
        {
            return std::wstring(L"收尾中");
        }
        if (task.FileCount == 0u)
        {
            return std::wstring(L"准备中");
        }
        // 百分比是按文件数折算的，这里反过来把"第几个文件"也显示出来，
        // 用户才能判断"不动"和"只是慢"的区别。
        const unsigned int done = task.Percent * 90u / 100u;
        const unsigned int files = task.FileCount * done / 100u;
        return FormatString(L"%u%% (%u/%u)", done, files, task.FileCount);
    }

    // 只在界面线程调用：行不存在就新建，存在就原地刷新。
    void RefreshTaskRow(UiContext* context, int taskIndex)
    {
        if (!context || taskIndex < 0 || taskIndex >= static_cast<int>(context->Tasks.size()))
        {
            return;
        }

        TaskRecord snapshot;
        ::EnterCriticalSection(&context->QueueLock);
        snapshot = context->Tasks[static_cast<size_t>(taskIndex)];
        ::LeaveCriticalSection(&context->QueueLock);

        int row = FindListRowByTaskId(context->TaskList, snapshot.Id);
        if (row < 0)
        {
            LVITEMW item{};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            item.iItem = ListView_GetItemCount(context->TaskList);
            item.pszText = const_cast<LPWSTR>(snapshot.PackageName.c_str());
            item.lParam = static_cast<LPARAM>(snapshot.Id);
            row = ListView_InsertItem(context->TaskList, &item);
        }

        ListView_SetItemText(context->TaskList, row, 0, const_cast<LPWSTR>(snapshot.PackageName.c_str()));
        ListView_SetItemText(context->TaskList, row, 1, const_cast<LPWSTR>(ToStateText(snapshot.State)));

        const std::wstring progress = ToProgressText(snapshot);
        ListView_SetItemText(context->TaskList, row, 2, const_cast<LPWSTR>(progress.c_str()));
        ListView_SetItemText(context->TaskList, row, 3, const_cast<LPWSTR>(snapshot.CurrentEntry.c_str()));
        ListView_SetItemText(context->TaskList, row, 4, const_cast<LPWSTR>(snapshot.Info.c_str()));
    }

    void UpdateSummary(UiContext* context)
    {
        if (!context)
        {
            return;
        }

        unsigned int queued = 0u;
        unsigned int running = 0u;
        unsigned int completed = 0u;
        unsigned int failed = 0u;
        unsigned long long aggregate = 0u;
        size_t total = 0u;

        ::EnterCriticalSection(&context->QueueLock);
        total = context->Tasks.size();
        for (const TaskRecord& task : context->Tasks)
        {
            switch (task.State)
            {
                case kStateQueued: ++queued; break;
                case kStateRunning: ++running; break;
                case kStateCompleted: ++completed; break;
                case kStateFailed: ++failed; break;
            }
            aggregate += ComputeTaskProgressPercent(task.Step, task.Percent);
        }
        ::LeaveCriticalSection(&context->QueueLock);

        const unsigned int percent =
            total > 0u ? static_cast<unsigned int>(aggregate / total) : 0u;
        // 进度条单调不回退：并发下任务完成先后会让裸平均分上下跳。
        if (static_cast<int>(percent) > context->LastAggregatePercent)
        {
            context->LastAggregatePercent = static_cast<int>(percent);
        }
        const int shown = context->LastAggregatePercent < 0 ? 0 : context->LastAggregatePercent;

        ::SetWindowTextW(context->StatusText,
                         FormatString(L"总任务 %u | 运行中 %u | 排队 %u | 完成 %u | 失败 %u",
                                      static_cast<unsigned int>(total), running, queued, completed, failed)
                             .c_str());
        ::SendMessageW(context->ProgressBar, PBM_SETPOS, static_cast<WPARAM>(shown), 0);
        ::SetWindowTextW(context->Window,
                         FormatString(L"%s - 运行中 %u / 排队 %u / 完成 %u", kWindowTitle, running, queued,
                                      completed)
                             .c_str());
    }

    // ---------- 收尾 ----------

    void SetInputsEnabled(UiContext* context, bool enabled)
    {
        ::EnableWindow(context->OutputEdit, enabled);
        ::EnableWindow(context->WorkerEdit, enabled);
        ::EnableWindow(context->KeepCheck, enabled);
        ::EnableWindow(context->InferCheck, enabled);
        ::EnableWindow(context->StartButton, enabled);
    }

    void NotifyOwner(UiContext* context, RestoreTaskState state, unsigned int percent)
    {
        if (context->Owner && ::IsWindow(context->Owner))
        {
            // 只是"该刷新概览了"。投递失败也无所谓 —— 真正的结果写在报告文件里，
            // 主界面自己有定时器会重新读。
            ::PostMessageW(context->Owner, kRestoreNotifyMessage, static_cast<WPARAM>(state),
                           static_cast<LPARAM>(percent));
        }
    }

    // 全部任务收尾之后跑一次：汇总写报告、停线程、刷新界面。
    void FinishRun(UiContext* context)
    {
        if (!context->FinishIssued)
        {
            return;
        }

        RestoreReportData report;
        report.sourceDirectory = context->SourceDirectory;
        report.hashDirectory = context->HashDirectory;
        report.outputDirectory = context->OutputDirectory;
        report.mapSources = context->Tables.sources;
        report.directoryMapEntries = context->DirectoryMapEntries;
        report.fileNameMapEntries = context->FileNameMapEntries;
        // 补充还原的结论要如实进报告：扫描数/候选数/试算数/命中数/是否触顶，
        // 以及盐不可用时的原因。用户是照着报告判断"这次为什么只还原了这么多"的。
        report.supplement = context->Supplement;
        report.minedListPath = context->MinedListPath;

        unsigned int failed = 0u;
        {
            ::EnterCriticalSection(&context->QueueLock);
            for (const TaskRecord& task : context->Tasks)
            {
                if (task.Finished)
                {
                    report.packages.push_back(task.Result);
                    if (task.State == kStateFailed)
                    {
                        ++failed;
                    }
                }
            }
            ::LeaveCriticalSection(&context->QueueLock);
        }

        // 两遍式：把第一遍（普通还原）的结果并进报告。两个 pass 的**交付集互斥**
        //（第二遍跳过第一遍已交付的），所以只有"已还原/复制量"是可加的；
        // 其余计数（缺目录/缺文件/丢弃/补后缀/原样/失败/重名）反映的是**输出目录的
        // 最终状态**，以第二遍为准 —— 第一遍的同类数字描述的是中间状态，相加会把
        // 同一批文件数两遍（实测：保留未还原显示 3,709，磁盘实际 2,292）。
        if (!context->FirstPassResults.empty())
        {
            std::map<std::wstring, size_t> secondPassIndex;
            for (size_t index = 0; index < report.packages.size(); ++index)
            {
                secondPassIndex[report.packages[index].packageName] = index;
            }
            for (const RestorePackageResult& first : context->FirstPassResults)
            {
                const auto it = secondPassIndex.find(first.packageName);
                if (it == secondPassIndex.end())
                {
                    report.packages.push_back(first);
                    continue;
                }
                RestorePackageResult& second = report.packages[it->second];
                second.restoredFiles += first.restoredFiles;
                second.copiedBytes += first.copiedBytes;
                // 其余字段保持第二遍的值（磁盘最终状态），不动。
            }
        }

        const unsigned int packageCount = static_cast<unsigned int>(report.packages.size());
        const RestoreTotals totals = SumRestoreResults(report.packages);

        bool reportOk = false;
        std::wstring reportError;
        if (packageCount > 0u)
        {
            LogLine(context, false, L"全部任务结束，正在汇总还原报告……");
            reportOk = WriteRestoreReport(report, reportError);
            if (reportOk)
            {
                LogLine(context, false, L"还原报告：%s",
                        JoinPath(context->OutputDirectory, L"RestoreReport.txt").c_str());
            }
            else
            {
                LogLine(context, false, L"还原报告写出失败：%s", reportError.c_str());
            }
        }
        else
        {
            LogLine(context, false, L"没有可汇总的封包，跳过报告。");
        }

        // 停线程：先让工作线程退出去，再动任务表 —— 否则它们还会继续写。
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

        {
            ::EnterCriticalSection(&context->QueueLock);
            for (TaskRecord& task : context->Tasks)
            {
                if (task.State == kStateRunning)
                {
                    // 理论路径不会走到这里（工作线程写完终态才减少 ActiveWorkers），
                    // 留着是为了"永远停在处理中"这件事彻底不可能发生。
                    task.State = kStateFailed;
                    task.Info = L"任务未正常收尾，已按失败记";
                    task.Finished = true;
                    ++failed;
                }
                if (task.Step != RestoreProgressStep::Queued)
                {
                    task.Step = RestoreProgressStep::Finishing;
                    task.Percent = 100u;
                }
            }
            ::LeaveCriticalSection(&context->QueueLock);
        }

        for (size_t index = 0; index < context->Tasks.size(); ++index)
        {
            RefreshTaskRow(context, static_cast<int>(index));
        }

        context->Running = false;
        context->FinishIssued = false;
        context->ActiveWorkers = 0u;
        if (context->QueueEvent)
        {
            ::ResetEvent(context->QueueEvent);
        }
        ::KillTimer(context->Window, kWatchdogTimerId);
        SetInputsEnabled(context, true);
        UpdateSummary(context);

        // 两条摘要的占位符数量必须一致：分支只换格式串，实参列表是同一份。
        // 保留模式下第 4 个位置放"保留未还原 N"，默认模式下放"未还原被丢弃 N"，
        // 第 5 个位置两条都用不上时也照样传一个值（格式化函数不会去取多余的实参）。
        const std::wstring summary = FormatString(
            context->KeepUnrestored
                ? L"还原结束：%u 个封包，已还原 %s / %s，保留未还原 %s（其中按文件头补后缀 %s），失败 %u"
                : L"还原结束：%u 个封包，已还原 %s / %s，未还原被丢弃 %s（按文件头补后缀 %s），失败 %u",
            packageCount, FormatCount(totals.restoredFiles).c_str(),
            FormatCount(totals.hashNamedFiles).c_str(),
            FormatCount(context->KeepUnrestored
                            ? static_cast<unsigned long long>(totals.headerSuffixedFiles)
                            : static_cast<unsigned long long>(totals.droppedUnrestoredFiles))
                .c_str(),
            FormatCount(totals.headerSuffixedFiles).c_str(), failed);
        ::SetWindowTextW(context->StatusText, summary.c_str());
        AppendLogRaw(context, summary);

        // 还原率：详细日志里补一行"分母是什么"的口径说明 —— 状态栏那行只有绝对数，
        // 用户要判断"还差多少"就得知道分母是哈希名文件数（真正需要还原的那批），
        // 不是文件总数（里面还有 .alst 清单之类的原样透传文件）。
        // 两遍式时再带上"推理补充还原新增"：它是**合并后 restoredFiles 减第一遍
        // restoredFiles**，不是映射新增条数 —— 挖到的名字里可能有一些最终没能落盘。
        const double restoreRate = totals.hashNamedFiles > 0u
            ? totals.restoredFiles * 100.0 / static_cast<double>(totals.hashNamedFiles)
            : 0.0;
        const bool twoPass = context->Pass2Started && !context->FirstPassResults.empty();
        if (twoPass)
        {
            const RestoreTotals firstTotals = SumRestoreResults(context->FirstPassResults);
            const unsigned long long added =
                totals.restoredFiles > firstTotals.restoredFiles
                    ? static_cast<unsigned long long>(totals.restoredFiles - firstTotals.restoredFiles)
                    : 0ull;
            AppendLogRaw(context,
                         FormatString(L"还原率 %.2f%%（已还原 %s / 哈希名文件 %s，推理补充还原新增 %s）",
                                      restoreRate, FormatCount(totals.restoredFiles).c_str(),
                                      FormatCount(totals.hashNamedFiles).c_str(),
                                      FormatCount(added).c_str()));
        }
        else
        {
            AppendLogRaw(context, FormatString(L"还原率 %.2f%%（已还原 %s / 哈希名文件 %s）", restoreRate,
                                               FormatCount(totals.restoredFiles).c_str(),
                                               FormatCount(totals.hashNamedFiles).c_str()));
        }

        if (packageCount > 0u && !reportOk)
        {
            Host::ShowError(context->Window, L"还原已跑完，但报告没写出来：\r\n" + reportError);
        }
        else if (failed > 0u)
        {
            Host::ShowInfo(context->Window,
                           summary + L"\r\n\r\n有封包存在失败条目或未正常收尾，"
                                     L"详见列表「详细信息」列与日志。");
        }

        NotifyOwner(context, failed > 0u ? kStateFailed : kStateCompleted, 100u);
    }

    // 两遍式的第一段（界面线程）：收集第一遍成果 → 打第一遍还原率 → 起挖掘线程 → 立即返回。
    // 返回 false = 这一轮起不来（窗口在关 / 建线程失败），调用方应直接走收尾。
    // 定义在 SupplementMiningThreadProc 之后（它要用那个线程体）。
    bool StartSupplementMining(UiContext* context);
    // 两遍式的第二段（界面线程，挖掘结论回来后）：重置任务表 → 重新排入全部封包。
    // 返回 false 表示窗口正在关闭，调用方应直接走收尾。
    bool StartPass2(UiContext* context);
    DWORD WINAPI SupplementMiningThreadProc(LPVOID parameter);

    // 界面线程判定"没有正在跑的任务"就收尾。这不是主路径（主路径是最后一个工作线程
    // 自己投递 kMsgRunFinished），而是"投递被丢弃"时的兜底 —— 只要界面还能收到定时器，
    // 任务就不会永远停在处理中。
    void CheckRunCompletion(UiContext* context)
    {
        if (!context->Running || context->FinishIssued)
        {
            return;
        }
        // 挖掘线程跑着的时候绝不判定收尾：见 InSupplementMining 的注释。
        if (context->InSupplementMining)
        {
            return;
        }

        bool allFinished = true;
        ::EnterCriticalSection(&context->QueueLock);
        const bool runningWorker = context->ActiveWorkers > 0u;
        const bool queueEmpty = context->Queue.empty();
        for (const TaskRecord& task : context->Tasks)
        {
            if (!task.Finished)
            {
                allFinished = false;
                break;
            }
        }
        ::LeaveCriticalSection(&context->QueueLock);

        if (allFinished && !runningWorker && queueEmpty)
        {
            // 两遍式：第一遍结束先**不收尾**，转去起挖掘线程；挖完由 kMsgSupplementDone
            // 的处理分支接着跑第二遍。FinishIssued 只在真正要写报告时才置位。
            if (context->SupplementPending && !context->Pass2Started)
            {
                context->SupplementPending = false;
                context->Pass2Started = true;
                if (StartSupplementMining(context))
                {
                    return;   // 挖掘线程已起：等它的结论消息回来再走第二遍
                }
                // 起不来（窗口关了 / 建线程失败）：StopEvent 已置位或已记过日志，落到收尾也安全。
            }
            context->FinishIssued = true;
            LogLine(context, false, L"所有任务已到终态，开始收尾。");
        }
    }

    // 两遍式的第一段（界面线程）：收集第一遍成果 → 打第一遍还原率 → 起挖掘线程。
    //
    // 挖掘要跑几分钟，**绝不能**像旧实现那样在界面线程里同步跑 —— 那正是用户反馈的
    // "卡顿"根源。这里只把线程起起来就返回，界面立刻回到消息循环；进度和结论都由
    // 挖掘线程投消息回来（kMsgSupplementProgress / kMsgSupplementDone）。
    bool StartSupplementMining(UiContext* context)
    {
        if (context->Closed || ::WaitForSingleObject(context->StopEvent, 0) == WAIT_OBJECT_0)
        {
            return false;
        }

        // 1) 收集第一遍的成果：结果（写报告合并用）+ 已交付目标（第二遍跳过用）。
        context->FirstPassResults.clear();
        context->FirstPassDelivered.clear();
        {
            ::EnterCriticalSection(&context->QueueLock);
            for (const TaskRecord& task : context->Tasks)
            {
                if (task.Finished)
                {
                    context->FirstPassResults.push_back(task.Result);
                    for (const std::wstring& target : task.Result.deliveredRelative)
                    {
                        context->FirstPassDelivered.insert(target);
                    }
                }
            }
            ::LeaveCriticalSection(&context->QueueLock);
        }
        LogLine(context, false, L"第一遍交付 %s 个文件，作为推理补充还原的跳过基线。",
                FormatCount(static_cast<unsigned long long>(context->FirstPassDelivered.size())).c_str());

        // 2) 第一遍还原率。分母取"哈希名文件"（真正需要还原的那批），不是文件总数 ——
        //    要还多少、还原出来多少，这两个数才是用户想看的比例。0 个哈希名文件时写
        //    0.00%，别在格式化参数里做除零。
        const RestoreTotals firstTotals = SumRestoreResults(context->FirstPassResults);
        AppendLogRaw(context, FormatString(L"第一遍普通还原：已还原 %s / %s（%.2f%%）",
                                           FormatCount(firstTotals.restoredFiles).c_str(),
                                           FormatCount(firstTotals.hashNamedFiles).c_str(),
                                           firstTotals.hashNamedFiles > 0u
                                               ? firstTotals.restoredFiles * 100.0 /
                                                     static_cast<double>(firstTotals.hashNamedFiles)
                                               : 0.0));

        // 3) 起挖掘线程。优先级降一档：挖掘是纯哈希试算，跑满一个核会跟界面重绘抢 CPU。
        LogLine(context, false, L"普通还原完成，开始推理补充还原（挖掘候选名）……");
        context->LastSupplementLogTick = 0;
        context->LastSupplementCandidates = 0u;
        context->InSupplementMining = true;
        context->MiningThread = ::CreateThread(nullptr, 0u, SupplementMiningThreadProc, context, 0u, nullptr);
        if (!context->MiningThread)
        {
            // 闸门必须先放开：否则 watchdog 会一直以为挖掘还跑着，这一轮永远收不了尾。
            context->InSupplementMining = false;
            LogLine(context, false, L"启动挖掘线程失败，跳过推理补充还原，直接收尾。");
            return false;
        }
        ::SetThreadPriority(context->MiningThread, THREAD_PRIORITY_BELOW_NORMAL);
        LogLine(context, false, L"推理补充还原：开始从游戏资源里挖掘候选名……");
        if (context->MinedListPath.empty())
        {
            LogLine(context, false, L"  提示：没有定位到映射目录，挖到的名字只用于本次还原，不落盘。");
        }
        return true;
    }

    // 两遍式的第二段（界面线程，由 kMsgSupplementDone 的处理分支调用）：
    // 挖掘结论已经拿到、共享映射表已经补全，这里把全部封包重新排一遍（Pass 2）。
    // 工作线程从第一遍起就一直活着（挖完没有收尾），所以只要重置任务表 + 放开队列事件。
    bool StartPass2(UiContext* context)
    {
        if (context->Closed || ::WaitForSingleObject(context->StopEvent, 0) == WAIT_OBJECT_0)
        {
            return false;
        }

        // 重置任务表，把所有封包重新排队（Pass 2）。
        const std::vector<RestorePackageTask> packages = EnumerateRestorePackages(context->SourceDirectory);
        ListView_DeleteAllItems(context->TaskList);
        ::EnterCriticalSection(&context->QueueLock);
        context->Tasks.clear();
        context->Queue.clear();
        context->NextTaskId = 1u;
        for (const RestorePackageTask& package : packages)
        {
            TaskRecord task;
            task.Id = context->NextTaskId++;
            task.PackageName = package.packageName;
            task.PackageDirectory = package.packageDirectory;
            task.RootLevel = package.rootLevel;
            task.FileCount = package.fileCount;
            task.Bytes = package.bytes;
            task.State = kStateQueued;
            task.Step = RestoreProgressStep::Queued;
            task.Percent = 0u;
            task.Info = FormatString(L"推理补充还原遍（第一遍已交付 %u 个）",
                                     static_cast<unsigned int>(context->FirstPassDelivered.size()));
            context->Tasks.push_back(task);
            context->Queue.push_back(task.Id);
        }
        ::LeaveCriticalSection(&context->QueueLock);

        for (size_t index = 0; index < context->Tasks.size(); ++index)
        {
            RefreshTaskRow(context, static_cast<int>(index));
        }

        context->PassNumber = 2u;
        context->LastAggregatePercent = -1;
        context->FinishIssued = false;
        // StopEvent 只被挖掘线程的取消回调**读过**，正常路径上没人置位过；这里防御性复位，
        // 保证第二遍的工作线程拿到的是复位状态。
        ::ResetEvent(context->StopEvent);
        if (context->QueueEvent)
        {
            ::SetEvent(context->QueueEvent);
        }
        UpdateSummary(context);

        LogLine(context, false, L"推理补充还原第二遍开始：%u 个封包，只补第一遍未交付的文件。",
                static_cast<unsigned int>(packages.size()));
        return true;
    }

    // ---------- 工作线程 ----------

    bool PopNextTask(UiContext* context, unsigned int& taskId)
    {
        bool hasTask = false;
        ::EnterCriticalSection(&context->QueueLock);
        if (!context->Queue.empty())
        {
            taskId = context->Queue.front();
            context->Queue.erase(context->Queue.begin());
            hasTask = true;
        }
        if (context->Queue.empty())
        {
            ::ResetEvent(context->QueueEvent);
        }
        ::LeaveCriticalSection(&context->QueueLock);
        return hasTask;
    }

    void PublishProgress(UiContext* context, unsigned int taskId, RestoreProgressStep step,
                         unsigned int percent, const std::wstring& entry)
    {
        // 任务记录永远先更新：它是唯一事实来源，界面重画时读的就是它。
        ::EnterCriticalSection(&context->QueueLock);
        const int index = FindTaskIndexLocked(context, taskId);
        if (index >= 0)
        {
            TaskRecord& task = context->Tasks[static_cast<size_t>(index)];
            task.Step = step;
            task.Percent = percent;
            if (!entry.empty())
            {
                task.CurrentEntry = entry;
            }
            if (task.State == kStateQueued)
            {
                task.State = kStateRunning;
            }
        }
        ::LeaveCriticalSection(&context->QueueLock);

        if (index < 0)
        {
            return;
        }

        // 只有"还原中"的高频进度才节流；排队/收尾这类台阶无条件照发。
        // 节流丢掉的只是这条通知，不影响上面已经写好的记录。
        if (step == RestoreProgressStep::Restoring)
        {
            const DWORD now = ::GetTickCount();
            if (static_cast<DWORD>(now - context->LastProgressPostTick) < kProgressThrottleMs)
            {
                return;
            }
            context->LastProgressPostTick = now;
        }

        // 这条消息只影响"实时感"。丢了大不了界面下一帧照任务记录画。
        TaskUpdateMessage* message = static_cast<TaskUpdateMessage*>(
            ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TaskUpdateMessage)));
        if (!message)
        {
            return;
        }
        message->TaskId = taskId;
        message->State = static_cast<unsigned int>(kStateRunning);
        message->Step = static_cast<unsigned int>(step);
        message->Percent = percent;
        if (!entry.empty())
        {
            ::StringCchCopyW(message->Entry, _countof(message->Entry), entry.c_str());
        }
        if (!::PostMessageW(context->Window, kMsgTaskUpdate, 0, reinterpret_cast<LPARAM>(message)))
        {
            ::HeapFree(::GetProcessHeap(), 0, message);
        }
    }

    // 写终态。这是"任务终态由工作线程按返回值决定"的落点：
    // 先改任务记录（一定成功），再发通知消息（可能被丢弃）。
    void PublishTerminalState(UiContext* context, unsigned int taskId, RestoreTaskState state,
                              const RestorePackageResult& result, DWORD elapsedMs)
    {
        ::EnterCriticalSection(&context->QueueLock);
        const int index = FindTaskIndexLocked(context, taskId);
        if (index >= 0)
        {
            TaskRecord& task = context->Tasks[static_cast<size_t>(index)];
            task.State = state;
            task.Step = RestoreProgressStep::Finishing;
            task.Percent = 100u;
            task.Result = result;
            task.Finished = true;
            task.ElapsedMs = elapsedMs;
            task.CurrentEntry = L"（已结束）";
            task.Info = result.info;
            if (result.cancelled)
            {
                task.Info += L"（用户中止）";
            }
            task.Info += FormatString(L"，耗时 %u ms", elapsedMs);
        }
        ::LeaveCriticalSection(&context->QueueLock);

        if (index < 0)
        {
            // 任务记录都没了（不可能，但别让线程带着未定义行为跑），直接返回。
            return;
        }

        TaskUpdateMessage* message = static_cast<TaskUpdateMessage*>(
            ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(TaskUpdateMessage)));
        if (message)
        {
            message->TaskId = taskId;
            message->State = static_cast<unsigned int>(state);
            message->Step = static_cast<unsigned int>(RestoreProgressStep::Finishing);
            message->Percent = 100u;
            ::StringCchCopyW(message->Info, _countof(message->Info),
                             (result.info + (result.cancelled ? std::wstring(L"（用户中止）")
                                                              : std::wstring()))
                                 .c_str());
            if (!::PostMessageW(context->Window, kMsgTaskUpdate, 0, reinterpret_cast<LPARAM>(message)))
            {
                ::HeapFree(::GetProcessHeap(), 0, message);
            }
        }
    }

    DWORD WINAPI WorkerThreadProc(LPVOID parameter)
    {
        UiContext* context = static_cast<UiContext*>(parameter);
        HANDLE waitHandles[2]{ context->StopEvent, context->QueueEvent };

        while (true)
        {
            unsigned int taskId = 0u;
            if (!PopNextTask(context, taskId))
            {
                // 队列空了。等新任务或停止信号。
                // StopEvent 必须排在前面：QueueEvent 是手动重置事件，收尾时会一直保持
                // 置位，只等它就会变成 100% 占着 CPU 的死循环。
                const DWORD waitResult = ::WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
                if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_FAILED)
                {
                    break;
                }
                continue;
            }

            ::InterlockedIncrement(&context->ActiveWorkers);

            TaskRecord snapshot;
            {
                ::EnterCriticalSection(&context->QueueLock);
                const int index = FindTaskIndexLocked(context, taskId);
                if (index >= 0)
                {
                    snapshot = context->Tasks[static_cast<size_t>(index)];
                }
                ::LeaveCriticalSection(&context->QueueLock);
            }

            if (snapshot.Id == 0u)
            {
                ::InterlockedDecrement(&context->ActiveWorkers);
                continue;
            }

            PublishProgress(context, taskId, RestoreProgressStep::Restoring, 0u, L"准备中");

            RestorePackageTask packageTask;
            packageTask.packageName = snapshot.PackageName;
            packageTask.packageDirectory = snapshot.PackageDirectory;
            packageTask.rootLevel = snapshot.RootLevel;
            packageTask.fileCount = snapshot.FileCount;
            packageTask.bytes = snapshot.Bytes;

            // 选项在每个工作线程里各拷一份：勾选框在运行期间是禁用的，
            // 读一次就固定下来，工作线程之间不会看到不同的值。
            RestoreOptions taskOptions;
            taskOptions.sourceDirectory = context->SourceDirectory;
            taskOptions.outputDirectory = context->OutputDirectory;
            taskOptions.logDirectory = context->LogDirectory;
            taskOptions.keepUnrestored =
                ::SendMessageW(context->KeepCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            // 两遍式的第二遍：第一遍已交付的目标直接跳过（只补漏，不重拷不造副本）。
            // 集合在启动第二遍前由界面线程填好，工作线程全程只读。
            taskOptions.alreadyDelivered = context->FirstPassDelivered;
            taskOptions.cancelRequested = [context]()
            { return ::WaitForSingleObject(context->StopEvent, 0) == WAIT_OBJECT_0; };
            taskOptions.progress = [context, taskId](unsigned int percent, const std::wstring& entry)
            { PublishProgress(context, taskId, RestoreProgressStep::Restoring, percent, entry); };

            const DWORD started = ::GetTickCount();
            const RestorePackageResult result =
                RestoreSinglePackage(packageTask, context->Tables, taskOptions);
            const DWORD elapsed = static_cast<DWORD>(::GetTickCount() - started);

            // 终态：先写记录，再发消息。消息丢了也不影响这一行的最终状态。
            PublishTerminalState(context, taskId,
                                 result.succeeded ? kStateCompleted : kStateFailed, result, elapsed);

            ::InterlockedDecrement(&context->ActiveWorkers);
            ::PostMessageW(context->Window, kMsgWatchdog, 0, 0);
        }

        return 0u;
    }

    // ---------- 启动一次还原 ----------

    bool ValidateOutputDirectory(UiContext* context, const std::wstring& output)
    {
        if (output.empty())
        {
            Host::ShowInfo(context->Window, L"请先填写还原输出目录。");
            return false;
        }
        if (_wcsicmp(output.c_str(), context->SourceDirectory.c_str()) == 0 ||
            StartsWithNoCase(context->SourceDirectory, output + L"\\"))
        {
            // 输出目录盖住了解包目录就是就地改写，违反"源目录只读"。
            Host::ShowError(context->Window,
                            L"输出目录不能等于、也不能包含解包目录：\r\n" + context->SourceDirectory +
                                L"\r\n\r\n请换一个目录，源解包结果必须保持原样。");
            return false;
        }
        return true;
    }

    // 挖掘成功后把结论写进窗口日志。要不要打日志由调用方按节流决定，
    // 这里只负责把摘要写出去，保证"扫描数/候选数/试算数/命中数/是否触顶"一条不少。
    void LogSupplementOutcome(UiContext* context, const SupplementOutcome& outcome)
    {
        if (!outcome.attempted)
        {
            return;
        }
        if (!outcome.saltAvailable)
        {
            // 盐不可用**绝不能静默跳过**：这是用户勾了框却什么都没发生时的唯一解释。
            LogLine(context, false, L"推理补充还原未执行：%s", outcome.message.c_str());
            LogLine(context, false, L"请先执行一次步骤2（启动游戏收集 Hash Seed），然后重试推理补充还原。");
            return;
        }
        LogLine(context, false, L"推理补充还原：%s", outcome.message.c_str());
        LogLine(context, false, L"  扫描文件 %s / 候选名 %s",
                FormatCount(outcome.scannedFiles).c_str(), FormatCount(outcome.candidateCount).c_str());
        LogLine(context, false, L"  试算哈希 %s / 命中 %s（新增映射 %s）",
                FormatCount(outcome.triedHashes).c_str(), FormatCount(outcome.hitCount).c_str(),
                FormatCount(outcome.newEntries).c_str());
        LogLine(context, false, L"  目标哈希 %s 个 / 扫描根 %u 个 / 候选触顶：%s",
                FormatCount(outcome.targetHashes).c_str(), outcome.scanRoots,
                outcome.limitHit ? L"是（结果不完整）" : L"否");
    }

    // 挖掘线程 → 界面线程的进度通知。
    // **本函数跑在挖掘线程里，绝不能碰窗口 API**（LogLine / SetWindowTextW / 列表都是
    // 界面线程的东西，而且窗口随时可能被销毁）。只把三个计数搬到堆上投出去，
    // 怎么写日志、怎么节流，全由界面线程决定。
    void PublishSupplementProgress(UiContext* context, const SupplementProgress& progress)
    {
        SupplementProgressMessage* message = static_cast<SupplementProgressMessage*>(
            ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(SupplementProgressMessage)));
        if (!message)
        {
            return;
        }
        message->ScannedFiles = progress.scannedFiles;
        message->Candidates = progress.candidates;
        message->Hits = progress.hits;
        if (!::PostMessageW(context->Window, kMsgSupplementProgress, 0, reinterpret_cast<LPARAM>(message)))
        {
            // 窗口已经销毁：PostMessage 失败时载荷还在我们手里，必须自己释放。
            ::HeapFree(::GetProcessHeap(), 0, message);
        }
    }

    // 挖掘线程。只做一件事：跑内核，把进度和结论投回界面线程。
    //
    // 取消判据是 StopEvent（窗口销毁时置位）或 context->Closed —— **不再泵消息**：
    // 泵消息会执行窗口过程，而窗口过程会读这份正在被本线程改写的映射表。
    DWORD WINAPI SupplementMiningThreadProc(LPVOID parameter)
    {
        UiContext* context = static_cast<UiContext*>(parameter);

        SupplementOptions options;
        options.sourceDirectory = context->SourceDirectory;
        options.outputDirectory = context->OutputDirectory;
        options.logDirectory = context->LogDirectory;
        options.minedListPath = context->MinedListPath;
        // Closed 只在 WM_DESTROY 里、join 本线程**之后**才写，所以这里读它是安全的。
        options.cancelRequested = [context]()
        {
            return context->Closed ||
                   ::WaitForSingleObject(context->StopEvent, 0) == WAIT_OBJECT_0;
        };

        SupplementOutcome outcome =
            RunSupplementRestore(context->Tables, options,
                                 [context](const SupplementProgress& progress)
                                 { PublishSupplementProgress(context, progress); });

        // 结论交给界面线程：它负责写日志、更新映射计数、启动第二遍。
        // 载荷用 new 而不是 HeapAlloc —— SupplementOutcome 里有 std::wstring，
        // HeapAlloc 只给裸内存、不跑构造函数，字符串会在第一次赋值时崩掉。
        SupplementOutcome* payload = new (std::nothrow) SupplementOutcome(outcome);
        if (!payload)
        {
            // 连载荷都申请不出来：仍然要投一条空结论，否则界面侧的闸门永远开不了。
            ::PostMessageW(context->Window, kMsgSupplementDone, 0, 0);
            return 0u;
        }
        if (!::PostMessageW(context->Window, kMsgSupplementDone, 0, reinterpret_cast<LPARAM>(payload)))
        {
            // 窗口已经销毁：界面线程不会再来接管这份载荷，必须自己释放。
            delete payload;
        }
        return 0u;
    }

    // 界面线程处理挖掘进度：写日志 + 刷状态栏，节流也搬到这一侧（挖掘线程只负责投数字，
    // 1 秒一条、且候选数没变就不重复刷 —— 对撞阶段扫描数不再增长，只有命中数在涨）。
    void OnSupplementProgress(UiContext* context, SupplementProgressMessage* message)
    {
        if (!message)
        {
            return;
        }
        const unsigned int scannedFiles = message->ScannedFiles;
        const unsigned int candidates = message->Candidates;
        const unsigned int hits = message->Hits;
        ::HeapFree(::GetProcessHeap(), 0, message);

        const DWORD now = ::GetTickCount();
        if (context->LastSupplementLogTick != 0u &&
            static_cast<DWORD>(now - context->LastSupplementLogTick) < kSupplementLogIntervalMs &&
            candidates == context->LastSupplementCandidates)
        {
            return;
        }
        context->LastSupplementLogTick = now;
        context->LastSupplementCandidates = candidates;

        LogLine(context, true, L"挖掘中：扫描 %s 个文件 / 候选 %s / 命中 %s",
                FormatCount(scannedFiles).c_str(), FormatCount(candidates).c_str(),
                FormatCount(hits).c_str());
        ::SetWindowTextW(context->StatusText,
                         FormatString(L"推理补充还原挖掘中：扫描 %s 个文件 / 候选 %s / 命中 %s",
                                      FormatCount(scannedFiles).c_str(), FormatCount(candidates).c_str(),
                                      FormatCount(hits).c_str())
                             .c_str());
    }

    // 界面线程处理挖掘结论：写结论日志 → 更新映射计数 → 启动第二遍。
    // 这条消息在挖掘线程写完整张映射表之后才投出（FIFO 保证它排在所有进度消息后面），
    // 所以从这里往后，映射表又可以当只读快照用了。
    void OnSupplementDone(UiContext* context, SupplementOutcome* outcome)
    {
        // 界面线程接管载荷：先拷一份到栈上立刻 delete —— 后面每个分支都别再碰它。
        // lParam 为空只有一种可能：挖掘线程连结论载荷都申请不出来（内存耗尽）。
        // 那种情况下也必须把闸门放开、把第二遍跑起来，否则这一轮永远停在"挖掘中"。
        SupplementOutcome mined;
        if (outcome)
        {
            mined = *outcome;
            delete outcome;
        }
        else
        {
            mined.attempted = true;
            mined.message = L"内存不足，未能取回挖掘结论。";
        }

        // 挖掘线程已经投完结论，收掉它的句柄（它此刻只剩一句 return，join 立即返回）。
        if (context->MiningThread)
        {
            ::WaitForSingleObject(context->MiningThread, INFINITE);
            ::CloseHandle(context->MiningThread);
            context->MiningThread = nullptr;
        }
        context->InSupplementMining = false;
        context->Supplement = mined;

        LogSupplementOutcome(context, mined);

        context->DirectoryMapEntries = static_cast<unsigned int>(context->Tables.directories.size());
        context->FileNameMapEntries = static_cast<unsigned int>(context->Tables.files.size());
        if (mined.saltAvailable)
        {
            LogLine(context, false, L"推理补充还原后映射：目录 %s 条 / 文件 %s 条（本次新增 %s 条）",
                    FormatCount(context->DirectoryMapEntries).c_str(),
                    FormatCount(context->FileNameMapEntries).c_str(),
                    FormatCount(mined.newEntries).c_str());
        }

        if (!StartPass2(context))
        {
            // 窗口在挖完之后、第二遍之前被关掉了：StopEvent 已置位，直接走收尾。
            context->FinishIssued = true;
            LogLine(context, false, L"所有任务已到终态，开始收尾。");
            FinishRun(context);
        }
    }

    void OnStart(UiContext* context)
    {
        if (context->Running)
        {
            Host::ShowInfo(context->Window, L"还原正在进行中，请等它跑完或直接关闭窗口中止。");
            return;
        }
        if (context->InSupplementMining)
        {
            // 兜底：挖掘线程正在改共享映射表，这时再开一次会踩到同一份表
            //（正常路径下 Running 也是 true，上面那条分支已经拦住并给过提示了）。
            return;
        }

        if (!DirectoryExistsAt(context->SourceDirectory))
        {
            Host::ShowInfo(context->Window,
                           L"还没有解包结果，先执行步骤1：\r\n" + context->SourceDirectory);
            return;
        }
        if (!DirectoryExistsAt(context->HashDirectory))
        {
            Host::ShowInfo(context->Window,
                           L"还没有 Hash 映射目录，先执行步骤2 并跑一段游戏：\r\n" + context->HashDirectory);
            return;
        }

        // 输出目录取界面上的值；线程数做一次夹取，并且把夹取结果写回控件 ——
        // 用户填 999 的时候得让他看见实际用了多少线程。
        std::wstring output = TrimTrailingSeparators(GetControlText(context->OutputEdit));
        if (!ValidateOutputDirectory(context, output))
        {
            return;
        }

        unsigned int workers = kDefaultWorkers;
        {
            const std::wstring text = GetControlText(context->WorkerEdit);
            wchar_t* end = nullptr;
            const unsigned long parsed = wcstoul(text.c_str(), &end, 10);
            if (end != text.c_str() && parsed > 0ul)
            {
                workers = static_cast<unsigned int>(parsed);
            }
        }
        if (workers < kMinWorkers)
        {
            workers = kMinWorkers;
        }
        if (workers > kMaxWorkers)
        {
            workers = kMaxWorkers;
        }
        ::SetWindowTextW(context->WorkerEdit, std::to_wstring(workers).c_str());

        const bool keepUnrestored =
            ::SendMessageW(context->KeepCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        context->KeepUnrestored = keepUnrestored;

        context->OutputDirectory = output;
        ::SetWindowTextW(context->OutputEdit, output.c_str());

        // 挖掘结果落到映射目录（= <工作区>\StringHashDumper_Output）下：那儿所有 *.lst
        // 是既有契约，下次步骤3 会直接读到，不用为了同一份结果再挖一遍。
        // 游戏目录为空（工作区推导不出来）时就不落盘，只用于本次还原。
        std::wstring workspace = context->GameDirectory;
        if (workspace.empty())
        {
            const size_t separator = context->SourceDirectory.find_last_of(L"\\/");
            if (separator != std::wstring::npos)
            {
                workspace = context->SourceDirectory.substr(0u, separator);
            }
        }
        context->MinedListPath =
            workspace.empty() ? std::wstring() : JoinPath(context->HashDirectory, L"MinedNames.lst");

        // 映射表只载入一次，之后所有工作线程共享同一份只读快照。
        LogLine(context, false, L"载入 Hash 映射：%s", context->HashDirectory.c_str());
        context->Tables = LoadHashMapTables(context->HashDirectory);
        context->DirectoryMapEntries = static_cast<unsigned int>(context->Tables.directories.size());
        context->FileNameMapEntries = static_cast<unsigned int>(context->Tables.files.size());
        if (context->Tables.directories.empty() && context->Tables.files.empty())
        {
            Host::ShowInfo(context->Window,
                           L"映射目录里没有可用的 Hash 映射，先执行步骤2 并跑一段游戏：\r\n" +
                               context->HashDirectory);
            return;
        }
        LogLine(context, false, L"映射载入完成：目录 %u 条 / 文件 %u 条，来源 %u 个文件",
                context->DirectoryMapEntries, context->FileNameMapEntries,
                static_cast<unsigned int>(context->Tables.sources.size()));

        // StopEvent 必须在这里复位：补充还原与还原回调都会读它，而**上一次 run 的收尾**
        // 把它置位过（见 FinishRun）。复位点若留在建任务表之后，挖掘的取消回调一开始
        // 就会读到已置位的事件，整个挖掘被"用户中止"秒退 —— 第二次在同一窗口点开始时
        // 必现（第一次跑完 → StopEvent 置位 → 第二次点开始 → 挖掘回调读到置位 → 中止）。
        ::ResetEvent(context->StopEvent);

        // 两遍式（勾了补充还原）：**Pass 1 不挖**。先用步骤2 收集的映射做一遍普通还原，
        // 挖掘挪到 Pass 1 全部结束后（StartSupplementMining），这样"普通还原基线"先行、
        // 挖掘只补漏。挖完再建第二遍任务表。
        context->PassNumber = 1u;
        context->FirstPassResults.clear();
        context->FirstPassDelivered.clear();
        // Pass2Started 必须复位：它只在 CheckRunCompletion 里被置位过，不复位的话
        // 同一个窗口跑第二轮时"第一遍已到终态 + Pass2Started 为 true"会直接收尾，
        // 挖掘和第二遍永远不跑（第二轮静默退化成单遍）。
        context->Pass2Started = false;
        // 上一轮的挖掘结论不能留给这一轮：挖不成（窗口被关/盐不可用）时报告里那一节
        // 必须是"本次没挖"，而不是上一轮的数字。
        context->Supplement = SupplementOutcome();
        context->SupplementPending =
            ::SendMessageW(context->InferCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (context->SupplementPending)
        {
            LogLine(context, false,
                    L"推理补充还原：先做一遍普通还原（只用步骤2 收集的映射），"
                    L"完成后再挖候选名补漏。");
        }

        // 枚举封包（Extractor_Output 的顶层子目录），一个封包一个任务。
        const std::vector<RestorePackageTask> packages = EnumerateRestorePackages(context->SourceDirectory);
        if (packages.empty())
        {
            Host::ShowInfo(context->Window,
                           L"解包目录里没有可还原的封包（顶层子目录）：\r\n" + context->SourceDirectory);
            return;
        }

        // 上一轮的行全清掉：同一份任务表既能显示"排队中"，也能显示终态，
        // 留着旧行只会让"总任务"这个数字对不上。
        ListView_DeleteAllItems(context->TaskList);
        ::EnterCriticalSection(&context->QueueLock);
        context->Tasks.clear();
        context->Queue.clear();
        context->NextTaskId = 1u;
        for (const RestorePackageTask& package : packages)
        {
            TaskRecord task;
            task.Id = context->NextTaskId++;
            task.PackageName = package.packageName;
            task.PackageDirectory = package.packageDirectory;
            task.RootLevel = package.rootLevel;
            task.FileCount = package.fileCount;
            task.Bytes = package.bytes;
            task.State = kStateQueued;
            task.Step = RestoreProgressStep::Queued;
            task.Percent = 0u;
            task.Info = FormatString(L"%s 个文件 / %s", FormatCount(package.fileCount).c_str(),
                                     FormatSize(package.bytes).c_str());
            context->Tasks.push_back(task);
            context->Queue.push_back(task.Id);
        }
        ::LeaveCriticalSection(&context->QueueLock);

        for (size_t index = 0; index < context->Tasks.size(); ++index)
        {
            RefreshTaskRow(context, static_cast<int>(index));
        }
        ListView_SetColumnWidth(context->TaskList, 4, LVSCW_AUTOSIZE_USEHEADER);

        context->LastAggregatePercent = -1;
        context->Running = true;
        context->FinishIssued = false;
        context->ActiveWorkers = 0u;
        // StopEvent 已在挖掘之前复位过（见上面）；这里再复位一次是防御性的 ——
        // 不勾补充还原时也保证工作线程拿到的是复位状态。
        ::ResetEvent(context->StopEvent);
        if (context->QueueEvent)
        {
            ::SetEvent(context->QueueEvent);
        }

        SetInputsEnabled(context, false);
        UpdateSummary(context);
        ::SetTimer(context->Window, kWatchdogTimerId, kWatchdogIntervalMs, nullptr);

        LogLine(context, false, L"已准备 %u 个封包任务，工作线程 %u，%s",
                static_cast<unsigned int>(packages.size()), workers,
                keepUnrestored ? L"保留未还原文件名的文件（按文件头补后缀）" : L"只输出已还原的文件");
        if (keepUnrestored)
        {
            LogLine(context, false,
                    L"未还原文件会在输出目录里落成「<64位Hash><按文件头判定的后缀>」，"
                    L"识别不出类型时落 .bin；源目录不动。");
        }
        for (const RestorePackageTask& package : packages)
        {
            LogLine(context, true, L"封包 %s | 排队中 | 等待开始（%s 个文件）", package.packageName.c_str(),
                    FormatCount(package.fileCount).c_str());
        }
        LogLine(context, false, L"开始还原……");

        context->Workers.clear();
        for (unsigned int index = 0u; index < workers; ++index)
        {
            HANDLE worker = ::CreateThread(nullptr, 0u, WorkerThreadProc, context, 0u, nullptr);
            if (worker)
            {
                context->Workers.push_back(worker);
            }
        }
        if (context->Workers.empty())
        {
            context->Running = false;
            SetInputsEnabled(context, true);
            ::KillTimer(context->Window, kWatchdogTimerId);
            Host::ShowError(context->Window, L"创建工作线程失败，无法开始还原。");
            return;
        }
        LogLine(context, false, L"已启动 %u 个工作线程。", static_cast<unsigned int>(context->Workers.size()));
        // 通报"进度在动态推进"，好让主界面的「检查状态」知道该禁止清理目录。
        // 关键状态（完成/失败）由 FinishRun 再报一次。
        NotifyOwner(context, kStateRunning, 0u);
        UpdateSummary(context);
    }

    // ---------- 布局 ----------

    void InitializeTaskListColumns(HWND listView)
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

        column.pszText = const_cast<LPWSTR>(L"封包");
        column.cx = 130;
        ListView_InsertColumn(listView, 0, &column);

        column.pszText = const_cast<LPWSTR>(L"状态");
        column.cx = 70;
        column.iSubItem = 1;
        ListView_InsertColumn(listView, 1, &column);

        column.pszText = const_cast<LPWSTR>(L"进度");
        column.cx = 110;
        column.iSubItem = 2;
        ListView_InsertColumn(listView, 2, &column);

        column.pszText = const_cast<LPWSTR>(L"当前条目");
        column.cx = 230;
        column.iSubItem = 3;
        ListView_InsertColumn(listView, 3, &column);

        column.pszText = const_cast<LPWSTR>(L"详细信息");
        column.cx = 200;
        column.iSubItem = 4;
        ListView_InsertColumn(listView, 4, &column);

        ListView_SetExtendedListViewStyle(listView, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES |
                                                        LVS_EX_DOUBLEBUFFER);
    }

    void MoveControl(HWND control, int x, int y, int width, int height)
    {
        if (control)
        {
            ::MoveWindow(control, x, y, width, height, TRUE);
        }
    }

    void LayoutWindow(UiContext* context)
    {
        RECT client{};
        ::GetClientRect(context->Window, &client);
        const int width = client.right - client.left;
        const int height = client.bottom - client.top;
        const int contentRight = width - kMargin;

        int y = kMargin;
        MoveControl(context->OutputLabel, kMargin, y + 3, kOutputLabelWidth, kRowHeight - 4);
        const int workerBlock = kWorkerLabelWidth + 4 + kWorkerEditWidth;
        const int outputWidth = std::max(140, contentRight - kMargin - kOutputLabelWidth - 20 - workerBlock);
        MoveControl(context->OutputEdit, kMargin + kOutputLabelWidth, y, outputWidth, kRowHeight);
        MoveControl(context->WorkerLabel, kMargin + kOutputLabelWidth + outputWidth + 20,
                    y + 3, kWorkerLabelWidth, kRowHeight - 4);
        MoveControl(context->WorkerEdit, kMargin + kOutputLabelWidth + outputWidth + 20 + kWorkerLabelWidth + 4,
                    y, kWorkerEditWidth, kRowHeight);
        y += kRowHeight + kRowGap;

        MoveControl(context->StatusText, kMargin, y, width - kMargin * 2, 20);
        y += 22;

        MoveControl(context->ProgressBar, kMargin, y, width - kMargin * 2, kProgressHeight);
        y += kProgressHeight + kRowGap;

        // 底部固定：复选框两行 + 按钮一行。
        const int bottomY = height - kMargin - kButtonHeight;
        const int checkTop = bottomY - kCheckHeight * 2 - 6;
        const int listBottom = checkTop - kRowGap;

        MoveControl(context->LogLabel, kMargin, y, 200, kSplitTitleHeight - 4);
        y += kSplitTitleHeight;

        const int remaining = listBottom - y;
        const int logHeight = std::max(kMinLogHeight, (remaining - kSplitTitleHeight) * 55 / 100);

        MoveControl(context->LogView, kMargin, y, width - kMargin * 2, logHeight);
        y += logHeight + kRowGap;

        MoveControl(context->ListLabel, kMargin, y, 200, kSplitTitleHeight - 4);
        y += kSplitTitleHeight;

        MoveControl(context->TaskList, kMargin, y, width - kMargin * 2,
                    std::max(kMinListHeight, listBottom - y));

        // 复选框的文字不短（第二个要如实描述机制，更长），给足宽度：
        // 按 contentRight 的 85% 排，最小 460 —— 折行了就成两行文字，看着像排版坏了。
        const int checkWidth = std::max(460, (contentRight - kMargin) * 85 / 100);
        MoveControl(context->KeepCheck, kMargin, checkTop, checkWidth, kCheckHeight);
        MoveControl(context->InferCheck, kMargin, checkTop + kCheckHeight, checkWidth, kCheckHeight);

        int buttonX = contentRight - kButtonWidth;
        MoveControl(context->BackButton, buttonX, bottomY, kButtonWidth, kButtonHeight);
        buttonX -= kButtonWidth + kRowGap;
        MoveControl(context->OpenDirButton, buttonX, bottomY, kButtonWidth, kButtonHeight);
        buttonX -= kButtonWidth + kRowGap;
        MoveControl(context->StartButton, buttonX, bottomY, kButtonWidth, kButtonHeight);
    }

    // ---------- 控件创建 ----------

    HWND CreateChild(HWND parent, UiContext* context, const wchar_t* className, const wchar_t* text,
                     DWORD style, DWORD exStyle, int id)
    {
        HWND control = ::CreateWindowExW(exStyle, className, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
                                         parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                         context->Instance, nullptr);
        if (control && context->FontUi)
        {
            ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(context->FontUi), TRUE);
        }
        return control;
    }

    void CreateControls(UiContext* context)
    {
        HWND window = context->Window;

        context->OutputLabel =
            CreateChild(window, context, L"STATIC", L"还原输出目录", SS_LEFT, 0, IDC_RS_OUTPUT_LABEL);
        context->OutputEdit = CreateChild(window, context, L"EDIT", L"",
                                          WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE,
                                          IDC_RS_OUTPUT_EDIT);
        context->WorkerLabel =
            CreateChild(window, context, L"STATIC", L"工作线程", SS_LEFT, 0, IDC_RS_WORKER_LABEL);
        context->WorkerEdit = CreateChild(window, context, L"EDIT", L"4",
                                          WS_BORDER | ES_NUMBER | ES_CENTER | WS_TABSTOP,
                                          WS_EX_CLIENTEDGE, IDC_RS_WORKER_EDIT);

        context->StatusText = CreateChild(window, context, L"STATIC", L"总任务 0 | 运行中 0 | 排队 0 | 完成 0 | 失败 0",
                                          SS_LEFT, 0, IDC_RS_STATUS);

        context->ProgressBar = CreateChild(window, context, PROGRESS_CLASSW, L"",
                                           PBS_SMOOTH, 0, IDC_RS_PROGRESS);
        ::SendMessageW(context->ProgressBar, PBM_SETRANGE32, 0, 100);

        // 两个分组标题只是"分开两块区域"的视觉分隔，不是交互控件。
        context->LogLabel = CreateChild(window, context, L"STATIC", L"详细日志", SS_LEFT, 0, IDC_RS_LOG_LABEL);
        context->LogView = CreateChild(window, context, L"EDIT", L"",
                                       WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                       WS_EX_CLIENTEDGE, IDC_RS_LOG);
        if (context->LogView)
        {
            ::SendMessageW(context->LogView, WM_SETFONT, reinterpret_cast<WPARAM>(context->FontLog), TRUE);
        }

        context->ListLabel = CreateChild(window, context, L"STATIC", L"任务概览", SS_LEFT, 0,
                                         IDC_RS_LIST_LABEL);
        context->TaskList = CreateChild(window, context, WC_LISTVIEWW, L"",
                                        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP |
                                            WS_BORDER,
                                        0, IDC_RS_LIST);
        if (context->TaskList)
        {
            InitializeTaskListColumns(context->TaskList);
        }

        context->KeepCheck = CreateChild(window, context, L"BUTTON",
                                         L"保留未还原文件名的文件（文件后缀已确定）",
                                         BS_AUTOCHECKBOX | WS_TABSTOP, 0, IDC_RS_KEEP_CHECK);
        // 复选框二的名字要如实描述机制：它不是"推理"出名字，而是从游戏资源里**挖**
        // 候选名、再用哈希**验证**后写入 —— 用户看到"推理"会以为是猜的。
        context->InferCheck = CreateChild(window, context, L"BUTTON",
                                          L"推理补充还原（从游戏资源中挖掘候选名，哈希验证后写入）",
                                          BS_AUTOCHECKBOX | WS_TABSTOP, 0, IDC_RS_INFER_CHECK);

        context->StartButton = CreateChild(window, context, L"BUTTON", L"开始", BS_PUSHBUTTON | WS_TABSTOP, 0,
                                           IDC_RS_START);
        context->OpenDirButton = CreateChild(window, context, L"BUTTON", L"打开还原目录",
                                             BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_RS_OPEN_DIR);
        context->BackButton = CreateChild(window, context, L"BUTTON", L"返回主程序",
                                          BS_PUSHBUTTON | WS_TABSTOP, 0, IDC_RS_BACK);
    }

    // ---------- 窗口过程 ----------

    void OnTaskUpdate(UiContext* context, TaskUpdateMessage* message)
    {
        if (!message)
        {
            return;
        }

        int index = -1;
        {
            ::EnterCriticalSection(&context->QueueLock);
            index = FindTaskIndexLocked(context, message->TaskId);
            if (index >= 0)
            {
                TaskRecord& task = context->Tasks[static_cast<size_t>(index)];
                if (message->State == static_cast<unsigned int>(kStateCompleted) ||
                    message->State == static_cast<unsigned int>(kStateFailed))
                {
                    // 唯一可信的终态在任务记录里（工作线程按返回值写的），
                    // 这条消息只是"叫你刷新一下"。
                    if (!task.Finished)
                    {
                        task.State = static_cast<RestoreTaskState>(message->State);
                        task.Finished = true;
                        task.Step = RestoreProgressStep::Finishing;
                        task.Percent = 100u;
                    }
                }
                else if (!task.Finished)
                {
                    task.State = kStateRunning;
                    task.Step = static_cast<RestoreProgressStep>(message->Step);
                    task.Percent = message->Percent;
                    if (message->Entry[0] != L'\0')
                    {
                        task.CurrentEntry = message->Entry;
                    }
                }
            }
            ::LeaveCriticalSection(&context->QueueLock);
        }

        if (index >= 0)
        {
            RefreshTaskRow(context, index);

            // 日志文案要的是封包名，从任务记录里拷一份出来用 —— 别在锁外直接读
            // Tasks[]，那是工作线程还在写的容器。
            std::wstring packageName;
            {
                ::EnterCriticalSection(&context->QueueLock);
                packageName = context->Tasks[static_cast<size_t>(index)].PackageName;
                ::LeaveCriticalSection(&context->QueueLock);
            }

            if (message->State == static_cast<unsigned int>(kStateCompleted))
            {
                LogLine(context, true, L"封包 %s | 已完成 | %s", packageName.c_str(), message->Info);
            }
            else if (message->State == static_cast<unsigned int>(kStateFailed))
            {
                LogLine(context, true, L"封包 %s | 失败 | %s", packageName.c_str(), message->Info);
            }
        }

        ::HeapFree(::GetProcessHeap(), 0, message);
        UpdateSummary(context);
    }

    // 窗口销毁时兜底：把还没派发的私有消息载荷捡回来释放。
    // WM_DESTROY 之后这些消息不会再被派发（处理函数也就绝不会再去释放它们），
    // 而"挖掘中关窗口"是常规操作，每次都漏一份挖掘结论就太说不过去了。
    // 这里只动本窗口的私有消息范围（kMsgTaskUpdate..kMsgSupplementDone），
    // 别的消息一律不碰 —— 尤其不能把 WM_QUIT / WM_PAINT 吃掉。
    void DrainUndispatchedPayloads(HWND window)
    {
        MSG message{};
        while (::PeekMessageW(&message, window, kMsgTaskUpdate, kMsgSupplementDone, PM_REMOVE))
        {
            switch (message.message)
            {
                case kMsgSupplementDone:
                    // 挖掘结论是 new 出来的（含 std::wstring），只能 delete。
                    delete reinterpret_cast<SupplementOutcome*>(message.lParam);
                    break;
                case kMsgSupplementProgress:
                case kMsgTaskUpdate:
                    // 这两条是 HeapAlloc 出来的定长 POD。
                    ::HeapFree(::GetProcessHeap(), 0, reinterpret_cast<void*>(message.lParam));
                    break;
                default:
                    // kMsgRunFinished / kMsgWatchdog 本来就没有载荷。
                    break;
            }
        }
    }

    void OnOpenOutputDirectory(UiContext* context)
    {
        std::wstring output = TrimTrailingSeparators(GetControlText(context->OutputEdit));
        if (output.empty())
        {
            output = context->OutputDirectory;
        }
        if (!DirectoryExistsAt(output))
        {
            Host::ShowInfo(context->Window, L"还原目录还不存在，先点「开始」跑一次还原。\r\n" + output);
            return;
        }
        Host::OpenInExplorer(context->Window, output);
    }

    void OnBackToMain(UiContext* context)
    {
        if (context->Running)
        {
            if (!Host::Confirm(context->Window,
                               L"还原还在进行中。关闭本窗口会中止所有任务"
                               L"（已经复制出去的文件保留，源目录不受影响）。\r\n\r\n确定关闭？",
                               L"中止还原"))
            {
                return;
            }
        }
        ::DestroyWindow(context->Window);
    }

    LRESULT CALLBACK RestoreWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
    {
        UiContext* context = reinterpret_cast<UiContext*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (message)
        {
            case WM_CREATE:
            {
                context = reinterpret_cast<UiContext*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
                context->Window = hwnd;
                ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
                CreateControls(context);
                ::SetWindowTextW(context->OutputEdit, context->OutputDirectory.c_str());
                ::SetWindowTextW(context->WorkerEdit, std::to_wstring(kDefaultWorkers).c_str());
                LayoutWindow(context);
                UpdateSummary(context);

                LogLine(context, false, L"已准备资源名还原任务。");
                LogLine(context, false, L"请根据需要勾选选项，然后点击「开始」按钮。");
                LogLine(context, false,
                        L"默认只输出名字全是真名的文件；未解析的 Hash 名不进输出目录。");
                LogLine(context, false,
                        L"勾「保留未还原文件名的文件」会把它们也复制出去，并按文件头补后缀。");
                LogLine(context, false,
                        L"勾「推理补充还原」会在普通还原跑完后挖候选名、哈希验证后并入映射表，"
                        L"再补一遍未还原的文件（需要先跑过步骤2）。");
                return 0;
            }

            case WM_SIZE:
            {
                if (context)
                {
                    LayoutWindow(context);
                }
                return 0;
            }

            case WM_GETMINMAXINFO:
            {
                MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
                info->ptMinTrackSize.x = 720;
                info->ptMinTrackSize.y = 620;
                return 0;
            }

            case WM_CTLCOLORSTATIC:
            {
                if (context && reinterpret_cast<HWND>(lParam) == context->LogView)
                {
                    ::SetBkColor(reinterpret_cast<HDC>(wParam), kColorLogBack);
                    return reinterpret_cast<LRESULT>(context->LogBrush);
                }
                break;
            }

            case kMsgTaskUpdate:
            {
                OnTaskUpdate(context, reinterpret_cast<TaskUpdateMessage*>(lParam));
                return 0;
            }

            case kMsgSupplementProgress:
            {
                if (context)
                {
                    OnSupplementProgress(context, reinterpret_cast<SupplementProgressMessage*>(lParam));
                }
                return 0;
            }

            case kMsgSupplementDone:
            {
                if (context)
                {
                    OnSupplementDone(context, reinterpret_cast<SupplementOutcome*>(lParam));
                }
                return 0;
            }

            case kMsgWatchdog:
            {
                CheckRunCompletion(context);
                if (context->FinishIssued)
                {
                    FinishRun(context);
                }
                return 0;
            }

            case WM_TIMER:
            {
                if (wParam == kWatchdogTimerId)
                {
                    CheckRunCompletion(context);
                    if (context->FinishIssued)
                    {
                        FinishRun(context);
                    }
                }
                return 0;
            }

            case WM_COMMAND:
            {
                if (!context)
                {
                    break;
                }
                switch (LOWORD(wParam))
                {
                    case IDC_RS_START:
                        OnStart(context);
                        return 0;
                    case IDC_RS_OPEN_DIR:
                        OnOpenOutputDirectory(context);
                        return 0;
                    case IDC_RS_BACK:
                        OnBackToMain(context);
                        return 0;
                    case IDC_RS_INFER_CHECK:
                        if (HIWORD(wParam) == BN_CLICKED &&
                            ::SendMessageW(context->InferCheck, BM_GETCHECK, 0, 0) == BST_CHECKED)
                        {
                            LogLine(context, false,
                                    L"推理补充还原：开始时会先做一遍普通还原，跑完后挖掘候选名、"
                                    L"用哈希验证后写入映射表，再补一遍未还原的文件。");
                            LogLine(context, false,
                                    L"  它是「写不错、只会补不上」的路子：每条名字都要过一次哈希相等，"
                                    L"验证不通过的一律不写。");
                            LogLine(context, false,
                                    L"  前提是已经跑过一次步骤2（需要 <工具目录>\\Log\\Universal.log 里的 Hash Seed）。");
                        }
                        return 0;
                    default:
                        break;
                }
                break;
            }

            case WM_CLOSE:
            {
                OnBackToMain(context);
                return 0;
            }

            case WM_DESTROY:
            {
                if (context)
                {
                    // 先让工作线程停干净再放掉 context —— 它们还在往 context 里写。
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

                    // 挖掘线程同样在用 context（共享映射表 + 目录字符串），必须等它退出来。
                    // StopEvent 上面已经置位，挖掘的取消回调一读到就会中止（最坏情况是
                    // 走到当前这个文件/候选的边界，量级是毫秒）。
                    if (context->MiningThread)
                    {
                        ::WaitForSingleObject(context->MiningThread, INFINITE);
                        ::CloseHandle(context->MiningThread);
                        context->MiningThread = nullptr;
                        context->InSupplementMining = false;
                    }

                    // 挖掘线程可能刚投过进度/结论消息，而窗口销毁后这些消息不会再被派发：
                    // 把载荷捡回来释放掉，否则"挖掘中关窗口"每次都漏一份（结论那份是 new 的）。
                    DrainUndispatchedPayloads(hwnd);

                    if (context->Running)
                    {
                        LogLine(context, false, L"用户关闭了窗口，还原已中止（已复制的文件保留）。");
                    }

                    ::KillTimer(hwnd, kWatchdogTimerId);
                    ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                    context->Closed = true;

                    if (g_Context == context)
                    {
                        g_Context = nullptr;
                    }
                    if (context->QueueEvent)
                    {
                        ::CloseHandle(context->QueueEvent);
                    }
                    ::CloseHandle(context->StopEvent);
                    ::DeleteCriticalSection(&context->QueueLock);
                    if (context->FontUi)
                    {
                        ::DeleteObject(context->FontUi);
                    }
                    if (context->FontLog)
                    {
                        ::DeleteObject(context->FontLog);
                    }
                    if (context->LogBrush)
                    {
                        ::DeleteObject(context->LogBrush);
                    }
                    delete context;
                }
                return 0;
            }

            default:
                break;
        }

        return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }

    HFONT CreateUiFont(int pointSize, bool bold, const wchar_t* face)
    {
        LOGFONTW lf{};
        lf.lfHeight = -::MulDiv(pointSize, 96, 72);
        lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        ::wcsncpy_s(lf.lfFaceName, face, _TRUNCATE);
        return ::CreateFontIndirectW(&lf);
    }

    bool EnsureWindowClass(HINSTANCE instance)
    {
        static bool registered = false;
        if (registered)
        {
            return true;
        }

        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.style = CS_HREDRAW | CS_VREDRAW;
        cls.lpfnWndProc = RestoreWindowProc;
        cls.hInstance = instance;
        cls.hIcon = ::LoadIconW(instance, L"AmetoYuki_1");
        cls.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        cls.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        cls.lpszClassName = kRestoreWindowClass;
        cls.hIconSm = cls.hIcon;

        if (::RegisterClassExW(&cls) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
        registered = true;
        return true;
    }
}

// ---------- 对外入口 ----------

HWND ShowRestoreWindow(HINSTANCE instance, const WindowOptions& options)
{
    if (g_Context && g_Context->Window && ::IsWindow(g_Context->Window))
    {
        // 只允许开一个：重复点「步骤3」把它拉到前面就够了，多开会同时往同一个
        // 输出目录写，重名改名逻辑会把结果拆得到处都是。
        ::SetForegroundWindow(g_Context->Window);
        if (::IsIconic(g_Context->Window))
        {
            ::ShowWindow(g_Context->Window, SW_RESTORE);
        }
        return g_Context->Window;
    }

    INITCOMMONCONTROLSEX commonControls{};
    commonControls.dwSize = sizeof(commonControls);
    commonControls.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
    ::InitCommonControlsEx(&commonControls);

    if (!EnsureWindowClass(instance))
    {
        ::MessageBoxW(options.Owner, L"注册还原窗口类失败。", Host::kProductName, MB_OK | MB_ICONERROR);
        return nullptr;
    }

    UiContext* context = new (std::nothrow) UiContext();
    if (!context)
    {
        ::MessageBoxW(options.Owner, L"内存不足，打不开还原窗口。", Host::kProductName, MB_OK | MB_ICONERROR);
        return nullptr;
    }

    context->Instance = instance;
    context->Owner = options.Owner;
    context->GameDirectory = options.GameDirectory;
    context->SourceDirectory = TrimTrailingSeparators(options.SourceDirectory);
    context->HashDirectory = TrimTrailingSeparators(options.HashDirectory);
    context->LogDirectory = options.LogDirectory;
    context->OutputDirectory = options.DefaultOutputDirectory.empty()
        ? options.SourceDirectory
        : TrimTrailingSeparators(options.DefaultOutputDirectory);
    context->FontUi = CreateUiFont(9, false, L"微软雅黑");
    context->FontLog = CreateUiFont(9, false, L"Consolas");
    context->LogBrush = ::CreateSolidBrush(kColorLogBack);
    ::InitializeCriticalSection(&context->QueueLock);
    context->StopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    context->QueueEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!context->StopEvent || !context->QueueEvent)
    {
        if (context->StopEvent)
        {
            ::CloseHandle(context->StopEvent);
        }
        if (context->QueueEvent)
        {
            ::CloseHandle(context->QueueEvent);
        }
        ::DeleteCriticalSection(&context->QueueLock);
        delete context;
        ::MessageBoxW(options.Owner, L"创建同步事件失败，打不开还原窗口。", Host::kProductName,
                      MB_OK | MB_ICONERROR);
        return nullptr;
    }
    g_Context = context;

    // 宽高按图2 的比例给：顶部一行、统计行、日志、任务列表、复选框、按钮。
    RECT ownerRect{};
    int width = 780;
    int height = 740;
    int left = CW_USEDEFAULT;
    int top = CW_USEDEFAULT;
    if (options.Owner && ::GetWindowRect(options.Owner, &ownerRect))
    {
        left = ownerRect.left + 40;
        top = ownerRect.top + 40;
    }

    HWND window = ::CreateWindowExW(WS_EX_CONTROLPARENT, kRestoreWindowClass, kWindowTitle,
                                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, left, top, width, height,
                                    options.Owner, nullptr, instance, context);
    if (!window)
    {
        g_Context = nullptr;
        ::CloseHandle(context->StopEvent);
        ::CloseHandle(context->QueueEvent);
        ::DeleteCriticalSection(&context->QueueLock);
        if (context->FontUi)
        {
            ::DeleteObject(context->FontUi);
        }
        if (context->FontLog)
        {
            ::DeleteObject(context->FontLog);
        }
        if (context->LogBrush)
        {
            ::DeleteObject(context->LogBrush);
        }
        delete context;
        ::MessageBoxW(options.Owner, L"创建还原窗口失败。", Host::kProductName, MB_OK | MB_ICONERROR);
        return nullptr;
    }

    ::ShowWindow(window, SW_SHOWNORMAL);
    ::UpdateWindow(window);
    ::SetForegroundWindow(window);
    return window;
}

void CloseRestoreWindow(HWND window)
{
    if (window && ::IsWindow(window))
    {
        ::SendMessageW(window, WM_CLOSE, 0, 0);
    }
}

bool IsRestoreRunning(HWND window)
{
    if (!window || !::IsWindow(window))
    {
        return false;
    }
    // 走窗口消息问，别去读 g_Context：那个指针在 WM_DESTROY 之后就可能已经失效了。
    const UiContext* context =
        reinterpret_cast<const UiContext*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    return context != nullptr && context->Running;
}
}
}
