#pragma once

// 扩展集的「在线更新」：从一个**独立的远端仓库**只读拉取扩展集，校验后装进本地扩展集库。
//
// 设计口径见 docs\扩展集设计.md §14。三条必须记住的：
//   1. **仓库布局与本地库同构**：远端 `<会社>/<作品>/rules.int` 就是本地
//      `<工具目录>\ExtensionLibrary\<会社>\<作品>\rules.int` 的同一份东西；
//      索引 `index.txt` 就是一份 manifest.txt（同 10 列 TSV），所以**没有新格式**。
//   2. **下载来的东西一律当不可信输入**：https + 超时 + 体积上限 → sha256 对索引 →
//      解码过 magic/version/CRC → 内嵌 exe 摘要等于**索引里的**（索引是信任根）→
//      档案自洽性（EXCEPTIONS 为 0 + 展开条数与 STATS 一致）。任一不过就丢弃临时文件，
//      **本地一个字节都不改**。
//      **注意（2026-10-15 纠正）**：档案里**不存哈希**（§3.1 L3 默认不存），所以
//      "名字对不对"在本地**验不了** —— 信任根就是索引，工具不得对外声称能验名字。
//      名字的最终把关是事实：归于本作用它还原，还原率不对就是不对。
//   3. **绝不自动加载、绝不后台联网**：只在用户点「在线更新」时拉一次；入库只写库，
//      用不用仍由「归属校验」（§2.1）把关。
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
        std::wstring path;                 // 落盘的临时档案
        unsigned long long bytes = 0;
        std::wstring sha256;               // 大写 hex
    };

    // GET <base><会社>/<作品>/rules.int → 落 stagingDirectory\Online_<会社>_<作品>.rules.int。
    // stagingDirectory 为空时用 DefaultStagingDirectory()。
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

    // 写入<工具目录>\Log\OnlineStaging（工具目录不可写时回退 %TEMP%\YuaExtract\OnlineStaging）。
    std::wstring DefaultStagingDirectory();
    // 尽力删掉 staging 目录（失败不报错、不抛）。校验失败与入库成功后都应调用。
    void CleanupStaging(const std::wstring& stagingDirectory);

    // 入库直接用既有的 ExtensionSet::InstallToLibrary（本层不再包一层同义函数）。
    // **入库时要把 sourceLabel 传成 ArchiveUrl(base, 会社, 作品)**（2026-10-16，独立验证 P-3）：
    // 传进来的 archivePath 是暂存文件，入库后立刻被 CleanupStaging 删掉，不换的话清单「来源」列
    // 永远指向一个不存在的路径。
    // 覆盖前是否确认由**界面**决定：同 exe 摘要但档案摘要不同 → 先弹框列出新旧条数/摘要。
}
}
