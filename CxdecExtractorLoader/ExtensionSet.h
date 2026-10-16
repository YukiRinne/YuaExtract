#pragma once

// 扩展集内核（一期：同游戏闭环）—— 见 docs\扩展集设计.md。
//
// 扩展集 = 一部作品的「名字 <-> 哈希」知识档案，本体是单一文件 rules.int。
// 本内核负责：采集 -> 归一化 -> **生成期自校验** -> 写档案 / 读档案（含归属校验）
//            -> 展开静态 Hash 表 -> 只读干跑 -> 安装进扩展集库。
//
// 三条不可动摇的口径（整个设计的地基）：
//   1. file_hash   = BLAKE2s-256( UTF16LE(裸文件名 + 盐) )            —— 不含目录
//      domain_hash = SipHash-2-4( UTF16LE(目录 + 盐), key = 0^16 )     —— 小写 + 结尾斜杠
//      两者都取自 CxdecRepacker\core\resource_hash.{h,cpp}，本内核逐字对齐，不另立口径。
//   2. **盐必须显式**：Log\Universal.log 的 `Hash Seed:` -> ExtractKey_Output\Static\*.json 的
//      `hash_domain` -> 都没有就明确报错中止。空串是合法盐，所以另配 saltKnown 标志区分
//      「记了但是空」和「压根没记」（与 Hxv4p::Parameters::hash_domain_known 同一惯例）。
//   3. **绝不自己造名字**：每条名字都要过一次「名字 + 盐 -> 重算哈希 == 采集到的哈希」，
//      不等的一律进 EXCEPTIONS 并计数。所以本内核最坏情况只是补不上，不会补错。
//
// 调用方（UI / 验证脚手架）只依赖本头文件，实现全在 ExtensionSet.cpp。

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace Yua
{
    namespace ExtensionSet
    {
        // ---------- 容器常量（rules.int，惯例照 CxdecRepacker\hxv4p\Hxv4p.h） ----------

        // magic 'YXS1'：按小端写进文件后，磁盘上的四个字节就是 'Y''X''S''1'。
        constexpr uint32_t kMagic = 0x31535859u;
        constexpr uint16_t kFormatVersion = 1;
        constexpr uint16_t kHeaderSize = 20u;   // magic u32 / version u16 / headerSize u16 /
                                                // flags u32 / crc32 u32 / chunkCount u32

        constexpr uint32_t kFlagZlib = 0x0001u; // bit0 = payload 经 zlib 压缩

        // chunk 表项 12 字节：id u16 / flags u16 / offset u32 / size u32（offset 相对 payload 起点）
        constexpr uint16_t kChunkMeta = 0x0001;       // UTF-8，逐行 key=value
        constexpr uint16_t kChunkIdentity = 0x0002;   // 目标 exe + 盐 + 来源
        constexpr uint16_t kChunkDirs = 0x0003;       // 规范化目录名，\n 分隔
        constexpr uint16_t kChunkFiles = 0x0004;      // 规范化文件名（裸名），\n 分隔
        constexpr uint16_t kChunkExceptions = 0x0005; // 大写HEX:名字，\n 分隔
        constexpr uint16_t kChunkRules = 0x0006;      // 预留（二期 L1 模式层）
        constexpr uint16_t kChunkStats = 0x0007;      // 逐行 key=value 计数

        // 文件与目录名约定（设计稿 §3 / §6 / §9）
        constexpr const wchar_t* kArchiveFileName = L"rules.int";
        constexpr const wchar_t* kInfoFileName = L"info.txt";
        constexpr const wchar_t* kManifestFileName = L"manifest.txt";
        constexpr const wchar_t* kStaticHashFileName = L"StaticHash.lst";
        constexpr const wchar_t* kDraftFolderName = L"ExtensionDraft";
        constexpr const wchar_t* kLibraryFolderName = L"ExtensionLibrary";
        constexpr const wchar_t* kStaticHashFolderName = L"StaticHash_Output";
        constexpr const wchar_t* kToolVersion = L"YuaExtract extension-set/1";

        // ---------- 档案内容 ----------

        // META chunk（图 2 的字段）。
        struct Meta
        {
            std::wstring publisher;    // 会社（库路径第一级，手工填，不猜）
            std::wstring title;        // 作品名（默认 = 目标 exe 名去扩展名）
            std::wstring contributor;  // 贡献者
            std::wstring version;      // 自由文本（如 v1）
            std::wstring date;         // 用户填的日期（DATETIMEPICK_CLASS）
            std::wstring comment;      // 扩展集留言
            // 生成时间。**默认留空**：留空时档案里不写这一项，同一份输入重跑逐字节可复现
            //（设计稿 E-7 的幂等断言）。UI 真正给用户生成时把它填成 TimestampNow()，
            // 或把 GenerateOptions::stampGeneratedAt 置 true 让内核自己填。
            std::wstring generatedAt;
            std::wstring toolVersion;  // 留空则用 kToolVersion
        };

        // 目标 exe 摘要（一个 exe 的指纹）。
        struct ExeDigest
        {
            std::wstring name;   // 文件名（不含路径）
            unsigned long long size = 0;
            std::wstring sha256; // 大写 hex；空 = 未采集
            bool Present() const { return !name.empty() || !sha256.empty(); }
        };

        // IDENTITY chunk。
        struct Identity
        {
            ExeDigest targetExe;             // 目标 exe（可能是补丁产物 <原名>_crack.exe）
            ExeDigest siblingExe;            // 去掉 _crack 的兄弟 exe；name 为空 = 没有
            std::wstring salt;               // 盐（UTF-8 存盘，内存里是宽字符）
            bool saltKnown = false;          // false = 压根没记（加载时必须拒绝）
            std::wstring sourceDirectory;    // 采集来源目录（相对工作区，仅供人看）
            std::vector<std::wstring> collectionSources; // 采集来源清单（文件名 + 条数）
        };

        // 各来源条数（STATS chunk 的一部分）。
        struct SourceCounts
        {
            unsigned int directoryLog = 0;   // DirectoryHash.log
            unsigned int fileNameLog = 0;    // FileNameHash.log
            unsigned int minedNames = 0;     // MinedNames.lst
            unsigned int recoveredNames = 0; // HashRestore_RecoveredNames.lst
            unsigned int otherLists = 0;     // 同目录其它 *.lst
        };

        // STATS chunk。**只存计数**：生成耗时属于报告，不进档案，否则档案无法逐字节复现。
        struct Stats
        {
            SourceCounts sources;
            unsigned int directories = 0;  // 去重后的目录条数
            unsigned int files = 0;        // 去重后的文件条数
            unsigned int exceptions = 0;   // 自校验失败条数
            unsigned int duplicates = 0;   // 采集阶段被去重掉的条数
        };

        // 一份扩展集档案。
        struct Archive
        {
            Meta meta;
            Identity identity;
            std::vector<std::wstring> directories; // 规范化：小写 + 结尾斜杠；根记 "/"
            std::vector<std::wstring> files;       // 规范化：裸名（哈希不含目录，不改大小写）
            std::vector<std::wstring> exceptions;  // "大写HEX:原样名字"，按 HEX 排序
            std::vector<unsigned char> rules;      // 预留 chunk（未知 chunk 也原样留在解码结果里）
            Stats stats;

            // 由 directories/files + 盐展开出的查表（RebuildLookup 填充，不参与序列化）。
            std::unordered_map<std::wstring, std::wstring> directoryNames; // 大写16hex -> 目录名
            std::unordered_map<std::wstring, std::wstring> fileNames;      // 大写64hex -> 裸名
            bool lookupReady = false;
        };

        // 用 identity.salt 把 directories/files 展开成 directoryNames/fileNames。
        // 盐不可用（saltKnown=false）时返回 false 并给出显式原因。
        bool RebuildLookup(Archive& archive, std::wstring& error);

        // ---------- 哈希与归一化（E-9 单测直接调这些） ----------

        // 64 位大写 hex：BLAKE2s-256( UTF16LE(裸文件名 + 盐) )，unkeyed。
        std::wstring FileHashHex(const std::wstring& fileName, const std::wstring& salt);

        // 16 位大写 hex：SipHash-2-4( UTF16LE(目录 + 盐), key=0^16 ) 之后字节反转的值。
        // 传 "/" 或空串 = 根域（等价于只哈希盐）。
        std::wstring DomainHashHex(const std::wstring& directory, const std::wstring& salt);

        // 目录归一化：'\' -> '/'、转小写、补结尾斜杠、根域（空 / "/" / "%EmptyString%"）记 "/"。
        // 幂等：已经是 "system/" 的输入不会被补成 "system//"。
        std::wstring NormalizeDirectoryName(const std::wstring& raw);

        // 文件归一化：只取裸名（最后一个分隔符之后的部分），**不改大小写**（哈希区分大小写）。
        std::wstring NormalizeFileName(const std::wstring& raw);

        bool IsHexLength(const std::wstring& value, size_t length);
        std::wstring ToUpperString(const std::wstring& value);
        std::wstring ToLowerString(const std::wstring& value);

        // 会社 / 作品名 -> 可以当目录名用的字符串（去掉 Windows 非法字符与首尾空白）。
        // 全被去掉时返回空串，调用方必须据此报错，不要拿空目录名去落盘。
        std::wstring MakeSafePathComponent(const std::wstring& raw);

        // 库安装前的校验：会社与作品名必须能生成非空目录名。
        bool ValidateMeta(const Meta& meta, std::wstring& error);

        // ---------- 摘要（sha256，大写 hex） ----------

        std::wstring Sha256HexBytes(const void* data, size_t size);
        std::wstring Sha256HexFile(const std::wstring& path);   // 读不到返回空串
        std::wstring Sha256HexText(const std::wstring& text);   // 按 UTF-8 编码后摘要

        // ---------- 盐 ----------

        enum class SaltOrigin
        {
            None = 0,
            UniversalLog = 1,  // Log\Universal.log 的 `Hash Seed:`
            StaticKeyJson = 2, // ExtractKey_Output\Static\*.json 的 hash_domain
            Provided = 3       // 调用方直接给的（例如从档案里读回来的盐）
        };

        struct SaltResult
        {
            bool available = false;       // true = 拿到了明确的盐来源（**空串也算**）
            bool known = false;           // 同 available，但语义上强调"记了"（对齐 hash_domain_known）
            std::wstring salt;            // 盐本体（可为空串）
            SaltOrigin origin = SaltOrigin::None;
            std::wstring source;          // 实际读到的文件路径
            std::wstring message;         // 不可用时的显式原因，要能直接给用户看
        };

        // 盐来源顺序：Log\Universal.log 的 `Hash Seed:` -> 静态派生（*.json 的 hash_domain）
        // -> 都没有则 available=false 且 message 说明原因（**绝不静默回落到默认值**）。
        //
        // 与 Yua::Miner::ReadSalt 的唯一有意差别：那边把"Hash Seed 为空串"当成不可用，
        // 这里按设计稿把空串当**合法盐**（saltKnown=true），因为空盐是真实存在的配置。
        SaltResult ResolveSalt(const std::wstring& logDirectory, const std::wstring& staticKeyDirectory);

        // ---------- 生成 ----------

        struct GenerateOptions
        {
            std::wstring hashDirectory;      // <工作区>\StringHashDumper_Output（必填）
            std::wstring logDirectory;       // <工具目录>\Log（盐来源 1）
            std::wstring staticKeyDirectory; // <工作区>\ExtractKey_Output\Static（盐来源 2）
            std::wstring sourceDirectory;    // 采集来源目录（默认 Restored_Extractor_Output）
            std::wstring workspaceRoot;      // 用来把 sourceDirectory 记成相对路径；可空
            std::wstring targetExe;          // 目标 exe 全路径（可空 = 不记 exe 摘要）
            Meta meta;
            // true = 由内核把 meta.generatedAt 填成当前时间。默认 false，保证同一输入重跑
            // 逐字节一致（E-7）。UI 真正生成时置 true。
            bool stampGeneratedAt = false;

            std::function<bool()> cancelRequested;
            std::function<void(unsigned int percent, const std::wstring& stage)> progress;
        };

        struct GenerateResult
        {
            bool ok = false;
            std::wstring error;      // 失败原因（显式，绝不为空）
            Archive archive;
            SaltResult salt;

            SourceCounts sources;                  // 各来源实际读到的条数
            unsigned int collectedDirectories = 0; // 归一化前的目录候选数
            unsigned int collectedFiles = 0;       // 归一化前的文件候选数
            unsigned int normalizedDirectories = 0;
            unsigned int normalizedFiles = 0;
            unsigned int verifiedDirectories = 0;  // 自校验通过的目录条数
            unsigned int verifiedFiles = 0;        // 自校验通过的文件条数
            unsigned int exceptionCount = 0;       // 自校验失败条数（= EXCEPTIONS 条数）
            unsigned int duplicateEntries = 0;     // 去重掉的重名/重哈希条数
            unsigned long long elapsedMillis = 0;  // 生成耗时（进报告，不进档案）

            std::wstring archivePath;        // GenerateArchive 时填
            unsigned long long archiveBytes = 0;
            std::wstring archiveSha256;
            std::wstring infoPath;           // 同目录 info.txt（meta 的只读副本）

            std::vector<std::wstring> warnings; // 非致命问题，按发生顺序
        };

        // 采集 -> 归一化 -> 自校验 -> 组装档案（**不落盘**）。
        GenerateResult BuildArchive(const GenerateOptions& options);

        // BuildArchive + 写 <archivePath>（zlib）+ 同目录 info.txt。
        GenerateResult GenerateArchive(const GenerateOptions& options, const std::wstring& archivePath);

        // ---------- 编解码 ----------

        std::vector<unsigned char> EncodeArchive(const Archive& archive, std::wstring& error);
        bool DecodeArchive(const std::vector<unsigned char>& blob, Archive& out, std::wstring& error);
        bool WriteArchiveFile(const std::wstring& path, const Archive& archive, std::wstring& error);
        bool ReadArchiveFile(const std::wstring& path, Archive& out, std::wstring& error);

        // ---------- 加载（归属校验，§2.1 三分支） ----------

        // 归属是怎么认定的。**不是**"加载成功/失败"——失败性质另见 LoadFailure。
        enum class IdentityMatch
        {
            ExeMatch = 0,   // exe 摘要命中：最强信号
            SaltMatch = 1,  // 摘要不中但**盐确实一致**（可能打过补丁 / 换版本）
            Rejected = 2,   // 拒绝加载（盐不一致 / 无法校验，见 LoadFailure）
            // 摘要不中、**又拿不到当前游戏的盐**，调用方显式 allowUnverified 放行。
            // 单独一个值而不是复用 SaltMatch：这条路上**根本没有盐可比**，
            // 渲染成"盐一致"是与事实矛盾的说法（独立验证 P-2）。
            Unverified = 3
        };

        // 加载失败的性质。UI 据此选出口：**档案结构坏了**和**这是别的游戏的档案**
        // 要给出完全不同的下一步（独立验证 P-3：把两者合并会把用户引去折腾 Universal.log）。
        enum class LoadFailure
        {
            None = 0,          // 没有失败
            Structure = 1,     // 档案结构缺失 / 损坏 / 读不到：
                               // magic、版本、截断、CRC、缺 META、缺 IDENTITY、名字层展开失败、文件读不到
            SaltMismatch = 2,  // 盐不一致 —— 这是**别的游戏**的档案（§2.1 第三条）
            Unverified = 3,    // 摘要不中且拿不到当前盐，而调用方没有打开 allowUnverified
            BadArgument = 4    // 调用方参数不完整（库根 / 会社 / 作品名为空）
        };

        struct LoadOptions
        {
            std::wstring targetExe;        // 当前目标 exe 全路径（可空 = 不做摘要比对）
            std::wstring currentSalt;      // 当前游戏的盐（配合 currentSaltKnown）
            bool currentSaltKnown = false; // 调用方是否拿到了当前游戏的盐
            // true = 摘要不中、又拿不到当前盐时仍允许加载。
            // 放行后 `match` 是 **IdentityMatch::Unverified**（不是 SaltMatch），
            // `matchNote` 里写明"未能校验归属"——**绝不声称盐一致**，因为此时没有盐可比。
            // 默认 false：宁可明确拒绝，也不给出"看起来有数据、实际一条不中"的静默失效。
            bool allowUnverified = false;
        };

        struct LoadResult
        {
            bool ok = false;
            std::wstring error;                        // 拒绝时的显式原因
            IdentityMatch match = IdentityMatch::Rejected;
            LoadFailure failure = LoadFailure::None;   // ok=true 时恒为 None
            std::wstring matchNote;                    // 给报告用的一行
            Archive archive;
            std::wstring archivePath;
            unsigned long long archiveBytes = 0;
            std::wstring archiveSha256;
        };

        // 由目标 exe + 盐来源组装一份 LoadOptions（内部调 ResolveSalt）。
        LoadOptions ResolveLoadOptions(const std::wstring& targetExe, const std::wstring& logDirectory,
                                       const std::wstring& staticKeyDirectory);

        LoadResult LoadArchiveFile(const std::wstring& archivePath, const LoadOptions& options);
        LoadResult LoadArchiveFromLibrary(const std::wstring& libraryRoot, const std::wstring& publisher,
                                          const std::wstring& title, const LoadOptions& options);

        // ---------- 静态 Hash 表（模式1 步骤1 的产物） ----------

        struct StaticHashResult
        {
            bool ok = false;
            std::wstring error;
            std::wstring path;   // <outputDirectory>\StaticHash.lst
            unsigned int directoryEntries = 0;
            unsigned int fileEntries = 0;
            unsigned long long bytes = 0;
        };

        // 用档案内嵌的盐重算哈希，落 <outputDirectory>\StaticHash.lst：
        // `HASH:name`，UTF-8（无 BOM，与 HashRestore_RecoveredNames.lst 一致），
        // 整份重写，16 位目录在前 / 64 位文件在后、按名字不区分大小写排序。
        // 目录名写档案里的形态（"system/"、"根 = /"）：步骤3 的 NormalizeDirectoryName
        // 会把它折回 "system" 与根域空串，口径一致。
        StaticHashResult WriteStaticHashMap(const Archive& archive, const std::wstring& outputDirectory);

        // ---------- 干跑（只读推演，§5） ----------

        struct DryRunOptions
        {
            std::wstring sourceDirectory; // 测试来源目录
            std::function<bool()> cancelRequested;
            std::function<void(unsigned int percent, const std::wstring& currentEntry)> progress;
        };

        struct DryRunMiss
        {
            std::wstring relativePath;
            std::wstring directoryHash; // 大写 16 位
            std::wstring fileHash;      // 大写 64 位
            bool directoryResolved = false;
            bool fileResolved = false;
        };

        struct DryRunResult
        {
            bool ok = false;
            std::wstring error;

            unsigned int totalFiles = 0;
            unsigned int hashNamedFiles = 0;      // 形如 <目录Hash>\<文件Hash>[.后缀]
            unsigned int passthroughFiles = 0;    // 非哈希名文件（.alst 等）
            unsigned int resolvedDirectories = 0; // 命中的目录哈希数（去重）
            unsigned int resolvedFiles = 0;       // 命中的文件哈希数（去重）
            unsigned int fullyResolvedFiles = 0;  // 整路径可还原
            unsigned int missingDirectory = 0;    // 缺目录 Hash（与"缺文件名"正交，两个都缺时各计一次）
            unsigned int missingFileName = 0;
            // 0..100，分母 = hashNamedFiles。口径与步骤3 报告的"还原成功率"一致。
            unsigned int successRate = 0;

            std::vector<DryRunMiss> misses;                // 未命中明细（可导出）
            std::vector<std::wstring> missingDirectoryHashes; // 去重后的未命中目录哈希
            std::vector<std::wstring> missingFileHashes;      // 去重后的未命中文件哈希
        };

        // **不复制、不重命名、不新建任何文件**：只读枚举 + 查表。
        DryRunResult DryRun(const Archive& archive, const DryRunOptions& options);

        // ---------- 扩展集库（§6） ----------

        struct LibraryEntry
        {
            std::wstring publisher;
            std::wstring title;
            std::wstring exeDigest;     // 目标 exe 摘要（主键）
            std::wstring siblingDigest; // _crack 兄弟 exe 摘要（查找时两者都试）
            std::wstring saltDigest;    // 盐摘要
            std::wstring archiveDigest; // 档案摘要
            unsigned int entryCount = 0;
            unsigned int directoryCount = 0;
            unsigned int fileCount = 0;
            unsigned int exceptionCount = 0;
            std::wstring generatedAt;
            std::wstring installedAt;
            std::wstring source;
            std::wstring archivePath; // <库根>\<会社>\<作品>\rules.int（解析出来的绝对路径）
        };

        struct LibraryCatalog
        {
            bool ok = false;
            std::wstring error;
            std::wstring root;
            std::wstring manifestPath;
            std::vector<LibraryEntry> entries;
            std::vector<std::wstring> publishers;                     // 排序去重
            std::map<std::wstring, std::vector<std::wstring>> titles; // 会社 -> 作品（排序去重）
        };

        // 扫 manifest.txt + 实际目录，供 UI 的会社/作品两个列表框用。
        // 库根不存在不算错：返回 ok=true 的空清单（UI 显示"库是空的"）。
        LibraryCatalog EnumerateLibrary(const std::wstring& libraryRoot);

        // 装进库：复制档案到 <库根>\<会社>\<作品>\rules.int + info.txt，并按主键 upsert 清单。
        // 会社/作品取自档案的 META；同一 exe 摘要（或同一 会社/作品）的旧条目被替换，不产生重复条目。
        //
        // sourceLabel 非空时**原样**写进清单的「来源」列；留空 = 沿用 archivePath（既有行为）。
        // 在线更新那条路必须传它：ArchiveUrl(base, 会社, 作品) —— 传进来的 archivePath 是暂存文件
        // `<工具目录>\Log\OnlineStaging\Online_*.rules.int`，入库后就被 CleanupStaging 删掉了，
        // 不换的话「来源」列永远指向一个不存在的路径（独立验证 P-3）。
        bool InstallToLibrary(const std::wstring& libraryRoot, const std::wstring& archivePath,
                              std::wstring& error, const std::wstring& sourceLabel = std::wstring());

        // 解析一份「清单 / 索引」文本：TSV，10 列（表头与 WriteManifest 一致），'#' 开头是注释行。
        // **本地库清单（EnumerateLibrary）与在线索引（Online::FetchIndex）共用这一份实现**，
        // 不许各写一遍 —— 两处口径一分叉就会出现"本地能读、远端读不了"这种怪事。
        //
        // 出错（缺列 / 多列 / 基本信息非法）时 error 必须**点名行号与原因**，不静默跳过。
        // 返回值 false 只代表整份文本不可用；调用方不得把 out 当成有效结果。
        bool ParseManifestText(const std::string& utf8, std::vector<LibraryEntry>& out, std::wstring& error);

        // ---------- 默认路径（§9 约定，UI 与验证脚本共用） ----------

        std::wstring DefaultDraftDirectory(const std::wstring& workspaceRoot);
        std::wstring DefaultArchivePath(const std::wstring& workspaceRoot);
        std::wstring DefaultLibraryRoot(const std::wstring& toolDirectory);
        std::wstring DefaultStaticHashDirectory(const std::wstring& workspaceRoot);
        std::wstring DefaultStaticKeyDirectory(const std::wstring& workspaceRoot);

        // ---------- 给界面/日志用的一行行文本 ----------

        std::wstring DescribeSalt(const SaltResult& salt);
        std::wstring DescribeGenerateResult(const GenerateResult& result);
        std::wstring DescribeLoadResult(const LoadResult& result);
        std::wstring DescribeDryRunResult(const DryRunResult& result, const LoadResult* load);
    }
}
