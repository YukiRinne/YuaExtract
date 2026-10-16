// YuaExtract 主界面。
//
// 这一层只做三件事：把上游的功能组织成"解包 → 收集 Hash → 还原名字"三步流程、
// 把每一步的结果随时显示在右侧概览里、把上游原版入口收进"高级操作"备查。
//
// 控件全部用 CreateWindowW 建（不是对话框资源）：布局要随窗口尺寸重排，
// 概览内容要按状态动态生成，资源脚本那种固定坐标反而更难维护。

#include "YuaExtractMainWindow.h"

#include "ExtensionSet.h"
#include "HashNameRestorer.h"
#include "OnlineUpdate.h"
#include "YuaExtractHost.h"
#include "YuaExtractWorkspace.h"
#include "YuaRestoreWindow.h"
#include "resource.h"

#include <commctrl.h>
#include <richedit.h>
#include <shellapi.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")

namespace Yua
{
namespace
{
    // ---------- 控件编号 ----------

    enum : int
    {
        IDC_YUA_TAB_HOME = 4001,
        IDC_YUA_TAB_MAKE_SET = 4002,
        IDC_YUA_TAB_SET = 4003,
        IDC_YUA_TAB_ABOUT = 4004,

        IDC_YUA_GROUP_EXE = 4010,
        IDC_YUA_GROUP_FUNC = 4011,
        IDC_YUA_GROUP_ADV = 4012,
        IDC_YUA_GROUP_FLOW = 4013,
        IDC_YUA_GROUP_AUX = 4014,

        IDC_YUA_EXE_LABEL = 4020,
        IDC_YUA_EXE_EDIT = 4021,
        IDC_YUA_EXE_BROWSE = 4022,

        IDC_YUA_FUNC_SET_EXTRACT = 4030, // 该作扩展集提取（预留）
        IDC_YUA_FUNC_SET_CRACK = 4031,   // 该会社合集撞新作（预留）
        IDC_YUA_FUNC_CLASSIC = 4032,     // 传统动态模式

        IDC_YUA_ADVANCED = 4040, // CxdecExtractorLoader 原版功能

        IDC_YUA_STEP1 = 4050,
        IDC_YUA_STEP2 = 4051,
        IDC_YUA_STEP3 = 4052,

        IDC_YUA_AUX_CHECK = 4060,
        IDC_YUA_AUX_WORKSPACE = 4061,
        IDC_YUA_AUX_CLEAN = 4062,
        IDC_YUA_AUX_RESTOREDIR = 4063,
        IDC_YUA_AUX_REPORT = 4064,

        IDC_YUA_OVERVIEW = 4070,
        IDC_YUA_PAGE = 4071,
        IDC_YUA_LOGBTN = 4072,
        IDC_YUA_LOGVIEW = 4073,
        IDC_YUA_HINT = 4074,

        // 首页「扩展集选择」（设计稿 §8.3 图3 左列）：会社 + 作品两个列表框。
        IDC_YUA_GROUP_LIB = 4080,
        IDC_YUA_LIB_PUB_LABEL = 4081,
        IDC_YUA_LIB_TITLE_LABEL = 4082,
        IDC_YUA_LIB_PUBLISHERS = 4083,
        IDC_YUA_LIB_TITLES = 4084,

        // 「任务进度」：生成 / 静态 Hash 表 / 干跑这类后台任务共用的一行。
        // 首页与「制作扩展集」页签都显示它，只是摆放位置不同。
        IDC_YUA_GROUP_TASK = 4085,
        IDC_YUA_TASK_PROGRESS = 4086,
        IDC_YUA_TASK_TEXT = 4087,
        IDC_YUA_TASK_CANCEL = 4088,

        // 「制作扩展集」页签（设计稿 §8.1）。
        IDC_YUA_SET_GROUP_MAKE = 4100,
        IDC_YUA_SET_GENERATE = 4101,
        IDC_YUA_SET_EDITINFO = 4102,
        IDC_YUA_SET_TEST = 4103,
        IDC_YUA_SET_GROUP_DIR = 4104,
        IDC_YUA_SET_OPEN_DRAFT = 4105,
        IDC_YUA_SET_INSTALL = 4106,
        IDC_YUA_SET_OPEN_LIB = 4107,
        IDC_YUA_SET_GROUP_CUSTOM = 4108,
        IDC_YUA_SET_PICK_SOURCE = 4109,
        IDC_YUA_SET_PICK_TEST = 4110,
        IDC_YUA_SET_STATUS = 4111,

        // 「编辑扩展集信息」弹窗（设计稿 §8.2 图2）。
        IDC_YUA_META_PUBLISHER = 4120,
        IDC_YUA_META_TITLE = 4121,
        IDC_YUA_META_CONTRIBUTOR = 4122,
        IDC_YUA_META_VERSION = 4123,
        IDC_YUA_META_DATE = 4124,
        IDC_YUA_META_COMMENT = 4125,
        IDC_YUA_META_OK = 4126,
        IDC_YUA_META_CANCEL = 4127,

        // 「扩展集」页签（设计稿 §8.5，2026-10-15 冻结）。
        IDC_YUA_SETM_GROUP_BROWSE = 4130,
        IDC_YUA_SETM_REFRESH = 4131,
        IDC_YUA_SETM_OPEN_ROOT = 4132,
        IDC_YUA_SETM_OPEN_ENTRY = 4133,
        IDC_YUA_SETM_UPDATE = 4134,
        IDC_YUA_SETM_SELFCHECK = 4135,
        IDC_YUA_SETM_NOTE = 4136,

        // 「在线更新」弹窗（设计稿 §8.5 / §14）。
        IDC_YUA_ONLINE_ENDPOINT = 4140,
        IDC_YUA_ONLINE_REFRESH = 4141,
        IDC_YUA_ONLINE_LIST = 4142,
        IDC_YUA_ONLINE_INSTALL = 4143,
        IDC_YUA_ONLINE_PROGRESS = 4144,
        IDC_YUA_ONLINE_STATUS = 4145,
        IDC_YUA_ONLINE_CLOSE = 4146,
    };

    // 旧版这里是"主界面自己开线程跑同步还原"的两个消息；现在还原全部交给
    // YuaRestoreWindow（按封包多线程 + 任务列表），主界面只收它的通报。
    // 消息号定义在 YuaRestoreWindow.h（Restore::kRestoreNotifyMessage）。

    // 首页功能三个按钮是互斥的**模式选择**（设计稿 §8.3），不是三个独立动作。
    enum class ActiveMode
    {
        SetExtract = 0, // 该作扩展集提取（模式1，一期）
        SetCrack = 1,   // 该会社合集撞新作（二期预留）
        Classic = 2     // 传统动态模式（既有行为，原样保留）
    };

    // 后台任务种类：决定线程体干什么、回来之后怎么落到界面状态。
    enum class JobKind
    {
        None = 0,
        Generate = 1,   // 生成当前游戏扩展集
        StaticHash = 2, // 模式1 步骤1：加载扩展集 + 写静态 Hash 表
        DryRun = 3      // 测试当前扩展集（只读干跑）
    };

    // 加载失败的细分直接用内核的 ExtensionSet::LoadFailure（ok=true 时恒为 None）。
    // 早先这里按"解出了什么"自己猜（当时内核把"档案坏了"和"盐不同"都报成 Rejected）；
    // 内核补上 failure 字段后改为读官方字段，猜法已删除。

    // 概览「扩展集状态 → 归属校验」那一行的结论。
    // 必须长期留在面板上：成功模态框一闪而过，"用户强行用了未校验的档案"这种事
    // 事后翻概览/日志必须还能看出来（设计稿 §13.12）。
    enum class LoadVerdict
    {
        NotLoaded = 0,
        Verified = 1,   // 通过归属校验（exe 摘要命中 / 盐一致）
        Unverified = 2, // 用户确认后放宽 allowUnverified 放行
        Rejected = 3    // 明确拒绝加载
    };

    // 后台任务。生成 / 加载 / 干跑都可能跑几十秒，绝不能在界面线程里同步跑 ——
    // 沿用 YuaRestoreWindow 挖掘线程的做法：起线程 → 投进度消息 → 回来读结论。
    // 结果字段由任务线程写、界面线程在 kSetDoneMessage 里读，靠 Done 标志同步。
    struct SetJob
    {
        JobKind Kind = JobKind::None;
        HANDLE Thread = nullptr;
        volatile LONG Cancel = 0;  // 1 = 用户点了取消
        volatile LONG Done = 0;    // 1 = 线程体已跑完（结果字段此时已写完）
        volatile LONG Percent = 0;
        CRITICAL_SECTION Lock{};
        bool LockReady = false;
        std::wstring Stage;
        std::wstring Error;
        bool Ok = false;
        // 失败细分（内核 LoadFailure）与"这次加载是放宽后放行的"（UnverifiedLoad）。
        // 概览面板要长期显示归属结论，所以这两个必须由任务线程带回来。
        ExtensionSet::LoadFailure Failure = ExtensionSet::LoadFailure::None;
        bool UnverifiedLoad = false;

        // 线程体的输入（起线程之前填好，之后只读）。
        ExtensionSet::GenerateOptions GenerateOptions;
        ExtensionSet::LoadOptions LoadOptions;
        ExtensionSet::DryRunOptions DryRunOptions;
        std::wstring LibraryRoot;
        std::wstring ArchivePath;
        std::wstring OutputDirectory;
        std::wstring Publisher;
        std::wstring Title;

        // 线程体的输出。
        ExtensionSet::GenerateResult Generate;
        ExtensionSet::StaticHashResult Static;
        ExtensionSet::DryRunResult DryRun;
        ExtensionSet::LoadResult Load;
    };

    // 后台任务的私有消息（主窗口自己收，不跨窗口）：
    //   kSetProgressMessage：进度与阶段文字在 SetJob 里，加锁读；
    //   kSetDoneMessage：线程已把结果写进 SetJob 并置 Done。
    constexpr UINT kSetProgressMessage = WM_APP + 0x5B0;
    constexpr UINT kSetDoneMessage = WM_APP + 0x5B1;

    // 「在线更新」弹窗自己的私有消息（弹窗的窗口过程自己收，与主窗口那两条互不相干）。
    constexpr UINT kOnlineProgressMessage = WM_APP + 0x5C0;
    constexpr UINT kOnlineDoneMessage = WM_APP + 0x5C1;

    // 「编辑扩展集信息」弹窗。不新增对话框资源（那要动 .rc / resource.h），
    // 这里注册一个自己的顶层窗口类，按图2 的字段手工建控件。
    constexpr wchar_t kMetaDialogClass[] = L"YuaExtractMetaDialog";
    constexpr int kMetaDialogWidth = 470;
    constexpr int kMetaDialogHeight = 336;

    // 「在线更新」弹窗：与图2 同一手法（自己的顶层窗口类 + 手工建控件），
    // 但多一件事 —— 联网动作跑在后台线程里，所以它还有自己的一份 job 结构。
    constexpr wchar_t kOnlineDialogClass[] = L"YuaExtractOnlineDialog";
    constexpr int kOnlineDialogWidth = 640;
    constexpr int kOnlineDialogHeight = 452;

    // 在线更新弹窗的后台任务种类。
    enum class OnlineJobKind
    {
        None = 0,
        FetchIndex = 1, // 拉远端索引
        Download = 2    // 下载 + 校验（**入库不在这里**，见 OnlineJobThreadProc 的注释）
    };

    // 在线更新弹窗的后台任务。结果字段由任务线程写、界面线程在 kOnlineDoneMessage 里读，
    // 靠 Done 标志同步 —— 与主窗口的 SetJob 同一手法。
    //
    // **为什么不复用 SetJob**：那个结构体的字段全是"扩展集生成 / 干跑"的结果，它的收尾
    // 逻辑（OnJobDone）还会去写主窗口的概览、状态栏与归属结论。网络任务的失败口径与
    // 那套毫无关系，混进同一个结构体只会让两边互相牵制（比如"下载失败"要往
    // LastLoadVerdict 里写什么？没有答案）。手法照抄一份，比硬凑更省事。
    struct OnlineJob
    {
        OnlineJobKind Kind = OnlineJobKind::None;
        HANDLE Thread = nullptr;
        volatile LONG Cancel = 0;
        volatile LONG Done = 0;
        volatile LONG Percent = 0;
        CRITICAL_SECTION Lock{};
        bool LockReady = false;
        std::wstring Stage;
        std::wstring Error;
        bool Ok = false;

        // 线程体的输入（起线程之前填好，之后只读）。
        std::wstring Base;
        std::wstring StagingDirectory;
        ExtensionSet::LibraryEntry Entry;

        // 线程体的输出。
        std::vector<ExtensionSet::LibraryEntry> Entries;
        Online::IndexResult Index;
        Online::DownloadResult Download;
        std::wstring VerifyDetail; // VerifyArchive 的结论原文（给确认框、状态栏与日志用）
    };

    constexpr wchar_t kWindowClass[] = L"YuaExtractMainWindow";
    constexpr UINT_PTR kRefreshTimerId = 1;
    constexpr UINT kRefreshIntervalMs = 1500;

    // 96 DPI 下的布局常量。
    constexpr int kMargin = 10;
    constexpr int kTabHeight = 30;
    constexpr int kTabGap = 4;
    constexpr int kLogButtonHeight = 24;
    constexpr int kGroupTitleHeight = 20;
    constexpr int kRowHeight = 24;
    constexpr int kRowGap = 6;
    constexpr int kRightPanelMinWidth = 380;
    constexpr int kLeftPanelMinWidth = 430;
    constexpr int kTaskGroupHeight = 68; // 「任务进度」一行（首页与制作页签共用）

    COLORREF kColorAccent = RGB(0x1F, 0x6F, 0xD6);
    COLORREF kColorText = RGB(0x33, 0x33, 0x33);
    COLORREF kColorMuted = RGB(0x7A, 0x7A, 0x7A);
    COLORREF kColorOk = RGB(0x1B, 0x7F, 0x3B);
    COLORREF kColorWarn = RGB(0xB0, 0x50, 0x00);
    COLORREF kColorSection = RGB(0x1F, 0x4E, 0x79);
    COLORREF kColorTabIdleText = RGB(0x55, 0x55, 0x55);
    COLORREF kColorTabBorder = RGB(0xC8, 0xC8, 0xC8);
    COLORREF kColorLogBackground = RGB(0x1E, 0x1E, 0x1E);
    COLORREF kColorLogText = RGB(0xD4, 0xD4, 0xD4);

    // ---------- 概览面板的富文本 ----------

    struct TextRun
    {
        std::wstring Text;
        COLORREF Color = kColorText;
        bool Bold = false;
        int SizePt = 9;
    };

    void AppendRun(std::vector<TextRun>& runs, const std::wstring& text, COLORREF color, bool bold,
                   int sizePt = 9)
    {
        if (text.empty())
        {
            return;
        }
        TextRun run;
        run.Text = text;
        run.Color = color;
        run.Bold = bold;
        run.SizePt = sizePt;
        runs.push_back(run);
    }

    void AppendSectionTitle(std::vector<TextRun>& runs, const wchar_t* title)
    {
        AppendRun(runs, title, kColorSection, true, 10);
        AppendRun(runs, L"\r\n", kColorText, false);
    }

    // 「运行环境自检」的单项结论。
    //
    // Severity：0 = 正常；1 = 问题（计入"共 N 项问题"）；2 = 提示（只报告、不判错）。
    // 之所以存成数据而不是当场拼字符串：同一份结论要落三处（右侧详情面板、弹框、日志），
    // 三处措辞必须来自同一份事实 —— 否则又会冒出"面板说好、弹框说坏"这种自相矛盾。
    struct SelfCheckItem
    {
        std::wstring Label;
        std::wstring Detail;
        int Severity = 0;
    };

    // ---------- 状态 ----------

    struct UiContext
    {
        HINSTANCE Instance = nullptr;
        HWND Window = nullptr;
        bool RichEditAvailable = false;
        std::wstring RichEditClass;

        HFONT FontUi = nullptr;
        HFONT FontBold = nullptr;
        HFONT FontMono = nullptr;
        HBRUSH WhiteBrush = nullptr;
        HBRUSH LogBrush = nullptr;

        std::wstring ExePath;
        WorkspacePaths Paths;

        // 探测结果
        DirProbe ExtractProbe;   // deep
        DirProbe HashProbe;      // deep
        DirProbe RestoreProbe;   // deep
        bool ProbedDeep = false;

        MappingSummary Mapping;

        // 0 = 未开始；1/2/3 = 已经走到第几步
        int Stage = 0;
        std::wstring StageNote;

        // 步骤3 的还原窗口句柄（可能为 nullptr = 没开或已关闭）。
        // 还原本身跑在那个窗口自己的线程里，主界面不参与调度，只靠它的通报刷新概览。
        HWND RestoreWindow = nullptr;

        int ActiveTab = IDC_YUA_TAB_HOME;
        bool LogVisible = false;
        std::wstring RenderedOverview; // 和当前面板内容比对，避免每次定时器都重绘

        std::vector<HWND> HomeControls;
        std::vector<HWND> MakeControls;   // 「制作扩展集」页签的控件
        std::vector<HWND> SetPageControls; // 「扩展集」页签自己的控件（「扩展集管理与浏览」一栏）
        // 「扩展集选择」（会社 / 作品两级列表）：首页与「扩展集」页签**共用同一份**。
        // 两个页签看的是同一份库清单、同一份 SelectedPublisher/SelectedTitle，所以只做一对列表；
        // 各做一份必然会出"首页选的是 A、扩展集页签显示的是 B"这种怪事（设计稿 §8.5）。
        std::vector<HWND> LibControls;
        std::vector<HWND> TargetControls; // 目标游戏一行：首页与制作页签共用
        std::vector<HWND> TaskControls;   // 任务进度一行：首页与制作页签共用

        // ---- 首页模式（设计稿 §8.3）----
        ActiveMode Mode = ActiveMode::Classic;

        // ---- 扩展集（制作页签 + 模式1）----
        std::wstring LibraryRoot;        // 扩展集库根：<工具目录>\ExtensionLibrary
        std::wstring DraftDir;           // <工作区>\ExtensionDraft
        std::wstring DraftArchive;       // <草案>\rules.int
        std::wstring StaticHashDir;      // <工作区>\StaticHash_Output（模式1 步骤1 的产物）
        std::wstring SourceOverride;     // 采集来源覆盖（空 = 默认 Restored_Extractor_Output，只影响本次会话）
        std::wstring TestSourceOverride; // 测试来源覆盖（空 = 跟随采集来源）
        ExtensionSet::Meta Meta;         // 「编辑扩展集信息」的当前值，生成时写进档案
        std::wstring SelectedPublisher;
        std::wstring SelectedTitle;
        ExtensionSet::LibraryCatalog Catalog;
        std::wstring MakeNote;        // 制作页签最近一次操作的结论
        std::wstring LastGenerateNote;
        std::wstring LastTestNote;
        std::wstring LastStaticNote;
        // 「扩展集」页签最近一次操作的结论（刷新 / 打开目录 / 自检），显示在右侧「扩展集详情」里。
        std::wstring SetPageNote;
        // 最近一次「运行环境自检」的逐项结论。**存下来**而不是每次重绘都重跑：
        // 详情面板会被反复重绘（切页签、改窗口大小、定时器），自检要读盘 + 解档案，不能当重绘副作用。
        std::vector<SelfCheckItem> SelfCheck;
        bool HasSelfCheck = false;
        // 最近一次扩展集加载的**归属结论**（内核 LoadResult::matchNote 原文）。
        // 概览面板要长期显示它：成功模态框一闪而过，事后翻概览/日志必须能看出
        // "这次到底是 exe 摘要命中，还是用户强行用了未校验的档案"（设计稿 §13.12）。
        std::wstring LastLoadNote;
        LoadVerdict LastLoadVerdict = LoadVerdict::NotLoaded;
        bool HasGenerate = false;
        bool HasDryRun = false;
        bool HasStatic = false;

        // 后台任务（生成 / 静态 Hash 表 / 干跑）。
        SetJob Job;
        bool JobActive = false;
    };

    UiContext* ContextOf(HWND hwnd)
    {
        return reinterpret_cast<UiContext*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    // ---------- 扩展集：路径、模式与库清单的小工具 ----------

    bool EnsureDirectory(const std::wstring& directory, std::wstring& error)
    {
        if (directory.empty())
        {
            error = L"目录路径为空。";
            return false;
        }
        if (::CreateDirectoryW(directory.c_str(), nullptr))
        {
            return true;
        }
        const DWORD last = ::GetLastError();
        if (last == ERROR_ALREADY_EXISTS)
        {
            return true;
        }
        error = L"无法创建目录（错误码 " + std::to_wstring(last) + L"）：" + directory;
        return false;
    }

    const wchar_t* ModeName(ActiveMode mode)
    {
        switch (mode)
        {
            case ActiveMode::SetExtract: return L"该作扩展集提取";
            case ActiveMode::SetCrack: return L"该会社合集撞新作（二期预留）";
            default: return L"传统动态模式";
        }
    }

    // 目标 exe 名去扩展名：图2 的「游戏名称」默认值（设计稿 §8.2）。
    std::wstring ExeStem(const std::wstring& path)
    {
        const size_t slash = path.find_last_of(L"\\/");
        std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
        const size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos && dot > 0)
        {
            name.resize(dot);
        }
        return name;
    }

    std::wstring TodayText()
    {
        SYSTEMTIME time{};
        ::GetLocalTime(&time);
        wchar_t buffer[32]{};
        ::swprintf_s(buffer, L"%04u-%02u-%02u", time.wYear, time.wMonth, time.wDay);
        return buffer;
    }

    // 工作区变了就重算三个扩展集路径（工作区 = 游戏目录，设计稿 §9）。
    void RefreshSetPaths(UiContext& context)
    {
        if (context.Paths.GameDir.empty())
        {
            context.DraftDir.clear();
            context.DraftArchive.clear();
            context.StaticHashDir.clear();
            return;
        }
        context.DraftDir = ExtensionSet::DefaultDraftDirectory(context.Paths.GameDir);
        context.DraftArchive = ExtensionSet::DefaultArchivePath(context.Paths.GameDir);
        context.StaticHashDir = ExtensionSet::DefaultStaticHashDirectory(context.Paths.GameDir);
    }

    bool FileExistsAt(const std::wstring& path)
    {
        return !path.empty() && ::GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    }

    // 档案大小（"测试结果（仅供参考）"里要显示 rules.int 的字节数）；读不到回 0。
    unsigned long long FileSizeBytes(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (path.empty() || !::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
        {
            return 0ull;
        }
        return (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) |
               static_cast<unsigned long long>(data.nFileSizeLow);
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

    // 按原始字节读一个文件（自检用）。要"原样"读是因为 manifest.txt 要按 UTF-8 原文交给内核
    // 那份解析器：自己先解码成宽字符再编码回去，非法字节会被洗掉，正好把"清单坏了"这条问题掩盖过去。
    bool ReadFileBytes(const std::wstring& path, std::string& out, std::wstring& error)
    {
        out.clear();
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = L"打开失败（Win32 错误码 " + std::to_wstring(::GetLastError()) + L"）：" + path;
            return false;
        }
        LARGE_INTEGER size{};
        if (!::GetFileSizeEx(file, &size) || size.QuadPart < 0)
        {
            ::CloseHandle(file);
            error = L"读不到文件大小：" + path;
            return false;
        }
        // 清单 / 索引都是几十 KB 的文件，"几十 MB 的清单"只可能是拿错了文件 —— 别把它读进内存。
        if (size.QuadPart > (64ll * 1024ll * 1024ll))
        {
            ::CloseHandle(file);
            error = L"文件过大（" + FormatSize(static_cast<unsigned long long>(size.QuadPart)) +
                    L"），不像是清单文件：" + path;
            return false;
        }
        out.resize(static_cast<size_t>(size.QuadPart));
        if (!out.empty())
        {
            DWORD read = 0u;
            const BOOL ok = ::ReadFile(file, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
            const DWORD readError = ok ? 0u : ::GetLastError();
            if (!ok || read != static_cast<DWORD>(out.size()))
            {
                ::CloseHandle(file);
                out.clear();
                error = L"读取失败（Win32 错误码 " + std::to_wstring(readError) + L"）：" + path;
                return false;
            }
        }
        ::CloseHandle(file);
        return true;
    }

    // 文件里找一段 ASCII 文字（自检里找 `Hash Seed:` 用）。
    // 按字节找即可：要找的是 ASCII，日志是 UTF-8/ANSI 都包含同样的字节。
    // 只读前 8 MB：Universal.log 是会话日志，盐写在开头附近，为一行字把整份几 MB 读进来不划算
    //（读不到就如实报"没读到" —— 这一条本来只报告、不判错）。
    bool FileContainsAscii(const std::wstring& path, const std::string& needle, unsigned long long maxBytes)
    {
        if (needle.empty())
        {
            return false;
        }
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        std::string chunk(64u * 1024u, '\0');
        std::string carry; // 上一块的尾巴：匹配可能跨块
        unsigned long long total = 0ull;
        bool found = false;
        for (;;)
        {
            DWORD read = 0u;
            if (!::ReadFile(file, &chunk[0], static_cast<DWORD>(chunk.size()), &read, nullptr) || read == 0u)
            {
                break;
            }
            const std::string window = carry + chunk.substr(0u, read);
            if (window.find(needle) != std::string::npos)
            {
                found = true;
                break;
            }
            carry = window.size() >= needle.size()
                        ? window.substr(window.size() - (needle.size() - 1u))
                        : window;
            total += read;
            if (total >= maxBytes)
            {
                break;
            }
        }
        ::CloseHandle(file);
        return found;
    }

    // 目录可写探测：写一个小临时文件再删掉。
    // 用唯一名 + FILE_ATTRIBUTE_TEMPORARY：不会因为"目录可写但盘满了"留下半截文件。
    bool ProbeDirectoryWritable(const std::wstring& directory, std::wstring& error)
    {
        if (!DirectoryExistsAt(directory))
        {
            error = L"目录不存在";
            return false;
        }
        wchar_t name[64]{};
        ::swprintf_s(name, L"YuaExtract_selfcheck_%lu_%lu.tmp",
                     static_cast<unsigned long>(::GetCurrentProcessId()),
                     static_cast<unsigned long>(::GetTickCount64() & 0xFFFFFFull));
        const std::wstring probe = JoinPath(directory, name);

        HANDLE file = ::CreateFileW(probe.c_str(), GENERIC_WRITE, 0u, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = L"建不出文件（Win32 错误码 " + std::to_wstring(::GetLastError()) + L"）";
            return false;
        }
        const char payload[] = "YuaExtract self-check";
        DWORD written = 0u;
        const BOOL wrote = ::WriteFile(file, payload, sizeof(payload) - 1u, &written, nullptr);
        const DWORD writeError = wrote ? 0u : ::GetLastError();
        ::CloseHandle(file);
        if (!wrote || written != sizeof(payload) - 1u)
        {
            ::DeleteFileW(probe.c_str());
            error = L"写不进内容（Win32 错误码 " + std::to_wstring(writeError) + L"）";
            return false;
        }
        if (!::DeleteFileW(probe.c_str()))
        {
            error = L"写得进去但删不掉（Win32 错误码 " + std::to_wstring(::GetLastError()) +
                    L"）：目录可能只读或被别的程序占着";
            return false;
        }
        return true;
    }

    // 读 PE 头里的机器类型。只读文件头 4 KB 就够（DOS 头 + PE 签名 + FileHeader 都在最前面）。
    // 为什么要看这个：注入模块是 32 位的，注不进 64 位游戏进程 —— 这个 DLL 的位数不对时
    // 表现为"步骤2 点了没反应"，自检里直接点名比事后去猜省事。
    bool ReadPeMachineType(const std::wstring& path, WORD& machine, std::wstring& error)
    {
        HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            error = L"打不开文件（Win32 错误码 " + std::to_wstring(::GetLastError()) + L"）";
            return false;
        }
        unsigned char header[4096]{};
        DWORD read = 0u;
        const BOOL ok = ::ReadFile(file, header, sizeof(header), &read, nullptr);
        const DWORD readError = ok ? 0u : ::GetLastError();
        ::CloseHandle(file);
        if (!ok)
        {
            error = L"读不到文件（Win32 错误码 " + std::to_wstring(readError) + L"）";
            return false;
        }
        if (read < sizeof(IMAGE_NT_HEADERS32) + 0x40u)
        {
            error = L"文件太小，装不下 PE 头";
            return false;
        }
        DWORD peOffset = 0u;
        ::memcpy(&peOffset, header + 0x3C, sizeof(peOffset));
        if (peOffset + sizeof(IMAGE_NT_HEADERS32) > read)
        {
            error = L"PE 头超出了已读入的范围";
            return false;
        }
        IMAGE_NT_HEADERS32 headers{};
        ::memcpy(&headers, header + peOffset, sizeof(headers));
        if (headers.Signature != IMAGE_NT_SIGNATURE)
        {
            error = L"找不到 PE 签名";
            return false;
        }
        machine = headers.FileHeader.Machine;
        return true;
    }

    // 模式1 步骤1 的产物在不在：StaticHash_Output\StaticHash.lst。
    bool StaticHashReady(const UiContext& context)
    {
        return FileExistsAt(JoinPath(context.StaticHashDir, ExtensionSet::kStaticHashFileName));
    }

    // 采集来源：默认 Restored_Extractor_Output，「选择扩展集来源目录」可覆盖（只影响本次会话）。
    std::wstring EffectiveSourceDirectory(const UiContext& context)
    {
        if (!context.SourceOverride.empty())
        {
            return context.SourceOverride;
        }
        return context.Paths.RestoreDir;
    }

    // 测试来源：默认跟随采集来源。
    std::wstring EffectiveTestDirectory(const UiContext& context)
    {
        if (!context.TestSourceOverride.empty())
        {
            return context.TestSourceOverride;
        }
        return EffectiveSourceDirectory(context);
    }

    std::wstring GetControlText(HWND control)
    {
        if (!control)
        {
            return std::wstring();
        }
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

    // 作品列表随会社选择刷新。切换会社时旧的作品选择必须清掉，
    // 否则会把 A 社的作品名当成 B 社的选择去加载。
    void RefillTitleList(UiContext& context)
    {
        HWND titles = ::GetDlgItem(context.Window, IDC_YUA_LIB_TITLES);
        if (titles)
        {
            ::SendMessageW(titles, LB_RESETCONTENT, 0, 0);
        }
        context.SelectedTitle.clear();
        if (context.SelectedPublisher.empty())
        {
            return;
        }
        const auto found = context.Catalog.titles.find(context.SelectedPublisher);
        if (found == context.Catalog.titles.end() || !titles)
        {
            return;
        }
        for (const std::wstring& title : found->second)
        {
            ::SendMessageW(titles, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(title.c_str()));
        }
    }

    // 扫库清单，填会社/作品两个列表框。库根不存在不算错（内核返回空清单）。
    void RefreshLibrary(UiContext& context)
    {
        if (context.LibraryRoot.empty())
        {
            context.LibraryRoot = ExtensionSet::DefaultLibraryRoot(Host::LoaderDirectory());
        }
        context.Catalog = ExtensionSet::EnumerateLibrary(context.LibraryRoot);

        HWND publishers = ::GetDlgItem(context.Window, IDC_YUA_LIB_PUBLISHERS);
        if (publishers)
        {
            ::SendMessageW(publishers, LB_RESETCONTENT, 0, 0);
        }
        int selected = -1;
        int index = 0;
        for (const std::wstring& publisher : context.Catalog.publishers)
        {
            if (publishers)
            {
                ::SendMessageW(publishers, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(publisher.c_str()));
            }
            if (publisher == context.SelectedPublisher)
            {
                selected = index;
            }
            ++index;
        }
        if (selected < 0)
        {
            context.SelectedPublisher.clear();
        }
        if (publishers && selected >= 0)
        {
            ::SendMessageW(publishers, LB_SETCURSEL, static_cast<WPARAM>(selected), 0);
        }
        RefillTitleList(context);
    }

    // ---------- 字体与画刷 ----------

    HFONT CreateUiFont(int pointSize, bool bold)
    {
        LOGFONTW lf{};
        lf.lfHeight = -::MulDiv(pointSize, 96, 72);
        lf.lfWeight = bold ? FW_SEMIBOLD : FW_NORMAL;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        ::wcscpy_s(lf.lfFaceName, L"微软雅黑");
        return ::CreateFontIndirectW(&lf);
    }

    // ---------- 文本视图（RichEdit 优先，失败退普通 EDIT） ----------

    HWND CreateTextView(HWND parent, UiContext* context, int controlId, bool darkBackground)
    {
        HWND view = nullptr;
        const DWORD common = WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL;

        if (!context->RichEditClass.empty())
        {
            view = ::CreateWindowExW(WS_EX_CLIENTEDGE, context->RichEditClass.c_str(), L"", common, 0, 0, 10,
                                     10, parent, (HMENU)(INT_PTR)controlId, context->Instance, nullptr);
            if (view)
            {
                ::SendMessageW(view, EM_SETBKGNDCOLOR, 0,
                               darkBackground ? static_cast<LPARAM>(kColorLogBackground)
                                              : static_cast<LPARAM>(RGB(255, 255, 255)));
                ::SendMessageW(view, EM_SETREADONLY, TRUE, 0);
                // 富文本控件默认会把 URL 变成可点的链接，概览里全是路径，别让它抢焦点。
                ::SendMessageW(view, EM_SETEVENTMASK, 0, 0);
            }
        }

        if (!view)
        {
            view = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", common, 0, 0, 10, 10, parent,
                                     (HMENU)(INT_PTR)controlId, context->Instance, nullptr);
        }

        if (view)
        {
            ::SendMessageW(view, WM_SETFONT,
                           reinterpret_cast<WPARAM>(darkBackground ? context->FontMono : context->FontUi),
                           TRUE);
        }
        return view;
    }

    void SetViewRuns(HWND view, UiContext* context, const std::vector<TextRun>& runs, bool dark)
    {
        if (!view)
        {
            return;
        }

        // 只在真正支持富文本时才逐段设格式，否则普通 EDIT 会当成乱码。
        if (context->RichEditClass.empty())
        {
            std::wstring plain;
            for (const TextRun& run : runs)
            {
                plain += run.Text;
            }
            ::SetWindowTextW(view, plain.c_str());
            return;
        }

        ::SendMessageW(view, WM_SETREDRAW, FALSE, 0);
        ::SetWindowTextW(view, L"");
        for (const TextRun& run : runs)
        {
            CHARRANGE end{ -1, -1 };
            ::SendMessageW(view, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&end));

            CHARFORMAT2W format{};
            format.cbSize = sizeof(format);
            format.dwMask = CFM_COLOR | CFM_BOLD | CFM_SIZE | CFM_FACE;
            format.crTextColor = dark ? kColorLogText : run.Color;
            format.dwEffects = run.Bold ? CFE_BOLD : 0;
            format.yHeight = run.SizePt * 20;
            ::wcscpy_s(format.szFaceName, dark ? L"Consolas" : L"微软雅黑");
            ::SendMessageW(view, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));

            ::SendMessageW(view, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(run.Text.c_str()));
        }

        CHARRANGE top{ 0, 0 };
        ::SendMessageW(view, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&top));
        ::SendMessageW(view, WM_SETREDRAW, TRUE, 0);
        ::InvalidateRect(view, nullptr, TRUE);
    }

    // ---------- 面板文字 ----------

    std::wstring DescribeProbe(const DirProbe& probe, const wchar_t* missingText, const wchar_t* readyText)
    {
        if (!probe.exists)
        {
            return missingText;
        }
        wchar_t buffer[256]{};
        ::swprintf_s(buffer, L"%s（%u 个目录 / %s 个文件 / %s）", readyText, probe.topLevelEntries,
                     FormatCount(probe.files).c_str(), FormatSize(probe.bytes).c_str());
        return buffer;
    }

    std::wstring NextStepAdvice(const UiContext& context)
    {
        if (context.Mode == ActiveMode::SetCrack)
        {
            return L"「该会社合集撞新作」是二期预留功能，当前版本请改用「该作扩展集提取」或「传统动态模式」。";
        }
        if (context.ExePath.empty())
        {
            return L"先选择目标游戏主程序（.exe）。";
        }
        if (context.Mode == ActiveMode::SetExtract)
        {
            if (context.SelectedPublisher.empty() || context.SelectedTitle.empty())
            {
                return L"先在左侧「扩展集选择」里选择会社，再选择该作扩展集。";
            }
            if (!StaticHashReady(context))
            {
                return L"执行「步骤1：根据该作扩展集生成静态 Hash 表」——从扩展集离线展开映射，不启动游戏。";
            }
            if (!context.ExtractProbe.exists)
            {
                return L"执行「步骤2：提取动态 XP3 资源」——启动游戏后在解包窗口拖入 *.xp3。";
            }
            if (!context.RestoreProbe.exists)
            {
                return L"执行「步骤3：用静态 Hash 还原资源名」——用扩展集展开的静态表还原明文名。";
            }
            return L"模式1 三步已完成（全程没有跑游戏动态采集）。若还原率不理想："
                   L"多半是扩展集的目录覆盖不足（目录名只能来自动态采集），重跑步骤3 也补不出来。";
        }
        if (!context.ExtractProbe.exists)
        {
            return L"执行「步骤1：提取动态 XP3 资源」——启动游戏后在解包窗口拖入 *.xp3。";
        }
        if (!context.HashProbe.exists || (context.Mapping.directoryEntries == 0 &&
                                          context.Mapping.fileNameEntries == 0))
        {
            return L"执行「步骤2：加载动态Hash收集模块」——启动游戏后正常游玩，让资源加载路径尽量多地触发。";
        }
        if (!context.RestoreProbe.exists)
        {
            return L"执行「步骤3：用动态Hash还原资源名」——把 Hash 目录/文件名还原成明文。";
        }
        return L"三步已完成。若还原率不理想：重复步骤2 补充映射，再重跑步骤3 即可。";
    }

    std::wstring DescribeStage(const UiContext& context)
    {
        const std::wstring prefix = std::wstring(ModeName(context.Mode)) + L" · ";
        switch (context.Stage)
        {
            case 1:
                return prefix + (context.Mode == ActiveMode::SetExtract ? L"已生成静态 Hash 表"
                                                                       : L"已启动解包");
            case 2:
                return prefix + (context.Mode == ActiveMode::SetExtract ? L"已启动解包"
                                                                       : L"已启动 Hash 收集");
            case 3: return prefix + L"已还原资源名";
            default: return std::wstring(ModeName(context.Mode)) + L"（未开始）";
        }
    }

    std::vector<TextRun> BuildOverviewRuns(UiContext& context)
    {
        std::vector<TextRun> runs;

        AppendSectionTitle(runs, L"功能介绍");
        if (context.Mode == ActiveMode::SetCrack)
        {
            AppendRun(runs,
                      L"「该会社合集撞新作」是二期预留功能：一期只服务同一部游戏，跨作品复用尚未实现。"
                      L"当前版本请改用「该作扩展集提取」（有该作的扩展集时）或「传统动态模式」。\r\n",
                      kColorText, false);
        }
        else if (context.Mode == ActiveMode::SetExtract)
        {
            switch (context.Stage)
            {
                case 1:
                    AppendRun(runs,
                              L"步骤1 已完成：已用选中的扩展集离线展开静态 Hash 表，写入 StaticHash_Output。"
                              L"接下来执行步骤2 提取动态 XP3 资源。\r\n",
                              kColorText, false);
                    break;
                case 2:
                    AppendRun(runs,
                              L"步骤2 已启动：游戏进程里已注入 XP3 解包界面。请把要处理的 *.xp3 拖进那个窗口排队，"
                              L"解包结果写入 Extractor_Output；做完回到本窗口执行步骤3。\r\n",
                              kColorText, false);
                    break;
                case 3:
                    AppendRun(runs,
                              L"已用扩展集展开的静态 Hash 还原资源名，结果在 Restored_Extractor_Output"
                              L"（源解包目录不变，全程没有启动游戏做动态采集）。"
                              L"默认只输出名字全是真名的文件（已还原的 + 本来就是明文名的）；"
                              L"需要保留未还原的就到还原窗口勾选「保留未还原文件名的文件」"
                              L"（它们会按文件头补上 .png/.ogg 之类后缀）。"
                              L"注意：模式1 的映射完全来自扩展集，覆盖不到「只有动态采集才采得到的名字」，"
                              L"所以还原率低于传统动态模式是正常的。\r\n",
                              kColorText, false);
                    break;
                default:
                    AppendRun(runs,
                              L"模式1「该作扩展集提取」：用一部作品已经做好的扩展集（名字 ↔ 哈希知识档案）"
                              L"离线展开静态 Hash 表，再解包、还原 —— 全程不启动游戏做动态采集。三步是"
                              L"① 根据该作扩展集生成静态 Hash 表；② 提取动态 XP3 资源；"
                              L"③ 用静态 Hash 还原资源名。先在左侧「扩展集选择」里选会社，再选作品。\r\n",
                              kColorText, false);
                    break;
            }
        }
        else
        {
            switch (context.Stage)
            {
                case 1:
                    AppendRun(runs,
                              L"步骤1 已启动：游戏进程里已注入 XP3 解包界面。请把要处理的 *.xp3 拖进那个窗口排队，"
                              L"解包结果写入 Extractor_Output；做完回到本窗口执行步骤2。\r\n",
                              kColorText, false);
                    break;
                case 2:
                    AppendRun(runs,
                              L"步骤2 已启动：游戏进程里已注入动态 Hash 收集模块。正常运行游戏即可，"
                              L"触发到的路径与文件名会持续写成 StringHashDumper_Output 下的映射表；"
                              L"收集够了回到本窗口执行步骤3。\r\n",
                              kColorText, false);
                    break;
                case 3:
                    AppendRun(runs,
                              L"已用收集到的动态 Hash 还原资源名，结果在 Restored_Extractor_Output（源解包目录不变）。"
                              L"默认只输出名字全是真名的文件（已还原的 + 本来就是明文名的），"
                              L"名字里还有未解析 Hash 段的不进输出；需要保留就在还原窗口勾选"
                              L"「保留未还原文件名的文件」（它们会按文件头补上 .png/.ogg 之类后缀）。"
                              L"还可以勾选「推理补充还原」：工具会先用已收集的映射做一遍普通还原，"
                              L"再从游戏资源里挖候选名、逐个用哈希验证后并进映射表，"
                              L"最后拿补全后的映射再补一遍没还原到的文件。\r\n",
                              kColorText, false);
                    break;
                default:
                    AppendRun(runs,
                              L"传统动态模式：适合没有扩展集、或扩展集数据不足的情况。流程是"
                              L"① 提取 Extractor_Output；② 启动游戏由 Dumper 动态收集字符串 Hash，"
                              L"生成 StringHashDumper_Output；③ 用收集到的动态 Hash 还原资源名。\r\n",
                              kColorText, false);
                    break;
            }
        }
        AppendRun(runs, L"\r\n", kColorText, false);

        AppendSectionTitle(runs, L"当前任务");
        AppendRun(runs, L"模式：" + DescribeStage(context) + L"\r\n", kColorText, false);
        AppendRun(runs,
                  L"目标游戏：" + (context.ExePath.empty() ? std::wstring(L"（未选择）") : context.ExePath) +
                      L"\r\n",
                  context.ExePath.empty() ? kColorWarn : kColorText, false);
        AppendRun(runs,
                  L"工作区：" + (context.Paths.GameDir.empty() ? std::wstring(L"（未选择）") : context.Paths.GameDir) +
                      L"\r\n",
                  context.Paths.GameDir.empty() ? kColorWarn : kColorText, false);
        if (!context.StageNote.empty())
        {
            AppendRun(runs, L"状态：" + context.StageNote + L"\r\n", kColorMuted, false);
        }
        AppendRun(runs, L"\r\n", kColorText, false);

        AppendSectionTitle(runs, L"扩展集状态");
        if (context.Mode == ActiveMode::SetCrack)
        {
            AppendRun(runs, L"二期预留模式，当前版本没有可用的扩展集选择。\r\n", kColorMuted, false);
        }
        else
        {
            if (context.SelectedPublisher.empty())
            {
                AppendRun(runs, L"请先在左侧选择会社，再选择该作扩展集。\r\n", kColorWarn, false);
            }
            else if (context.SelectedTitle.empty())
            {
                AppendRun(runs, L"会社：" + context.SelectedPublisher + L"\r\n", kColorText, false);
                AppendRun(runs, L"再选一个该会社下的作品（扩展集）。\r\n", kColorWarn, false);
            }
            else
            {
                AppendRun(runs, L"会社：" + context.SelectedPublisher + L"\r\n", kColorText, false);
                AppendRun(runs, L"作品：" + context.SelectedTitle + L"\r\n", kColorText, false);
            }
            AppendRun(runs,
                      L"扩展集库：" + (context.LibraryRoot.empty() ? std::wstring(L"（未解析）") : context.LibraryRoot) +
                          L"\r\n",
                      kColorMuted, false);
            if (context.Mode == ActiveMode::SetExtract)
            {
                AppendRun(runs, L"静态 Hash 表：", kColorText, false);
                if (StaticHashReady(context))
                {
                    AppendRun(runs, context.HasStatic ? context.LastStaticNote : std::wstring(L"已生成"), kColorOk,
                              true);
                }
                else
                {
                    AppendRun(runs, L"未生成", kColorWarn, true);
                }
                AppendRun(runs, L"\r\n", kColorText, false);

                // 归属结论必须长期留在面板上：成功模态框一闪而过，
                // "用户强行用了未校验的档案"这种事事后必须还能翻出来（设计稿 §13.12）。
                AppendRun(runs, L"归属校验：", kColorText, false);
                switch (context.LastLoadVerdict)
                {
                    case LoadVerdict::Unverified:
                        AppendRun(runs, L"未能校验归属（用户已确认强行使用）", kColorWarn, true);
                        break;
                    case LoadVerdict::Rejected:
                        AppendRun(runs, L"拒绝加载", kColorWarn, true);
                        break;
                    case LoadVerdict::Verified:
                        AppendRun(runs, context.LastLoadNote.empty() ? std::wstring(L"已通过")
                                                                    : context.LastLoadNote,
                                  kColorOk, true);
                        break;
                    default:
                        AppendRun(runs, L"（还没有加载过扩展集）", kColorMuted, false);
                        break;
                }
                AppendRun(runs, L"\r\n", kColorText, false);
                if ((context.LastLoadVerdict == LoadVerdict::Unverified ||
                     context.LastLoadVerdict == LoadVerdict::Rejected) &&
                    !context.LastLoadNote.empty())
                {
                    AppendRun(runs, context.LastLoadNote + L"\r\n",
                              context.LastLoadVerdict == LoadVerdict::Unverified ? kColorWarn : kColorMuted, false);
                }
            }
        }
        AppendRun(runs, L"\r\n", kColorText, false);

        AppendSectionTitle(runs, L"流程进度");

        if (context.Mode == ActiveMode::SetExtract)
        {
            AppendRun(runs, L"1. 静态 Hash 表：", kColorText, false);
            if (StaticHashReady(context))
            {
                AppendRun(runs, context.HasStatic ? context.LastStaticNote : std::wstring(L"已生成"), kColorOk, true);
            }
            else
            {
                AppendRun(runs, L"未生成", kColorWarn, true);
            }
            AppendRun(runs, L"\r\n", kColorText, false);

            AppendRun(runs, L"2. 动态 XP3 输出：", kColorText, false);
            if (context.ExtractProbe.exists)
            {
                AppendRun(runs, DescribeProbe(context.ExtractProbe, L"", L"已生成"), kColorOk, true);
            }
            else
            {
                AppendRun(runs, L"未生成", kColorWarn, true);
            }
            AppendRun(runs, L"\r\n", kColorText, false);

            AppendRun(runs, L"3. 资源名还原：", kColorText, false);
            if (context.RestoreProbe.exists)
            {
                AppendRun(runs, DescribeProbe(context.RestoreProbe, L"", L"已生成"), kColorOk, true);
            }
            else
            {
                AppendRun(runs, L"未生成", kColorWarn, true);
            }
            AppendRun(runs, L"\r\n", kColorText, false);
        }
        else
        {

        // 1. 动态 XP3 输出
        AppendRun(runs, L"1. 动态 XP3 输出：", kColorText, false);
        if (context.ExtractProbe.exists)
        {
            AppendRun(runs, DescribeProbe(context.ExtractProbe, L"", L"已生成"), kColorOk, true);
        }
        else
        {
            AppendRun(runs, L"未生成", kColorWarn, true);
        }
        AppendRun(runs, L"\r\n", kColorText, false);

        // 2. 动态 Hash 输出
        AppendRun(runs, L"2. 动态 Hash 输出：", kColorText, false);
        if (context.HashProbe.exists && (context.Mapping.directoryEntries > 0 || context.Mapping.fileNameEntries > 0))
        {
            wchar_t buffer[256]{};
            ::swprintf_s(buffer, L"已生成（目录 %s 条 / 文件 %s 条）",
                         FormatCount(context.Mapping.directoryEntries).c_str(),
                         FormatCount(context.Mapping.fileNameEntries).c_str());
            AppendRun(runs, buffer, kColorOk, true);
        }
        else if (context.HashProbe.exists)
        {
            AppendRun(runs, L"目录存在，但还没有可用映射", kColorWarn, true);
        }
        else
        {
            AppendRun(runs, L"未生成", kColorWarn, true);
        }
        AppendRun(runs, L"\r\n", kColorText, false);

        // 3. 资源名还原
        // 这里只看"输出目录/报告在不在"，具体数字去 RestoreReport.txt 里看 ——
        // 还原跑在独立窗口里，主界面拿不到也不必维护一份自己的统计。
        AppendRun(runs, L"3. 资源名还原：", kColorText, false);
        if (context.RestoreProbe.exists)
        {
            AppendRun(runs, DescribeProbe(context.RestoreProbe, L"", L"已生成"), kColorOk, true);
        }
        else
        {
            AppendRun(runs, L"未生成", kColorWarn, true);
        }
        AppendRun(runs, L"\r\n", kColorText, false);

        }

        if (context.RestoreWindow && !::IsWindow(context.RestoreWindow))
        {
            // 用户已经把还原窗口关掉了，清掉句柄免得后面一直拿到一个野值。
            context.RestoreWindow = nullptr;
        }
        else if (context.RestoreWindow)
        {
            // 还原窗口自己会写 RestoreReport.txt；主界面只负责显示"它在跑"。
            AppendRun(runs, L"还原窗口已打开，进度与任务状态在那边的列表里。\r\n", kColorAccent, true);
        }
        else
        {
            AppendRun(runs, L"下一步：" + NextStepAdvice(context) + L"\r\n", kColorMuted, false);
        }
        AppendRun(runs, L"\r\n", kColorText, false);

        AppendSectionTitle(runs, L"还原结果");
        const bool reportExists = !context.Paths.RestoreReport.empty() &&
                                  ::GetFileAttributesW(context.Paths.RestoreReport.c_str()) != INVALID_FILE_ATTRIBUTES;
        AppendRun(runs, L"还原报告：", kColorText, false);
        if (reportExists)
        {
            const std::wstring stamp = FileTimestampText(context.Paths.RestoreReport);
            AppendRun(runs, L"已生成" + (stamp.empty() ? std::wstring() : L"（" + stamp + L"）"), kColorOk, true);
        }
        else
        {
            AppendRun(runs, L"未生成", kColorWarn, true);
        }
        AppendRun(runs, L"\r\n", kColorText, false);
        AppendRun(runs,
                  L"位置：" +
                      (context.Paths.RestoreReport.empty() ? std::wstring(L"（未选择目标游戏）")
                                                          : context.Paths.RestoreReport) +
                      L"\r\n\r\n",
                  kColorMuted, false);

        AppendSectionTitle(runs, L"关键目录");
        if (context.Paths.GameDir.empty())
        {
            AppendRun(runs, L"（未选择目标游戏）\r\n", kColorMuted, false);
        }
        else if (context.Mode == ActiveMode::SetExtract)
        {
            // 模式1 的关键目录含 StaticHash_Output（设计稿 §8.3）；
            // 它不读 StringHashDumper_Output，列出来只会让人以为要用动态映射。
            AppendRun(runs, context.StaticHashDir + L"\r\n", kColorMuted, false);
            AppendRun(runs, context.Paths.ExtractDir + L"\r\n", kColorMuted, false);
            AppendRun(runs, context.Paths.RestoreDir + L"\r\n", kColorMuted, false);
        }
        else
        {
            AppendRun(runs, context.Paths.ExtractDir + L"\r\n", kColorMuted, false);
            AppendRun(runs, context.Paths.HashDir + L"\r\n", kColorMuted, false);
            AppendRun(runs, context.Paths.RestoreDir + L"\r\n", kColorMuted, false);
        }
        return runs;
    }

    // 运行时读自身 exe 算出来的构建指纹。
    //
    // 这台机器上同时躺着好几个同名构建（Release、临时目录下的抓图副本、验证脚手架的副本），
    // 光看标题根本分不清正在看的是哪一个 —— 排查界面残留时就踩过这个坑。
    //
    // **每个进程只读一次**（下面那个 static 缓存）：指纹回答的是"正在跑的这个进程是哪个构建"，
    // 而运行中的 exe 在 Windows 上被占用、不会被换掉，所以缓存不会说谎，同时省掉每次打开
    //「关于」页重读一遍 1 MB 再算 sha256。哪天要支持"不重启就换构建"（自更新），
    // 把 static 去掉即可，其余不用动。
    struct BuildFingerprint
    {
        std::wstring FileName;
        unsigned long long Size = 0;
        std::wstring Modified; // 本地时间的 YYYY-MM-DD HH:MM:SS
        std::wstring Sha256;   // 大写 hex；自身文件读不到时为空串
    };

    const BuildFingerprint& SelfFingerprint()
    {
        static const BuildFingerprint cached = []() -> BuildFingerprint {
            BuildFingerprint info;
            wchar_t path[MAX_PATH]{};
            if (::GetModuleFileNameW(nullptr, path, _countof(path)) == 0)
            {
                return info;
            }
            const std::wstring self = path;
            const size_t slash = self.find_last_of(L"\\/");
            info.FileName = slash == std::wstring::npos ? self : self.substr(slash + 1);

            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (::GetFileAttributesExW(self.c_str(), GetFileExInfoStandard, &attributes))
            {
                info.Size = (static_cast<unsigned long long>(attributes.nFileSizeHigh) << 32) |
                            static_cast<unsigned long long>(attributes.nFileSizeLow);
                SYSTEMTIME utc{}, local{};
                if (::FileTimeToSystemTime(&attributes.ftLastWriteTime, &utc) &&
                    ::SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
                {
                    info.Modified = Host::Format(L"%04d-%02d-%02d %02d:%02d:%02d", local.wYear, local.wMonth,
                                                 local.wDay, local.wHour, local.wMinute, local.wSecond);
                }
            }
            info.Sha256 = ExtensionSet::Sha256HexFile(self);
            return info;
        }();
        return cached;
    }

    // 标题栏只放 8 位：够区分构建，又不至于把标题挤满。
    std::wstring ShortFingerprint()
    {
        const std::wstring& sha256 = SelfFingerprint().Sha256;
        return sha256.size() >= 8 ? sha256.substr(0, 8) : std::wstring(L"--------");
    }

    std::wstring BuildCaption(const std::wstring& exePath)
    {
        std::wstring caption = std::wstring(Host::kProductName) + L" " + Host::kProductVersion;
        caption += L" [" + ShortFingerprint() + L"]";
        if (!exePath.empty())
        {
            const size_t slash = exePath.find_last_of(L"\\/");
            caption += L" - ";
            caption += slash == std::wstring::npos ? exePath : exePath.substr(slash + 1);
        }
        return caption;
    }

    std::vector<TextRun> BuildAboutRuns(const UiContext& context)
    {
        std::vector<TextRun> runs;
        AppendSectionTitle(runs, L"关于 YuaExtract");
        AppendRun(runs, std::wstring(Host::kProductName) + L" " + Host::kProductVersion + L"\r\n\r\n",
                  kColorText, true, 11);

        AppendRun(runs, L"面向 Wamsoft / KiriKiri Z Hxv4 2021.11+ / CxdecV2 系列加密游戏的动态分析与资源整理工具。"
                        L"本版本在上游 KrkrExtractForCxdecV2Extra 基础上重做界面，新增"
                        L"「解包 → 动态 Hash 收集 → 离线还原资源名」三步流程。\r\n\r\n",
                  kColorText, false);

        AppendSectionTitle(runs, L"功能模块");
        // 用分隔线而不是空格对齐：面板是比例字体，拿空格凑列宽只会歪。
        AppendRun(runs, L"CxdecExtractor.dll　—　XP3 动态解包核心\r\n", kColorText, false);
        AppendRun(runs, L"CxdecExtractorUI.dll　—　XP3 批量解包界面\r\n", kColorText, false);
        AppendRun(runs, L"CxdecStringDumper.dll　—　运行时 Hash 映射收集\r\n", kColorText, false);
        AppendRun(runs, L"CxdecHashRestore.dll　—　Hook 撞库补充映射\r\n", kColorText, false);
        AppendRun(runs, L"CxdecKeyStatic.dll　—　静态 Key 提取\r\n", kColorText, false);
        AppendRun(runs, L"CxdecRepacker.dll　—　目录重封包 XP3\r\n\r\n", kColorText, false);

        AppendSectionTitle(runs, L"目录约定");
        AppendRun(runs, L"工具目录： " + Host::LoaderDirectory() + L"\r\n", kColorMuted, false);
        AppendRun(runs, L"日志目录： " + Host::LogDirectory() + L"\r\n", kColorMuted, false);
        AppendRun(runs, L"模块目录： " + JoinPath(Host::LoaderDirectory(), L"CxdecExtractordll") + L"\r\n\r\n",
                  kColorMuted, false);

        AppendSectionTitle(runs, L"构建指纹");
        // 只报"我读到的这个 exe"，不做任何推断：对不上就是拿错了构建。
        const BuildFingerprint& fingerprint = SelfFingerprint();
        AppendRun(runs, L"程序文件： " + fingerprint.FileName + L"\r\n", kColorMuted, false);
        AppendRun(runs, L"文件大小： " + Host::Format(L"%llu", fingerprint.Size) + L" 字节\r\n", kColorMuted,
                  false);
        AppendRun(runs, L"修改时间： " + (fingerprint.Modified.empty() ? std::wstring(L"（读不到）")
                                                                      : fingerprint.Modified) + L"\r\n",
                  kColorMuted, false);
        AppendRun(runs, L"SHA-256： " +
                            (fingerprint.Sha256.empty() ? std::wstring(L"（读不到）")
                                                        : fingerprint.Sha256.substr(0, 16)) +
                            L"（前 16 位，标题栏取前 8 位）\r\n\r\n",
                  kColorMuted, false);

        AppendSectionTitle(runs, L"扩展集");
        AppendRun(runs, L"「制作扩展集」页签已接入：可把当前游戏的「名字 ↔ 哈希」成果生成扩展集档案"
                        L"（rules.int）、编辑档案信息、只读干跑测试、安装进扩展集库。\r\n"
                        L"首页「该作扩展集提取」用扩展集离线展开静态 Hash 表，"
                        L"同一部游戏重解包时不必再跑游戏做动态采集。\r\n"
                        L"「扩展集」页签已接入：浏览扩展集库、打开选中扩展集、"
                        L"从远端仓库在线更新、运行环境自检、查看档案留言。\r\n\r\n",
                  kColorText, false);

        AppendSectionTitle(runs, L"免责声明");
        AppendRun(runs, L"仅用于学习研究、兼容性分析、个人合法备份、汉化与资源修复等场景。"
                        L"请在你合法拥有的游戏副本上使用，不要用于传播游戏资源。\r\n",
                  kColorText, false);
        return runs;
    }

    // ---------- 「扩展集」页签：库里选中项的小工具 ----------

    // 摘要只显示前 16 位：够区分两份档案，又不至于把面板挤满（标题栏的构建指纹是同一取舍）。
    std::wstring ShortDigest(const std::wstring& digest)
    {
        if (digest.empty())
        {
            return L"（未记录）";
        }
        return digest.size() > 16u ? digest.substr(0u, 16u) + L"…" : digest;
    }

    const ExtensionSet::LibraryEntry* FindLibraryEntry(const ExtensionSet::LibraryCatalog& catalog,
                                                      const std::wstring& publisher,
                                                      const std::wstring& title)
    {
        for (const ExtensionSet::LibraryEntry& entry : catalog.entries)
        {
            if (entry.publisher == publisher && entry.title == title)
            {
                return &entry;
            }
        }
        return nullptr;
    }

    // 选中项在库清单里的那一条。清单里可能没有（内核会把"目录里真有 rules.int 但清单没记"的
    // 补列出来，那种条目的摘要字段是空的），所以"找不到"不等于出错，只是没有清单信息可显示。
    const ExtensionSet::LibraryEntry* FindCatalogEntry(const UiContext& context)
    {
        if (context.SelectedPublisher.empty() || context.SelectedTitle.empty())
        {
            return nullptr;
        }
        return FindLibraryEntry(context.Catalog, context.SelectedPublisher, context.SelectedTitle);
    }

    // 会社 / 作品 → 库里**实际用的**两级目录（sanitize 之后的形态）。
    //
    // 界面上的原始名与磁盘上的目录名可以不一样：`..\..\Windows\Temp` 落库是 `WindowsTemp`，
    // `a:b` 落库是 `ab`，`  padded  ` 落库是 `padded`（MakeSafePathComponent 去掉 Windows
    // 目录名非法字符与首尾空格 / 点）。所以**任何"东西落在哪儿"的显示都必须用这一份**，
    // 拿原始名去拼路径，用户照着去库里找是找不到的（验证 P-4）。
    // 任一分量 sanitize 后为空 = 这条根本做不出目录名，返回空串，由调用方明确说出来。
    std::wstring LibraryDirectoryFor(const std::wstring& libraryRoot, const std::wstring& publisher,
                                     const std::wstring& title)
    {
        const std::wstring publisherDirectory = ExtensionSet::MakeSafePathComponent(publisher);
        const std::wstring titleDirectory = ExtensionSet::MakeSafePathComponent(title);
        if (libraryRoot.empty() || publisherDirectory.empty() || titleDirectory.empty())
        {
            return std::wstring();
        }
        return JoinPath(JoinPath(libraryRoot, publisherDirectory), titleDirectory);
    }

    // 选中扩展集在库里的目录：<库根>\<会社>\<作品>（sanitize 之后的形态）。
    // 目录可能是手工放进库里的，名字含非法字符时宁可拼不出来，也不要拼到库外面去。
    std::wstring SelectedLibraryDirectory(const UiContext& context)
    {
        if (context.SelectedPublisher.empty() || context.SelectedTitle.empty())
        {
            return std::wstring();
        }
        return LibraryDirectoryFor(context.LibraryRoot, context.SelectedPublisher, context.SelectedTitle);
    }

    // 选中扩展集的档案路径：优先用清单解析出来的那份（本地清单与在线索引共用同一套口径），
    // 没有再按库目录推导。
    std::wstring SelectedArchivePath(const UiContext& context)
    {
        if (const ExtensionSet::LibraryEntry* entry = FindCatalogEntry(context))
        {
            if (!entry->archivePath.empty())
            {
                return entry->archivePath;
            }
        }
        const std::wstring directory = SelectedLibraryDirectory(context);
        return directory.empty() ? std::wstring() : JoinPath(directory, ExtensionSet::kArchiveFileName);
    }

    // ---------- 自检结论的三种落法（面板 / 纯文本 / 日志） ----------

    unsigned int SelfCheckProblemCount(const std::vector<SelfCheckItem>& items)
    {
        unsigned int problems = 0u;
        for (const SelfCheckItem& item : items)
        {
            if (item.Severity == 1)
            {
                ++problems;
            }
        }
        return problems;
    }

    // 总结论那半句：「全部正常」或「N 项问题」。
    std::wstring SelfCheckSummary(const std::vector<SelfCheckItem>& items)
    {
        const unsigned int problems = SelfCheckProblemCount(items);
        return problems == 0u ? std::wstring(L"全部正常") : std::to_wstring(problems) + L" 项问题";
    }

    // 面板版：逐项一行，问题项标红加粗（失败项要点名，不能只给个总数）。
    void AppendSelfCheckRuns(std::vector<TextRun>& runs, const std::vector<SelfCheckItem>& items)
    {
        const unsigned int problems = SelfCheckProblemCount(items);
        AppendRun(runs, L"结论：" + SelfCheckSummary(items) + L"\r\n", problems == 0u ? kColorOk : kColorWarn, true);
        for (const SelfCheckItem& item : items)
        {
            AppendRun(runs, L"· " + item.Label + L"：", kColorText, false);
            AppendRun(runs, item.Detail + L"\r\n",
                      item.Severity == 1 ? kColorWarn : (item.Severity == 0 ? kColorOk : kColorMuted),
                      item.Severity == 1);
        }
    }

    // 纯文本版（弹框与日志用）。
    std::wstring SelfCheckPlainText(const std::vector<SelfCheckItem>& items)
    {
        std::wstring text = L"运行环境自检：" + SelfCheckSummary(items) + L"\r\n\r\n";
        for (const SelfCheckItem& item : items)
        {
            text += L"· " + item.Label + L"：" + item.Detail + L"\r\n";
        }
        return text;
    }

    // ---------- 「扩展集」页签右栏：扩展集详情（设计稿 §8.5） ----------
    //
    // 这一栏**不读档案本体**：面板会被反复重绘（切页签、改窗口大小、定时器），每次解一份
    // 0.25 MB 的档案没有意义。这里显示的全是库清单里现成的东西（条数、摘要、时间、来源），
    // META 细节由「扩展集留言」按钮按需读。
    std::vector<TextRun> BuildSetDetailRuns(UiContext& context)
    {
        std::vector<TextRun> runs;

        AppendSectionTitle(runs, L"扩展集详情");
        AppendRun(runs,
                  L"「扩展集」页签管的是库里的扩展集：浏览、打开、在线更新、运行环境自检、看档案留言。"
                  L"要用扩展集跑还原，去首页选「该作扩展集提取」。\r\n\r\n",
                  kColorMuted, false);

        AppendSectionTitle(runs, L"扩展集库");
        AppendRun(runs,
                  L"库根：" + (context.LibraryRoot.empty() ? std::wstring(L"（未解析）") : context.LibraryRoot) +
                      L"\r\n",
                  kColorMuted, false);
        if (!context.Catalog.ok)
        {
            AppendRun(runs,
                      L"清单不可用：" +
                          (context.Catalog.error.empty() ? std::wstring(L"（内核没有给出原因）")
                                                         : context.Catalog.error) +
                          L"\r\n",
                      kColorWarn, true);
        }
        else if (context.Catalog.entries.empty())
        {
            AppendRun(runs,
                      L"库里一份扩展集都没有。两条来路：\r\n"
                      L"· 「制作扩展集」页签生成当前游戏的档案，再「安装到扩展集库」；\r\n"
                      L"· 本页签「在线更新」从远端仓库下载安装。\r\n",
                      kColorWarn, false);
        }
        else
        {
            AppendRun(runs,
                      L"共 " + FormatCount(context.Catalog.publishers.size()) + L" 个会社 / " +
                          FormatCount(context.Catalog.entries.size()) + L" 份扩展集\r\n",
                      kColorText, false);
        }
        if (context.Catalog.ok && !context.Catalog.error.empty())
        {
            // 解析成功但清单里有不合规被跳过的行 —— 不是失败，但**不能静默**（2026-10-15 补）。
            AppendRun(runs, L"清单提示：" + context.Catalog.error, kColorWarn, false);
        }
        AppendRun(runs, L"\r\n", kColorText, false);

        AppendSectionTitle(runs, L"当前选择");
        if (context.SelectedPublisher.empty())
        {
            AppendRun(runs, L"还没有选会社。请在左侧「扩展集选择」里先选会社，再选作品。\r\n", kColorWarn, false);
        }
        else if (context.SelectedTitle.empty())
        {
            AppendRun(runs, L"会社：" + context.SelectedPublisher + L"\r\n", kColorText, false);
            AppendRun(runs, L"再选一个该会社下的作品（扩展集）。\r\n", kColorWarn, false);
        }
        else
        {
            AppendRun(runs, L"会社：" + context.SelectedPublisher + L"\r\n", kColorText, false);
            AppendRun(runs, L"作品：" + context.SelectedTitle + L"\r\n", kColorText, false);

            // 「库内目录」给的是**实际落库目录**（sanitize 之后的形态），不是原始名的拼接。
            // 名字里带 \ / : * ? " < > | 之类字符时两者不同，这一行必须让用户能照着去库里找（验证 P-4）。
            const std::wstring publisherDirectory = ExtensionSet::MakeSafePathComponent(context.SelectedPublisher);
            const std::wstring titleDirectory = ExtensionSet::MakeSafePathComponent(context.SelectedTitle);
            const std::wstring directory =
                LibraryDirectoryFor(context.LibraryRoot, context.SelectedPublisher, context.SelectedTitle);
            if (directory.empty())
            {
                AppendRun(runs,
                          L"库内目录：这条做不出库目录名 —— " +
                              std::wstring(publisherDirectory.empty() ? L"会社名" : L"作品名") +
                              L"里全是 Windows 目录名不能用的字符（\\ / : * ? \" < > | 与控制字符），"
                              L"装不进扩展集库。\r\n",
                          kColorWarn, true);
            }
            else
            {
                AppendRun(runs, L"库内目录：" + directory + L"\r\n", kColorMuted, false);
                if (publisherDirectory != context.SelectedPublisher || titleDirectory != context.SelectedTitle)
                {
                    AppendRun(runs,
                              L"（库里的目录名是改写过的：名字里的 \\ / : * ? \" < > | 之类字符被去掉了 —— "
                              L"实际目录就是上面那一串，按它去库里找。）\r\n",
                              kColorWarn, false);
                }
            }

            const std::wstring archive = SelectedArchivePath(context);
            if (FileExistsAt(archive))
            {
                AppendRun(runs,
                          L"档案：" + archive + L"（" + FormatSize(FileSizeBytes(archive)) + L"）\r\n",
                          kColorMuted, false);
            }
            else
            {
                AppendRun(runs,
                          L"档案：" + (archive.empty() ? std::wstring(L"（路径未知）") : archive) +
                              L"（不存在）\r\n",
                          kColorWarn, true);
            }

            if (const ExtensionSet::LibraryEntry* entry = FindCatalogEntry(context))
            {
                AppendRun(runs, L"条数：" + FormatCount(entry->entryCount) + L" 条\r\n", kColorText, false);
                AppendRun(runs, L"目标 exe 摘要：" + ShortDigest(entry->exeDigest) + L"\r\n", kColorMuted, false);
                if (!entry->siblingDigest.empty())
                {
                    AppendRun(runs, L"兄弟 exe 摘要：" + ShortDigest(entry->siblingDigest) + L"\r\n", kColorMuted,
                              false);
                }
                AppendRun(runs, L"盐摘要：" + ShortDigest(entry->saltDigest) + L"\r\n", kColorMuted, false);
                AppendRun(runs, L"档案摘要：" + ShortDigest(entry->archiveDigest) + L"\r\n", kColorMuted, false);
                AppendRun(runs,
                          L"生成时间：" +
                              (entry->generatedAt.empty() ? std::wstring(L"（未记录）") : entry->generatedAt) +
                              L"\r\n",
                          kColorMuted, false);
                AppendRun(runs,
                          L"安装时间：" +
                              (entry->installedAt.empty() ? std::wstring(L"（未记录）") : entry->installedAt) +
                              L"\r\n",
                          kColorMuted, false);
                AppendRun(runs,
                          L"来源：" + (entry->source.empty() ? std::wstring(L"（未记录）") : entry->source) +
                              L"\r\n",
                          kColorMuted, false);
            }
            else
            {
                AppendRun(runs,
                          L"库清单里没有这一条（目录里真有 rules.int，但清单没记）。"
                          L"重新走一次安装会把清单补上。\r\n",
                          kColorWarn, true);
            }
        }
        AppendRun(runs, L"\r\n", kColorText, false);

        AppendSectionTitle(runs, L"名字对不对，本工具验不了");
        AppendRun(runs,
                  L"档案里只存「名字」和它的落档信息，不含哈希层，所以拿到一份别人给的档案时，"
                  L"本工具没法验证里面的名字对不对（设计文档 §3.1 / §14.4）。\r\n",
                  kColorMuted, false);
        AppendRun(runs,
                  L"名字的最终把关是事实：把它用在本作上还原，还原率不对就是不对。"
                  L"「在线更新」能保证的只是「下到的字节等于索引里那一条，且档案自洽、没被截断」。\r\n",
                  kColorMuted, false);
        AppendRun(runs, L"\r\n", kColorText, false);

        AppendSectionTitle(runs, L"最近操作");
        if (context.SetPageNote.empty())
        {
            AppendRun(runs, L"（本页签还没有执行过操作）\r\n", kColorMuted, false);
        }
        else
        {
            AppendRun(runs, context.SetPageNote + L"\r\n", kColorAccent, true);
        }
        AppendRun(runs, L"\r\n", kColorText, false);

        AppendSectionTitle(runs, L"运行环境自检");
        if (!context.HasSelfCheck)
        {
            // 措辞不能写"只读"：第 1 项（工具目录可写）与第 3 项（库根可写）会往那个目录里
            // 写一个临时文件再删掉。事实是"不留下改动"，不是"一个字节都没写"。
            AppendRun(runs,
                      L"还没跑过。「运行环境自检」只做检查、不在任何地方留下改动：工具目录能不能写、"
                      L"Hash 收集模块是不是 32 位、"
                      L"库与清单在不在、每份档案能不能解码且自洽、盐日志读不读得到。\r\n",
                      kColorMuted, false);
        }
        else
        {
            AppendSelfCheckRuns(runs, context.SelfCheck);
        }
        return runs;
    }

    // 档案是否已经按「会社 / 作品」落在扩展集库里（只看库内真身 rules.int 在不在）。
    bool InstalledInLibrary(const UiContext& context)
    {
        const std::wstring publisher = ExtensionSet::MakeSafePathComponent(context.Meta.publisher);
        const std::wstring title = ExtensionSet::MakeSafePathComponent(context.Meta.title);
        if (publisher.empty() || title.empty() || context.LibraryRoot.empty())
        {
            return false;
        }
        const std::wstring directory = JoinPath(JoinPath(context.LibraryRoot, publisher), title);
        return FileExistsAt(JoinPath(directory, ExtensionSet::kArchiveFileName));
    }

    // 「制作扩展集」页签右侧的「制作状态」（设计稿 §8.1）。
    std::vector<TextRun> BuildMakeSetRuns(UiContext& context)
    {
        std::vector<TextRun> runs;

        AppendSectionTitle(runs, L"功能介绍");
        AppendRun(runs,
                  L"把当前游戏已经采集/还原出来的「名字 ↔ 哈希」成果整理成一份扩展集档案（rules.int），"
                  L"留档之后可以安装进扩展集库；以后同一部游戏重解包时，首页「该作扩展集提取」"
                  L"直接用这份档案离线展开静态 Hash 表，不必再跑一遍游戏做动态采集。\r\n"
                  L"档案只含名字与哈希，不含任何资源本体。\r\n",
                  kColorText, false);
        AppendRun(runs,
                  L"采集来源目录：" + EffectiveSourceDirectory(context) +
                      (context.SourceOverride.empty() ? L"（默认）" : L"（已覆盖）") + L"\r\n",
                  kColorMuted, false);
        AppendRun(runs,
                  L"测试来源目录：" + EffectiveTestDirectory(context) +
                      (context.TestSourceOverride.empty() ? L"（跟随采集来源）" : L"（已覆盖）") + L"\r\n",
                  kColorMuted, false);
        AppendRun(runs,
                  L"档案路径：" +
                      (context.DraftArchive.empty() ? std::wstring(L"（未选择目标游戏）") : context.DraftArchive) +
                      L"\r\n",
                  context.DraftArchive.empty() ? kColorWarn : kColorMuted, false);
        if (!context.MakeNote.empty())
        {
            AppendRun(runs, L"最近操作：" + context.MakeNote + L"\r\n", kColorAccent, true);
        }
        if (context.HasGenerate && !context.LastGenerateNote.empty())
        {
            AppendRun(runs, context.LastGenerateNote + L"\r\n", kColorMuted, false);
        }
        AppendRun(runs, L"\r\n", kColorText, false);

        AppendSectionTitle(runs, L"测试结果（仅供参考）");
        if (context.HasDryRun)
        {
            AppendRun(runs, context.LastTestNote + L"\r\n", kColorText, false);
        }
        else
        {
            AppendRun(runs,
                      L"还没有跑过干跑测试。「测试当前扩展集」是只读推演："
                      L"不复制、不重命名、不新建任何文件，只统计这份档案能解出多少。\r\n",
                      kColorMuted, false);
        }
        AppendRun(runs, L"\r\n", kColorText, false);

        AppendSectionTitle(runs, L"下一步建议");
        if (context.ExePath.empty())
        {
            AppendRun(runs, L"先选择目标游戏主程序（.exe）。\r\n", kColorWarn, false);
        }
        else if (!context.HasGenerate)
        {
            AppendRun(runs,
                      L"先点「生成当前游戏扩展集」——它合并 StringHashDumper_Output 下的采集与还原成果，"
                      L"逐条用「名字 + 盐」重算校验后写进档案；重算不回去的进例外表并如实报数。"
                      L"生成前记得先用「编辑当前扩展集信息」填好会社与游戏名称（会社是库路径第一级，工具不猜）。\r\n",
                      kColorText, false);
        }
        else if (!InstalledInLibrary(context))
        {
            AppendRun(runs, L"档案已生成，可以点「安装到扩展集库」留档；装好后首页「该作扩展集提取」就能选到它。\r\n",
                      kColorText, false);
        }
        else
        {
            AppendRun(runs,
                      L"已安装进扩展集库。回到首页选「该作扩展集提取」，在「扩展集选择」里选好会社与作品，"
                      L"按三步走即可（全程不启动游戏做动态采集）。\r\n",
                      kColorText, false);
        }
        AppendRun(runs, L"\r\n", kColorText, false);

        AppendSectionTitle(runs, L"安装位置");
        AppendRun(runs,
                  L"扩展集库：" + (context.LibraryRoot.empty() ? std::wstring(L"（未解析）") : context.LibraryRoot) +
                      L"\r\n",
                  kColorMuted, false);
        AppendRun(runs,
                  L"当前扩展集：" +
                      (context.DraftArchive.empty() ? std::wstring(L"（未选择目标游戏）") : context.DraftArchive) +
                      L"\r\n",
                  kColorMuted, false);
        {
            const std::wstring publisher = ExtensionSet::MakeSafePathComponent(context.Meta.publisher);
            const std::wstring title = ExtensionSet::MakeSafePathComponent(context.Meta.title);
            if (!publisher.empty() && !title.empty() && !context.LibraryRoot.empty())
            {
                AppendRun(runs,
                          L"库内档案：" + JoinPath(JoinPath(context.LibraryRoot, publisher), title) + L"\r\n",
                          kColorMuted, false);
            }
            else
            {
                AppendRun(runs, L"库内档案：（先在「编辑当前扩展集信息」里填好会社与游戏名称）\r\n", kColorMuted, false);
            }
        }
        return runs;
    }

    // ---------- 状态刷新 ----------

    void SyncTargetFromEdit(UiContext& context)
    {
        wchar_t buffer[MAX_PATH * 2]{};
        ::GetWindowTextW(::GetDlgItem(context.Window, IDC_YUA_EXE_EDIT), buffer, _countof(buffer));
        const std::wstring text = buffer;
        if (text != context.ExePath)
        {
            context.ExePath = text;
            context.Paths = BuildWorkspace(text);
            RefreshSetPaths(context);
            Host::SetTargetExe(text);
        }
    }

    void RefreshProbes(UiContext& context, bool deep)
    {
        if (context.Paths.GameDir.empty())
        {
            context.ExtractProbe = DirProbe{};
            context.HashProbe = DirProbe{};
            context.RestoreProbe = DirProbe{};
            context.Mapping = MappingSummary{};
            context.ProbedDeep = false;
            return;
        }

        context.ExtractProbe = ProbeDirectory(context.Paths.ExtractDir, deep);
        context.HashProbe = ProbeDirectory(context.Paths.HashDir, deep);
        context.RestoreProbe = ProbeDirectory(context.Paths.RestoreDir, deep);
        if (deep)
        {
            context.Mapping = LoadMappingSummary(context.Paths.HashDir);
        }
        context.ProbedDeep = deep;
    }

    // 只做"目录在不在"的便宜检查，给定时器用。
    bool CheapStateChanged(const UiContext& context)
    {
        if (context.Paths.GameDir.empty())
        {
            return false;
        }
        const bool extract = ::GetFileAttributesW(context.Paths.ExtractDir.c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool hash = ::GetFileAttributesW(context.Paths.HashDir.c_str()) != INVALID_FILE_ATTRIBUTES;
        const bool restore = ::GetFileAttributesW(context.Paths.RestoreDir.c_str()) != INVALID_FILE_ATTRIBUTES;
        return extract != context.ExtractProbe.exists || hash != context.HashProbe.exists ||
               restore != context.RestoreProbe.exists;
    }

    // 切页签时统一决定谁可见：首页 / 制作页签各有一组控件，目标游戏与任务进度两行两边共用。
    // 可见性必须最后统一决定：布局里的 MoveControl 都带 SWP_SHOWWINDOW，
    // 中途 ShowWindow(SW_HIDE) 会被随后的布局重新显示出来。
    void ShowTabControls(UiContext& context)
    {
        const bool home = context.ActiveTab == IDC_YUA_TAB_HOME;
        const bool make = context.ActiveTab == IDC_YUA_TAB_MAKE_SET;
        const bool set = context.ActiveTab == IDC_YUA_TAB_SET;
        for (HWND control : context.HomeControls)
        {
            ::ShowWindow(control, home ? SW_SHOW : SW_HIDE);
        }
        for (HWND control : context.MakeControls)
        {
            ::ShowWindow(control, make ? SW_SHOW : SW_HIDE);
        }
        for (HWND control : context.SetPageControls)
        {
            ::ShowWindow(control, set ? SW_SHOW : SW_HIDE);
        }
        // 「扩展集选择」这一组是首页与「扩展集」页签共用的（设计稿 §8.5）：
        // 两个页签操作的是同一份清单与同一份选中状态，所以是同一组控件、只是摆放位置不同。
        for (HWND control : context.LibControls)
        {
            ::ShowWindow(control, (home || set) ? SW_SHOW : SW_HIDE);
        }
        for (HWND control : context.TargetControls)
        {
            ::ShowWindow(control, (home || make) ? SW_SHOW : SW_HIDE);
        }
        for (HWND control : context.TaskControls)
        {
            ::ShowWindow(control, (home || make) ? SW_SHOW : SW_HIDE);
        }
    }

    void UpdateOverview(UiContext& context)
    {
        std::vector<TextRun> runs;
        switch (context.ActiveTab)
        {
            case IDC_YUA_TAB_MAKE_SET:
                runs = BuildMakeSetRuns(context);
                break;
            case IDC_YUA_TAB_SET:
                runs = BuildSetDetailRuns(context);
                break;
            case IDC_YUA_TAB_ABOUT:
                runs = BuildAboutRuns(context);
                break;
            default:
                runs = BuildOverviewRuns(context);
                break;
        }

        std::wstring plain;
        plain.reserve(4096);
        for (const TextRun& run : runs)
        {
            plain += run.Text;
        }
        if (plain == context.RenderedOverview)
        {
            return;
        }
        context.RenderedOverview = plain;

        HWND view = nullptr;
        if (context.ActiveTab == IDC_YUA_TAB_HOME)
        {
            view = ::GetDlgItem(context.Window, IDC_YUA_OVERVIEW);
        }
        else if (context.ActiveTab == IDC_YUA_TAB_MAKE_SET)
        {
            view = ::GetDlgItem(context.Window, IDC_YUA_SET_STATUS);
        }
        else
        {
            view = ::GetDlgItem(context.Window, IDC_YUA_PAGE);
        }
        SetViewRuns(view, &context, runs, false);
    }

    void UpdateLogView(UiContext& context)
    {
        if (!context.LogVisible)
        {
            return;
        }
        std::wstring path = JoinPath(Host::LogDirectory(), Host::kLogFileName);
        std::wstring tail = ReadTextFileTail(path, 400u);
        if (tail.empty())
        {
            tail = L"（日志为空或尚未生成：" + path + L"）";
        }
        std::vector<TextRun> runs;
        AppendRun(runs, tail, kColorLogText, false);
        HWND view = ::GetDlgItem(context.Window, IDC_YUA_LOGVIEW);
        SetViewRuns(view, &context, runs, true);
    }

    void RefreshAll(UiContext& context, bool deep)
    {
        SyncTargetFromEdit(context);
        RefreshProbes(context, deep);
        UpdateOverview(context);
        UpdateLogView(context);
    }

    // ---------- 布局 ----------

    void MoveControl(HWND window, int id, int x, int y, int width, int height, bool visible = true)
    {
        HWND control = ::GetDlgItem(window, id);
        if (!control)
        {
            return;
        }
        ::SetWindowPos(control, nullptr, x, y, width, height,
                       SWP_NOZORDER | SWP_NOACTIVATE | (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }

    // 「任务进度」一行：首页与制作页签共用同一组控件（不做两份进度）。
    void LayoutTaskRow(UiContext& context, int leftWidth, int y)
    {
        HWND window = context.Window;
        const int innerWidth = leftWidth - 24;
        const int cancelWidth = 66;
        MoveControl(window, IDC_YUA_GROUP_TASK, kMargin, y, leftWidth, kTaskGroupHeight);
        MoveControl(window, IDC_YUA_TASK_PROGRESS, kMargin + 12, y + 24, innerWidth - cancelWidth - kRowGap, 18);
        MoveControl(window, IDC_YUA_TASK_CANCEL, kMargin + 12 + innerWidth - cancelWidth, y + 21, cancelWidth, 24);
        MoveControl(window, IDC_YUA_TASK_TEXT, kMargin + 12, y + 46, innerWidth, 18);
    }

    // 首页左列：首页功能（模式选择）→ 扩展集选择 → 高级操作 → 主流程 → 任务进度 → 辅助操作。
    void LayoutHomeColumn(UiContext& context, int leftWidth, int y, int contentBottom)
    {
        HWND window = context.Window;

        const int funcGroupHeight = 74;
        MoveControl(window, IDC_YUA_GROUP_FUNC, kMargin, y, leftWidth, funcGroupHeight);
        {
            const int innerWidth = leftWidth - 24;
            const int buttonWidth = (innerWidth - 2 * kRowGap) / 3;
            const int ids[] = { IDC_YUA_FUNC_SET_EXTRACT, IDC_YUA_FUNC_SET_CRACK, IDC_YUA_FUNC_CLASSIC };
            for (int index = 0; index < 3; ++index)
            {
                MoveControl(window, ids[index], kMargin + 12 + index * (buttonWidth + kRowGap), y + 28,
                            buttonWidth, 30);
            }
        }
        y += funcGroupHeight + 8;

        // 扩展集选择（设计稿 §8.3 图3 左列）：会社 → 作品两级列表。
        const int libGroupHeight = 116;
        MoveControl(window, IDC_YUA_GROUP_LIB, kMargin, y, leftWidth, libGroupHeight);
        {
            const int innerWidth = leftWidth - 24;
            const int half = (innerWidth - kRowGap) / 2;
            const int listHeight = libGroupHeight - 50;
            MoveControl(window, IDC_YUA_LIB_PUB_LABEL, kMargin + 12, y + 22, half, 18);
            MoveControl(window, IDC_YUA_LIB_TITLE_LABEL, kMargin + 12 + half + kRowGap, y + 22, half, 18);
            MoveControl(window, IDC_YUA_LIB_PUBLISHERS, kMargin + 12, y + 42, half, listHeight);
            MoveControl(window, IDC_YUA_LIB_TITLES, kMargin + 12 + half + kRowGap, y + 42, half, listHeight);
        }
        y += libGroupHeight + 8;

        const int advGroupHeight = 68;
        MoveControl(window, IDC_YUA_GROUP_ADV, kMargin, y, leftWidth, advGroupHeight);
        {
            const int buttonWidth = static_cast<int>((leftWidth - 24) * 0.62f);
            MoveControl(window, IDC_YUA_ADVANCED, kMargin + (leftWidth - buttonWidth) / 2, y + 28, buttonWidth, 28);
        }
        y += advGroupHeight + 8;

        const int flowGroupHeight = 132;
        MoveControl(window, IDC_YUA_GROUP_FLOW, kMargin, y, leftWidth, flowGroupHeight);
        {
            const int ids[] = { IDC_YUA_STEP1, IDC_YUA_STEP2, IDC_YUA_STEP3 };
            for (int index = 0; index < 3; ++index)
            {
                MoveControl(window, ids[index], kMargin + 12, y + 26 + index * (kRowHeight + kRowGap),
                            leftWidth - 24, kRowHeight);
            }
        }
        y += flowGroupHeight + 8;

        LayoutTaskRow(context, leftWidth, y);
        y += kTaskGroupHeight + 8;

        const int auxHeight = contentBottom - y;
        MoveControl(window, IDC_YUA_GROUP_AUX, kMargin, y, leftWidth,
                    auxHeight > 90 ? auxHeight : 90);
        {
            const int innerWidth = leftWidth - 24;
            const int half = (innerWidth - kRowGap) / 2;
            int rowY = y + 26;
            MoveControl(window, IDC_YUA_AUX_CHECK, kMargin + 12, rowY, innerWidth, kRowHeight);
            rowY += kRowHeight + kRowGap;
            MoveControl(window, IDC_YUA_AUX_WORKSPACE, kMargin + 12, rowY, half, kRowHeight);
            MoveControl(window, IDC_YUA_AUX_CLEAN, kMargin + 12 + half + kRowGap, rowY, half, kRowHeight);
            rowY += kRowHeight + kRowGap;
            MoveControl(window, IDC_YUA_AUX_RESTOREDIR, kMargin + 12, rowY, half, kRowHeight);
            MoveControl(window, IDC_YUA_AUX_REPORT, kMargin + 12 + half + kRowGap, rowY, half, kRowHeight);

            MoveControl(window, IDC_YUA_HINT, kMargin + 12, rowY + kRowHeight + 4, innerWidth, 18);
        }
    }

    // 「制作扩展集」页签左列：制作扩展集 → 目录 → 自定义 → 任务进度（设计稿 §8.1）。
    void LayoutMakeSetColumn(UiContext& context, int leftWidth, int y)
    {
        HWND window = context.Window;
        const int innerWidth = leftWidth - 24;

        const int makeGroupHeight = 122;
        MoveControl(window, IDC_YUA_SET_GROUP_MAKE, kMargin, y, leftWidth, makeGroupHeight);
        {
            const int ids[] = { IDC_YUA_SET_GENERATE, IDC_YUA_SET_EDITINFO, IDC_YUA_SET_TEST };
            for (int index = 0; index < 3; ++index)
            {
                MoveControl(window, ids[index], kMargin + 12, y + 26 + index * (kRowHeight + kRowGap),
                            innerWidth, kRowHeight);
            }
        }
        y += makeGroupHeight + 8;

        const int dirGroupHeight = 122;
        MoveControl(window, IDC_YUA_SET_GROUP_DIR, kMargin, y, leftWidth, dirGroupHeight);
        {
            const int ids[] = { IDC_YUA_SET_OPEN_DRAFT, IDC_YUA_SET_INSTALL, IDC_YUA_SET_OPEN_LIB };
            for (int index = 0; index < 3; ++index)
            {
                MoveControl(window, ids[index], kMargin + 12, y + 26 + index * (kRowHeight + kRowGap),
                            innerWidth, kRowHeight);
            }
        }
        y += dirGroupHeight + 8;

        const int customGroupHeight = 90;
        MoveControl(window, IDC_YUA_SET_GROUP_CUSTOM, kMargin, y, leftWidth, customGroupHeight);
        {
            const int ids[] = { IDC_YUA_SET_PICK_SOURCE, IDC_YUA_SET_PICK_TEST };
            for (int index = 0; index < 2; ++index)
            {
                MoveControl(window, ids[index], kMargin + 12, y + 26 + index * (kRowHeight + kRowGap),
                            innerWidth, kRowHeight);
            }
        }
        y += customGroupHeight + 8;

        LayoutTaskRow(context, leftWidth, y);
    }

    // 「扩展集」页签左列：扩展集管理与浏览（两行三列）+ 会社/作品两个列表（设计稿 §8.5）。
    //
    // 这一列**不给任务进度与目标游戏留位置**：本页签做的事（浏览库、拉远端、自检）都不针对
    // 目标游戏、也不跑主界面的后台任务，摆两行用不上的控件只会把列表挤矮。
    // 列表占满剩余高度 —— 这一页的主动作就是在库里翻找。
    void LayoutSetColumn(UiContext& context, int leftWidth, int y, int contentBottom)
    {
        HWND window = context.Window;
        const int innerWidth = leftWidth - 24;

        const int browseGroupHeight = 96;
        MoveControl(window, IDC_YUA_SETM_GROUP_BROWSE, kMargin, y, leftWidth, browseGroupHeight);
        {
            const int buttonWidth = (innerWidth - 2 * kRowGap) / 3;
            const int ids[] = { IDC_YUA_SETM_REFRESH,      IDC_YUA_SETM_OPEN_ROOT, IDC_YUA_SETM_OPEN_ENTRY,
                                IDC_YUA_SETM_UPDATE,       IDC_YUA_SETM_SELFCHECK, IDC_YUA_SETM_NOTE };
            for (int index = 0; index < 6; ++index)
            {
                const int column = index % 3;
                const int row = index / 3;
                MoveControl(window, ids[index], kMargin + 12 + column * (buttonWidth + kRowGap),
                            y + 26 + row * (kRowHeight + kRowGap), buttonWidth, kRowHeight);
            }
        }
        y += browseGroupHeight + 8;

        // 会社 / 作品列表：与首页是同一组控件（LibControls），这里只换位置与高度。
        // 50 = 分组框标题（22）+ 标签行（20）+ 间距（8），与首页那处的算法一致。
        const int libGroupHeight = (contentBottom - y) > 130 ? (contentBottom - y) : 130;
        MoveControl(window, IDC_YUA_GROUP_LIB, kMargin, y, leftWidth, libGroupHeight);
        {
            const int half = (innerWidth - kRowGap) / 2;
            const int listHeight = libGroupHeight - 50;
            MoveControl(window, IDC_YUA_LIB_PUB_LABEL, kMargin + 12, y + 22, half, 18);
            MoveControl(window, IDC_YUA_LIB_TITLE_LABEL, kMargin + 12 + half + kRowGap, y + 22, half, 18);
            MoveControl(window, IDC_YUA_LIB_PUBLISHERS, kMargin + 12, y + 42, half, listHeight);
            MoveControl(window, IDC_YUA_LIB_TITLES, kMargin + 12 + half + kRowGap, y + 42, half, listHeight);
        }
    }

    // 擦除前的"全藏"：用 EnumChildWindows 把子窗口全部枚举藏掉，**不**去维护一张
    // "哪些控件需要藏"的名单。
    //
    // 名单这种写法天生是脆的：漏一个就是一块擦不到的像素。实测漏掉「显示详细日志」按钮
    //（它不属于任何一个页签控件组），改窗口大小后按钮旧位置的像素就印进了它下面的
    //「辅助操作」分组框里。全量枚举之后，以后再加控件也不会漏 —— 前提是 LayoutWindow
    // 会把每个控件都摆到（本文件本来就是这么设计的），所以藏了的都会按布局重新显示。
    BOOL CALLBACK HideChildForErase(HWND child, LPARAM)
    {
        ::ShowWindow(child, SW_HIDE);
        return TRUE;
    }

    // 把控件占的地方先擦成背景色。
    //
    // 缺陷现场：切到「制作扩展集」后首页控件还留在画面上，和制作页签的控件叠成两层；
    // 可见性其实是对的（首页控件 IsWindowVisible 全是 FALSE），是像素没被擦掉。
    //
    // 原因是分组框：BS_GROUPBOX 只画边框和标题，从不擦自己内部（单独给它
    // RedrawWindow(RDW_ERASE) 一个像素都不会变），而父窗口又被 WS_CLIPCHILDREN
    // 挡在这些控件外面。于是"某个控件还没显示时、别的控件在同一块像素上画过的东西"
    // 就永远留在分组框里了 —— 切页签时就是那层幽灵。
    //
    // 擦不到就换个顺序：先把子窗口全藏起来，父窗口整块客户区就都能被背景刷擦到，
    // 擦完再让布局与 ShowTabControls 把该显示的显示回来
    //（LayoutWindow 会摆到每个控件，所以藏了的都会被重新显示）。
    void EraseBehindTabControls(UiContext& context)
    {
        ::EnumChildWindows(context.Window, HideChildForErase, 0);
        // RDW_UPDATENOW 必须当场把这次擦除画完：等消息返回再擦，分组框已经显示出来，
        // 父窗口的擦除会被它们裁掉，等于没擦。
        ::RedrawWindow(context.Window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }

    // 布局 / 可见性改完之后统一整窗重绘一次。
    //
    // 光 InvalidateRect(父窗口) 只失效父窗口自己：分组框、列表、页签这些子窗口不会被重画，
    // 被隐藏的控件腾出来的矩形就留在那儿了。RDW_ALLCHILDREN 才是关键。
    void RedrawAllControls(const UiContext& context)
    {
        ::RedrawWindow(context.Window, nullptr, nullptr,
                       RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }

    void LayoutWindow(UiContext& context)
    {
        HWND window = context.Window;
        // 隐藏会顺手把键盘焦点带走，所以先记下来，等控件都显示回来再还回去
        //（否则在目标游戏输入框里改到一半、动一下窗口大小，光标就没了）。
        HWND focused = ::GetFocus();
        // 先藏后擦：分组框内部只能靠父窗口在"它还没显示"的时候擦干净（见上）。
        EraseBehindTabControls(context);

        RECT client{};
        ::GetClientRect(window, &client);
        const int clientWidth = client.right;
        const int clientHeight = client.bottom;

        // 顶部页签
        const int tabWidths[] = { 78, 96, 78, 68 };
        const int tabIds[] = { IDC_YUA_TAB_HOME, IDC_YUA_TAB_MAKE_SET, IDC_YUA_TAB_SET, IDC_YUA_TAB_ABOUT };
        int tabX = kMargin;
        for (int index = 0; index < 4; ++index)
        {
            // 页签按钮恒可见，选中态由自绘（WM_DRAWITEM → DrawTab）决定。
            // 这里曾经写成 `context.ActiveTab == tabIds[index] || true`：恒真，行为与传 true 一致，
            // 但谁"清理"掉那个 `|| true` 就会变成只有当前页签可见 —— 界面直接坏。传 true 说清楚。
            MoveControl(window, tabIds[index], tabX, kMargin, tabWidths[index], kTabHeight, true);
            tabX += tabWidths[index] + kTabGap;
        }

        const int contentTop = kMargin + kTabHeight + 8;
        const int bottomBarY = clientHeight - kMargin - kLogButtonHeight;
        const int contentBottom = context.LogVisible ? static_cast<int>(clientHeight * 0.60f) : bottomBarY - 6;

        // 左右分栏：右侧概览占约一半，但不挤掉左侧最小宽度。
        int rightWidth = static_cast<int>(clientWidth * 0.50f);
        if (rightWidth < kRightPanelMinWidth)
        {
            rightWidth = kRightPanelMinWidth;
        }
        const int maxRight = clientWidth - kLeftPanelMinWidth - 3 * kMargin;
        if (rightWidth > maxRight)
        {
            rightWidth = maxRight > kRightPanelMinWidth ? maxRight : kRightPanelMinWidth;
        }
        const int leftWidth = clientWidth - rightWidth - 3 * kMargin;
        const int rightX = kMargin + leftWidth + 2 * kMargin;

        const bool homeTab = context.ActiveTab == IDC_YUA_TAB_HOME;
        const bool makeTab = context.ActiveTab == IDC_YUA_TAB_MAKE_SET;
        // 「扩展集」页签有左列（管理与浏览 + 会社/作品列表），右栏复用 IDC_YUA_PAGE 当详情视图；
        // 「关于」页签没有左列，PAGE 铺满整宽 —— 两者只差这一点。
        const bool setTab = context.ActiveTab == IDC_YUA_TAB_SET;

        if (!homeTab && !makeTab && !setTab)
        {
            MoveControl(window, IDC_YUA_OVERVIEW, 0, 0, 0, 0, false);
            MoveControl(window, IDC_YUA_SET_STATUS, 0, 0, 0, 0, false);
            MoveControl(window, IDC_YUA_PAGE, kMargin, contentTop, clientWidth - 2 * kMargin,
                        contentBottom - contentTop);
        }
        else if (makeTab)
        {
            MoveControl(window, IDC_YUA_OVERVIEW, 0, 0, 0, 0, false);
            MoveControl(window, IDC_YUA_PAGE, 0, 0, 0, 0, false);
            MoveControl(window, IDC_YUA_SET_STATUS, rightX, contentTop, rightWidth, contentBottom - contentTop);
        }
        else if (setTab)
        {
            MoveControl(window, IDC_YUA_OVERVIEW, 0, 0, 0, 0, false);
            MoveControl(window, IDC_YUA_SET_STATUS, 0, 0, 0, 0, false);
            MoveControl(window, IDC_YUA_PAGE, rightX, contentTop, rightWidth, contentBottom - contentTop);
        }
        else
        {
            MoveControl(window, IDC_YUA_PAGE, 0, 0, 0, 0, false);
            MoveControl(window, IDC_YUA_SET_STATUS, 0, 0, 0, 0, false);
            MoveControl(window, IDC_YUA_OVERVIEW, rightX, contentTop, rightWidth, contentBottom - contentTop);
        }

        // 目标游戏一行：首页与「制作扩展集」页签共用（同一份 exe 状态，不做第二个输入框）。
        // 「扩展集」/「关于」页签没有这一行，所以也不摆它 —— 摆了会在左列顶部留一条空带
        //（ShowTabControls 是最后才把它藏掉的，位置却已经被占住）。
        int y = contentTop;
        if (homeTab || makeTab)
        {
            const int exeGroupHeight = 58;
            MoveControl(window, IDC_YUA_GROUP_EXE, kMargin, y, leftWidth, exeGroupHeight);
            MoveControl(window, IDC_YUA_EXE_LABEL, kMargin + 12, y + 26, 60, 18);
            const int browseWidth = 70;
            const int editWidth = leftWidth - 12 - 60 - 8 - browseWidth - 8 - 12;
            MoveControl(window, IDC_YUA_EXE_EDIT, kMargin + 12 + 60 + 8, y + 23,
                        editWidth > 120 ? editWidth : 120, 22);
            MoveControl(window, IDC_YUA_EXE_BROWSE, kMargin + leftWidth - 12 - browseWidth, y + 22, browseWidth, 24);
            y += exeGroupHeight + 8;
        }

        if (makeTab)
        {
            LayoutMakeSetColumn(context, leftWidth, y);
        }
        else if (setTab)
        {
            LayoutSetColumn(context, leftWidth, y, contentBottom);
        }
        else
        {
            LayoutHomeColumn(context, leftWidth, y, contentBottom);
        }

        // 底部：日志开关 + 日志窗口
        MoveControl(window, IDC_YUA_LOGBTN, kMargin, bottomBarY, 150, kLogButtonHeight);
        if (context.LogVisible)
        {
            const int logTop = contentBottom + 6;
            MoveControl(window, IDC_YUA_LOGVIEW, kMargin, logTop, clientWidth - 2 * kMargin,
                        bottomBarY - 6 - logTop);
        }
        else
        {
            MoveControl(window, IDC_YUA_LOGVIEW, 0, 0, 0, 0, false);
        }

        // 可见性必须最后统一决定：上面的 MoveControl 都带 SWP_SHOWWINDOW，
        // 中途 ShowWindow(SW_HIDE) 会被随后的布局重新显示出来
        //（"关于"页曾因此和首页控件叠在一起）。
        ShowTabControls(context);
        // 焦点还回去（原来的控件这次布局后仍然显示着才还，否则交给系统决定）。
        if (focused && ::IsWindow(focused) && ::IsWindowVisible(focused))
        {
            ::SetFocus(focused);
        }
        // 显示/隐藏都定下来之后再整窗（含子窗口）重绘，别让上一页的像素留在原处。
        RedrawAllControls(context);
    }

    // ---------- 页签绘制 ----------

    void DrawTab(const DRAWITEMSTRUCT* item, bool selected, HFONT font, HFONT fontBold)
    {
        HDC dc = item->hDC;
        RECT rect = item->rcItem;

        const COLORREF background = selected ? RGB(255, 255, 255) : ::GetSysColor(COLOR_BTNFACE);
        HBRUSH brush = ::CreateSolidBrush(background);
        ::FillRect(dc, &rect, brush);
        ::DeleteObject(brush);

        HPEN pen = ::CreatePen(PS_SOLID, 1, selected ? kColorAccent : kColorTabBorder);
        HGDIOBJ oldPen = ::SelectObject(dc, pen);
        HGDIOBJ oldBrush = ::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
        ::Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
        ::SelectObject(dc, oldBrush);
        ::SelectObject(dc, oldPen);
        ::DeleteObject(pen);

        if (selected)
        {
            RECT bar = rect;
            bar.top = bar.bottom - 3;
            HBRUSH accent = ::CreateSolidBrush(kColorAccent);
            ::FillRect(dc, &bar, accent);
            ::DeleteObject(accent);
        }

        wchar_t text[64]{};
        ::GetWindowTextW(item->hwndItem, text, _countof(text));
        ::SetBkMode(dc, TRANSPARENT);
        ::SetTextColor(dc, selected ? kColorAccent : kColorTabIdleText);
        // 选中的页签用粗体，看得出一栏里哪个是当前页。
        HGDIOBJ oldFont = ::SelectObject(dc, selected ? fontBold : font);
        ::DrawTextW(dc, text, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        ::SelectObject(dc, oldFont);

        if (item->itemState & ODS_FOCUS)
        {
            RECT focus = rect;
            ::InflateRect(&focus, -3, -3);
            ::DrawFocusRect(dc, &focus);
        }
    }

    void SwitchTab(UiContext& context, int tabId)
    {
        if (context.ActiveTab == tabId)
        {
            return;
        }
        context.ActiveTab = tabId;
        context.RenderedOverview.clear();
        LayoutWindow(context);
        UpdateOverview(context);
        // 页签选中态与右栏内容都换过了，这里再整窗重绘一次（含自绘页签按钮）。
        RedrawAllControls(context);
    }

    // ---------- 动作 ----------

    void ApplyTarget(UiContext& context, const std::wstring& exePath); // 定义在后面，RequireTarget 要用

    // 取当前目标 exe；没选就先把选择框弹出来（别让用户先去点"选择"再回来点步骤）。
    // 返回空串表示放弃。
    std::wstring RequireTarget(UiContext& context)
    {
        SyncTargetFromEdit(context);
        if (context.ExePath.empty())
        {
            std::wstring chosen;
            if (!Host::PickGameExe(context.Window, chosen))
            {
                return std::wstring();
            }
            ApplyTarget(context, chosen);
            return context.ExePath;
        }
        if (::GetFileAttributesW(context.ExePath.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Host::ShowError(context.Window, L"目标游戏主程序不存在：\r\n" + context.ExePath);
            return std::wstring();
        }
        return context.ExePath;
    }

    void ApplyTarget(UiContext& context, const std::wstring& exePath)
    {
        context.ExePath = exePath;
        context.Paths = BuildWorkspace(exePath);
        RefreshSetPaths(context);
        ::SetWindowTextW(::GetDlgItem(context.Window, IDC_YUA_EXE_EDIT), exePath.c_str());
        Host::SetTargetExe(exePath);

        ::SetWindowTextW(context.Window, BuildCaption(exePath).c_str());
        context.RenderedOverview.clear();
        RefreshProbes(context, true);
        UpdateOverview(context);
    }

    void OnBrowseExe(UiContext& context)
    {
        std::wstring chosen;
        if (!Host::PickGameExe(context.Window, chosen))
        {
            return;
        }
        ApplyTarget(context, chosen);
        Host::LogLine(Log::Level::Info, L"[YuaExtract] 目标游戏：" + chosen);
    }

    // 启动游戏之前必须先过一遍上游的「反篡改补丁」流程。
    //
    // 它**不是只读检查**：需要补丁时会真的跑一遍目标、产出 <名字>_crack.exe。
    // 上游的做法是弹框让用户"把 _crack.exe 拖回本程序继续"——在只能拖拽的老界面里
    // 那是唯一办法，但在这里完全没必要：我们自己把目标换成 _crack.exe 接着启动就行。
    // 这样"点按钮 → 游戏起来"第一次点击就成立，而不是点了没反应。
    //
    // 返回 false = 本次启动应当中止。
    bool EnsureTargetReadyForLaunch(UiContext& context)
    {
        // 补丁流程可能要跑几秒，先把状态写进面板并强制重绘一次 ——
        // 否则这几秒里界面没有任何反馈，看起来就像"按钮坏了"。
        context.StageNote = L"正在检查目标（反篡改补丁流程）……";
        context.RenderedOverview.clear();
        UpdateOverview(context);
        ::UpdateWindow(context.Window);

        if (!Host::PrepareTargetExe(context.Window, context.ExePath))
        {
            // 退出码 2/3：无需处理，或需要补但没补上（模块自己弹过警告了）。
            // 按上游语义继续启动，不拦。
            return true;
        }

        const std::wstring cracked = Host::CrackedExePath(context.ExePath);
        if (!cracked.empty())
        {
            Host::LogLine(Log::Level::Info,
                          L"[YuaExtract] 补丁流程已产出 " + cracked + L"，自动改用该文件作为目标继续启动");
            ApplyTarget(context, cracked);
            context.StageNote = L"已自动改用补丁产物 " + cracked;
            return true;
        }

        // 补丁流程接管了、但没产出成品：用户取消了脱壳确认，或者目标根本不是这个引擎的游戏。
        Host::LogLine(Log::Level::Warn, L"[YuaExtract] 补丁流程未产出 _crack.exe，本次启动中止");
        context.StageNote = L"启动已中止（反篡改补丁流程未产出 _crack.exe）";
        context.RenderedOverview.clear();
        UpdateOverview(context);
        Host::ShowInfo(context.Window,
                       L"目标没有通过反篡改补丁流程，本次启动已中止。\r\n\r\n"
                       L"常见原因：\r\n"
                       L"· 目标不是 KiriKiri / Hxv4 游戏（补丁流程找不到可补的东西）；\r\n"
                       L"· 脱壳确认框被取消；\r\n"
                       L"· 补丁没能应用（此时会有单独的警告提示）。\r\n\r\n"
                       L"确认目标正确后可以再点一次本步骤重试。");
        return false;
    }

    void OnStep1(UiContext& context)
    {
        const std::wstring exe = RequireTarget(context);
        if (exe.empty())
        {
            return;
        }
        if (!Host::ModuleExists(L"CxdecExtractorUI.dll"))
        {
            Host::ShowError(context.Window, Host::ModuleMissingHint(L"CxdecExtractorUI.dll"));
            return;
        }
        if (!EnsureTargetReadyForLaunch(context))
        {
            return;
        }

        context.StageNote = L"正在启动游戏并注入解包模块……";
        context.RenderedOverview.clear();
        UpdateOverview(context);
        ::UpdateWindow(context.Window);

        std::wstring error;
        if (!Host::LaunchGameWithModule(context.Window, L"CxdecExtractorUI.dll", L"", error))
        {
            Host::ShowError(context.Window, L"启动游戏并注入解包模块失败：\r\n" + error);
            return;
        }

        context.Stage = 1;
        context.StageNote = L"已启动游戏并注入解包界面（" + TimestampNow() + L"）";
        Host::LogLine(Log::Level::Info, L"[YuaExtract] 步骤1：已注入 CxdecExtractorUI.dll");
        RefreshProbes(context, false);
        UpdateOverview(context);
    }

    void OnStep2(UiContext& context)
    {
        const std::wstring exe = RequireTarget(context);
        if (exe.empty())
        {
            return;
        }
        if (!Host::ModuleExists(L"CxdecStringDumper.dll"))
        {
            Host::ShowError(context.Window, Host::ModuleMissingHint(L"CxdecStringDumper.dll"));
            return;
        }
        if (!EnsureTargetReadyForLaunch(context))
        {
            return;
        }

        // 待恢复目录预填成 Extractor_Output（存在才填），省掉上游那个手动选目录的弹窗。
        std::wstring target = context.ExtractProbe.exists ? context.Paths.ExtractDir : std::wstring();

        context.StageNote = L"正在启动游戏并注入 Hash 收集模块……";
        context.RenderedOverview.clear();
        UpdateOverview(context);
        ::UpdateWindow(context.Window);

        std::wstring error;
        if (!Host::LaunchGameWithModule(context.Window, L"CxdecStringDumper.dll", target, error))
        {
            Host::ShowError(context.Window, L"启动游戏并注入 Hash 收集模块失败：\r\n" + error);
            return;
        }

        context.Stage = 2;
        context.StageNote = L"已启动游戏并注入 Hash 收集模块（" + TimestampNow() + L"）";
        Host::LogLine(Log::Level::Info, L"[YuaExtract] 步骤2：已注入 CxdecStringDumper.dll");
        RefreshProbes(context, false);
        UpdateOverview(context);
    }

    // ---------- 后台任务（生成 / 静态 Hash 表 / 干跑） ----------

    void SetTaskText(UiContext& context, const std::wstring& text)
    {
        if (HWND label = ::GetDlgItem(context.Window, IDC_YUA_TASK_TEXT))
        {
            ::SetWindowTextW(label, text.c_str());
        }
    }

    void SetTaskProgress(UiContext& context, unsigned int percent)
    {
        if (HWND bar = ::GetDlgItem(context.Window, IDC_YUA_TASK_PROGRESS))
        {
            ::SendMessageW(bar, PBM_SETPOS, percent > 100u ? 100u : percent, 0);
        }
    }

    void SetTaskIdle(UiContext& context)
    {
        SetTaskText(context, L"（空闲）");
        SetTaskProgress(context, 0);
        if (HWND cancel = ::GetDlgItem(context.Window, IDC_YUA_TASK_CANCEL))
        {
            ::EnableWindow(cancel, FALSE);
        }
    }

    void ResetJob(UiContext& context, JobKind kind, const std::wstring& stage)
    {
        SetJob& job = context.Job;
        ::InterlockedExchange(&job.Cancel, 0);
        ::InterlockedExchange(&job.Done, 0);
        ::InterlockedExchange(&job.Percent, 0);
        job.Kind = kind;
        job.Ok = false;
        job.Failure = ExtensionSet::LoadFailure::None;
        job.UnverifiedLoad = false;
        job.Error.clear();
        job.Stage = stage;
        job.GenerateOptions = ExtensionSet::GenerateOptions{};
        job.LoadOptions = ExtensionSet::LoadOptions{};
        job.DryRunOptions = ExtensionSet::DryRunOptions{};
        job.LibraryRoot.clear();
        job.ArchivePath.clear();
        job.OutputDirectory.clear();
        job.Publisher.clear();
        job.Title.clear();
        job.Generate = ExtensionSet::GenerateResult{};
        job.Static = ExtensionSet::StaticHashResult{};
        job.DryRun = ExtensionSet::DryRunResult{};
        job.Load = ExtensionSet::LoadResult{};
    }

    void SetJobStage(SetJob& job, unsigned int percent, const std::wstring& stage)
    {
        if (job.LockReady)
        {
            ::EnterCriticalSection(&job.Lock);
        }
        job.Percent = static_cast<LONG>(percent);
        job.Stage = stage;
        if (job.LockReady)
        {
            ::LeaveCriticalSection(&job.Lock);
        }
    }

    // 这次加载是不是"未校验放行"。
    // 内核在 allowUnverified 且拿不到当前盐时会把 match 标成 IdentityMatch::Unverified
    //（并保证 matchNote 里写明"未能校验归属"）；这里同时认 matchNote 措辞，免得将来漏判。
    bool IsUnverifiedLoad(const SetJob& job)
    {
        if (job.Load.match == ExtensionSet::IdentityMatch::Unverified)
        {
            return true;
        }
        return job.Load.matchNote.find(L"未能校验归属") != std::wstring::npos;
    }

    // 任务线程。只碰 SetJob 与 PostMessage，不直接动界面控件。
    DWORD WINAPI SetJobThreadProc(LPVOID parameter)
    {
        UiContext* context = static_cast<UiContext*>(parameter);
        SetJob& job = context->Job;
        const HWND notify = context->Window;

        const auto cancelled = [&job](void) -> bool
        {
            return ::InterlockedCompareExchange(&job.Cancel, 0, 0) != 0;
        };
        const auto progress = [&job, notify](unsigned int percent, const std::wstring& stage)
        {
            SetJobStage(job, percent, stage);
            ::PostMessageW(notify, kSetProgressMessage, 0, 0);
        };

        switch (job.Kind)
        {
            case JobKind::Generate:
            {
                job.GenerateOptions.cancelRequested = cancelled;
                job.GenerateOptions.progress = progress;
                job.Generate = ExtensionSet::GenerateArchive(job.GenerateOptions, job.ArchivePath);
                job.Ok = job.Generate.ok;
                job.Error = job.Generate.error;
                break;
            }
            case JobKind::StaticHash:
            {
                progress(3u, L"正在加载扩展集档案……");
                job.Load = ExtensionSet::LoadArchiveFromLibrary(job.LibraryRoot, job.Publisher, job.Title,
                                                               job.LoadOptions);
                if (!job.Load.ok)
                {
                    job.Error = job.Load.error.empty() ? job.Load.matchNote : job.Load.error;
                    // "档案结构坏了"与"这是别的游戏的档案"要分开报，出路完全不同（验证 P-3）。
                    job.Failure = job.Load.failure;
                    break;
                }
                job.UnverifiedLoad = IsUnverifiedLoad(job);
                if (cancelled())
                {
                    job.Error = L"已取消。";
                    break;
                }
                // 加载没有进度回调（内核接口如此），所以这里按阶段报进度。
                progress(45u, L"正在用档案内嵌的盐展开静态 Hash 表……");
                job.Static = ExtensionSet::WriteStaticHashMap(job.Load.archive, job.OutputDirectory);
                job.Ok = job.Static.ok;
                job.Error = job.Static.error;
                break;
            }
            case JobKind::DryRun:
            {
                progress(3u, L"正在加载扩展集档案……");
                job.Load = ExtensionSet::LoadArchiveFile(job.ArchivePath, job.LoadOptions);
                if (!job.Load.ok)
                {
                    job.Error = job.Load.error.empty() ? job.Load.matchNote : job.Load.error;
                    break;
                }
                job.DryRunOptions.cancelRequested = cancelled;
                job.DryRunOptions.progress = progress;
                job.DryRun = ExtensionSet::DryRun(job.Load.archive, job.DryRunOptions);
                job.Ok = job.DryRun.ok;
                job.Error = job.DryRun.error;
                break;
            }
            default:
                break;
        }

        ::InterlockedExchange(&job.Done, 1);
        ::PostMessageW(notify, kSetDoneMessage, 0, 0);
        return 0;
    }

    // 起线程。输入字段必须在这之前填好（线程一起来就会读）。
    bool StartJob(UiContext& context)
    {
        if (context.JobActive)
        {
            Host::ShowInfo(context.Window, L"已有后台任务在跑，等它结束或点「取消」之后再试。");
            return false;
        }
        SetJob& job = context.Job;
        job.Thread = ::CreateThread(nullptr, 0u, SetJobThreadProc, &context, 0u, nullptr);
        if (!job.Thread)
        {
            job.Kind = JobKind::None;
            Host::ShowError(context.Window, L"启动后台任务线程失败。");
            return false;
        }
        // 降一档优先级：这些任务都会吃满一个核（逐条哈希重算），别跟界面重绘抢 CPU。
        ::SetThreadPriority(job.Thread, THREAD_PRIORITY_BELOW_NORMAL);
        context.JobActive = true;
        if (HWND cancel = ::GetDlgItem(context.Window, IDC_YUA_TASK_CANCEL))
        {
            ::EnableWindow(cancel, TRUE);
        }
        SetTaskText(context, job.Stage);
        SetTaskProgress(context, 0);
        return true;
    }

    void OnJobProgress(UiContext& context)
    {
        SetJob& job = context.Job;
        unsigned int percent = 0u;
        std::wstring stage;
        if (job.LockReady)
        {
            ::EnterCriticalSection(&job.Lock);
        }
        percent = static_cast<unsigned int>(job.Percent);
        stage = job.Stage;
        if (job.LockReady)
        {
            ::LeaveCriticalSection(&job.Lock);
        }
        SetTaskProgress(context, percent);
        SetTaskText(context, stage.empty() ? L"正在处理……" : stage);
    }

    // 模式1 步骤1：加载选中的扩展集 → 写 StaticHash_Output\StaticHash.lst。
    // allowUnverified 只在用户明确同意"不做归属校验"时为 true（设计稿 §2.1）。
    bool BeginStaticHashJob(UiContext& context, bool allowUnverified)
    {
        ResetJob(context, JobKind::StaticHash, L"正在加载扩展集档案……");
        ExtensionSet::LoadOptions options = ExtensionSet::ResolveLoadOptions(
            context.ExePath, Host::LogDirectory(),
            ExtensionSet::DefaultStaticKeyDirectory(context.Paths.GameDir));
        options.allowUnverified = allowUnverified;
        context.Job.LoadOptions = options;
        context.Job.LibraryRoot = context.LibraryRoot;
        context.Job.Publisher = context.SelectedPublisher;
        context.Job.Title = context.SelectedTitle;
        context.Job.OutputDirectory = context.StaticHashDir;
        return StartJob(context);
    }

    // 任务收尾：结论落到界面状态与右侧概览。
    void OnJobDone(UiContext& context)
    {
        SetJob& job = context.Job;
        if (job.Thread)
        {
            ::WaitForSingleObject(job.Thread, 5000);
            ::CloseHandle(job.Thread);
            job.Thread = nullptr;
        }
        context.JobActive = false;
        SetTaskIdle(context);

        const JobKind kind = job.Kind;
        job.Kind = JobKind::None;
        const bool ok = job.Ok;
        const std::wstring error = job.Error;
        const bool cancelledByUser = ::InterlockedCompareExchange(&job.Cancel, 0, 0) != 0;

        if (!ok && cancelledByUser)
        {
            context.MakeNote = L"任务已取消（" + TimestampNow() + L"）";
            Host::LogLine(Log::Level::Warn, L"[YuaExtract] 后台任务被取消");
            Host::ShowInfo(context.Window, L"任务已取消。已经写出的文件保留。");
            job.Generate = ExtensionSet::GenerateResult{};
            job.Static = ExtensionSet::StaticHashResult{};
            job.DryRun = ExtensionSet::DryRunResult{};
            job.Load = ExtensionSet::LoadResult{};
            context.RenderedOverview.clear();
            RefreshProbes(context, true);
            UpdateOverview(context);
            return;
        }

        switch (kind)
        {
            case JobKind::Generate:
            {
                if (ok)
                {
                    context.HasGenerate = true;
                    context.LastGenerateNote = ExtensionSet::DescribeGenerateResult(job.Generate);
                    context.MakeNote = L"已生成扩展集档案（" + FormatSize(job.Generate.archiveBytes) + L"）：" +
                                       job.Generate.archivePath;
                    Host::LogLine(Log::Level::Info, L"[YuaExtract] 生成扩展集：" + context.MakeNote);
                    Host::ShowInfo(context.Window,
                                   L"扩展集档案已生成：\r\n" + job.Generate.archivePath + L"\r\n\r\n" +
                                       context.LastGenerateNote);
                }
                else
                {
                    context.MakeNote = L"生成扩展集失败。";
                    Host::LogLine(Log::Level::Warn, L"[YuaExtract] 生成扩展集失败：" + error);
                    Host::ShowError(context.Window,
                                    L"生成当前游戏扩展集失败：\r\n" +
                                        (error.empty() ? std::wstring(L"（内核没有给出原因）") : error));
                }
                break;
            }
            case JobKind::StaticHash:
            {
                if (ok)
                {
                    context.HasStatic = true;
                    wchar_t buffer[128]{};
                    ::swprintf_s(buffer, L"已生成（目录 %s 条 / 文件 %s 条）",
                                 FormatCount(job.Static.directoryEntries).c_str(),
                                 FormatCount(job.Static.fileEntries).c_str());
                    context.LastStaticNote = buffer;
                    context.Stage = 1;
                    // 归属结论留在上下文里，概览的「扩展集状态」会一直显示（不能只留在弹框里）。
                    context.LastLoadNote = job.Load.matchNote;
                    context.LastLoadVerdict = job.UnverifiedLoad ? LoadVerdict::Unverified : LoadVerdict::Verified;
                    if (job.UnverifiedLoad)
                    {
                        // 强行使用这条路径必须留下痕迹：StageNote 不能被"成功"盖成一句无事发生的话。
                        context.StageNote = L"已生成静态 Hash 表，但未能校验归属（用户已确认强行使用）（" +
                                            TimestampNow() + L"）";
                        Host::LogLine(Log::Level::Warn,
                                      L"[YuaExtract] 模式1 步骤1：未能校验归属（用户已确认强行使用）—— " +
                                          context.LastLoadNote);
                    }
                    else
                    {
                        context.StageNote = L"已用扩展集生成静态 Hash 表（" + TimestampNow() + L"）";
                        Host::LogLine(Log::Level::Info, L"[YuaExtract] 模式1 步骤1 归属：" + context.LastLoadNote);
                    }
                    Host::LogLine(Log::Level::Info, L"[YuaExtract] 模式1 步骤1：" + context.LastStaticNote + L" → " +
                                                        job.Static.path);
                    Host::ShowInfo(context.Window,
                                   L"静态 Hash 表已生成：\r\n" + job.Static.path + L"\r\n\r\n" +
                                       ExtensionSet::DescribeLoadResult(job.Load) + L"\r\n" + context.LastStaticNote);
                }
                else
                {
                    // 归属校验被拒时不能只甩一句死胡同提示 —— 模式1 的主用例就是"打过补丁 /
                    // 换过版本"的游戏，必须给出可操作的出路。**不同失败分类的出路完全不同**
                    //（设计稿 §2.1 + 验证报告 P-3）：档案坏了要重新生成/重新安装，
                    // 盐不同要换回本作的盐，只有"拿不到当前盐"才谈得上放宽。
                    context.LastLoadNote = job.Load.matchNote.empty() ? error : job.Load.matchNote;
                    context.LastLoadVerdict = LoadVerdict::Rejected;
                    switch (job.Failure)
                    {
                        case ExtensionSet::LoadFailure::Unverified:
                        {
                            const bool extractFirst = Host::Confirm(
                                context.Window,
                                L"这份扩展集无法在当前游戏上校验归属：\r\n" + error +
                                    L"\r\n\r\n推荐出路：先做一次「静态 Key 提取」（离线，不用启动游戏），"
                                    L"拿到本作的盐之后再自动重试。\r\n\r\n"
                                    L"点「是」= 现在做静态 Key 提取并重试；\r\n"
                                    L"点「否」= 再问一次要不要「不校验、强行使用」。",
                                L"扩展集未能校验归属");
                            if (extractFirst)
                            {
                                Host::LogLine(Log::Level::Info,
                                              L"[YuaExtract] 模式1 步骤1：先做静态 Key 提取，再重试加载扩展集");
                                context.StageNote = L"正在提取静态 Key，随后自动重试加载扩展集……";
                                context.RenderedOverview.clear();
                                UpdateOverview(context);
                                ::UpdateWindow(context.Window);
                                Host::RunStaticKeyExtractionForTarget(context.Window);
                                BeginStaticHashJob(context, false);
                                return;
                            }
                            if (Host::Confirm(
                                    context.Window,
                                    L"不校验归属、强行使用这份扩展集？\r\n\r\n"
                                    L"强行使用不会报错，但展开出的静态 Hash 表可能一条都不中"
                                    L"（看起来有数据、实际静默失效）。\r\n\r\n"
                                    L"点「是」= 强行使用（allowUnverified=true）；点「否」= 放弃本次步骤1。",
                                    L"强行使用扩展集（未校验）"))
                            {
                                Host::LogLine(Log::Level::Warn,
                                              L"[YuaExtract] 模式1 步骤1：用户选择强行使用未校验的扩展集");
                                context.StageNote = L"扩展集未能校验归属（用户已确认强行使用）";
                                BeginStaticHashJob(context, true);
                                return;
                            }
                            break;
                        }
                        case ExtensionSet::LoadFailure::Structure:
                        {
                            // 结构缺失 / 损坏 / 读不到（magic、版本、截断、CRC、缺 META、缺 IDENTITY…）：
                            // 出路只有"换一份能用的档案"，**绝不能**引导用户去折腾 Universal.log
                            //（那是"盐不同"那条路的做法，对坏档案毫无用处 —— 验证 P-3）。
                            const bool openLibrary = Host::Confirm(
                                context.Window,
                                L"这份扩展集档案结构缺失或已损坏（读不出来 / 版本不符 / 截断 / CRC 错 / "
                                L"缺 META 或 IDENTITY）：\r\n" +
                                    error +
                                    L"\r\n\r\n出路：用当前游戏重新执行一次「生成当前游戏扩展集」，"
                                    L"再「安装到扩展集库」覆盖掉库里这份档案；"
                                    L"如果这份档案是从别处拿来的，换一份重新导出的。\r\n\r\n"
                                    L"点「是」= 打开扩展集库目录去处理；点「否」= 关掉。",
                                L"扩展集档案结构缺失或已损坏");
                            if (openLibrary)
                            {
                                Host::OpenInExplorer(context.Window, context.LibraryRoot);
                            }
                            break;
                        }
                        case ExtensionSet::LoadFailure::SaltMismatch:
                        {
                            const ExtensionSet::SaltResult currentSalt = ExtensionSet::ResolveSalt(
                                Host::LogDirectory(),
                                ExtensionSet::DefaultStaticKeyDirectory(context.Paths.GameDir));
                            const bool openLog = Host::Confirm(
                                context.Window,
                                L"这份扩展集属于别的游戏，与当前游戏的盐不同，已拒绝加载：\r\n" + error +
                                    L"\r\n\r\n当前游戏的盐来源：" + ExtensionSet::DescribeSalt(currentSalt) +
                                    L"\r\n\r\n工具目录只有一份会话日志（Log\\Universal.log）。它现在装着别的游戏"
                                    L"跑步骤2 时留下的盐，所以必须换回本作的盐：\r\n"
                                    L"· 对当前游戏执行一次「步骤2：加载动态Hash收集模块」，让收集模块覆写这份日志；"
                                    L"或\r\n"
                                    L"· 把那份 Universal.log 移开/改名，让工具改用静态派生"
                                    L"（ExtractKey_Output\\Static，可用「静态 Key 提取」生成）。"
                                    L"\r\n\r\n点「是」= 打开日志目录去处理；点「否」= 关掉。",
                                L"扩展集与当前游戏盐不同");
                            if (openLog)
                            {
                                Host::OpenInExplorer(context.Window, Host::LogDirectory());
                            }
                            break;
                        }
                        case ExtensionSet::LoadFailure::BadArgument:
                        {
                            // 调用方参数不完整（库根 / 会社 / 作品名为空）：这不是档案的问题，
                            // 出路是让用户把「扩展集选择」补齐。
                            Host::ShowError(context.Window,
                                            L"无法定位扩展集档案：\r\n" +
                                                (error.empty() ? std::wstring(L"（库根 / 会社 / 作品名不完整）")
                                                               : error) +
                                                L"\r\n\r\n请在左侧「扩展集选择」里重新选择会社与作品"
                                                L"（还没有装过扩展集就先去「制作扩展集」页签生成并安装）。");
                            break;
                        }
                        default:
                        {
                            // 兜底：没归类的失败只如实报错，不编造"别的游戏"这种结论。
                            Host::ShowError(context.Window,
                                            L"生成静态 Hash 表失败：\r\n" +
                                                (error.empty() ? std::wstring(L"（内核没有给出原因）") : error));
                            break;
                        }
                    }
                    context.StageNote = L"生成静态 Hash 表失败。";
                    Host::LogLine(Log::Level::Warn, L"[YuaExtract] 模式1 步骤1 失败：" + error);
                }
                break;
            }
            case JobKind::DryRun:
            {
                if (ok)
                {
                    context.HasDryRun = true;
                    // 展示口径（设计稿 §5，2026-10-13 定）：三行数字必须分开显示 ——
                    // 「文件名可解（去重）」是 resolvedFiles，「整路径可还原」是 fullyResolvedFiles，
                    // 两者不是同一个数（差在"目录未知但文件名已知"的那批，默认语义下会被丢弃）。
                    {
                        wchar_t buffer[1024]{};
                        ::swprintf_s(buffer,
                                     L"可解目录（去重）：%s\r\n"
                                     L"文件名可解（去重）：%s\r\n"
                                     L"整路径可还原：%s\r\n"
                                     L"成功率：%u%%（整路径可还原 %s ÷ 哈希名文件 %s，与步骤3 同口径）\r\n"
                                     L"哈希名文件：%s / 扫描文件总数：%s\r\n"
                                     L"缺目录 Hash：%s / 缺文件名 Hash：%s\r\n"
                                     L"未命中明细：目录 %s 个 / 文件 %s 个\r\n"
                                     L"扩展集归属：%s\r\n"
                                     L"测试来源目录：%s\r\n"
                                     L"扩展集档案：%s（%s）\r\n",
                                     FormatCount(job.DryRun.resolvedDirectories).c_str(),
                                     FormatCount(job.DryRun.resolvedFiles).c_str(),
                                     FormatCount(job.DryRun.fullyResolvedFiles).c_str(),
                                     job.DryRun.successRate,
                                     FormatCount(job.DryRun.fullyResolvedFiles).c_str(),
                                     FormatCount(job.DryRun.hashNamedFiles).c_str(),
                                     FormatCount(job.DryRun.hashNamedFiles).c_str(),
                                     FormatCount(job.DryRun.totalFiles).c_str(),
                                     FormatCount(job.DryRun.missingDirectory).c_str(),
                                     FormatCount(job.DryRun.missingFileName).c_str(),
                                     FormatCount(job.DryRun.missingDirectoryHashes.size()).c_str(),
                                     FormatCount(job.DryRun.missingFileHashes.size()).c_str(),
                                     job.Load.matchNote.empty() ? L"（内核没有给出结论）" : job.Load.matchNote.c_str(),
                                     job.DryRunOptions.sourceDirectory.c_str(), job.ArchivePath.c_str(),
                                     FormatSize(FileSizeBytes(job.ArchivePath)).c_str());
                        context.LastTestNote = buffer;
                    }
                    // 内核的整份结论进日志（面板按上面几行口径显示，免得两处措辞不一致）。
                    // 日志是一行一条，把多行结论折成一行，别把日志格式撑坏。
                    std::wstring detail = ExtensionSet::DescribeDryRunResult(job.DryRun, &job.Load);
                    for (size_t at = detail.find(L"\r\n"); at != std::wstring::npos; at = detail.find(L"\r\n"))
                    {
                        detail.replace(at, 2u, L" / ");
                    }
                    Host::LogLine(Log::Level::Info, L"[YuaExtract] 干跑测试完成：" + detail);
                    context.MakeNote = L"干跑测试完成（" + TimestampNow() + L"）";
                }
                else
                {
                    context.MakeNote = L"干跑测试失败。";
                    Host::LogLine(Log::Level::Warn, L"[YuaExtract] 干跑测试失败：" + error);
                    Host::ShowError(context.Window,
                                    L"测试当前扩展集失败：\r\n" +
                                        (error.empty() ? std::wstring(L"（内核没有给出原因）") : error));
                }
                break;
            }
            default:
                break;
        }

        // 结论已经转成文本，几 MB 的结果体不必一直留在内存里。
        job.Generate = ExtensionSet::GenerateResult{};
        job.Static = ExtensionSet::StaticHashResult{};
        job.DryRun = ExtensionSet::DryRunResult{};
        job.Load = ExtensionSet::LoadResult{};

        context.RenderedOverview.clear();
        RefreshProbes(context, true);
        UpdateOverview(context);
    }

    // 关窗兜底：置取消标志并等线程退出来。内核的取消回调是按条检查的，
    // 正常几百毫秒内就能退出；这里给 30 秒兜底，免得某次磁盘 I/O 卡住时窗口关不掉。
    void StopJobForClose(UiContext& context)
    {
        if (!context.Job.Thread)
        {
            return;
        }
        ::InterlockedExchange(&context.Job.Cancel, 1);
        SetTaskText(context, L"正在等待后台任务结束……");
        ::WaitForSingleObject(context.Job.Thread, 30000);
        ::CloseHandle(context.Job.Thread);
        context.Job.Thread = nullptr;
        context.JobActive = false;
    }

    // ---------- 首页模式（设计稿 §8.3） ----------

    void UpdateHint(UiContext& context); // 定义在后面

    // 主流程三个按钮的文案按模式分派（图3 的三步措辞与现行完全不同）。
    void UpdateStepButtons(UiContext& context)
    {
        struct StepTitle
        {
            int Id;
            const wchar_t* SetExtract;
            const wchar_t* SetCrack;
            const wchar_t* Classic;
        };
        static const StepTitle kSteps[] = {
            { IDC_YUA_STEP1, L"步骤1：根据该作扩展集生成静态 Hash 表", L"步骤1：待定（二期预留）",
              L"步骤1：提取动态 XP3 资源" },
            { IDC_YUA_STEP2, L"步骤2：提取动态 XP3 资源", L"步骤2：待定（二期预留）",
              L"步骤2：加载动态Hash收集模块" },
            { IDC_YUA_STEP3, L"步骤3：用静态 Hash 还原资源名", L"步骤3：待定（二期预留）",
              L"步骤3：用动态Hash还原资源名" },
        };
        for (const StepTitle& step : kSteps)
        {
            HWND control = ::GetDlgItem(context.Window, step.Id);
            if (!control)
            {
                continue;
            }
            const wchar_t* text = context.Mode == ActiveMode::SetExtract ? step.SetExtract
                                  : context.Mode == ActiveMode::SetCrack ? step.SetCrack
                                                                        : step.Classic;
            ::SetWindowTextW(control, text);
        }
    }

    // 首页功能三个按钮是互斥模式选择：选中项决定主流程三步的文案与行为。
    void ApplyMode(UiContext& context, ActiveMode mode)
    {
        context.Mode = mode;
        const struct
        {
            int Id;
            ActiveMode Value;
        } buttons[] = {
            { IDC_YUA_FUNC_SET_EXTRACT, ActiveMode::SetExtract },
            { IDC_YUA_FUNC_SET_CRACK, ActiveMode::SetCrack },
            { IDC_YUA_FUNC_CLASSIC, ActiveMode::Classic },
        };
        for (const auto& button : buttons)
        {
            if (HWND control = ::GetDlgItem(context.Window, button.Id))
            {
                ::SendMessageW(control, BM_SETCHECK,
                               button.Value == mode ? static_cast<WPARAM>(BST_CHECKED)
                                                    : static_cast<WPARAM>(BST_UNCHECKED),
                               0);
            }
        }
        UpdateStepButtons(context);
        context.StageNote = std::wstring(L"已选择") + ModeName(mode);
        context.RenderedOverview.clear();
        UpdateHint(context);
        UpdateOverview(context);
    }

    // 步骤3：打开资源名还原窗口。
    //   hashDirectory：映射目录。传统动态模式 = StringHashDumper_Output；
    //                  模式1 = StaticHash_Output（设计稿 §7：还原内核零改动，只换传参）。
    void OnStep3(UiContext& context, const std::wstring& hashDirectory, bool staticHash)
    {
        const std::wstring exe = RequireTarget(context);
        if (exe.empty())
        {
            return;
        }
        if (context.RestoreWindow && ::IsWindow(context.RestoreWindow))
        {
            // 还原窗口已经开着：把它拉到前面就行。多开一个窗口会同时往同一个输出目录写，
            // 重名改名逻辑会把结果拆得到处都是。
            ::SetForegroundWindow(context.RestoreWindow);
            if (::IsIconic(context.RestoreWindow))
            {
                ::ShowWindow(context.RestoreWindow, SW_RESTORE);
            }
            return;
        }
        if (!context.ExtractProbe.exists)
        {
            Host::ShowInfo(context.Window,
                           (staticHash ? L"还没有解包结果，先执行步骤2：\r\n" : L"还没有解包结果，先执行步骤1：\r\n") +
                               context.Paths.ExtractDir);
            return;
        }

        const MappingSummary mapping = LoadMappingSummary(hashDirectory);
        if (!mapping.ok)
        {
            Host::ShowInfo(context.Window,
                           (staticHash ? L"还没有静态 Hash 表，先执行步骤1：\r\n"
                                       : L"还没有可用的 Hash 映射，先执行步骤2 并跑一段游戏：\r\n") +
                               hashDirectory);
            return;
        }

        // 还原本身交给独立窗口：它自己按封包拆任务、起 N 个工作线程、跑完写
        // RestoreReport.txt。主界面不再自己开线程跑同步还原 —— 那个做法既没法看
        // 中间进度，也没法在跑的时候查看单个封包的明细。
        Restore::WindowOptions options;
        options.GameDirectory = context.Paths.GameDir;
        options.SourceDirectory = context.Paths.ExtractDir;
        options.HashDirectory = hashDirectory;
        options.DefaultOutputDirectory = context.Paths.RestoreDir;
        options.LogDirectory = Host::LogDirectory();
        options.Owner = context.Window;

        context.RestoreWindow = Restore::ShowRestoreWindow(context.Instance, options);

        Host::LogLine(Log::Level::Info,
                      L"[YuaExtract] 步骤3：打开资源名还原窗口 " + context.Paths.ExtractDir);
    }

    void OnAuxCheck(UiContext& context)
    {
        SyncTargetFromEdit(context);
        if (context.Paths.GameDir.empty())
        {
            Host::ShowInfo(context.Window, L"请先选择目标游戏主程序（.exe）。");
            return;
        }
        // 这是唯一会递归扫盘的入口，显式点击才做。
        context.RenderedOverview.clear();
        RefreshProbes(context, true);
        UpdateOverview(context);
        Host::LogLine(Log::Level::Info, L"[YuaExtract] 检查状态完成，工作区 " + context.Paths.GameDir);
    }

    void OnAuxOpenWorkspace(UiContext& context)
    {
        SyncTargetFromEdit(context);
        if (context.Paths.GameDir.empty())
        {
            Host::ShowInfo(context.Window, L"请先选择目标游戏主程序（.exe）。");
            return;
        }
        Host::OpenInExplorer(context.Window, context.Paths.GameDir);
    }

    void OnAuxOpenRestoreDir(UiContext& context)
    {
        SyncTargetFromEdit(context);
        if (!context.RestoreProbe.exists)
        {
            Host::ShowInfo(context.Window, L"还原目录还不存在，先执行步骤3。");
            return;
        }
        Host::OpenInExplorer(context.Window, context.Paths.RestoreDir);
    }

    void OnAuxOpenReport(UiContext& context)
    {
        SyncTargetFromEdit(context);
        if (context.Paths.RestoreReport.empty() ||
            ::GetFileAttributesW(context.Paths.RestoreReport.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Host::ShowInfo(context.Window, L"还原报告还不存在，先执行步骤3。");
            return;
        }
        Host::OpenWithShell(context.Window, context.Paths.RestoreReport);
    }

    void OnAuxClean(UiContext& context)
    {
        SyncTargetFromEdit(context);
        if (context.Paths.GameDir.empty())
        {
            Host::ShowInfo(context.Window, L"请先选择目标游戏主程序（.exe）。");
            return;
        }
        if (context.RestoreWindow && ::IsWindow(context.RestoreWindow) &&
            Restore::IsRestoreRunning(context.RestoreWindow))
        {
            Host::ShowInfo(context.Window, L"还原窗口正在跑任务，先等它结束或关掉那个窗口，再清理目录。");
            return;
        }

        if (!Host::Confirm(context.Window,
                           L"将删除以下目录及其全部内容，不可恢复：\r\n\r\n" + context.Paths.ExtractDir +
                               L"\r\n" + context.Paths.HashDir + L"\r\n" + context.Paths.RestoreDir +
                               L"\r\n\r\n确认清理？",
                           L"清理目录"))
        {
            return;
        }

        std::wstring firstError;
        for (const std::wstring& directory :
             { context.Paths.ExtractDir, context.Paths.HashDir, context.Paths.RestoreDir })
        {
            std::wstring error;
            if (!RemoveDirectoryTree(directory, error) && firstError.empty())
            {
                firstError = error;
            }
        }

        context.Stage = 0;
        context.StageNote = L"已清理产物目录（" + TimestampNow() + L"）";
        context.RenderedOverview.clear();
        RefreshProbes(context, true);
        UpdateOverview(context);
        Host::LogLine(Log::Level::Info, L"[YuaExtract] 已清理产物目录");

        if (!firstError.empty())
        {
            Host::ShowError(context.Window, L"清理过程中有失败项：\r\n" + firstError);
        }
    }

    // 还原窗口跑完（或中止）后的通报。主界面既不管它的线程，也不去猜它的结果 ——
    // 真正的结果在 Restored_Extractor_Output\RestoreReport.txt 里，这里只负责
    // 重新探一次目录、刷新概览，让用户切回来就能看到最新状态。
    void OnRestoreWindowNotify(UiContext& context, Restore::RestoreTaskState state)
    {
        if (state == Restore::kStateRunning)
        {
            context.Stage = 3;
            context.StageNote = L"正在还原资源名（还原窗口）……";
        }
        else
        {
            context.Stage = 3;
            context.StageNote = (state == Restore::kStateCompleted ? L"资源名还原完成（" : L"资源名还原结束，有失败项（") +
                                TimestampNow() + L"）";
            Host::LogLine(state == Restore::kStateCompleted ? Log::Level::Info : Log::Level::Warn,
                          L"[YuaExtract] 还原窗口已结束：" + context.StageNote);
        }

        context.RenderedOverview.clear();
        RefreshProbes(context, true);
        UpdateOverview(context);
    }

    void OnReservedFeature(UiContext& context, const wchar_t* feature)
    {
        Host::LogLine(Log::Level::Warn, std::wstring(L"[YuaExtract] 预留接口被点击：") + feature);
        Host::ShowInfo(context.Window,
                       std::wstring(feature) +
                           L"\r\n\r\n该功能属于「扩展集」部分，当前版本仅预留接口，尚未实现。\r\n\r\n"
                           L"现在可用的是首页「主流程」的三步：提取动态 XP3 资源 → 加载动态Hash收集模块 → "
                           L"用动态Hash还原资源名。");
    }

    void ToggleLog(UiContext& context)
    {
        context.LogVisible = !context.LogVisible;
        ::SetWindowTextW(::GetDlgItem(context.Window, IDC_YUA_LOGBTN),
                         context.LogVisible ? L"隐藏详细日志 ▲" : L"显示详细日志 ▼");
        LayoutWindow(context);
        if (!context.LogVisible)
        {
            MoveControl(context.Window, IDC_YUA_LOGVIEW, 0, 0, 0, 0, false);
        }
        else
        {
            UpdateLogView(context);
        }
    }

    // ---------- 首页「扩展集选择」的列表交互（设计稿 §8.3） ----------

    std::wstring ListBoxSelectedText(HWND list)
    {
        if (!list)
        {
            return std::wstring();
        }
        const int index = static_cast<int>(::SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (index == LB_ERR)
        {
            return std::wstring();
        }
        const int length = static_cast<int>(::SendMessageW(list, LB_GETTEXTLEN, static_cast<WPARAM>(index), 0));
        if (length <= 0)
        {
            return std::wstring();
        }
        std::wstring text(static_cast<size_t>(length) + 1u, L'\0');
        ::SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&text[0]));
        text.resize(static_cast<size_t>(length));
        return text;
    }

    void OnPublisherSelected(UiContext& context)
    {
        context.SelectedPublisher = ListBoxSelectedText(::GetDlgItem(context.Window, IDC_YUA_LIB_PUBLISHERS));
        RefillTitleList(context);
        context.RenderedOverview.clear();
        UpdateOverview(context);
    }

    void OnTitleSelected(UiContext& context)
    {
        context.SelectedTitle = ListBoxSelectedText(::GetDlgItem(context.Window, IDC_YUA_LIB_TITLES));
        context.RenderedOverview.clear();
        UpdateOverview(context);
    }

    // ---------- 「编辑扩展集信息」弹窗（设计稿 §8.2 图2） ----------
    //
    // 不新增对话框资源（那要动 .rc / resource.h），所以自己注册窗口类 + 手工建控件，
    // 再自己跑一段模态消息循环。

    struct MetaDialogState
    {
        UiContext* Context = nullptr;
        ExtensionSet::Meta Value;
        bool Accepted = false;
        bool Done = false;
        HWND Window = nullptr;
        HWND DatePicker = nullptr;
    };

    HWND CreateMetaLabel(HWND dialog, MetaDialogState* state, int x, int y, int width, const wchar_t* text)
    {
        HWND label = ::CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT, x, y, width, 18,
                                       dialog, nullptr, state->Context->Instance, nullptr);
        ::SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(state->Context->FontUi), TRUE);
        return label;
    }

    HWND CreateMetaEdit(HWND dialog, MetaDialogState* state, int id, int x, int y, int width, int height,
                        DWORD extraStyle)
    {
        HWND edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | extraStyle, x, y, width, height, dialog,
                                      (HMENU)(INT_PTR)id, state->Context->Instance, nullptr);
        ::SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(state->Context->FontUi), TRUE);
        return edit;
    }

    HWND CreateMetaButton(HWND dialog, MetaDialogState* state, int id, const wchar_t* text, DWORD extraStyle)
    {
        HWND button = ::CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | extraStyle, 0, 0,
                                        10, 10, dialog, (HMENU)(INT_PTR)id, state->Context->Instance, nullptr);
        ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(state->Context->FontUi), TRUE);
        return button;
    }

    std::wstring ReadDateText(HWND picker)
    {
        SYSTEMTIME time{};
        if (!picker ||
            ::SendMessageW(picker, DTM_GETSYSTEMTIME, 0, reinterpret_cast<LPARAM>(&time)) != GDT_VALID)
        {
            return std::wstring();
        }
        wchar_t buffer[32]{};
        ::swprintf_s(buffer, L"%04u-%02u-%02u", time.wYear, time.wMonth, time.wDay);
        return buffer;
    }

    bool ParseDateText(const std::wstring& text, SYSTEMTIME& out)
    {
        unsigned int year = 0u;
        unsigned int month = 0u;
        unsigned int day = 0u;
        if (::swscanf_s(text.c_str(), L"%u-%u-%u", &year, &month, &day) != 3)
        {
            return false;
        }
        if (year < 1601u || year > 9999u || month < 1u || month > 12u || day < 1u || day > 31u)
        {
            return false;
        }
        out = SYSTEMTIME{};
        out.wYear = static_cast<WORD>(year);
        out.wMonth = static_cast<WORD>(month);
        out.wDay = static_cast<WORD>(day);
        return true;
    }

    LRESULT CALLBACK MetaDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
        {
            CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            MetaDialogState* state = static_cast<MetaDialogState*>(create->lpCreateParams);
            state->Window = hwnd;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        MetaDialogState* state = reinterpret_cast<MetaDialogState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!state)
        {
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        switch (msg)
        {
            case WM_CREATE:
            {
                const int labelX = 16;
                const int labelWidth = 76;
                const int fieldX = 100;
                const int fieldWidth = kMetaDialogWidth - fieldX - 16;
                int y = 16;

                CreateMetaLabel(hwnd, state, labelX, y + 3, labelWidth, L"会社");
                ::SetWindowTextW(
                    CreateMetaEdit(hwnd, state, IDC_YUA_META_PUBLISHER, fieldX, y, fieldWidth, 24, ES_AUTOHSCROLL),
                    state->Value.publisher.c_str());
                y += 32;

                CreateMetaLabel(hwnd, state, labelX, y + 3, labelWidth, L"游戏名称");
                ::SetWindowTextW(CreateMetaEdit(hwnd, state, IDC_YUA_META_TITLE, fieldX, y, fieldWidth, 24, ES_AUTOHSCROLL),
                                 state->Value.title.c_str());
                y += 32;

                CreateMetaLabel(hwnd, state, labelX, y + 3, labelWidth, L"贡献者");
                ::SetWindowTextW(
                    CreateMetaEdit(hwnd, state, IDC_YUA_META_CONTRIBUTOR, fieldX, y, fieldWidth, 24, ES_AUTOHSCROLL),
                    state->Value.contributor.c_str());
                y += 32;

                CreateMetaLabel(hwnd, state, labelX, y + 3, labelWidth, L"版本");
                ::SetWindowTextW(CreateMetaEdit(hwnd, state, IDC_YUA_META_VERSION, fieldX, y, fieldWidth, 24, ES_AUTOHSCROLL),
                                 state->Value.version.c_str());
                y += 32;

                CreateMetaLabel(hwnd, state, labelX, y + 3, labelWidth, L"日期");
                HWND picker = ::CreateWindowExW(0, DATETIMEPICK_CLASSW, L"",
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | DTS_SHORTDATEFORMAT, fieldX, y,
                                                fieldWidth, 24, hwnd, (HMENU)(INT_PTR)IDC_YUA_META_DATE,
                                                state->Context->Instance, nullptr);
                ::SendMessageW(picker, WM_SETFONT, reinterpret_cast<WPARAM>(state->Context->FontUi), TRUE);
                state->DatePicker = picker;
                {
                    SYSTEMTIME time{};
                    if (!ParseDateText(state->Value.date, time))
                    {
                        ::GetLocalTime(&time);
                    }
                    ::SendMessageW(picker, DTM_SETSYSTEMTIME, GDT_VALID, reinterpret_cast<LPARAM>(&time));
                }
                y += 36;

                CreateMetaLabel(hwnd, state, labelX, y, labelWidth, L"扩展集留言");
                ::SetWindowTextW(CreateMetaEdit(hwnd, state, IDC_YUA_META_COMMENT, labelX, y + 20,
                                                kMetaDialogWidth - 32, 84,
                                                ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL),
                                 state->Value.comment.c_str());
                y += 112;

                HWND ok = CreateMetaButton(hwnd, state, IDC_YUA_META_OK, L"确定", BS_DEFPUSHBUTTON | WS_GROUP);
                ::SetWindowPos(ok, nullptr, kMetaDialogWidth - 16 - 80 - 8 - 80, y, 80, 26,
                               SWP_NOZORDER | SWP_NOACTIVATE);
                HWND cancel = CreateMetaButton(hwnd, state, IDC_YUA_META_CANCEL, L"取消", 0);
                ::SetWindowPos(cancel, nullptr, kMetaDialogWidth - 16 - 80, y, 80, 26,
                               SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            }
            case WM_COMMAND:
            {
                switch (LOWORD(wParam))
                {
                    // IsDialogMessage 会把回车 / ESC 翻成 IDOK / IDCANCEL，
                    // 所以这两个标准号也要接（本弹窗的按钮用的是自己的控件号）。
                    case IDOK:
                    case IDC_YUA_META_OK:
                    {
                        state->Value.publisher = GetControlText(::GetDlgItem(hwnd, IDC_YUA_META_PUBLISHER));
                        state->Value.title = GetControlText(::GetDlgItem(hwnd, IDC_YUA_META_TITLE));
                        state->Value.contributor = GetControlText(::GetDlgItem(hwnd, IDC_YUA_META_CONTRIBUTOR));
                        state->Value.version = GetControlText(::GetDlgItem(hwnd, IDC_YUA_META_VERSION));
                        state->Value.comment = GetControlText(::GetDlgItem(hwnd, IDC_YUA_META_COMMENT));
                        state->Value.date = ReadDateText(state->DatePicker);
                        // generatedAt 故意不动：留空 = 档案里不写这一项（内核的默认口径）。
                        state->Accepted = true;
                        ::DestroyWindow(hwnd);
                        return 0;
                    }
                    case IDCANCEL:
                    case IDC_YUA_META_CANCEL:
                        ::DestroyWindow(hwnd);
                        return 0;
                    default:
                        break;
                }
                break;
            }
            case WM_KEYDOWN:
            {
                if (wParam == VK_ESCAPE)
                {
                    ::DestroyWindow(hwnd);
                    return 0;
                }
                break;
            }
            case WM_CLOSE:
            {
                ::DestroyWindow(hwnd);
                return 0;
            }
            case WM_DESTROY:
            {
                state->Done = true;
                return 0;
            }
            default:
                break;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool EnsureMetaDialogClass(HINSTANCE instance)
    {
        static bool registered = false;
        if (registered)
        {
            return true;
        }
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = MetaDialogProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        windowClass.lpszClassName = kMetaDialogClass;
        registered = ::RegisterClassExW(&windowClass) != 0;
        return registered;
    }

    // 模态弹窗。主窗口的定时刷新在嵌套消息循环里照常跑；IsDialogMessage 负责 Tab 与默认按钮。
    bool EditMetaDialog(UiContext& context, ExtensionSet::Meta& meta)
    {
        if (!EnsureMetaDialogClass(context.Instance))
        {
            Host::ShowError(context.Window, L"无法注册「编辑扩展集信息」窗口类。");
            return false;
        }

        MetaDialogState state;
        state.Context = &context;
        state.Value = meta;

        RECT rect{ 0, 0, kMetaDialogWidth, kMetaDialogHeight };
        ::AdjustWindowRectEx(&rect, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, 0);
        HWND dialog = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kMetaDialogClass, L"编辑扩展集信息",
                                        WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                        rect.right - rect.left, rect.bottom - rect.top, context.Window, nullptr,
                                        context.Instance, &state);
        if (!dialog)
        {
            Host::ShowError(context.Window, L"无法创建「编辑扩展集信息」窗口。");
            return false;
        }

        RECT ownerRect{};
        if (::GetWindowRect(context.Window, &ownerRect))
        {
            RECT dialogRect{};
            ::GetWindowRect(dialog, &dialogRect);
            const int width = dialogRect.right - dialogRect.left;
            const int height = dialogRect.bottom - dialogRect.top;
            const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
            const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
            ::SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        ::EnableWindow(context.Window, FALSE);
        ::ShowWindow(dialog, SW_SHOW);
        ::SetFocus(::GetDlgItem(dialog, IDC_YUA_META_PUBLISHER));

        MSG message{};
        while (!state.Done)
        {
            const BOOL result = ::GetMessageW(&message, nullptr, 0u, 0u);
            if (result == 0 || result == -1)
            {
                break;
            }
            if (!::IsDialogMessageW(dialog, &message))
            {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
        }

        if (!state.Done && ::IsWindow(dialog))
        {
            ::DestroyWindow(dialog);
        }
        ::EnableWindow(context.Window, TRUE);
        ::SetActiveWindow(context.Window);

        if (!state.Accepted)
        {
            return false;
        }
        meta = state.Value;
        return true;
    }

    // ---------- 模式1 步骤1：根据该作扩展集生成静态 Hash 表（设计稿 §7） ----------

    void OnMode1Step1(UiContext& context)
    {
        const std::wstring exe = RequireTarget(context);
        if (exe.empty())
        {
            return;
        }
        if (context.SelectedPublisher.empty() || context.SelectedTitle.empty())
        {
            Host::ShowInfo(context.Window, L"请先在左侧「扩展集选择」里选择会社，再选择该作扩展集。");
            return;
        }
        if (context.LibraryRoot.empty())
        {
            context.LibraryRoot = ExtensionSet::DefaultLibraryRoot(Host::LoaderDirectory());
        }
        if (!BeginStaticHashJob(context, false))
        {
            return;
        }
        Host::LogLine(Log::Level::Info, L"[YuaExtract] 模式1 步骤1：加载扩展集 " + context.SelectedPublisher + L"/" +
                                            context.SelectedTitle + L" → " + context.StaticHashDir);
    }

    // ---------- 「制作扩展集」页签的动作（设计稿 §8.1） ----------

    void OnSetGenerate(UiContext& context)
    {
        const std::wstring exe = RequireTarget(context);
        if (exe.empty())
        {
            return;
        }
        if (context.Meta.title.empty())
        {
            context.Meta.title = ExeStem(exe);
        }
        if (context.Meta.publisher.empty())
        {
            Host::ShowInfo(context.Window,
                           L"还没有填「会社」。\r\n\r\n会社是扩展集库的第一级目录，必须手工填（工具不猜）。\r\n"
                           L"先点「编辑当前扩展集信息」填好会社与游戏名称，再生成。");
            return;
        }
        std::wstring error;
        if (!EnsureDirectory(context.DraftDir, error))
        {
            Host::ShowError(context.Window, L"无法准备扩展集草案目录：\r\n" + error);
            return;
        }

        ResetJob(context, JobKind::Generate, L"正在采集并生成扩展集档案……");
        ExtensionSet::GenerateOptions options;
        options.hashDirectory = context.Paths.HashDir;
        options.logDirectory = Host::LogDirectory();
        options.staticKeyDirectory = ExtensionSet::DefaultStaticKeyDirectory(context.Paths.GameDir);
        options.sourceDirectory = EffectiveSourceDirectory(context);
        options.workspaceRoot = context.Paths.GameDir;
        options.targetExe = exe;
        options.meta = context.Meta;
        // UI 真正给用户生成时把生成时间写进档案（设计稿 §3 的 META 字段）。
        // 内核默认留空是为了"同一输入重跑逐字节一致"的幂等断言（E-7），那是内核/脚手架的用法。
        options.stampGeneratedAt = true;
        context.Job.GenerateOptions = options;
        context.Job.ArchivePath = context.DraftArchive;
        StartJob(context);
    }

    void OnSetEditInfo(UiContext& context)
    {
        SyncTargetFromEdit(context);
        if (context.Meta.title.empty())
        {
            context.Meta.title = ExeStem(context.ExePath);
        }
        if (context.Meta.date.empty())
        {
            context.Meta.date = TodayText();
        }
        // 已经生成过档案时，用档案里的 META 预填，免得用户改完又被旧值覆盖。
        if (FileExistsAt(context.DraftArchive))
        {
            const ExtensionSet::LoadOptions options = ExtensionSet::ResolveLoadOptions(
                context.ExePath, Host::LogDirectory(),
                ExtensionSet::DefaultStaticKeyDirectory(context.Paths.GameDir));
            const ExtensionSet::LoadResult load = ExtensionSet::LoadArchiveFile(context.DraftArchive, options);
            if (load.ok)
            {
                context.Meta = load.archive.meta;
                if (context.Meta.title.empty())
                {
                    context.Meta.title = ExeStem(context.ExePath);
                }
                if (context.Meta.date.empty())
                {
                    context.Meta.date = TodayText();
                }
            }
        }

        ExtensionSet::Meta edited = context.Meta;
        if (!EditMetaDialog(context, edited))
        {
            return;
        }
        context.Meta = edited;
        context.MakeNote = L"扩展集信息已更新（" + TimestampNow() + L"）";
        context.RenderedOverview.clear();
        UpdateOverview(context);

        if (FileExistsAt(context.DraftArchive))
        {
            Host::ShowInfo(context.Window,
                           L"扩展集信息已更新。\r\n\r\n档案里写着的还是上一次生成时的值 —— "
                           L"重新执行「生成当前游戏扩展集」后新信息才会写进档案。");
        }
        else
        {
            Host::ShowInfo(context.Window, L"扩展集信息已记录，生成时会写进档案。");
        }
    }

    void OnSetTest(UiContext& context)
    {
        SyncTargetFromEdit(context);
        if (!FileExistsAt(context.DraftArchive))
        {
            Host::ShowInfo(context.Window,
                           L"还没有当前游戏的扩展集档案，先执行「生成当前游戏扩展集」。\r\n\r\n" +
                               (context.DraftArchive.empty() ? std::wstring(L"（未选择目标游戏）")
                                                             : context.DraftArchive));
            return;
        }
        const std::wstring source = EffectiveTestDirectory(context);
        if (!DirectoryExistsAt(source))
        {
            Host::ShowInfo(context.Window,
                           L"测试来源目录不存在：\r\n" + (source.empty() ? std::wstring(L"（未设置）") : source) +
                               L"\r\n\r\n可以用「选择测试来源目录」指定一份解包目录。");
            return;
        }

        ResetJob(context, JobKind::DryRun, L"正在只读干跑（不复制、不重命名任何文件）……");
        context.Job.LoadOptions = ExtensionSet::ResolveLoadOptions(
            context.ExePath, Host::LogDirectory(),
            ExtensionSet::DefaultStaticKeyDirectory(context.Paths.GameDir));
        context.Job.ArchivePath = context.DraftArchive;
        context.Job.DryRunOptions.sourceDirectory = source;
        StartJob(context);
    }

    void OnSetOpenDraft(UiContext& context)
    {
        SyncTargetFromEdit(context);
        if (context.DraftDir.empty())
        {
            Host::ShowInfo(context.Window, L"请先选择目标游戏主程序（.exe）。");
            return;
        }
        std::wstring error;
        if (!EnsureDirectory(context.DraftDir, error))
        {
            Host::ShowError(context.Window, error);
            return;
        }
        Host::OpenInExplorer(context.Window, context.DraftDir);
    }

    void OnSetInstall(UiContext& context)
    {
        SyncTargetFromEdit(context);
        if (!FileExistsAt(context.DraftArchive))
        {
            Host::ShowInfo(context.Window,
                           L"还没有当前游戏的扩展集档案，先执行「生成当前游戏扩展集」。\r\n\r\n" +
                               (context.DraftArchive.empty() ? std::wstring(L"（未选择目标游戏）")
                                                             : context.DraftArchive));
            return;
        }
        if (context.LibraryRoot.empty())
        {
            context.LibraryRoot = ExtensionSet::DefaultLibraryRoot(Host::LoaderDirectory());
        }
        std::wstring error;
        if (!EnsureDirectory(context.LibraryRoot, error))
        {
            // 设计稿 §6 / §9：工具目录不可写时不静默回落，明确报错让用户自己决定放哪儿。
            Host::ShowError(context.Window, L"扩展集库目录不可写：\r\n" + context.LibraryRoot + L"\r\n\r\n" +
                                                error +
                                                L"\r\n\r\n请把本工具挪到可写的目录（例如桌面或 D 盘），再重新安装。");
            return;
        }
        if (!ExtensionSet::InstallToLibrary(context.LibraryRoot, context.DraftArchive, error))
        {
            Host::ShowError(context.Window, L"安装到扩展集库失败：\r\n" +
                                                (error.empty() ? std::wstring(L"（内核没有给出原因）") : error));
            return;
        }
        context.MakeNote = L"已安装到扩展集库：" + context.LibraryRoot + L"（" + TimestampNow() + L"）";
        Host::LogLine(Log::Level::Info, L"[YuaExtract] 安装扩展集：" + context.MakeNote);
        RefreshLibrary(context);
        context.RenderedOverview.clear();
        UpdateOverview(context);
        Host::ShowInfo(context.Window,
                       L"已安装到扩展集库：\r\n" + context.LibraryRoot + L"\r\n\r\n会社 / 作品：" +
                           (context.Meta.publisher.empty() ? std::wstring(L"（未填）") : context.Meta.publisher) +
                           L" / " + (context.Meta.title.empty() ? std::wstring(L"（未填）") : context.Meta.title) +
                           L"\r\n\r\n首页「该作扩展集提取」现在可以选到它了。");
    }

    void OnSetOpenLibrary(UiContext& context)
    {
        if (context.LibraryRoot.empty())
        {
            context.LibraryRoot = ExtensionSet::DefaultLibraryRoot(Host::LoaderDirectory());
        }
        std::wstring error;
        if (!EnsureDirectory(context.LibraryRoot, error))
        {
            Host::ShowError(context.Window, L"扩展集库目录不可用：\r\n" + context.LibraryRoot + L"\r\n\r\n" + error);
            return;
        }
        Host::OpenInExplorer(context.Window, context.LibraryRoot);
    }

    void OnSetPickSource(UiContext& context)
    {
        const std::wstring chosen =
            Host::BrowseForFolder(context.Window, L"选择扩展集来源目录（默认 Restored_Extractor_Output）");
        if (chosen.empty())
        {
            return;
        }
        context.SourceOverride = chosen;
        context.MakeNote = L"采集来源目录已改为：" + chosen + L"（只影响本次会话）";
        context.RenderedOverview.clear();
        UpdateOverview(context);
    }

    void OnSetPickTest(UiContext& context)
    {
        const std::wstring chosen =
            Host::BrowseForFolder(context.Window, L"选择测试来源目录（默认跟随采集来源）");
        if (chosen.empty())
        {
            return;
        }
        context.TestSourceOverride = chosen;
        context.MakeNote = L"测试来源目录已改为：" + chosen + L"（只影响本次会话）";
        context.RenderedOverview.clear();
        UpdateOverview(context);
    }

    // ---------- 「扩展集」页签的动作（设计稿 §8.5） ----------

    // 定义在「控件创建」一节之后（它要用 CreateButton）：在线更新弹窗。
    void OnlineUpdateDialog(UiContext& context);

    // 「运行环境自检」：只读。逐项给结论，失败项点名（设计稿 §13.9：成功不弹模态框，失败才弹）。
    //
    // 为什么要有它：这套工具对环境有四五处隐含要求（工具目录可写、Dumper 是 32 位、库清单能解析、
    // 每份档案自洽、盐日志读得到）。任何一处不对，表现出来的都是"点了某个按钮没反应"，
    // 而症状离原因隔着好几个环节（比如"在线更新装不进去"其实可能是工具目录在 Program Files 下）。
    // 一次列清楚，省掉这一轮轮猜。
    std::vector<SelfCheckItem> RunEnvironmentSelfCheck(UiContext& context)
    {
        std::vector<SelfCheckItem> items;
        const std::wstring toolDirectory = Host::LoaderDirectory();

        // 1. 工具目录可写 —— 扩展集库默认就在它下面，写不了的话半条链都是死的。
        {
            SelfCheckItem item;
            item.Label = L"工具目录可写";
            std::wstring error;
            if (ProbeDirectoryWritable(toolDirectory, error))
            {
                item.Detail = L"可写 —— " + toolDirectory;
            }
            else
            {
                item.Severity = 1;
                item.Detail = L"不可写（" + error + L"）—— " + toolDirectory +
                              L"\r\n  出路：把工具挪到可写目录（例如桌面或 D 盘），否则「安装到扩展集库」"
                              L"与「在线更新」都会失败。";
            }
            items.push_back(item);
        }

        // 2. Hash 收集模块：在不在 + 是不是 32 位 + 摘要（摘要只登记，不判对错）。
        {
            SelfCheckItem item;
            item.Label = L"Hash 收集模块 CxdecStringDumper.dll";
            const std::wstring path = Host::ResolveModulePath(L"CxdecStringDumper.dll");
            if (!FileExistsAt(path))
            {
                item.Severity = 1;
                item.Detail = L"找不到：" + (path.empty() ? std::wstring(L"（模块路径解析不出来）") : path) +
                              L"\r\n  " + Host::ModuleMissingHint(L"CxdecStringDumper.dll");
            }
            else
            {
                std::wstring detail = path;
                WORD machine = 0u;
                std::wstring error;
                if (!ReadPeMachineType(path, machine, error))
                {
                    item.Severity = 1;
                    detail += L"\r\n  读不出 PE 头（" + error + L"）：文件可能坏了或被换成了别的东西。";
                }
                else if (machine == IMAGE_FILE_MACHINE_I386)
                {
                    detail += L"\r\n  32 位（IMAGE_FILE_MACHINE_I386），能注进 32 位游戏进程。";
                }
                else
                {
                    item.Severity = 1;
                    detail += L"\r\n  不是 32 位（Machine = " +
                              Host::Format(L"%04X", static_cast<unsigned int>(machine)) +
                              L"）：本工具只支持 32 位游戏，这个 DLL 注不进去。"
                              L"\r\n  出路：换回随本工具发布的 32 位 CxdecStringDumper.dll"
                              L"（发布结构见「关于」页签的模块目录）。";
                }
                const std::wstring sha256 = ExtensionSet::Sha256HexFile(path);
                detail += L"\r\n  SHA-256：" + ShortDigest(sha256);
                item.Detail = detail;
            }
            items.push_back(item);
        }

        // 3. 库根：不存在就报"未创建"（**不算错** —— 一份扩展集都还没装过就是这样），存在则验可写。
        {
            SelfCheckItem item;
            item.Label = L"扩展集库目录";
            if (context.LibraryRoot.empty())
            {
                context.LibraryRoot = ExtensionSet::DefaultLibraryRoot(toolDirectory);
            }
            if (!DirectoryExistsAt(context.LibraryRoot))
            {
                item.Severity = 2;
                item.Detail = L"未创建 —— " + context.LibraryRoot +
                              L"\r\n  一份扩展集都还没装过就会是这样（不算错）：安装或在线更新之后它会自动建出来。";
            }
            else
            {
                std::wstring error;
                if (ProbeDirectoryWritable(context.LibraryRoot, error))
                {
                    item.Detail = L"存在且可写 —— " + context.LibraryRoot;
                }
                else
                {
                    item.Severity = 1;
                    item.Detail = L"不可写（" + error + L"）—— " + context.LibraryRoot +
                                  L"\r\n  出路：把工具挪到可写目录，或把这一份档案手工拷到别处去用；"
                                  L"库里这份读得出来但装不进去新的。";
                }
            }
            items.push_back(item);
        }

        // 4. 库清单：文件自己读（读到的就是字节），解析交给内核那一份实现 ——
        //    它在在线索引那条路上是同一个函数，这里过一遍等于把"本地能读"钉死。
        {
            SelfCheckItem item;
            item.Label = L"库清单 manifest.txt";
            const std::wstring path = JoinPath(context.LibraryRoot, ExtensionSet::kManifestFileName);
            if (!FileExistsAt(path))
            {
                item.Severity = 2;
                item.Detail = L"还没有这份文件（库是空的，不算错）—— " + path;
            }
            else
            {
                std::string bytes;
                std::wstring error;
                if (!ReadFileBytes(path, bytes, error))
                {
                    item.Severity = 1;
                    item.Detail = L"读不到：" + error;
                }
                else
                {
                    std::vector<ExtensionSet::LibraryEntry> entries;
                    if (ExtensionSet::ParseManifestText(bytes, entries, error))
                    {
                        item.Detail = L"可解析，共 " + FormatCount(entries.size()) + L" 条 —— " + path;
                        // 解析**成功**但 error 非空 = 里面有不合规、被跳过的行（不是整份失败）。
                        // 这一条必须报出来：否则"清单里少了一行"对用户是静默的（2026-10-15 补）。
                        if (!error.empty())
                        {
                            item.Severity = 1;
                            item.Detail += L"\r\n  但清单里有不合规的行，已被跳过：\r\n  " + error +
                                           L"\r\n  出路：按上面点名的行号修好 manifest.txt；改不动就把它改个名，"
                                           L"工具会改按目录里的 rules.int 重新列出库内容（清单里记的摘要与"
                                           L"时间会丢，下一次安装 / 在线更新会重新写出清单）。";
                        }
                    }
                    else
                    {
                        item.Severity = 1;
                        item.Detail = L"解析失败：" +
                                      (error.empty() ? std::wstring(L"（内核没有给出原因）") : error) +
                                      L"\r\n  " + path +
                                      L"\r\n  出路：换回上一份能用的清单（有备份的话）；或者把它改个名 ——"
                                      L"工具会按目录里的 rules.int 重新列出库内容，清单里记的摘要与时间会丢，"
                                      L"下一次安装 / 在线更新时清单会被重新写出来。";
                    }
                }
            }
            items.push_back(item);
        }

        // 5. 库里的每一份档案：存在 + 能解码 + 例外为 0 + 内嵌目标 exe 摘要非空。
        //    逐份报、坏的点名是哪一份：库里有两三份时，"有一份坏了"必须能定位到它。
        {
            SelfCheckItem item;
            item.Label = L"库内档案";
            if (context.Catalog.entries.empty())
            {
                item.Severity = 2;
                item.Detail = L"库里还没有档案（不算错）";
            }
            else
            {
                unsigned int bad = 0u;
                std::wstring detail;
                for (const ExtensionSet::LibraryEntry& entry : context.Catalog.entries)
                {
                    const std::wstring where = entry.publisher + L" / " + entry.title;
                    const std::wstring path = entry.archivePath;
                    std::wstring problem;
                    if (path.empty() || !FileExistsAt(path))
                    {
                        problem = L"档案文件不存在（" +
                                  (path.empty() ? std::wstring(L"路径未知") : path) + L"）";
                    }
                    else
                    {
                        ExtensionSet::Archive archive;
                        std::wstring error;
                        if (!ExtensionSet::ReadArchiveFile(path, archive, error))
                        {
                            problem = L"解码失败：" +
                                      (error.empty() ? std::wstring(L"（内核没有给出原因）") : error);
                        }
                        else if (!archive.exceptions.empty())
                        {
                            problem = L"档案里有 " + FormatCount(archive.exceptions.size()) +
                                      L" 条生成期自校验例外（名字 + 盐 → 哈希没算回去，设计稿 §3.2）";
                        }
                        else if (archive.identity.targetExe.sha256.empty())
                        {
                            problem = L"内嵌的目标 exe 摘要为空 —— 这份档案做不了归属校验（设计稿 §2.1）";
                        }
                    }
                    if (problem.empty())
                    {
                        continue;
                    }
                    ++bad;
                    // 坏得很多时不必把面板写满：前几条点名 + 总数就够定位了。
                    if (bad <= 5u)
                    {
                        detail += (detail.empty() ? std::wstring() : std::wstring(L"\r\n")) + where + L"：" +
                                  problem + (path.empty() ? std::wstring() : std::wstring(L"\r\n  ") + path);
                    }
                }
                if (bad == 0u)
                {
                    item.Detail = L"共 " + FormatCount(context.Catalog.entries.size()) +
                                  L" 份，全部可解码、无例外、带目标 exe 摘要";
                }
                else
                {
                    item.Severity = 1;
                    item.Detail = FormatCount(bad) + L" / " + FormatCount(context.Catalog.entries.size()) +
                                  L" 份有问题：\r\n  " + detail +
                                  L"\r\n  出路：坏的那一份用「制作扩展集」重新生成后安装覆盖，"
                                  L"或用「在线更新」重新装一份 —— 库里这一份本身是修不回来的。";
                }
            }
            items.push_back(item);
        }

        // 6. 盐来源：`Log\Universal.log` 里有没有 `Hash Seed:`。**只报告、不判错** ——
        //    没有它的时候工具本来就会回落到静态派生（ExtractKey_Output\Static），
        //    那是一条正当路径，不是缺陷。
        {
            SelfCheckItem item;
            item.Label = L"盐来源（Log\\Universal.log 的 Hash Seed:）";
            const std::wstring log = JoinPath(Host::LogDirectory(), L"Universal.log");
            if (!FileExistsAt(log))
            {
                item.Severity = 2;
                item.Detail = L"没有这份日志 —— " + log +
                              L"\r\n  （只报告不判错：没有它时盐走静态派生 ExtractKey_Output\\Static。）";
            }
            else if (FileContainsAscii(log, "Hash Seed:", 8ull * 1024ull * 1024ull))
            {
                item.Detail = L"读到 Hash Seed: —— " + log;
            }
            else
            {
                item.Severity = 2;
                item.Detail = L"这份日志里没读到 Hash Seed: —— " + log +
                              L"\r\n  （只报告不判错：盐可以来自静态派生 ExtractKey_Output\\Static。）";
            }
            items.push_back(item);
        }

        return items;
    }

    // 刷新列表：重扫库清单 → 落到详情面板（UpdateOverview 按当前页签分发，本页签落在 IDC_YUA_PAGE）。
    void OnSetRefresh(UiContext& context)
    {
        RefreshLibrary(context);
        if (!context.Catalog.ok)
        {
            const std::wstring error =
                context.Catalog.error.empty() ? std::wstring(L"（内核没有给出原因）") : context.Catalog.error;
            context.SetPageNote = L"刷新列表失败：" + error;
            Host::LogLine(Log::Level::Warn, L"[YuaExtract] 刷新扩展集列表失败：" + error);
            context.RenderedOverview.clear();
            UpdateOverview(context);
            Host::ShowError(context.Window,
                            L"扫扩展集库失败：\r\n" +
                                (context.LibraryRoot.empty() ? std::wstring(L"（库根未解析）")
                                                             : context.LibraryRoot) +
                                L"\r\n\r\n" + error);
            return;
        }

        std::wstring note = L"已刷新列表：会社 " + FormatCount(context.Catalog.publishers.size()) +
                            L" 个 / 扩展集 " + FormatCount(context.Catalog.entries.size()) +
                            L" 份（" + TimestampNow() + L"）";
        // 解析成功但清单里有不合规被跳过的行 —— 不是失败，但**不能静默**（2026-10-15 补）：
        // 进日志 + 在结论里点出来，详情面板另有一节「清单提示」写清是第几行。
        if (!context.Catalog.error.empty())
        {
            Host::LogLine(Log::Level::Warn, L"[YuaExtract] 扩展集清单提示：" + context.Catalog.error);
            note += L"；清单里有不合规的行已被跳过（详情面板「清单提示」）";
        }
        context.SetPageNote = note;
        Host::LogLine(Log::Level::Info, L"[YuaExtract] 刷新扩展集列表：" + note);
        context.RenderedOverview.clear();
        UpdateOverview(context);
    }

    // 打开扩展集（库根）目录。
    // **不静默新建**：库里一份东西都没有时给一个空目录，只会让人以为"库在这儿、东西呢"；
    // 该建目录的是安装与在线更新那两条路（它们建完就有东西）。
    void OnSetOpenLibraryRoot(UiContext& context)
    {
        if (context.LibraryRoot.empty())
        {
            context.LibraryRoot = ExtensionSet::DefaultLibraryRoot(Host::LoaderDirectory());
        }
        if (!DirectoryExistsAt(context.LibraryRoot))
        {
            const std::wstring text = L"扩展集库还没有创建：\r\n" + context.LibraryRoot +
                                      L"\r\n\r\n库里现在一份扩展集都没有，所以没有可打开的目录。\r\n\r\n"
                                      L"两条来路：\r\n"
                                      L"· 「制作扩展集」页签生成当前游戏的档案，再「安装到扩展集库」；\r\n"
                                      L"· 本页签「在线更新」从远端仓库下载安装。";
            context.SetPageNote = L"扩展集库不存在：" + context.LibraryRoot;
            Host::LogLine(Log::Level::Warn, L"[YuaExtract] 扩展集库目录不存在（未新建）：" + context.LibraryRoot);
            context.RenderedOverview.clear();
            UpdateOverview(context);
            Host::ShowInfo(context.Window, text);
            return;
        }
        Host::OpenInExplorer(context.Window, context.LibraryRoot);
    }

    // 打开选中的那一个扩展集目录：<库根>\<会社>\<作品>。
    void OnSetOpenEntry(UiContext& context)
    {
        if (context.SelectedPublisher.empty() || context.SelectedTitle.empty())
        {
            Host::ShowInfo(context.Window, L"先在左侧「扩展集选择」里选一个会社，再选一个作品。");
            return;
        }
        const std::wstring directory = SelectedLibraryDirectory(context);
        if (directory.empty() || !DirectoryExistsAt(directory))
        {
            const std::wstring text = L"这个扩展集在库里没有目录：\r\n" +
                                      (directory.empty() ? std::wstring(L"（会社或作品名里全是非法字符，拼不出目录名）")
                                                         : directory) +
                                      L"\r\n\r\n会社 / 作品是从列表里选出来的，目录却不在 —— 多半是有人在资源管理器里"
                                      L"手工删掉了。点「刷新列表」重新扫一遍库即可。";
            context.SetPageNote = L"打开失败：库内目录不存在";
            Host::LogLine(Log::Level::Warn, L"[YuaExtract] 打开选中扩展集失败（目录不存在）：" + directory);
            context.RenderedOverview.clear();
            UpdateOverview(context);
            Host::ShowInfo(context.Window, text);
            return;
        }
        if (Host::OpenInExplorer(context.Window, directory))
        {
            context.SetPageNote = L"已打开 " + directory;
            Host::LogLine(Log::Level::Info, L"[YuaExtract] 已打开扩展集目录：" + directory);
            context.RenderedOverview.clear();
            UpdateOverview(context);
        }
    }

    // 运行环境自检：结论写进详情面板与日志；**失败才弹框**（与干跑 §13.9 同一口径 ——
    // 成功也弹的话，每点一次就要多点一次"确定"，很快就被无视了）。
    void OnSetSelfCheck(UiContext& context)
    {
        context.SelfCheck = RunEnvironmentSelfCheck(context);
        context.HasSelfCheck = true;
        const unsigned int problems = SelfCheckProblemCount(context.SelfCheck);
        context.SetPageNote = L"运行环境自检：" + SelfCheckSummary(context.SelfCheck) + L"（" + TimestampNow() +
                              L"）";

        // 日志一行一条：把多行结论折成 " / "，别把日志格式撑坏（与干跑同一做法）。
        std::wstring flattened = SelfCheckPlainText(context.SelfCheck);
        for (size_t at = flattened.find(L"\r\n"); at != std::wstring::npos; at = flattened.find(L"\r\n"))
        {
            flattened.replace(at, 2u, L" / ");
        }
        Host::LogLine(problems == 0u ? Log::Level::Info : Log::Level::Warn,
                      L"[YuaExtract] 运行环境自检：" + flattened);

        context.RenderedOverview.clear();
        UpdateOverview(context);

        if (problems > 0u)
        {
            Host::ShowError(context.Window, SelfCheckPlainText(context.SelfCheck));
        }
    }

    // 扩展集留言（只读弹窗）。
    // **与制作页签图2 里那个「扩展集留言」输入框不是一回事**：那个往 META.comment 里写，
    // 这个只是把选中档案里已经写着的留言读出来给人看。
    void OnSetShowNote(UiContext& context)
    {
        if (context.SelectedPublisher.empty() || context.SelectedTitle.empty())
        {
            Host::ShowInfo(context.Window, L"先在左侧「扩展集选择」里选一个会社，再选一个作品，\r\n"
                                           L"然后点「扩展集留言」看这份档案的留言。");
            return;
        }
        const std::wstring path = SelectedArchivePath(context);
        if (path.empty() || !FileExistsAt(path))
        {
            Host::ShowError(context.Window,
                            L"找不到这份扩展集的档案：\r\n" +
                                (path.empty() ? std::wstring(L"（路径未知）") : path) +
                                L"\r\n\r\n点「刷新列表」重新扫一遍库；如果它确实不在库里，"
                                L"先在「制作扩展集」页签生成并安装，或用本页签「在线更新」下载安装。");
            return;
        }

        // 只读 META，所以走 ReadArchiveFile 而不是 LoadArchiveFile：
        // 后者会连归属校验一起跑，而归属结论属于首页「该作扩展集提取」的步骤1。
        // 在这里没有目标游戏、也没有当前盐，跑它几乎必然得到"未能校验归属"，
        // 拿这个当失败提示只会把人引偏（本按钮根本不做归属判断）。
        ExtensionSet::Archive archive;
        std::wstring error;
        if (!ExtensionSet::ReadArchiveFile(path, archive, error))
        {
            Host::ShowError(context.Window,
                            L"读不出这份扩展集档案：\r\n" + path + L"\r\n\r\n" +
                                (error.empty() ? std::wstring(L"（内核没有给出原因）") : error));
            Host::LogLine(Log::Level::Warn, L"[YuaExtract] 读扩展集留言失败：" + path + L"：" + error);
            return;
        }

        const ExtensionSet::Meta& meta = archive.meta;
        const auto orEmpty = [](const std::wstring& value) -> std::wstring {
            return value.empty() ? std::wstring(L"（空）") : value;
        };

        std::wstring text;
        text += L"会社：" + orEmpty(meta.publisher) + L"\r\n";
        text += L"作品：" + orEmpty(meta.title) + L"\r\n";
        text += L"贡献者：" + orEmpty(meta.contributor) + L"\r\n";
        text += L"版本：" + orEmpty(meta.version) + L"\r\n";
        text += L"日期：" + orEmpty(meta.date) + L"\r\n";
        if (!meta.generatedAt.empty())
        {
            text += L"生成时间：" + meta.generatedAt + L"\r\n";
        }
        if (!meta.toolVersion.empty())
        {
            text += L"生成工具：" + meta.toolVersion + L"\r\n";
        }
        text += L"\r\n扩展集留言：\r\n" +
                (meta.comment.empty() ? std::wstring(L"（这份档案没有写留言）") : meta.comment) + L"\r\n";
        text += L"\r\n档案里记的条数：目录 " + FormatCount(archive.stats.directories) + L" 条 / 文件 " +
                FormatCount(archive.stats.files) + L" 条 / 自校验例外 " + FormatCount(archive.stats.exceptions) +
                L" 条\r\n";
        text += L"档案摘要：" + ShortDigest(ExtensionSet::Sha256HexFile(path)) + L"\r\n";
        text += L"档案路径：" + path + L"\r\n";
        text += L"\r\n说明：档案里不存哈希层，所以本工具无法验证这份档案里的名字对不对 —— "
                L"名字的最终把关是事实：归于本作后用它还原，还原率不对就是不对。";

        Host::LogLine(Log::Level::Info,
                      L"[YuaExtract] 查看扩展集留言：" + context.SelectedPublisher + L" / " +
                          context.SelectedTitle + L" ← " + path);
        Host::ShowInfo(context.Window, text);
    }

    // 在线更新：弹窗自己负责拉索引 / 下载 / 入库；关掉之后主界面重新扫一次库，
    // 免得"刚装完却在列表里看不到"。
    void OnSetOnlineUpdate(UiContext& context)
    {
        OnlineUpdateDialog(context);
        RefreshLibrary(context);
        context.RenderedOverview.clear();
        UpdateOverview(context);
    }

    // ---------- 控件创建 ----------

    HWND CreateButton(HWND parent, UiContext* context, int id, const wchar_t* text, DWORD extraStyle = 0)
    {
        HWND button = ::CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | extraStyle,
                                        0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, context->Instance, nullptr);
        if (button)
        {
            ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(context->FontUi), TRUE);
        }
        return button;
    }

    HWND CreateGroup(HWND parent, UiContext* context, int id, const wchar_t* text, bool keepInHome)
    {
        HWND group = CreateButton(parent, context, id, text, BS_GROUPBOX);
        if (group && keepInHome)
        {
            context->HomeControls.push_back(group);
        }
        return group;
    }

    void CreateControls(UiContext& context)
    {
        HWND window = context.Window;

        // 页签：自绘按钮，选中态由 WM_DRAWITEM 画。
        for (int id : { IDC_YUA_TAB_HOME, IDC_YUA_TAB_MAKE_SET, IDC_YUA_TAB_SET, IDC_YUA_TAB_ABOUT })
        {
            CreateButton(window, &context, id, L"", BS_OWNERDRAW);
        }
        ::SetWindowTextW(::GetDlgItem(window, IDC_YUA_TAB_HOME), L"首页");
        ::SetWindowTextW(::GetDlgItem(window, IDC_YUA_TAB_MAKE_SET), L"制作扩展集");
        ::SetWindowTextW(::GetDlgItem(window, IDC_YUA_TAB_SET), L"扩展集");
        ::SetWindowTextW(::GetDlgItem(window, IDC_YUA_TAB_ABOUT), L"关于");

        // 页面占位（非首页时的内容区）
        HWND page = CreateTextView(window, &context, IDC_YUA_PAGE, false);
        ::ShowWindow(page, SW_HIDE);

        // 目标游戏一行：首页与「制作扩展集」页签共用，所以单独一组（不放进 HomeControls）。
        CreateGroup(window, &context, IDC_YUA_GROUP_EXE, L"游戏的exe程序", false);
        context.TargetControls.push_back(::GetDlgItem(window, IDC_YUA_GROUP_EXE));
        {
            HWND label = ::CreateWindowExW(0, L"STATIC", L"目标游戏", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0,
                                           10, 10, window, (HMENU)(INT_PTR)IDC_YUA_EXE_LABEL, context.Instance,
                                           nullptr);
            ::SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(context.FontUi), TRUE);
            context.TargetControls.push_back(label);

            HWND edit = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 10, 10,
                                          window, (HMENU)(INT_PTR)IDC_YUA_EXE_EDIT, context.Instance, nullptr);
            ::SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(context.FontUi), TRUE);
            context.TargetControls.push_back(edit);

            context.TargetControls.push_back(CreateButton(window, &context, IDC_YUA_EXE_BROWSE, L"选择"));
        }

        // 首页功能 = 互斥模式选择（BS_PUSHLIKE 让它看起来仍是一排按钮，
        // 但选中项会保持按下态；分组由第一个按钮的 WS_GROUP 起头）。
        CreateGroup(window, &context, IDC_YUA_GROUP_FUNC, L"首页功能", true);
        context.HomeControls.push_back(CreateButton(window, &context, IDC_YUA_FUNC_SET_EXTRACT, L"该作扩展集提取",
                                                    BS_PUSHLIKE | BS_AUTORADIOBUTTON | WS_GROUP));
        context.HomeControls.push_back(CreateButton(window, &context, IDC_YUA_FUNC_SET_CRACK, L"该会社合集撞新作",
                                                    BS_PUSHLIKE | BS_AUTORADIOBUTTON));
        context.HomeControls.push_back(CreateButton(window, &context, IDC_YUA_FUNC_CLASSIC, L"传统动态模式",
                                                    BS_PUSHLIKE | BS_AUTORADIOBUTTON));

        // 扩展集选择：会社 → 作品两级列表（设计稿 §8.3 图3 左列）。
        // 首页与「扩展集」页签**共用同一份**（设计稿 §8.5）：两个页签看的是同一份库清单、
        // 同一份选中状态，所以是同一组控件、只是摆放位置不同（进 LibControls 而不是 HomeControls）。
        CreateGroup(window, &context, IDC_YUA_GROUP_LIB, L"扩展集选择", false);
        context.LibControls.push_back(::GetDlgItem(window, IDC_YUA_GROUP_LIB));
        {
            HWND label = ::CreateWindowExW(0, L"STATIC", L"会社", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10,
                                           window, (HMENU)(INT_PTR)IDC_YUA_LIB_PUB_LABEL, context.Instance, nullptr);
            ::SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(context.FontUi), TRUE);
            context.LibControls.push_back(label);

            label = ::CreateWindowExW(0, L"STATIC", L"作品", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10, window,
                                      (HMENU)(INT_PTR)IDC_YUA_LIB_TITLE_LABEL, context.Instance, nullptr);
            ::SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(context.FontUi), TRUE);
            context.LibControls.push_back(label);

            HWND publishers = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY, 0, 0,
                                                10, 10, window, (HMENU)(INT_PTR)IDC_YUA_LIB_PUBLISHERS,
                                                context.Instance, nullptr);
            ::SendMessageW(publishers, WM_SETFONT, reinterpret_cast<WPARAM>(context.FontUi), TRUE);
            context.LibControls.push_back(publishers);

            HWND titles = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY, 0, 0,
                                            10, 10, window, (HMENU)(INT_PTR)IDC_YUA_LIB_TITLES, context.Instance,
                                            nullptr);
            ::SendMessageW(titles, WM_SETFONT, reinterpret_cast<WPARAM>(context.FontUi), TRUE);
            context.LibControls.push_back(titles);
        }

        CreateGroup(window, &context, IDC_YUA_GROUP_ADV, L"高级操作", true);
        context.HomeControls.push_back(
            CreateButton(window, &context, IDC_YUA_ADVANCED, L"CxdecExtractorLoader 原版功能"));

        CreateGroup(window, &context, IDC_YUA_GROUP_FLOW, L"主流程", true);
        context.HomeControls.push_back(CreateButton(window, &context, IDC_YUA_STEP1, L"步骤1：提取动态 XP3 资源"));
        context.HomeControls.push_back(
            CreateButton(window, &context, IDC_YUA_STEP2, L"步骤2：加载动态Hash收集模块"));
        context.HomeControls.push_back(
            CreateButton(window, &context, IDC_YUA_STEP3, L"步骤3：用动态Hash还原资源名"));

        CreateGroup(window, &context, IDC_YUA_GROUP_AUX, L"辅助操作", true);
        context.HomeControls.push_back(CreateButton(window, &context, IDC_YUA_AUX_CHECK, L"检查状态"));
        context.HomeControls.push_back(CreateButton(window, &context, IDC_YUA_AUX_WORKSPACE, L"打开工作区"));
        context.HomeControls.push_back(CreateButton(window, &context, IDC_YUA_AUX_CLEAN, L"清理目录"));
        context.HomeControls.push_back(CreateButton(window, &context, IDC_YUA_AUX_RESTOREDIR, L"打开还原目录"));
        context.HomeControls.push_back(CreateButton(window, &context, IDC_YUA_AUX_REPORT, L"查看还原报告"));
        {
            HWND hint = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10,
                                          window, (HMENU)(INT_PTR)IDC_YUA_HINT, context.Instance, nullptr);
            ::SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(context.FontUi), TRUE);
            context.HomeControls.push_back(hint);
        }

        context.HomeControls.push_back(CreateTextView(window, &context, IDC_YUA_OVERVIEW, false));

        // 「制作扩展集」页签的控件（设计稿 §8.1）。不画 VoicePattern —— 一期没有这一栏。
        CreateGroup(window, &context, IDC_YUA_SET_GROUP_MAKE, L"制作扩展集", false);
        context.MakeControls.push_back(::GetDlgItem(window, IDC_YUA_SET_GROUP_MAKE));
        context.MakeControls.push_back(CreateButton(window, &context, IDC_YUA_SET_GENERATE, L"生成当前游戏扩展集"));
        context.MakeControls.push_back(CreateButton(window, &context, IDC_YUA_SET_EDITINFO, L"编辑当前扩展集信息"));
        context.MakeControls.push_back(CreateButton(window, &context, IDC_YUA_SET_TEST, L"测试当前扩展集"));

        CreateGroup(window, &context, IDC_YUA_SET_GROUP_DIR, L"目录", false);
        context.MakeControls.push_back(::GetDlgItem(window, IDC_YUA_SET_GROUP_DIR));
        context.MakeControls.push_back(CreateButton(window, &context, IDC_YUA_SET_OPEN_DRAFT, L"打开当前扩展集目录"));
        context.MakeControls.push_back(CreateButton(window, &context, IDC_YUA_SET_INSTALL, L"安装到扩展集库"));
        context.MakeControls.push_back(CreateButton(window, &context, IDC_YUA_SET_OPEN_LIB, L"打开扩展集库目录"));

        CreateGroup(window, &context, IDC_YUA_SET_GROUP_CUSTOM, L"自定义", false);
        context.MakeControls.push_back(::GetDlgItem(window, IDC_YUA_SET_GROUP_CUSTOM));
        context.MakeControls.push_back(CreateButton(window, &context, IDC_YUA_SET_PICK_SOURCE, L"选择扩展集来源目录"));
        context.MakeControls.push_back(CreateButton(window, &context, IDC_YUA_SET_PICK_TEST, L"选择测试来源目录"));

        context.MakeControls.push_back(CreateTextView(window, &context, IDC_YUA_SET_STATUS, false));

        // 「扩展集」页签的控件（设计稿 §8.5）：左列「扩展集管理与浏览」两行三列。
        // 列表用的是上面那组共用的 LibControls；右栏详情复用 IDC_YUA_PAGE（在布局里摆）。
        CreateGroup(window, &context, IDC_YUA_SETM_GROUP_BROWSE, L"扩展集管理与浏览", false);
        context.SetPageControls.push_back(::GetDlgItem(window, IDC_YUA_SETM_GROUP_BROWSE));
        context.SetPageControls.push_back(CreateButton(window, &context, IDC_YUA_SETM_REFRESH, L"刷新列表"));
        context.SetPageControls.push_back(
            CreateButton(window, &context, IDC_YUA_SETM_OPEN_ROOT, L"打开扩展集目录"));
        context.SetPageControls.push_back(
            CreateButton(window, &context, IDC_YUA_SETM_OPEN_ENTRY, L"打开选中扩展集"));
        context.SetPageControls.push_back(CreateButton(window, &context, IDC_YUA_SETM_UPDATE, L"在线更新"));
        context.SetPageControls.push_back(CreateButton(window, &context, IDC_YUA_SETM_SELFCHECK, L"运行环境自检"));
        context.SetPageControls.push_back(CreateButton(window, &context, IDC_YUA_SETM_NOTE, L"扩展集留言"));

        // 任务进度一行：首页与制作页签共用。
        CreateGroup(window, &context, IDC_YUA_GROUP_TASK, L"任务进度", false);
        context.TaskControls.push_back(::GetDlgItem(window, IDC_YUA_GROUP_TASK));
        {
            HWND bar = ::CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, window,
                                         (HMENU)(INT_PTR)IDC_YUA_TASK_PROGRESS, context.Instance, nullptr);
            ::SendMessageW(bar, PBM_SETRANGE32, 0, 100);
            ::SendMessageW(bar, PBM_SETPOS, 0, 0);
            context.TaskControls.push_back(bar);

            context.TaskControls.push_back(CreateButton(window, &context, IDC_YUA_TASK_CANCEL, L"取消"));

            HWND text = ::CreateWindowExW(0, L"STATIC", L"（空闲）", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 10, 10,
                                          window, (HMENU)(INT_PTR)IDC_YUA_TASK_TEXT, context.Instance, nullptr);
            ::SendMessageW(text, WM_SETFONT, reinterpret_cast<WPARAM>(context.FontUi), TRUE);
            context.TaskControls.push_back(text);
        }
        if (HWND cancel = ::GetDlgItem(window, IDC_YUA_TASK_CANCEL))
        {
            ::EnableWindow(cancel, FALSE);
        }

        HWND logView = CreateTextView(window, &context, IDC_YUA_LOGVIEW, true);
        ::ShowWindow(logView, SW_HIDE);

        CreateButton(window, &context, IDC_YUA_LOGBTN, L"显示详细日志 ▼");
    }

    // ---------- 「在线更新」弹窗（设计稿 §8.5 / §14） ----------
    //
    // 与图2 同一手法：不新增对话框资源（那要动 .rc / resource.h），注册一个自己的顶层窗口类 +
    // 手工建控件 + 自己跑一段模态消息循环。放在这里是因为它直接用 CreateButton。
    //
    // 与图2 的两处不同，都是设计稿明确要求的：
    //   1. 网络动作（拉索引 / 下载）跑在**后台线程**，进度走自己的私有消息 ——
    //      绝不在界面线程里同步联网：一次连接超时就是几十秒的假死；
    //   2. **弹窗必须显示当前基址**（§14.5）：基址可换镜像，不显示的话
    //      "点了没反应却不知道在打哪个地址"。

    struct OnlineDialogState
    {
        UiContext* Context = nullptr;
        HWND Window = nullptr;
        bool Done = false;
        // 当前**实际生效**的基址（归一化之后的形态，来自环境变量 / ini / 常量）。
        // 它不是「输入框里的文字」：输入框里是**用户敲的原文**（可能还没生效、甚至根本不能用），
        // 两者要分开显示 —— 混成一个才会出现"改了地址却像没改"（见 ApplyOnlineEndpoint，验证 P-2）。
        std::wstring EffectiveBase;
        std::vector<ExtensionSet::LibraryEntry> Entries; // 远端索引
        OnlineJob Job;
        bool JobActive = false;
    };

    void PlaceOnlineControl(HWND control, int x, int y, int width, int height)
    {
        if (control)
        {
            ::SetWindowPos(control, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    void SetOnlineStatus(OnlineDialogState& state, const std::wstring& text)
    {
        ::SetWindowTextW(::GetDlgItem(state.Window, IDC_YUA_ONLINE_STATUS), text.c_str());
    }

    void SetOnlineProgress(OnlineDialogState& state, unsigned int percent)
    {
        if (HWND bar = ::GetDlgItem(state.Window, IDC_YUA_ONLINE_PROGRESS))
        {
            ::SendMessageW(bar, PBM_SETPOS, percent > 100u ? 100u : percent, 0);
        }
    }

    // 生效基址的显示形态。**空串不能就这么印出去**：它说明配置里的地址过不了 https 校验
    //（NormalizeBase 把非 https 折成空串），印成空白用户会以为"我从没配过地址"（验证 P-2）。
    std::wstring DescribeBase(const std::wstring& base)
    {
        return base.empty() ? std::wstring(L"（无：配置里的地址没通过 https 校验）") : base;
    }

    // 请求地址的展示形态：基址能用就给完整 URL；不能用就**明说拼不出来**，
    // 具体是哪个基址由紧跟着的「请求基址：」那一行给（同一件事不重复印两遍）。
    std::wstring DescribeRequestUrl(const std::wstring& url)
    {
        return url.empty() ? std::wstring(L"（基址不可用，拼不出请求地址）") : url;
    }

    // 打开弹窗时填进「仓库地址」框里的初始文字。
    //
    // **这一行是 task-1 "原始串"接口的接入点**：§14.5 要求弹窗显示当前基址，而
    // ResolveRepositoryBase() 返回的是**归一化后**的值 —— 配置里写的是 http/ftp 时它返回空串，
    // 框就会变成空的，用户等于看见"我从没填过地址"（这就是 P-2 的现象）。
    // task-1 把"配置里的原文"那条路补出来之后，把这一行换成那个接口即可，其余逻辑不用动。
    std::wstring ConfiguredEndpointText()
    {
        return Online::ResolveRepositoryBase();
    }

    // 「仓库地址」框里的原文（用户敲什么就是什么，**不**先归一化）。
    std::wstring RawEndpointText(const OnlineDialogState& state)
    {
        return GetControlText(::GetDlgItem(state.Window, IDC_YUA_ONLINE_ENDPOINT));
    }

    // 起联网请求时用的基址 = 框里的原文。
    //
    // **不在这里自己 NormalizeBase**：归一化会把"非 https / 空"折成空串，那段信息就没了 ——
    // 内核的报错只能引用它拿到的那个串，传空串进去，用户就会看到"基址不可用：\"\"" 以及
    // 与事实相反的"地址也不能为空"（验证 P-2）。原文照传，内核才报得出"你填的是 X，X 不行"。
    // 框里是空的（用户没改过地址）才回落到配置解析出来的生效值。
    std::wstring EndpointForRequest(const OnlineDialogState& state)
    {
        const std::wstring typed = RawEndpointText(state);
        return typed.empty() ? state.EffectiveBase : typed;
    }

    // 状态栏（4145）里"当前生效基址 / 配置文件"那两行。任何改写基址的路径都走它，
    // 免得"输入框一个值、状态栏另一个值、日志第三个值"。
    // 跑任务时**不动**状态栏：那是进度与结论的位置，正有东西在写它。
    void UpdateOnlineBaseLine(OnlineDialogState& state, const std::wstring& note)
    {
        if (state.JobActive)
        {
            return;
        }
        std::wstring text = L"当前生效基址：" + DescribeBase(state.EffectiveBase) + L"\r\n配置文件：" +
                            Online::ConfigFilePath();
        if (!note.empty())
        {
            text += L"\r\n" + note;
        }
        SetOnlineStatus(state, text);
    }

    // 忙碌时两个动作按钮都禁用；空闲时按"有没有远端条目"决定 INSTALL 能不能点。
    // §8.5 驱动约定 5：驱动方就是靠 IDC_YUA_ONLINE_INSTALL 的 enable 状态判联网动作结束。
    void RefreshOnlineButtons(OnlineDialogState& state)
    {
        ::EnableWindow(::GetDlgItem(state.Window, IDC_YUA_ONLINE_REFRESH), state.JobActive ? FALSE : TRUE);
        ::EnableWindow(::GetDlgItem(state.Window, IDC_YUA_ONLINE_INSTALL),
                       (state.JobActive || state.Entries.empty()) ? FALSE : TRUE);
    }

    int OnlineSelectedIndex(const OnlineDialogState& state)
    {
        HWND list = ::GetDlgItem(state.Window, IDC_YUA_ONLINE_LIST);
        if (!list)
        {
            return -1;
        }
        const int index = static_cast<int>(::SendMessageW(list, LB_GETCURSEL, 0, 0));
        if (index == LB_ERR || static_cast<size_t>(index) >= state.Entries.size())
        {
            return -1;
        }
        return index;
    }

    // 每条远端条目在本地是什么状态（新 / 同一份 / 更新 / 本地更全）。
    // 列表与总结论都从这一份结果出，不各算一遍 —— 两处口径分叉的话，
    // "列表写着新增、结论却说更新"这种事立刻就会出现。
    std::vector<Online::EntryState> ClassifyRemoteEntries(const OnlineDialogState& state)
    {
        std::vector<Online::EntryState> states;
        states.reserve(state.Entries.size());
        const ExtensionSet::LibraryCatalog local =
            ExtensionSet::EnumerateLibrary(state.Context->LibraryRoot);
        for (const ExtensionSet::LibraryEntry& entry : state.Entries)
        {
            states.push_back(Online::Classify(entry, local));
        }
        return states;
    }

    void RefillOnlineList(OnlineDialogState& state, const std::vector<Online::EntryState>& states, int preferred)
    {
        HWND list = ::GetDlgItem(state.Window, IDC_YUA_ONLINE_LIST);
        if (!list)
        {
            return;
        }
        ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (size_t index = 0u; index < state.Entries.size(); ++index)
        {
            const ExtensionSet::LibraryEntry& entry = state.Entries[index];
            // 一行的口径就是设计稿说的那四项：会社 / 作品 / 条数 / 状态。
            const std::wstring label = entry.publisher + L" / " + entry.title + L" / " +
                                       FormatCount(entry.entryCount) + L" 条 / " +
                                       Online::DescribeState(index < states.size() ? states[index]
                                                                                  : Online::EntryState::New);
            ::SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        if (preferred >= 0 && static_cast<size_t>(preferred) < state.Entries.size())
        {
            ::SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(preferred), 0);
        }
        else if (!state.Entries.empty())
        {
            ::SendMessageW(list, LB_SETCURSEL, 0, 0);
        }
    }

    // 失败措辞：网络层与内核已经给了各自的明确原因（§14.6：每种失败各有各的措辞，
    // 不许合并成"更新失败"一句），这里只兜"什么都没给"和"用户取消"两种。
    std::wstring OnlineFailureText(const std::wstring& error, bool cancelled)
    {
        if (!error.empty())
        {
            return error;
        }
        return cancelled ? std::wstring(L"已取消。") : std::wstring(L"（没有给出原因）");
    }

    void SetOnlineJobStage(OnlineJob& job, unsigned int percent, const std::wstring& stage)
    {
        if (job.LockReady)
        {
            ::EnterCriticalSection(&job.Lock);
        }
        job.Percent = static_cast<LONG>(percent);
        job.Stage = stage;
        if (job.LockReady)
        {
            ::LeaveCriticalSection(&job.Lock);
        }
    }

    // 任务线程。只碰 OnlineJob 与 PostMessage，不碰任何控件。
    //
    // **下载任务不做入库**：入库前可能要问用户"要不要覆盖本地那份"（§14.4 第 7 条），
    // 而确认框是模态的 —— 在任务线程里弹模态框会把那个线程卡在 UI 里，进度与取消全乱套。
    // 所以这里只做"拿到字节 + 校验"，决策与入库回界面线程做。
    DWORD WINAPI OnlineJobThreadProc(LPVOID parameter)
    {
        OnlineDialogState* state = static_cast<OnlineDialogState*>(parameter);
        OnlineJob& job = state->Job;
        const HWND notify = state->Window;

        const auto cancelled = [&job](void) -> bool
        {
            return ::InterlockedCompareExchange(&job.Cancel, 0, 0) != 0;
        };
        const auto progress = [&job, notify](int percent, const std::wstring& stage)
        {
            SetOnlineJobStage(job, percent < 0 ? 0u : static_cast<unsigned int>(percent), stage);
            ::PostMessageW(notify, kOnlineProgressMessage, 0, 0);
        };

        Online::Request request;
        request.cancel = cancelled;
        request.progress = progress;

        switch (job.Kind)
        {
            case OnlineJobKind::FetchIndex:
            {
                job.Entries.clear();
                job.Ok = Online::FetchIndex(job.Base, job.Entries, job.Index, job.Error, request);
                break;
            }
            case OnlineJobKind::Download:
            {
                // 自己一份条目：线程里只读，不跟界面线程共用同一个对象。
                const ExtensionSet::LibraryEntry entry = job.Entry;
                if (!Online::DownloadArchive(job.Base, entry, job.StagingDirectory, job.Download, job.Error,
                                             request))
                {
                    break; // Ok 保持 false
                }
                job.Ok = Online::VerifyArchive(job.Download.path, entry, job.VerifyDetail, job.Error);
                break;
            }
            default:
                break;
        }

        ::InterlockedExchange(&job.Done, 1);
        ::PostMessageW(notify, kOnlineDoneMessage, 0, 0);
        return 0;
    }

    bool StartOnlineJob(OnlineDialogState& state)
    {
        if (state.JobActive)
        {
            return false; // 忙时按钮本来就是禁用的，这里只是兜底
        }
        OnlineJob& job = state.Job;
        ::InterlockedExchange(&job.Cancel, 0);
        ::InterlockedExchange(&job.Done, 0);
        ::InterlockedExchange(&job.Percent, 0);
        job.Ok = false;
        job.Error.clear();
        job.Thread = ::CreateThread(nullptr, 0u, OnlineJobThreadProc, &state, 0u, nullptr);
        if (!job.Thread)
        {
            job.Kind = OnlineJobKind::None;
            Host::ShowError(state.Window, L"启动联网任务线程失败。");
            return false;
        }
        // 降一档优先级：下载是纯 I/O，不该跟界面重绘抢 CPU。
        ::SetThreadPriority(job.Thread, THREAD_PRIORITY_BELOW_NORMAL);
        state.JobActive = true;
        RefreshOnlineButtons(state);
        SetOnlineProgress(state, 0);
        return true;
    }

    // 收线程。窗口销毁之前**必须**调：线程会往 state 里写结果，而 state 是
    // OnlineUpdateDialog 的栈对象 —— 线程还没退就销毁窗口，之后它就在往已经析构的对象上写。
    void StopOnlineJob(OnlineDialogState& state)
    {
        if (!state.Job.Thread)
        {
            return;
        }
        ::InterlockedExchange(&state.Job.Cancel, 1);
        ::WaitForSingleObject(state.Job.Thread, 30000);
        ::CloseHandle(state.Job.Thread);
        state.Job.Thread = nullptr;
        state.JobActive = false;
    }

    // 把「仓库地址」输入框里的值回写 ini（失败明确提示）。离开输入框、或点动作按钮时都调一次。
    //
    // 失败时**保留用户敲进去的原文**，绝不拿"清空输入框"来表达"没生效"：
    // 他要照着这句话去改，把他填的那一串抹掉，等于告诉他"你从没填过地址"（验证 P-2）。
    // "当前生效的是哪个"另行写在状态栏里（UpdateOnlineBaseLine），两件事分开说。
    //
    // reportFailure：离开输入框时为 true（当场告诉他这个地址没生效）；
    // 点动作按钮时为 false —— 那一步会拿着同一串原文去联网，由内核用自己的措辞报错，
    // 同一件事连弹两个框只会让人把第一个关掉、然后对着第二个发呆。
    void ApplyOnlineEndpoint(OnlineDialogState& state, bool reportFailure)
    {
        HWND edit = ::GetDlgItem(state.Window, IDC_YUA_ONLINE_ENDPOINT);
        const std::wstring typed = GetControlText(edit);
        if (typed.empty())
        {
            // 框是空的 = 不改配置（用回配置里解析出来的地址）。**不去填框**：填了就等于替用户
            // 写了他没写过的东西；"现在用的是哪个"由状态栏说。
            UpdateOnlineBaseLine(state, L"「仓库地址」为空：联网时用配置里的地址。");
            return;
        }

        // 已经就是配置里那个生效值（只是写法上多了空格之类）就不用回写。
        const std::wstring normalized = Online::NormalizeBase(typed);
        if (!normalized.empty() && normalized == state.EffectiveBase)
        {
            return;
        }

        // **把原文交给内核**（不是自己归一化过的那个）：它才知道"哪里不合法"（非 https / 空 /
        // 占位符 / URL 里不能出现的字符），报错也才引得到用户填的那一串。自己先归一化一遍
        // 会把原文折没（非 https 折成空串），报错就只剩一对引号（验证 P-2 的根）。
        std::wstring error;
        if (!Online::SetRepositoryBase(typed, error))
        {
            UpdateOnlineBaseLine(state, L"「仓库地址」没有生效 —— 输入框里保留的是你填的原文。");
            Host::LogLine(Log::Level::Warn, L"[YuaExtract] 在线更新基址未生效（保留原文 \"" + typed +
                                                L"\"）：" + error);
            if (reportFailure)
            {
                Host::ShowError(state.Window,
                                L"这个「仓库地址」没能生效。\r\n\r\n当前生效基址：" +
                                    DescribeBase(state.EffectiveBase) + L"\r\n\r\n" +
                                    (error.empty() ? std::wstring(L"（没有给出原因）") : error) +
                                    L"\r\n\r\n配置文件：" + Online::ConfigFilePath());
            }
            return;
        }

        // 成功：把归一化后的生效值写回框里 —— 这时"框里显示的"就是"真在打的地址"（§14.5）。
        state.EffectiveBase = normalized;
        ::SetWindowTextW(edit, normalized.c_str());
        UpdateOnlineBaseLine(state, L"「仓库地址」已生效并回写配置。");
        Host::LogLine(Log::Level::Info, L"[YuaExtract] 在线更新基址已回写：" + normalized + L" → " +
                                            Online::ConfigFilePath());
    }

    // 拉索引（4141）。
    void OnOnlineRefresh(OnlineDialogState& state)
    {
        // 用户可能改完地址没离开输入框就直接点按钮，所以动作入口也要落实一次基址
        //（失败不在这里报，交给下面的联网步骤报）。
        ApplyOnlineEndpoint(state, false);

        state.Entries.clear();
        if (HWND list = ::GetDlgItem(state.Window, IDC_YUA_ONLINE_LIST))
        {
            ::SendMessageW(list, LB_RESETCONTENT, 0, 0);
        }

        OnlineJob& job = state.Job;
        job.Kind = OnlineJobKind::FetchIndex;
        // **传框里的原文**（P-2）：不可用的地址由内核在自己的入口上报，
        // 报错里才引得到用户填的那一串，而不是一个空串。
        job.Base = EndpointForRequest(state);
        job.StagingDirectory = Online::DefaultStagingDirectory();
        job.Entry = ExtensionSet::LibraryEntry{};
        job.Entries.clear();
        job.Index = Online::IndexResult{};
        job.Download = Online::DownloadResult{};
        job.VerifyDetail.clear();
        job.Error.clear();
        job.Stage = L"正在拉取远端索引……";

        // 请求基址与生效基址**都显示**：前者是这次真正要打的地址（用户敲的原文），
        // 后者是配置里现在生效的那个；两者不同的时候，用户一眼能看出"我改的没生效"（P-2）。
        SetOnlineStatus(state,
                        L"正在拉取索引：\r\n" + DescribeRequestUrl(Online::IndexUrl(job.Base)) +
                            L"\r\n\r\n请求基址：" + DescribeBase(job.Base) + L"\r\n当前生效基址：" +
                            DescribeBase(state.EffectiveBase) + L"\r\n配置文件：" + Online::ConfigFilePath());
        if (StartOnlineJob(state))
        {
            Host::LogLine(Log::Level::Info, L"[YuaExtract] 在线更新：拉取索引（请求基址 " + job.Base + L"）");
        }
    }

    // 下载并安装选中（4143）：下载 + 校验在后台线程，覆盖决策与入库回界面线程（见线程体注释）。
    void OnOnlineInstall(OnlineDialogState& state)
    {
        ApplyOnlineEndpoint(state, false);

        const int index = OnlineSelectedIndex(state);
        if (index < 0)
        {
            Host::ShowInfo(state.Window, L"先在上面的列表里选一条远端扩展集，再点「下载并安装选中」。");
            return;
        }
        const ExtensionSet::LibraryEntry entry = state.Entries[static_cast<size_t>(index)];

        // 本地已经是同一份（档案摘要相同）就不下载了：再下一遍一模一样的字节、再覆盖一遍，
        // 本地什么都不会变，白跑一次网络（§14.6 要求任何失败都不动本地，成功也同理）。
        const ExtensionSet::LibraryCatalog local =
            ExtensionSet::EnumerateLibrary(state.Context->LibraryRoot);
        if (Online::Classify(entry, local) == Online::EntryState::Same)
        {
            const std::wstring text = entry.publisher + L" / " + entry.title +
                                      L" 本地已经是同一份（档案摘要相同），无需下载。";
            SetOnlineStatus(state, text);
            Host::LogLine(Log::Level::Info, L"[YuaExtract] 在线更新：" + text);
            return;
        }

        OnlineJob& job = state.Job;
        job.Kind = OnlineJobKind::Download;
        // 与拉索引同理：**传框里的原文**，让内核在自己的入口上报出"你填的这串不行"（P-2）。
        job.Base = EndpointForRequest(state);
        job.StagingDirectory = Online::DefaultStagingDirectory();
        job.Entry = entry;
        job.Entries.clear();
        job.Index = Online::IndexResult{};
        job.Download = Online::DownloadResult{};
        job.VerifyDetail.clear();
        job.Error.clear();
        job.Stage = L"正在下载扩展集档案……";

        SetOnlineStatus(state,
                        L"正在下载：" + entry.publisher + L" / " + entry.title + L"\r\n" +
                            DescribeRequestUrl(Online::ArchiveUrl(job.Base, entry.publisher, entry.title)) +
                            L"\r\n\r\n请求基址：" + DescribeBase(job.Base) + L"\r\n暂存目录：" +
                            job.StagingDirectory);
        if (StartOnlineJob(state))
        {
            Host::LogLine(Log::Level::Info,
                          L"[YuaExtract] 在线更新：开始下载 " + entry.publisher + L" / " + entry.title);
        }
    }

    // 校验通过之后的入库。**必须在界面线程**：覆盖前要问用户，而确认框是模态的。
    void InstallVerifiedArchive(OnlineDialogState& state, OnlineJob& job)
    {
        UiContext& context = *state.Context;
        const ExtensionSet::LibraryEntry remote = job.Entry;

        // 库根不存在就建（与「安装到扩展集库」同一口径：建不出来明确报错，不静默回落）。
        if (context.LibraryRoot.empty())
        {
            context.LibraryRoot = ExtensionSet::DefaultLibraryRoot(Host::LoaderDirectory());
        }
        std::wstring error;
        if (!EnsureDirectory(context.LibraryRoot, error))
        {
            Online::CleanupStaging(job.StagingDirectory);
            const std::wstring text = L"扩展集库目录不可写：\r\n" + context.LibraryRoot + L"\r\n" + error +
                                      L"\r\n\r\n本地库未被改动。";
            SetOnlineStatus(state, text);
            Host::LogLine(Log::Level::Warn, L"[YuaExtract] 在线更新：入库失败（库目录不可写）" + error);
            Host::ShowError(state.Window, text);
            return;
        }

        const ExtensionSet::LibraryCatalog local = ExtensionSet::EnumerateLibrary(context.LibraryRoot);
        const Online::EntryState entryState = Online::Classify(remote, local);
        const ExtensionSet::LibraryEntry* previous = FindLibraryEntry(local, remote.publisher, remote.title);

        // §14.4 第 7 条：本地已有、但档案摘要不同 → 先问用户（列出新旧条数/摘要），不静默覆盖。
        if (entryState == Online::EntryState::Update || entryState == Online::EntryState::LocalNewer)
        {
            std::wstring question = L"本地已经有这一份扩展集，且与远端这份的档案摘要不同：\r\n\r\n";
            question += L"本地：" + FormatCount(previous ? previous->entryCount : 0u) + L" 条，档案摘要 " +
                        ShortDigest(previous ? previous->archiveDigest : std::wstring()) + L"\r\n";
            question += L"远端：" + FormatCount(remote.entryCount) + L" 条，档案摘要 " +
                        ShortDigest(remote.archiveDigest) + L"\r\n\r\n";
            if (entryState == Online::EntryState::LocalNewer)
            {
                // 这条要比普通覆盖说得更重：远端条数更少，覆盖完能解出的名字会**变少**。
                question += L"注意：远端这份的条数比本地少 —— 覆盖之后本地能解出的名字会变少"
                            L"（覆盖度下降，且本地那份找不回来）。\r\n\r\n";
            }
            question += L"点「是」= 用远端这份覆盖本地；点「否」= 不动本地（暂存文件照旧清掉）。";

            if (!Host::Confirm(state.Window, question, L"覆盖本地扩展集？"))
            {
                Online::CleanupStaging(job.StagingDirectory);
                const std::wstring text = L"已取消：本地库里的 " + remote.publisher + L" / " + remote.title +
                                          L" 保持原样，没有写入本地库。";
                SetOnlineStatus(state, text);
                Host::LogLine(Log::Level::Info, L"[YuaExtract] 在线更新：用户取消覆盖 " + remote.publisher +
                                                    L" / " + remote.title);
                return;
            }
        }

        // 清单的「来源」列写**远端 URL**，不写 job.Download.path：
        // 那个路径是暂存文件，下面 CleanupStaging 一走就没了，写进清单等于记一个死路径
        //（独立验证 P-3；本地制作那条路没这个问题，它传的草案档案本来就长期留在盘上）。
        // ArchiveUrl 拼不出名字时返回空串 → 内核自动回退成 archivePath，不会写出半截值。
        if (!ExtensionSet::InstallToLibrary(context.LibraryRoot, job.Download.path, error,
                                            Online::ArchiveUrl(job.Base, remote.publisher, remote.title)))
        {
            Online::CleanupStaging(job.StagingDirectory);
            // **不在这里替内核下断言**：本地库到底动没动，只有 InstallToLibrary 自己知道 ——
            // 它可能已经写完 rules.int 才在清单那一步失败（那种回不去，error 里会写明
            // "本地库与操作前不一致"）。UI 再无条件下补一句"本地库未被改动"，
            // 就会在同一个弹窗里和它自相矛盾：等于对用户说两种结论（独立验证 P-3 的后续）。
            // 本地库状态一律以 error 里那句为准，这里只负责把 error 原样摆出来。
            const std::wstring text = L"安装失败：\r\n" +
                                      (error.empty() ? std::wstring(L"（内核没有给出原因）") : error);
            SetOnlineStatus(state, text);
            Host::LogLine(Log::Level::Warn, L"[YuaExtract] 在线更新：安装失败：" + error);
            Host::ShowError(state.Window, text);
            return;
        }
        Online::CleanupStaging(job.StagingDirectory);

        // 结论要能区分「新增」与「更新（覆盖了哪一条、旧档案摘要是什么）」（§14.6）。
        std::wstring text;
        if (previous)
        {
            text = L"已更新：" + remote.publisher + L" / " + remote.title + L"\r\n"
                   L"旧档案摘要：" + ShortDigest(previous->archiveDigest) + L"（" +
                       FormatCount(previous->entryCount) + L" 条）\r\n"
                   L"新档案摘要：" + ShortDigest(remote.archiveDigest) + L"（" +
                       FormatCount(remote.entryCount) + L" 条）\r\n";
        }
        else
        {
            text = L"已新增：" + remote.publisher + L" / " + remote.title + L"（" +
                   FormatCount(remote.entryCount) + L" 条）\r\n";
        }
        text += L"\r\n校验结论：" + (job.VerifyDetail.empty() ? std::wstring(L"（内核没有给出明细）")
                                                             : job.VerifyDetail) + L"\r\n";
        text += L"库：" + context.LibraryRoot + L"\r\n";
        // 落库目录是 sanitize 之后的形态，跟上面那条「会社 / 作品」原始名可能不是一回事
        //（`..\..\Windows\Temp` 落库是 `WindowsTemp`，验证 P-4）。刚装完的人最需要知道的就是
        // "东西到底进了哪个目录"，所以这里给实际路径，改写过就明说。
        {
            const std::wstring installedDirectory =
                LibraryDirectoryFor(context.LibraryRoot, remote.publisher, remote.title);
            if (installedDirectory.empty())
            {
                text += L"实际落库目录：（做不出目录名 —— 会社或作品名里全是非法字符）\r\n";
            }
            else
            {
                text += L"实际落库目录：" + installedDirectory + L"\r\n";
                if (ExtensionSet::MakeSafePathComponent(remote.publisher) != remote.publisher ||
                    ExtensionSet::MakeSafePathComponent(remote.title) != remote.title)
                {
                    text += L"（会社 / 作品名里有不能当目录名用的字符，库里那两级是改写过的）\r\n";
                }
            }
        }
        text += L"\r\n入库只是写进库 —— 要不要用它，仍由首页「该作扩展集提取」的归属校验把关。";
        SetOnlineStatus(state, text);

        std::wstring flattened = text;
        for (size_t at = flattened.find(L"\r\n"); at != std::wstring::npos; at = flattened.find(L"\r\n"))
        {
            flattened.replace(at, 2u, L" / ");
        }
        Host::LogLine(Log::Level::Info, L"[YuaExtract] 在线更新：" + flattened);

        const int keep = OnlineSelectedIndex(state);
        const std::vector<Online::EntryState> states = ClassifyRemoteEntries(state);
        RefillOnlineList(state, states, keep);
        Host::ShowInfo(state.Window, text);
    }

    void OnOnlineJobDone(OnlineDialogState& state)
    {
        OnlineJob& job = state.Job;
        if (job.Thread)
        {
            ::WaitForSingleObject(job.Thread, 5000);
            ::CloseHandle(job.Thread);
            job.Thread = nullptr;
        }
        state.JobActive = false;

        const OnlineJobKind kind = job.Kind;
        job.Kind = OnlineJobKind::None;
        const bool ok = job.Ok;
        const std::wstring error = job.Error;
        const bool cancelled = ::InterlockedCompareExchange(&job.Cancel, 0, 0) != 0;

        if (kind == OnlineJobKind::FetchIndex)
        {
            state.Entries = ok ? job.Entries : std::vector<ExtensionSet::LibraryEntry>();
            const std::vector<Online::EntryState> states = ClassifyRemoteEntries(state);
            RefillOnlineList(state, states, 0);
            RefreshOnlineButtons(state);

            if (ok)
            {
                unsigned int fresh = 0u;
                unsigned int update = 0u;
                unsigned int same = 0u;
                unsigned int localNewer = 0u;
                for (const Online::EntryState value : states)
                {
                    switch (value)
                    {
                        case Online::EntryState::New: ++fresh; break;
                        case Online::EntryState::Update: ++update; break;
                        case Online::EntryState::Same: ++same; break;
                        default: ++localNewer; break;
                    }
                }
                const std::wstring text =
                    L"索引拉取成功：共 " + FormatCount(job.Index.entryCount) + L" 条（实收 " +
                    FormatSize(job.Index.bytes) + L"）。\r\n" + job.Index.url + L"\r\n\r\n本地状态：新增 " +
                    FormatCount(fresh) + L" / 可更新 " + FormatCount(update) + L" / 已是同一份 " +
                    FormatCount(same) + L" / 本地条数更多 " + FormatCount(localNewer) +
                    L"\r\n\r\n选一条点「下载并安装选中」。档案摘要不同的覆盖前会再问一次。";
                SetOnlineStatus(state, text);
                Host::LogLine(Log::Level::Info,
                              L"[YuaExtract] 在线更新：索引拉取成功，共 " + FormatCount(job.Index.entryCount) +
                                  L" 条 ← " + job.Index.url);
            }
            else
            {
                // 拉索引本来不落暂存文件，这里仍然清一次：暂存目录只归本弹窗用，
                // 任何失败都别留下半截脏状态（§14.4 第 6 条的口气：本地一个字节都不改）。
                Online::CleanupStaging(job.StagingDirectory);
                const std::wstring reason = OnlineFailureText(error, cancelled);
                SetOnlineStatus(state, L"索引拉取失败：\r\n" + reason + L"\r\n\r\n请求基址：" +
                                           DescribeBase(job.Base) + L"\r\n当前生效基址：" +
                                           DescribeBase(state.EffectiveBase) + L"\r\n配置文件：" +
                                           Online::ConfigFilePath() + L"\r\n\r\n本地库未被改动。");
                Host::LogLine(Log::Level::Warn, L"[YuaExtract] 在线更新：索引拉取失败：" + reason);
                Host::ShowError(state.Window, L"拉取远端索引失败：\r\n" + reason);
            }
            return;
        }

        if (kind == OnlineJobKind::Download)
        {
            if (!ok)
            {
                // 失败**先清暂存**再报：§14.4 第 6 条要求本地一个字节都不改，
                // 留一堆半截临时文件在暂存目录里同样是脏状态。
                Online::CleanupStaging(job.StagingDirectory);
                RefreshOnlineButtons(state);
                const std::wstring reason = OnlineFailureText(error, cancelled);
                SetOnlineStatus(state, L"下载 / 校验失败：\r\n" + reason +
                                           L"\r\n\r\n本地库未被改动（档案没写进库）。");
                Host::LogLine(Log::Level::Warn, L"[YuaExtract] 在线更新：下载 / 校验失败：" + reason);
                Host::ShowError(state.Window, L"下载或校验失败：\r\n" + reason);
                return;
            }
            InstallVerifiedArchive(state, job);
            RefreshOnlineButtons(state);
            return;
        }

        RefreshOnlineButtons(state);
    }

    void CloseOnlineDialog(OnlineDialogState& state)
    {
        if (state.JobActive)
        {
            // 线程会往 state 里写结果，而 state 是调用方的栈对象：必须等它退出来才能销毁窗口。
            // 先问一句 —— 一次下载可能就差最后几秒。
            if (!Host::Confirm(state.Window,
                               L"联网任务还在跑（拉索引 / 下载）。关闭这个窗口会中止它。\r\n\r\n"
                               L"已经装进库的扩展集不受影响，未装进去的临时文件会被清掉。\r\n\r\n确认关闭？",
                               L"确认关闭"))
            {
                return;
            }
            Host::LogLine(Log::Level::Warn, L"[YuaExtract] 在线更新：用户关闭弹窗，已中止联网任务");
            StopOnlineJob(state);
        }
        ::DestroyWindow(state.Window);
    }

    LRESULT CALLBACK OnlineDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
        {
            CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            OnlineDialogState* state = static_cast<OnlineDialogState*>(create->lpCreateParams);
            state->Window = hwnd;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        OnlineDialogState* state =
            reinterpret_cast<OnlineDialogState*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!state)
        {
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        switch (msg)
        {
            case WM_CREATE:
            {
                UiContext& context = *state->Context;
                const int width = kOnlineDialogWidth;
                int y = 14;

                HWND label = ::CreateWindowExW(0, L"STATIC", L"仓库地址", WS_CHILD | WS_VISIBLE | SS_LEFT, 16,
                                               y + 3, 60, 18, hwnd, nullptr, context.Instance, nullptr);
                ::SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(context.FontUi), TRUE);

                const int buttonWidth = 96;
                const int editWidth = width - 80 - 16 - buttonWidth - 8;
                HWND endpoint = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 80, y,
                                                  editWidth, 24, hwnd,
                                                  (HMENU)(INT_PTR)IDC_YUA_ONLINE_ENDPOINT, context.Instance,
                                                  nullptr);
                ::SendMessageW(endpoint, WM_SETFONT, reinterpret_cast<WPARAM>(context.FontUi), TRUE);
                // 弹窗必须显示当前基址（§14.5）：否则"点了没反应却不知道在打哪个地址"。
                // 填的是**配置里的原文**（见 ConfiguredEndpointText）—— 用归一化后的生效值填，
                // 配置写成 http/ftp 时这里会是空的，用户等于看见"我从没填过地址"（验证 P-2）。
                ::SetWindowTextW(endpoint, ConfiguredEndpointText().c_str());

                PlaceOnlineControl(CreateButton(hwnd, &context, IDC_YUA_ONLINE_REFRESH, L"拉取索引"),
                                   width - 16 - buttonWidth, y, buttonWidth, 24);
                y += 32;

                // LBS_NOINTEGRALHEIGHT：列表必须是我给的高度，别被系统吸附到"整数行"上，
                // 否则它会比下面的进度条高出半行、把布局挤歪。
                HWND list = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY |
                                                  LBS_NOINTEGRALHEIGHT,
                                              16, y, width - 32, 190, hwnd, (HMENU)(INT_PTR)IDC_YUA_ONLINE_LIST,
                                              context.Instance, nullptr);
                ::SendMessageW(list, WM_SETFONT, reinterpret_cast<WPARAM>(context.FontUi), TRUE);
                y += 198;

                HWND bar = ::CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 16, y, width - 32, 18,
                                             hwnd, (HMENU)(INT_PTR)IDC_YUA_ONLINE_PROGRESS, context.Instance,
                                             nullptr);
                ::SendMessageW(bar, PBM_SETRANGE32, 0, 100);
                ::SendMessageW(bar, PBM_SETPOS, 0, 0);
                y += 26;

                HWND status = ::CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY |
                                                    ES_AUTOVSCROLL | WS_VSCROLL,
                                                16, y, width - 32, 118, hwnd,
                                                (HMENU)(INT_PTR)IDC_YUA_ONLINE_STATUS, context.Instance, nullptr);
                ::SendMessageW(status, WM_SETFONT, reinterpret_cast<WPARAM>(context.FontUi), TRUE);
                y += 126;

                PlaceOnlineControl(CreateButton(hwnd, &context, IDC_YUA_ONLINE_INSTALL, L"下载并安装选中"),
                                   width - 16 - 80 - 8 - 150, y, 150, 28);
                PlaceOnlineControl(CreateButton(hwnd, &context, IDC_YUA_ONLINE_CLOSE, L"关闭"),
                                   width - 16 - 80, y, 80, 28);

                // 初始状态栏同时给出两个值：框里那串是"待用的地址"（配置原文），
                // 「当前生效基址」是配置里真正能用的那个 —— 两者不一样时才是要注意的时候（P-2）。
                SetOnlineStatus(*state,
                                L"仓库地址（配置里的值）：" + DescribeBase(RawEndpointText(*state)) +
                                    L"\r\n当前生效基址：" + DescribeBase(state->EffectiveBase) + L"\r\n配置文件：" +
                                    Online::ConfigFilePath() +
                                    L"\r\n（改「仓库地址」后离开输入框即回写配置；点「拉取索引」开始。）\r\n\r\n"
                                    L"远端档案会先校验：档案摘要对索引 → 解码 → 内嵌 exe 摘要对索引 → 自洽性。"
                                    L"任一条不过就丢弃，库与库清单一个字节都不改。");
                RefreshOnlineButtons(*state);
                return 0;
            }
            case WM_COMMAND:
            {
                switch (LOWORD(wParam))
                {
                    case IDC_YUA_ONLINE_REFRESH:
                        OnOnlineRefresh(*state);
                        return 0;
                    case IDC_YUA_ONLINE_INSTALL:
                        OnOnlineInstall(*state);
                        return 0;
                    case IDCANCEL:
                    case IDC_YUA_ONLINE_CLOSE:
                        CloseOnlineDialog(*state);
                        return 0;
                    case IDC_YUA_ONLINE_ENDPOINT:
                        if (HIWORD(wParam) == EN_KILLFOCUS)
                        {
                            // 离开输入框时当场告诉他"这个地址没生效"（失败不改框里的原文）。
                            ApplyOnlineEndpoint(*state, true);
                        }
                        return 0;
                    default:
                        break;
                }
                break;
            }
            case kOnlineProgressMessage:
            {
                OnlineJob& job = state->Job;
                unsigned int percent = 0u;
                std::wstring stage;
                if (job.LockReady)
                {
                    ::EnterCriticalSection(&job.Lock);
                }
                percent = static_cast<unsigned int>(job.Percent);
                stage = job.Stage;
                if (job.LockReady)
                {
                    ::LeaveCriticalSection(&job.Lock);
                }
                SetOnlineProgress(*state, percent);
                if (!stage.empty())
                {
                    wchar_t buffer[32]{};
                    ::swprintf_s(buffer, L"%u%%", percent);
                    SetOnlineStatus(*state, stage + L"（" + buffer + L"）");
                }
                return 0;
            }
            case kOnlineDoneMessage:
            {
                OnOnlineJobDone(*state);
                return 0;
            }
            case WM_KEYDOWN:
            {
                if (wParam == VK_ESCAPE)
                {
                    CloseOnlineDialog(*state);
                    return 0;
                }
                break;
            }
            case WM_CLOSE:
            {
                CloseOnlineDialog(*state);
                return 0;
            }
            case WM_DESTROY:
            {
                state->Done = true;
                return 0;
            }
            default:
                break;
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    bool EnsureOnlineDialogClass(HINSTANCE instance)
    {
        static bool registered = false;
        if (registered)
        {
            return true;
        }
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = OnlineDialogProc;
        windowClass.hInstance = instance;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        windowClass.lpszClassName = kOnlineDialogClass;
        registered = ::RegisterClassExW(&windowClass) != 0;
        return registered;
    }

    void OnlineUpdateDialog(UiContext& context)
    {
        if (!EnsureOnlineDialogClass(context.Instance))
        {
            Host::ShowError(context.Window, L"无法注册「在线更新」窗口类。");
            return;
        }

        OnlineDialogState state;
        state.Context = &context;
        // 生效基址 = 配置解析出来的那个（环境变量 → ini → 常量，§14.5 的顺序在内核里）。
        // 它是**归一化之后**的值：配置里写的是 http/ftp 时它就在这里变成空串，
        // 所以弹窗显示基址用的是另一条路（ConfiguredEndpointText 的原文），两者不要混用。
        state.EffectiveBase = Online::ResolveRepositoryBase();
        ::InitializeCriticalSection(&state.Job.Lock);
        state.Job.LockReady = true;

        RECT rect{ 0, 0, kOnlineDialogWidth, kOnlineDialogHeight };
        ::AdjustWindowRectEx(&rect, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, 0);
        HWND dialog = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kOnlineDialogClass, L"在线更新扩展集",
                                        WS_POPUP | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                        rect.right - rect.left, rect.bottom - rect.top, context.Window, nullptr,
                                        context.Instance, &state);
        if (!dialog)
        {
            if (state.Job.LockReady)
            {
                ::DeleteCriticalSection(&state.Job.Lock);
                state.Job.LockReady = false;
            }
            Host::ShowError(context.Window, L"无法创建「在线更新」窗口。");
            return;
        }

        RECT ownerRect{};
        if (::GetWindowRect(context.Window, &ownerRect))
        {
            RECT dialogRect{};
            ::GetWindowRect(dialog, &dialogRect);
            const int width = dialogRect.right - dialogRect.left;
            const int height = dialogRect.bottom - dialogRect.top;
            const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
            const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
            ::SetWindowPos(dialog, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        ::EnableWindow(context.Window, FALSE);
        ::ShowWindow(dialog, SW_SHOW);
        ::SetFocus(::GetDlgItem(dialog, IDC_YUA_ONLINE_ENDPOINT));

        MSG message{};
        while (!state.Done)
        {
            const BOOL result = ::GetMessageW(&message, nullptr, 0u, 0u);
            if (result == 0 || result == -1)
            {
                break;
            }
            if (!::IsDialogMessageW(dialog, &message))
            {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
        }

        // WM_QUIT 那条路不会走 CloseOnlineDialog，所以这里再兜一次：循环退出时线程必须已经收干净
        //（state 是栈对象，线程活着就是往已析构的对象上写）。
        StopOnlineJob(state);
        if (!state.Done && ::IsWindow(dialog))
        {
            ::DestroyWindow(dialog);
        }
        ::EnableWindow(context.Window, TRUE);
        ::SetActiveWindow(context.Window);
        if (state.Job.LockReady)
        {
            ::DeleteCriticalSection(&state.Job.Lock);
            state.Job.LockReady = false;
        }
    }

    void UpdateHint(UiContext& context)
    {
        HWND hint = ::GetDlgItem(context.Window, IDC_YUA_HINT);
        if (!hint)
        {
            return;
        }
        std::wstring text;
        if (context.Paths.GameDir.empty())
        {
            text = L"提示：先选择游戏主程序；也可以把 .exe 直接拖到本窗口。";
        }
        else if (context.Mode == ActiveMode::SetExtract)
        {
            text = L"提示：模式1 步骤1 生成静态 Hash 表、步骤2 解包、步骤3 用静态 Hash 还原（不跑游戏采集）。";
        }
        else
        {
            text = L"提示：步骤1 解包、步骤2 收集 Hash、步骤3 离线还原；默认只输出名字全是真名的文件。";
        }
        ::SetWindowTextW(hint, text.c_str());
    }

    // ---------- 窗口过程 ----------

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        if (msg == WM_NCCREATE)
        {
            CREATESTRUCTW* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            UiContext* context = static_cast<UiContext*>(create->lpCreateParams);
            context->Window = hwnd;
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(context));
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        UiContext* context = ContextOf(hwnd);
        if (!context)
        {
            return ::DefWindowProcW(hwnd, msg, wParam, lParam);
        }

        switch (msg)
        {
            case WM_CREATE:
            {
                context->FontUi = CreateUiFont(9, false);
                context->FontBold = CreateUiFont(9, true);
                context->FontMono = ::CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                                  CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
                context->WhiteBrush = ::CreateSolidBrush(RGB(255, 255, 255));
                context->LogBrush = ::CreateSolidBrush(kColorLogBackground);

                ::InitializeCriticalSection(&context->Job.Lock);
                context->Job.LockReady = true;

                CreateControls(*context);
                RefreshSetPaths(*context);
                RefreshLibrary(*context);
                // 默认仍是「传统动态模式」：老用户的既有行为不变，要新模式自己点。
                ApplyMode(*context, ActiveMode::Classic);
                Host::EnableDropOnWindow(hwnd);
                LayoutWindow(*context);
                UpdateHint(*context);
                UpdateOverview(*context);
                ::SetTimer(hwnd, kRefreshTimerId, kRefreshIntervalMs, nullptr);
                return 0;
            }

            case WM_SIZE:
            {
                LayoutWindow(*context);
                return 0;
            }

            case WM_GETMINMAXINFO:
            {
                MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
                // 首页左列现在多了「扩展集选择」与「任务进度」两组，最小高度跟着抬一档，
                // 否则「辅助操作」会被挤出可视区。
                info->ptMinTrackSize.x = 960;
                info->ptMinTrackSize.y = 800;
                return 0;
            }

            case WM_DRAWITEM:
            {
                DRAWITEMSTRUCT* item = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
                if (item->CtlType == ODT_BUTTON &&
                    (item->CtlID == IDC_YUA_TAB_HOME || item->CtlID == IDC_YUA_TAB_MAKE_SET ||
                     item->CtlID == IDC_YUA_TAB_SET || item->CtlID == IDC_YUA_TAB_ABOUT))
                {
                    DrawTab(item, static_cast<int>(item->CtlID) == context->ActiveTab, context->FontUi,
                            context->FontBold);
                    return TRUE;
                }
                break;
            }

            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLOREDIT:
            {
                HWND control = reinterpret_cast<HWND>(lParam);
                if (control == ::GetDlgItem(hwnd, IDC_YUA_LOGVIEW))
                {
                    ::SetTextColor(reinterpret_cast<HDC>(wParam), kColorLogText);
                    ::SetBkColor(reinterpret_cast<HDC>(wParam), kColorLogBackground);
                    return reinterpret_cast<LRESULT>(context->LogBrush);
                }
                if (control == ::GetDlgItem(hwnd, IDC_YUA_OVERVIEW) ||
                    control == ::GetDlgItem(hwnd, IDC_YUA_PAGE) ||
                    control == ::GetDlgItem(hwnd, IDC_YUA_SET_STATUS))
                {
                    ::SetBkColor(reinterpret_cast<HDC>(wParam), RGB(255, 255, 255));
                    return reinterpret_cast<LRESULT>(context->WhiteBrush);
                }
                break;
            }

            case WM_TIMER:
            {
                if (wParam != kRefreshTimerId)
                {
                    break;
                }
                if (context->RestoreWindow && !::IsWindow(context->RestoreWindow))
                {
                    // 还原窗口自己关掉了：清句柄并刷一次概览，让"下一步"提示重新出现。
                    context->RestoreWindow = nullptr;
                    context->RenderedOverview.clear();
                    UpdateOverview(*context);
                }
                else if (CheapStateChanged(*context))
                {
                    // 目录出现/消失是便宜的判断；真正递归统计交给「检查状态」。
                    RefreshProbes(*context, false);
                    context->RenderedOverview.clear();
                    UpdateOverview(*context);
                }
                UpdateHint(*context);
                UpdateLogView(*context);
                return 0;
            }

            case kSetProgressMessage:
            {
                OnJobProgress(*context);
                return 0;
            }

            case kSetDoneMessage:
            {
                OnJobDone(*context);
                return 0;
            }

            case Restore::kRestoreNotifyMessage:
            {
                OnRestoreWindowNotify(*context, static_cast<Restore::RestoreTaskState>(wParam));
                return 0;
            }

            case WM_COMMAND:
            {
                switch (LOWORD(wParam))
                {
                    case IDC_YUA_TAB_HOME:
                    case IDC_YUA_TAB_MAKE_SET:
                    case IDC_YUA_TAB_SET:
                    case IDC_YUA_TAB_ABOUT:
                        SwitchTab(*context, LOWORD(wParam));
                        return 0;

                    case IDC_YUA_EXE_BROWSE:
                        OnBrowseExe(*context);
                        return 0;

                    case IDC_YUA_EXE_EDIT:
                        if (HIWORD(wParam) == EN_KILLFOCUS)
                        {
                            SyncTargetFromEdit(*context);
                            RefreshProbes(*context, true);
                            context->RenderedOverview.clear();
                            UpdateOverview(*context);
                        }
                        return 0;

                    case IDC_YUA_FUNC_SET_EXTRACT:
                        ApplyMode(*context, ActiveMode::SetExtract);
                        return 0;
                    case IDC_YUA_FUNC_SET_CRACK:
                        ApplyMode(*context, ActiveMode::SetCrack);
                        return 0;
                    case IDC_YUA_FUNC_CLASSIC:
                        ApplyMode(*context, ActiveMode::Classic);
                        return 0;

                    case IDC_YUA_LIB_PUBLISHERS:
                        if (HIWORD(wParam) == LBN_SELCHANGE || HIWORD(wParam) == LBN_DBLCLK)
                        {
                            OnPublisherSelected(*context);
                        }
                        return 0;
                    case IDC_YUA_LIB_TITLES:
                        if (HIWORD(wParam) == LBN_SELCHANGE || HIWORD(wParam) == LBN_DBLCLK)
                        {
                            OnTitleSelected(*context);
                        }
                        return 0;

                    case IDC_YUA_ADVANCED:
                        Host::ShowLegacyFunctionsWindow(hwnd);
                        return 0;

                    case IDC_YUA_STEP1:
                        if (context->Mode == ActiveMode::SetExtract)
                        {
                            OnMode1Step1(*context);
                        }
                        else if (context->Mode == ActiveMode::SetCrack)
                        {
                            OnReservedFeature(*context, L"该会社合集撞新作");
                        }
                        else
                        {
                            OnStep1(*context);
                        }
                        return 0;
                    case IDC_YUA_STEP2:
                        if (context->Mode == ActiveMode::SetExtract)
                        {
                            // 模式1 的步骤2 与传统模式的步骤1 是同一件事：提取动态 XP3 资源。
                            OnStep1(*context);
                        }
                        else if (context->Mode == ActiveMode::SetCrack)
                        {
                            OnReservedFeature(*context, L"该会社合集撞新作");
                        }
                        else
                        {
                            OnStep2(*context);
                        }
                        return 0;
                    case IDC_YUA_STEP3:
                        if (context->Mode == ActiveMode::SetExtract)
                        {
                            // 还原内核零改动：只把映射目录换成 StaticHash_Output（设计稿 §7）。
                            OnStep3(*context, context->StaticHashDir, true);
                        }
                        else if (context->Mode == ActiveMode::SetCrack)
                        {
                            OnReservedFeature(*context, L"该会社合集撞新作");
                        }
                        else
                        {
                            OnStep3(*context, context->Paths.HashDir, false);
                        }
                        return 0;

                    case IDC_YUA_SET_GENERATE:
                        OnSetGenerate(*context);
                        return 0;
                    case IDC_YUA_SET_EDITINFO:
                        OnSetEditInfo(*context);
                        return 0;
                    case IDC_YUA_SET_TEST:
                        OnSetTest(*context);
                        return 0;
                    case IDC_YUA_SET_OPEN_DRAFT:
                        OnSetOpenDraft(*context);
                        return 0;
                    case IDC_YUA_SET_INSTALL:
                        OnSetInstall(*context);
                        return 0;
                    case IDC_YUA_SET_OPEN_LIB:
                        OnSetOpenLibrary(*context);
                        return 0;
                    case IDC_YUA_SET_PICK_SOURCE:
                        OnSetPickSource(*context);
                        return 0;
                    case IDC_YUA_SET_PICK_TEST:
                        OnSetPickTest(*context);
                        return 0;

                    case IDC_YUA_SETM_REFRESH:
                        OnSetRefresh(*context);
                        return 0;
                    case IDC_YUA_SETM_OPEN_ROOT:
                        OnSetOpenLibraryRoot(*context);
                        return 0;
                    case IDC_YUA_SETM_OPEN_ENTRY:
                        OnSetOpenEntry(*context);
                        return 0;
                    case IDC_YUA_SETM_UPDATE:
                        OnSetOnlineUpdate(*context);
                        return 0;
                    case IDC_YUA_SETM_SELFCHECK:
                        OnSetSelfCheck(*context);
                        return 0;
                    case IDC_YUA_SETM_NOTE:
                        OnSetShowNote(*context);
                        return 0;

                    case IDC_YUA_TASK_CANCEL:
                        if (context->JobActive)
                        {
                            ::InterlockedExchange(&context->Job.Cancel, 1);
                            SetTaskText(*context, L"正在取消……（内核按条检查取消标志）");
                            Host::LogLine(Log::Level::Warn, L"[YuaExtract] 已请求取消后台任务");
                        }
                        return 0;

                    case IDC_YUA_AUX_CHECK:
                        OnAuxCheck(*context);
                        return 0;
                    case IDC_YUA_AUX_WORKSPACE:
                        OnAuxOpenWorkspace(*context);
                        return 0;
                    case IDC_YUA_AUX_CLEAN:
                        OnAuxClean(*context);
                        return 0;
                    case IDC_YUA_AUX_RESTOREDIR:
                        OnAuxOpenRestoreDir(*context);
                        return 0;
                    case IDC_YUA_AUX_REPORT:
                        OnAuxOpenReport(*context);
                        return 0;

                    case IDC_YUA_LOGBTN:
                        ToggleLog(*context);
                        return 0;
                }
                break;
            }

            case WM_DROPFILES:
            {
                HDROP drop = reinterpret_cast<HDROP>(wParam);
                const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0u);
                std::wstring path;
                if (count > 0u)
                {
                    const UINT length = ::DragQueryFileW(drop, 0u, nullptr, 0u);
                    path.resize(static_cast<size_t>(length) + 1u, L'\0');
                    ::DragQueryFileW(drop, 0u, &path[0], length + 1u);
                    path.resize(static_cast<size_t>(length));
                }
                ::DragFinish(drop);

                if (count > 1u)
                {
                    Host::ShowInfo(hwnd, L"一次只能处理一个游戏主程序，已取用第一个。");
                }
                if (Host::ValidateGameExe(hwnd, path))
                {
                    ApplyTarget(*context, path);
                    Host::LogLine(Log::Level::Info, L"[YuaExtract] 目标游戏（拖入）：" + path);
                }
                return 0;
            }

            case WM_CLOSE:
            {
                if (context->JobActive)
                {
                    // 生成 / 静态 Hash 表 / 干跑都跑在自己的线程里，关窗前必须问一句并等它退出来。
                    if (!Host::Confirm(hwnd,
                                       L"后台任务还在跑（生成扩展集 / 生成静态 Hash 表 / 干跑测试）。"
                                       L"关闭主程序会中止它（已经写出的文件保留）。\r\n\r\n确认关闭？",
                                       L"确认关闭"))
                    {
                        return 0;
                    }
                    StopJobForClose(*context);
                }
                if (context->RestoreWindow && ::IsWindow(context->RestoreWindow))
                {
                    // 还原窗口是本进程的另一个顶层窗口，它自己会在关闭时停掉所有工作线程。
                    // 先问一句再关，免得用户以为主界面关了还原还在跑。
                    if (!Host::Confirm(hwnd,
                                       L"资源名还原窗口还开着。关闭主程序会一并关掉它，"
                                       L"正在进行的还原会被中止（已复制出去的文件保留）。\r\n\r\n确认关闭？",
                                       L"确认关闭"))
                    {
                        return 0;
                    }
                    Restore::CloseRestoreWindow(context->RestoreWindow);
                    context->RestoreWindow = nullptr;
                }
                ::DestroyWindow(hwnd);
                return 0;
            }

            case WM_DESTROY:
            {
                ::KillTimer(hwnd, kRefreshTimerId);
                StopJobForClose(*context);
                if (context->RestoreWindow && ::IsWindow(context->RestoreWindow))
                {
                    Restore::CloseRestoreWindow(context->RestoreWindow);
                    context->RestoreWindow = nullptr;
                }
                if (context->Job.LockReady)
                {
                    ::DeleteCriticalSection(&context->Job.Lock);
                    context->Job.LockReady = false;
                }
                if (context->FontUi) ::DeleteObject(context->FontUi);
                if (context->FontBold) ::DeleteObject(context->FontBold);
                if (context->FontMono) ::DeleteObject(context->FontMono);
                if (context->WhiteBrush) ::DeleteObject(context->WhiteBrush);
                if (context->LogBrush) ::DeleteObject(context->LogBrush);
                ::PostQuitMessage(0);
                return 0;
            }
        }
        return ::DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

int RunMainWindow(HINSTANCE instance, const std::wstring& initialExe)
{
    INITCOMMONCONTROLSEX commonControls{ sizeof(commonControls),
                                         ICC_STANDARD_CLASSES | ICC_PROGRESS_CLASS | ICC_DATE_CLASSES };
    ::InitCommonControlsEx(&commonControls);

    // 富文本控件用于概览面板：先 Msftedit，再退 Riched20，最后退普通 EDIT。
    std::wstring richClass;
    if (::LoadLibraryW(L"Msftedit.dll"))
    {
        richClass = L"RICHEDIT50W";
    }
    else if (::LoadLibraryW(L"Riched20.dll"))
    {
        richClass = L"RICHEDIT20W";
    }

    static UiContext context;
    context.Instance = instance;
    context.RichEditClass = richClass;
    context.RichEditAvailable = !richClass.empty();

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = ::LoadIconW(instance, MAKEINTRESOURCEW(IDI_MainIcon));
    windowClass.hIconSm = windowClass.hIcon;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    windowClass.lpszClassName = kWindowClass;
    if (!::RegisterClassExW(&windowClass))
    {
        return -1;
    }

    std::wstring caption = BuildCaption(std::wstring());
    // 默认尺寸按"首页左列六组控件 + 右侧概览"给足高度（最小高度见 WM_GETMINMAXINFO）。
    HWND window = ::CreateWindowExW(WS_EX_ACCEPTFILES, kWindowClass, caption.c_str(),
                                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1180,
                                    840, nullptr, nullptr, instance, &context);
    if (!window)
    {
        return -1;
    }

    if (!initialExe.empty())
    {
        ApplyTarget(context, initialExe);
    }
    else
    {
        RefreshProbes(context, false);
        UpdateOverview(context);
    }

    ::ShowWindow(window, SW_SHOWNORMAL);
    ::UpdateWindow(window);

    MSG message{};
    while (BOOL result = ::GetMessageW(&message, nullptr, 0u, 0u))
    {
        if (result == -1)
        {
            return -1;
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
}
