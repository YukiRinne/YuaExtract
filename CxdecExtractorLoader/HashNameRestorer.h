#pragma once

// 离线资源名还原（YuaExtract 步骤3）的内核。
//
// 上游 v4.2 只有"注入游戏后实时就地改名"这一条路，缺一个不启动游戏就能跑的
// 离线还原器。这里补上：读 StringHashDumper_Output 里的 Hash 映射，把
// Extractor_Output 里 <封包>\<目录Hash>\<文件Hash> 形式的解包结果还原成明文名字，
// 输出到 Restored_Extractor_Output，并写 RestoreReport.txt。
//
// 口径与 CxdecStringDumper 的运行时恢复保持一致（见 HashRestoreUI.cpp）：
//   - DirectoryHash.log / FileNameHash.log：UTF-16，行格式 真名##YSig##HASH
//   - HashRestore_RecoveredNames.lst：UTF-8，行格式 HASH:name
//     16 位 hex 视为目录 Hash（名字 "/" 表示根域），64 位 hex 视为文件 Hash
//   - %EmptyString% 与 "/" 都是根域
//
// 与运行时恢复的唯一有意差别：**复制**到新目录而不是就地移动。
// 源目录 Extractor_Output 全程只读，映射补全后重跑一次即可，不会留下半还原的现场。
//
// 本内核按"一个封包一个任务"切分：Extractor_Output 的顶层子目录就是封包，
// 界面拿 EnumerateRestorePackages 生成任务列表，再让工作线程各自调
// RestoreSinglePackage。内核自己不建线程、不碰界面，全部状态经返回值交回调用方 ——
// 这样"任务终态"永远由拿到返回值的那一方决定，不依赖任何投递成功的进度消息
// （CxdecExtractorUI 曾因为 PostMessage 被静默丢弃而让任务永远停在"处理中"）。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Yua
{
    // ---------- 映射表 ----------

    // 一次还原里所有工作线程共享同一份映射快照。
    // 每个封包各自读一遍磁盘上几十 MB 的映射表纯属浪费，而且并发读同一批文件也没意义。
    // 载入之后只读，多线程并发 find 是安全的。
    struct HashMapTables
    {
        std::unordered_map<std::wstring, std::wstring> directories; // 16 位 hex -> 明文目录（空 = 根域）
        std::unordered_map<std::wstring, std::wstring> files;       // 64 位 hex -> 明文文件名
        std::vector<std::wstring> sources;                          // 实际读到的映射文件（写进报告，便于追溯）
    };

    // 载入映射目录下所有映射来源（DirectoryHash.log / FileNameHash.log / *.lst）。
    HashMapTables LoadHashMapTables(const std::wstring& hashDirectory);

    // 判断一个相对路径里是否还有没解析掉的 Hash 段：最后两段分别是 16 位 / 64 位 hex。
    // 界面和报告都用这一份，免得两处判法不一致。
    bool HasUnresolvedHashSegment(const std::wstring& relativePath);

    // ---------- 任务与结果 ----------

    // 一个封包 = Extractor_Output 的一个顶层子目录。
    //
    // 解包目录**根下**的散文件（.alst 清单、readme 之类）也必须有归宿：它们不属于
    // 任何封包，但"勾选保留时输出文件数应该等于源文件数"这条要求只有在把它们也算进
    // 任务时才成立。所以根目录也做成一个任务，名字固定用 kRootPackageName。
    struct RestorePackageTask
    {
        std::wstring packageName;      // 封面名，界面上"封包"那一列
        std::wstring packageDirectory; // 完整路径
        bool rootLevel = false;        // true = 只扫这个目录本层，不递归
        unsigned int fileCount = 0;    // 扫到的文件数（日志目录不计）
        unsigned long long bytes = 0;
    };

    // 根目录任务的显示名（带括号，一眼能看出这不是真的封包目录）。
    constexpr const wchar_t* kRootPackageName = L"(根目录)";

    // 单个封包的还原结果。字段对齐上游 RestoreReport 的口径。
    struct RestorePackageResult
    {
        std::wstring packageName;

        unsigned int totalFiles = 0;       // 扫到的源文件总数（跳过的日志目录不计）
        unsigned int hashNamedFiles = 0;   // 形如 <目录Hash>\<文件Hash> 的文件数
        unsigned int passthroughFiles = 0; // 非哈希名文件（.alst、说明文件等）
        unsigned int restoredFiles = 0;    // 成功还原成明文名
        unsigned int missingDirectory = 0; // 缺目录 Hash
        unsigned int missingFileName = 0;  // 缺文件 Hash
        // 缺目录 Hash、但文件名 Hash 其实已经能解析出来的条数。
        // 这类缺口只要补目录候选表就能解决，和"两个都缺"是两回事。
        unsigned int missingDirectoryWithKnownFile = 0;
        unsigned int renamedForConflict = 0; // 目标重名，落成 __dupN
        unsigned int failedFiles = 0;        // 复制/落盘失败
        unsigned int skippedLogFiles = 0;    // Extractor_Log / ExtractLog 里跳过的文件
        // 默认语义下被主动丢弃的未还原条目（文件名仍是 64 位 Hash、
        // 或路径里仍有未解析 Hash 段）。勾选"保留未还原文件"时这一项恒为 0。
        unsigned int droppedUnrestoredFiles = 0;
        // 未还原文件里靠文件头认出类型、后缀是工具补上去的条数（见 RestoreOptions::keepUnrestored）。
        // 和"原样保留"分开计：用户要能一眼看出有多少条拿到了可用后缀、多少条只能落 .bin。
        unsigned int headerSuffixedFiles = 0;
        // 补后缀的分布（后缀 -> 条数），报告里列出来便于核对识别结果。
        std::map<std::wstring, unsigned int> headerExtensionCounts;
        // 两遍式还原的第二遍里，因"第一遍已交付"而跳过的条数。
        unsigned int skippedAlreadyDelivered = 0;
        // 本次实际交付进输出目录的目标相对路径（重名改名的记改名**前**的基名）。
        // 两遍式还原（先普通还原、再补充还原补漏）时，第二遍拿第一遍的这份集合
        // 判断"这个文件已经交付过了"，直接跳过 —— 不重拷、也不再造 __dupN。
        std::vector<std::wstring> deliveredRelative;

        unsigned long long copiedBytes = 0;

        bool cancelled = false;  // 用户中止，结果不完整
        bool succeeded = true;   // 没有失败项就算成功（任务终态只看它）
        bool completed = false;  // 正常跑到收尾（含"被中止"）
        std::wstring info;       // 一行给界面"详细信息"列看的摘要
    };

    struct RestoreOptions
    {
        std::wstring sourceDirectory;   // Extractor_Output
        std::wstring outputDirectory;   // Restored_Extractor_Output
        std::wstring logDirectory;      // 工具目录\Log（还原过程日志，可为空）

        // 勾选"保留未还原文件名的文件"时为 true：未还原的文件也复制出去，
        // 并在**输出目录**里按文件头给它们补一个后缀（<64位Hash>.png / .ogg / ...，
        // 识别不出类型时落 .bin）。源目录全程只读，不在那边改任何名字。
        // 默认 false = 输出目录里只留已还原的（有真名的）文件。
        bool keepUnrestored = false;

        // 勾选"补充还原"时为 true：开始还原前先跑一次候选挖掘（HashNameMiner），
        // 把挖到的、已通过哈希验证的名字并进本次用的映射表。
        bool supplementRestore = false;

        // 返回 true 表示请求中止（窗口关闭 / 用户点了中止时用）。
        std::function<bool()> cancelRequested;
        // percent：0..100；currentEntry：当前处理的相对路径。实现方按文件逐个回调，
        // 由调用方（工作线程）决定要不要节流后再往界面投递。
        std::function<void(unsigned int percent, const std::wstring& currentEntry)> progress;

        // 前一遍已交付的目标相对路径集合（两遍式还原用）。本遍遇到同一目标直接跳过，
        // 不重拷也不再造副本 —— "补充还原"就是补漏，第二遍只处理第一遍没交付的。
        std::set<std::wstring> alreadyDelivered;
    };

    // 枚举封包（Extractor_Output 的顶层子目录，按名字排序，保证任务列表稳定）。
    // 源目录不存在时返回空列表。
    std::vector<RestorePackageTask> EnumerateRestorePackages(const std::wstring& sourceDirectory);

    // 还原一个封包。这是内核入口，供工作线程直接调用；
    // 返回的 result 里的终态字段就是任务的终态，界面不要再去猜。
    RestorePackageResult RestoreSinglePackage(const RestorePackageTask& task,
                                              const HashMapTables& tables,
                                              const RestoreOptions& options);

    // ---------- 补充还原（候选挖掘） ----------

    // 「补充还原」是一段**开始还原之前**的前置流程，不是每个封包各跑一遍：
    // 挖一次、把结果并进共享映射表，之后所有工作线程用的就是补全后的表。
    struct SupplementOptions
    {
        std::wstring sourceDirectory;  // Extractor_Output（挖的是它里面非二进制包的明文资源）
        std::wstring outputDirectory;  // 还原输出目录；它在源目录里时要排除，别把刚还原的东西再挖一遍
        std::wstring logDirectory;     // 工具目录\Log（Universal.log 在这儿）
        std::wstring minedListPath;    // <工作区>\StringHashDumper_Output\MinedNames.lst
        unsigned int maxCandidates = 4000000u;
        // 返回 true 表示请求中止挖掘（窗口关闭 / 用户点了中止时用）。
        std::function<bool()> cancelRequested;
    };

    struct SupplementProgress
    {
        unsigned int scannedFiles = 0u;
        unsigned int candidates = 0u;
        unsigned int hits = 0u;
    };

    struct SupplementOutcome
    {
        bool attempted = false;     // 勾了复选框：本次确实尝试过挖掘
        bool saltAvailable = false; // 读到 Hash Seed
        bool ran = false;           // 真的调用了挖掘内核（盐可用才调用）
        bool cancelRequested = false;
        bool limitHit = false;
        unsigned int scannedFiles = 0u;
        unsigned int candidateCount = 0u;
        unsigned int triedHashes = 0u;
        unsigned int hitCount = 0u;      // 内核返回的、已通过哈希验证的条数
        unsigned int newEntries = 0u;    // 其中本次映射表里原本没有的（真正新增的还原能力）
        unsigned int targetHashes = 0u;  // 参与比对的 64 位 Hash 名集合大小
        unsigned int scanRoots = 0u;
        unsigned int writtenEntries = 0u; // 落进 MinedNames.lst 的条数（含合并进来的旧内容）
        std::wstring saltSource;          // Universal.log 路径
        std::wstring message;             // 给用户看的结论，**盐不可用时必须说明原因**
    };

    // 挖掘 → 并入 tables → 落盘 MinedNames.lst。进度经 onProgress 回调（在调用线程里同步回调）。
    SupplementOutcome RunSupplementRestore(HashMapTables& tables, const SupplementOptions& options,
                                           const std::function<void(const SupplementProgress&)>& onProgress);

    // ---------- 报告 ----------

    // 所有封包的聚合结果。多线程下每个任务各自填一个 RestorePackageResult，
    // 跑完之后由界面线程汇总成这一份，再写报告。
    struct RestoreReportData
    {
        std::wstring sourceDirectory;
        std::wstring hashDirectory;
        std::wstring outputDirectory;
        std::wstring reportFileName = L"RestoreReport.txt";
        std::vector<RestorePackageResult> packages;
        // 写进"映射来源"一节（跨任务共享一份，各自记一遍只会把报告撑长）。
        std::vector<std::wstring> mapSources;
        unsigned int directoryMapEntries = 0;
        unsigned int fileNameMapEntries = 0;
        // 补充还原的结论。attempted=false 时报告里不出现这一节。
        SupplementOutcome supplement;
        std::wstring minedListPath; // 挖到的名字落到哪儿（写进报告便于追溯）
    };

    // 汇总统计。报告顶部和界面结束提示用同一份，避免两处算出的数字对不上。
    struct RestoreTotals
    {
        unsigned int packageCount = 0;
        unsigned int totalFiles = 0;
        unsigned int hashNamedFiles = 0;
        unsigned int restoredFiles = 0;
        unsigned int passthroughFiles = 0;
        unsigned int missingDirectory = 0;
        unsigned int missingDirectoryWithKnownFile = 0;
        unsigned int missingFileName = 0;
        unsigned int renamedForConflict = 0;
        unsigned int failedFiles = 0;
        unsigned int skippedLogFiles = 0;
        unsigned int droppedUnrestoredFiles = 0;
        unsigned int headerSuffixedFiles = 0; // 未还原文件里后缀是工具按文件头补的条数
        std::map<std::wstring, unsigned int> headerExtensionCounts; // 后缀 -> 条数
        unsigned int skippedAlreadyDelivered = 0; // 两遍式第二遍里"第一遍已交付"跳过数
        unsigned long long copiedBytes = 0;
        bool cancelled = false;
    };

    RestoreTotals SumRestoreResults(const std::vector<RestorePackageResult>& results);

    // 任务粒度的进度台阶：排队 → 还原 → 收尾。
    // 界面画列表和总进度条按这三档算（每个任务等权），所以放在内核旁边——
    // 界面和自检程序用同一套口径，免得自检算出来的进度跟用户看到的不一样。
    enum class RestoreProgressStep
    {
        Queued = 0,
        Restoring = 1,
        Finishing = 2
    };

    // 单个任务在"三档等权"口径下的完成度（0..100）。
    // 还原阶段占前 90%，剩下 10% 留给收尾（写明细、写报告），
    // 否则进度条会在最后一个文件处突然跳到 100% 然后卡住很久。
    unsigned int ComputeTaskProgressPercent(RestoreProgressStep step, unsigned int percent);

    // 写 Restored_Extractor_Output\RestoreReport.txt。返回 false 时 error 里是原因。
    bool WriteRestoreReport(const RestoreReportData& data, std::wstring& error);

    // ---------- 映射概览 ----------

    // 不还原，只回答"现在手上有多少映射可用"。
    // 界面用它显示"动态 Hash 输出：已生成（目录 1234 / 文件 5678）"。
    struct MappingSummary
    {
        bool ok = false;
        std::wstring message;
        unsigned int directoryEntries = 0;
        unsigned int fileNameEntries = 0;
        std::vector<std::wstring> sources;
    };

    MappingSummary LoadMappingSummary(const std::wstring& hashDirectory);
}
