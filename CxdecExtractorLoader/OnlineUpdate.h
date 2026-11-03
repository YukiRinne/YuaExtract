#pragma once

// 扩展集的「在线更新」：从一个**独立的远端仓库**只读拉取扩展集，校验后装进本地扩展集库。
//
// 三条必须记住的：
//   1. **仓库布局与本地库同构**：远端 `<会社>/<作品>/rules.int` 就是本地
//      `<工具目录>\ExtensionLibrary\<会社>\<作品>\rules.int` 的同一份东西；
//      索引 `index.txt` 就是一份 manifest.txt（同 10 列 TSV），所以**没有新格式**。
//   2. **下载来的东西一律当不可信输入**：https + 超时 + 体积上限 → sha256 对索引 →
//      解码过 magic/version/CRC → 内嵌 exe 摘要等于**索引里的**（索引是信任根）→
//      档案自洽性（EXCEPTIONS 为 0 + 展开条数与 STATS 一致）。任一不过就丢弃临时文件，
//      **本地一个字节都不改**。
//      注意：档案不保存常规哈希层，所以本地无法独立验证每个名字是否正确；信任根是索引，
//      工具不得对外声称能验名字。最终仍要用实际还原结果判断。
//   3. **绝不自动加载、绝不后台联网**：只在用户点「在线更新」时拉一次；入库只写库，
//      用不用仍由归属校验把关。
//
// 本层与界面无关：界面只负责起后台线程、把 progress/cancel 接上、把结论显示出来。

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

#include "ExtensionSet.h"

namespace Yua
{
namespace Online
{
    // 远端文件名（与本地库清单 kManifestFileName / kArchiveFileName 的对应关系写在 §14.2）
    constexpr const wchar_t* kIndexFileName = L"index.txt";
    constexpr const wchar_t* kRemoteArchiveFileName = L"rules.int";

    // 默认基址。仓库：https://github.com/YukiRinne/Yua-Extensions（index.txt + <会社>/<作品>/rules.int）。
    // raw 主；国内可达性通常更好的 jsDelivr 镜像见仓库 README，可在「在线更新」弹窗里填一次（走 ini）。
    constexpr const wchar_t* kDefaultRepositoryBase =
        L"https://raw.githubusercontent.com/YukiRinne/Yua-Extensions/main/";

    // 网络约束（§14.4 第 1 条）。超时是**每一段**的超时（连接 / 发送 / 接收各算）。
    constexpr int kConnectTimeoutMs = 10000;
    constexpr int kSendTimeoutMs = 15000;
    constexpr int kReceiveTimeoutMs = 30000;
    constexpr unsigned long long kMaxIndexBytes = 4ull * 1024ull * 1024ull;    // 4 MB
    constexpr unsigned long long kMaxArchiveBytes = 64ull * 1024ull * 1024ull; // 64 MB（真实档案约 0.25 MB）

    // ---------------- 基址 ----------------

    std::wstring NormalizeBase(const std::wstring& base); // 去首尾空白、末尾保证一个 '/'
    std::wstring DefaultRepositoryBase();                 // 常量
    // 解析顺序：环境变量 YUAEXT_ENDPOINT → ini 的 [online] base= → 常量。
    // ini 位置：优先 <工具目录>\YuaExtract.ini；工具目录不可写时用 %LOCALAPPDATA%\YuaExtract\YuaExtract.ini。
    std::wstring ResolveRepositoryBase();
    bool SetRepositoryBase(const std::wstring& base, std::wstring& error); // 回写 ini
    std::wstring ConfigFilePath(); // 实际会用的 ini 路径（弹窗要显示它）

    std::wstring IndexUrl(const std::wstring& base);
    std::wstring ArchiveUrl(const std::wstring& base, const std::wstring& publisher, const std::wstring& title);

    // ---------------- 进度与取消 ----------------

    // 都是可空的：不接就同步跑完。与 ExtensionSet::DryRunOptions 的写法一致。
    //   cancel 返回 true = 用户要求中止 → 调用方返回 false，error 里写"已取消"。
    //   progress 的 percent 是 0..100，stage 是给人看的一句话（会进日志与状态栏）。
    struct Request
    {
        std::function<bool()> cancel;
        std::function<void(int percent, const std::wstring& stage)> progress;
    };

    // ---------------- 拉索引 ----------------

    struct IndexResult
    {
        int httpStatus = 0;
        std::wstring url;
        unsigned long long bytes = 0;
        unsigned int entryCount = 0;
    };

    // GET <base>index.txt → 按 §14.3 解析成 LibraryEntry 列表（**复用 ExtensionSet::ParseManifestText**）。
    // 端点不可达 / DNS 失败 / 超时 / 非 200 / 超体积 / 格式错，各有各的 error 措辞，不许合并成一句。
    bool FetchIndex(const std::wstring& base, std::vector<ExtensionSet::LibraryEntry>& out,
                    IndexResult& info, std::wstring& error, const Request& request = Request());

    // ---------------- 与本地库的差异 ----------------

    enum class EntryState
    {
        New = 0,       // 本地没有
        Same = 1,      // 本地已有且档案摘要相同
        Update = 2,    // 本地已有但档案摘要不同（远端这份也算"更新"：条数更多时才算；见 .cpp 注释）
        LocalNewer = 3 // 本地那份条数更多 —— 覆盖会**减少**覆盖度，默认不覆盖，只提示
    };

    EntryState Classify(const ExtensionSet::LibraryEntry& remote, const ExtensionSet::LibraryCatalog& local);
    const wchar_t* DescribeState(EntryState state); // 给列表/面板用的一行中文

    // ---------------- 下载 + 校验 ----------------

    struct DownloadResult
    {
        std::wstring path;                 // 落盘的临时档案（调用方确认后不应再依赖它）
        std::wstring stagingDirectory;     // 本次实际使用的 staging 根（owned session 或 explicit）
        unsigned long long bytes = 0;
        std::wstring sha256;               // 大写 hex
        // **同一份字节**（与 path 里那份同源、与 sha256 同源）。2026-11 安全轮新增：
        // 界面确认后必须用 VerifyArchiveBytes + InstallBytesToLibrary，**不要再读 path** ——
        // 否则"校验 A、安装 B"的 TOCTOU 窗口就回来了。
        std::vector<unsigned char> blob;
    };

    // GET <base><会社>/<作品>/rules.int → 落 stagingDirectory 下的**唯一 CREATE_NEW** 文件。
    //   · stagingDirectory 为空：用 DefaultStagingDirectory()（返回本次调用独占的 owned session）。
    //   · stagingDirectory 非空：必须是已存在的普通目录且无重解析点；本层只写唯一文件名，
    //     失败只删自己创建的文件。该 explicit 目录**不会**登记为 owned，
    //     因此 CleanupStaging 会拒绝删除它；调用方若要清掉下载文件请自行删除 out.path。
    // 只负责「拿到字节」，**不做任何校验、不动本地库**。
    bool DownloadArchive(const std::wstring& base, const ExtensionSet::LibraryEntry& entry,
                         const std::wstring& stagingDirectory, DownloadResult& out,
                         std::wstring& error, const Request& request = Request());

    // 信任链 §14.4 第 2–5 条：sha256 对索引 → 解码过结构 → 内嵌 exe 摘要等于索引的 → 内部自校验 0 例外。
    // 另加一条**交叉校验**（2026-10-16，独立验证 P-1）：档案 META 自己的会社 / 作品必须与索引行一致
    //（_wcsicmp 比）—— 索引是信任根，装进哪个库目录必须可预测；不一致 = 索引写错或这个位置的档案
    // 被换成了别的东西，一律拒，**不是"照实显示然后照装"**。
    // 成功时 detail 是一段给人看的结论（会社/作品/条数/盐/exe 摘要/档案摘要），可直接显示或进确认框。
    bool VerifyArchive(const std::wstring& archivePath, const ExtensionSet::LibraryEntry& entry,
                       std::wstring& detail, std::wstring& error);

    // 字节版（2026-11 安全轮，在线更新唯一应使用的校验入口）：
    // sha256 与解码都针对**传入的同一份 blob**；内部不再打开任何路径。
    // 其余信任链 §14.4 第 2–5 条与索引/档案会社作品交叉校验完全一致。
    bool VerifyArchiveBytes(const std::vector<unsigned char>& blob,
                            const ExtensionSet::LibraryEntry& entry,
                            std::wstring& detail, std::wstring& error);

    // 写入 <工具目录>\Log\OnlineStaging\session_<guid>（工具目录不可写时回退
    // %TEMP%\YuaExtract\OnlineStaging\session_<guid>）。**每次调用生成一个全新的、
    // 本进程独占的子目录**（CREATE_NEW 防碰撞），并登记为 owned；父根必须无重解析点、
    // 真能创建（不能只靠 Exists 判断可写）。
    std::wstring DefaultStagingDirectory();

    // 清理 staging。约束（2026-11 安全轮）：
    //   · 空串 = no-op，**绝不**去碰 shared 默认根；
    //   · 只清理本进程 DefaultStagingDirectory 创建并登记的 owned 目录；
    //   · 调用方显式传入的 existing 目录若未被登记，拒绝清理（no-op）；
    //   · 遇重解析点只 RemoveDirectoryW 解除链接，绝不递归进入链接目标；
    //   · 绝不删除别实例目录或用户已有文件。
    void CleanupStaging(const std::wstring& stagingDirectory);

#ifdef YUA_SECURITY_EXTENSION_TEST_SEAM
    // **仅测试构建**：让 DownloadArchive 在建好 owned session 后、发网络请求前失败，
    // 用来验证"失败时自动清理本次私有 staging"这条路径。生产构建不编译。
    bool& TestFailDownloadAfterStaging();
#endif

    // 入库用 ExtensionSet::InstallBytesToLibrary（传 DownloadResult.blob）。
    // **不要**在确认之后再读 DownloadResult.path：那会重新引入 TOCTOU。
    // 清单「来源」列传 ArchiveUrl(base, 会社, 作品)，不要传暂存路径。
    // 覆盖前是否确认由**界面**决定：同 exe 摘要但档案摘要不同 → 先弹框列出新旧条数/摘要。
}
}
