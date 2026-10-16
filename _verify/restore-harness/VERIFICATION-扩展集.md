# 独立验证报告：扩展集一期（task-4）

- 验证者：`extension-verifier`（独立于 task-1/2/3 的实现者与脚手架作者；**不采信任何一方报告**，只认自己跑出来的证据）
- 验证时间：2026-10-13 21:16 ~ 21:45
- 脚手架：`_verify/restore-harness/`（task-3 交付；**本轮一个字节都没改**，只读使用）
- 验收依据：`docs/扩展集设计.md` §11（E-1..E-10）、§5（干跑展示口径）、§13.12（归属被拒的出口）
- **结论：一期闭环成立。** E-1..E-10 + X-6 的**实质判据**全部通过（30/30 + 83/83，退出码 0），未发现会让闭环失效的产品缺陷。
  但**发现 4 条产品侧问题（1 中 + 2 轻 + 1 极轻）**、**6 条脚手架侧问题（1 重 + 1 中 + 4 轻）**。
  其中**最严重的一条是脚手架侧**：E-4 的「盐不匹配」用例是**假通过**——它从来没有测到盐不匹配路径（§6.2 S-1）。
- 口径分歧裁决：**面板 5 / 12 / 12 正确**，`13` 是"文件条目数"口径（§4）。

---

## 1. 被验证产物（版本指纹）

| 产物 | 时间戳 | 大小 | SHA256 | 位数 |
|---|---|---|---|---|
| `Release\YuaExtract.exe` | 2026-10-13 21:15:27 | **998,400** | **`9CC65F9072DA91593EE05591EDD2F60BDAB02A040C8541CA60C59E1539D22C78`** | x86 (PE32) |
| `Release\CxdecExtractordll\CxdecStringDumper.dll` | 2026-10-12 02:38:37 | 342,528 | `F3F1E9371F07D881E2DF69ECA90126F38BC6E6F4ADE2675C1E3E6E2DAAA6E4B9` | x86 (PE32) |

构建（**我自己跑的**，全解决方案 Release|x86，跑前 `Get-Process YuaExtract | Stop-Process -Force`）：

```
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe' `
  'YuaExtract.sln' /p:Configuration=Release /p:Platform=x86 /m /v:minimal /nologo
→ 退出码 0；error 0 行、warning 0 行
→ 完整日志：Run\out\build-release-x86-verifier.log
```

**指纹纪律核对（本仓库的规矩）**：

| 项 | 构建前 | 构建后 | 判定 |
|---|---|---|---|
| `YuaExtract.exe` 大小 | 998,400 | 998,400 | 未变 |
| `YuaExtract.exe` mtime | 2026-10-13 21:15:27 | 2026-10-13 21:15:27 | 未变（未重链接） |
| `YuaExtract.exe` sha256 | `9CC65F90…22C78` | `9CC65F90…22C78` | **与冻结指纹逐字节一致** |
| 源码树摘要（379 个 `.cpp/.h/.rc/.vcxproj`，路径+大小+mtime） | `45F09561466E5FE2D81459BE7D0C6345E541008D2603B962D6B7AAF805386140` | 同左 | 我全程**没有碰过源码** |

> 源码树摘要在**验证结束时**再算一次仍为 `45F09561…6140`，且 `_verify\restore-harness\**` 下所有脚本的 mtime 全部 ≤ 20:34:45（早于我进场）
> —— 即"我没改产品代码、也没改 task-3 的脚本"这句话是**可核对的事实**，不是自述。

---

## 2. 命令与结果表

| 命令 | 结果 | 证据 |
|---|---|---|
| MSBuild 全解决方案 Release\|x86 | **exit 0，0 error / 0 warning**，产物指纹不变 | `Run\out\build-release-x86-verifier.log` |
| `pwsh -File '_verify\restore-harness\run-all.ps1' -Scope extension` | **exit 0；30/30 通过**（E 组 11 + 扩展集断言自检 19） | `Run\out\verifier-run-all-extension.log`、`checks-extension.json`、`checks-selftest-extension.json` |
| `pwsh -File '_verify\restore-harness\run-all.ps1' -Scope all` | **exit 0；83/83 通过**（A 21 + B 10 + C/E 14 + 自检 8 + E 11 + 扩展集自检 19） | `Run\out\verifier-run-all-all.log` |

`-Scope all` 的 83 项明细（我按汇总段逐条核对，非转抄）：

```
断言 A（默认语义）            21/21     断言 B（勾选保留）        10/10
断言 C + E（只收集/环境变量） 14/14     断言自检（A/B/C/D/P）      8/8
扩展集 E-1..E-10 + X-6        11/11     扩展集断言自检            19/19
```

> 两条命令都**独立跑过**，且 `-Scope extension` 跑了两次（第一次 + `-Scope all` 里的那次），两次结论一致。

---

## 3. 逐条断言实测（E-1..E-10 + X-6）

下表"实测"列全部是我**从证据文件里重新读出/重新枚举**的数，不是抄断言脚本的结论行。

| 断言 | 我的实测值 | 我的独立复核 | 证据 | 判定 |
|---|---|---|---|---|
| **E-1** 闭环免跑游戏 | 模式1 输出 15 个资源 / 传统动态模式 15 个；只在模式1 0、只在传统 0、内容不同 0；输入内容摘要两侧同为 `EE33C484411DE148`（20 文件）；模式1 工作区**盐文件 0 / 动态采集成果 0** | 15 = 夹具 12 可还原 + 3 明文名（我自己枚举 NewUnpacked：15 哈希名 = 12 可还原 + 3 不可还原，另 3 明文名 + 2 日志） | `extension-mode1.json`、`extension-classic.json`、`extension-manifest.json` | **PASS** |
| **E-2** 生成期自校验 | 未归一化夹具 EXCEPTIONS **5** 条，点名 5 个未归一化目录：`UI`、`sub\dir\nested`、`Characters\Yuki`、`voice\SE`、`Event\Cg`；正常夹具 EXCEPTIONS **0** | 5 个目录名确实一个都没做「小写 + 结尾斜杠」归一化；正常档案我 dump 过，EXCEPTIONS=0 | `extension-raw.json`、`extension-old.json` 的 `archiveDump` | **PASS** |
| **E-3** 干跑 == 实测 | 面板：**可解目录 5 / 文件名可解 12 / 整路径可还原 12 / 成功率 80%**；哈希名 15 / 扫描 19；缺目录 2 / 缺文件名 2；未命中明细 目录 1 / 文件 2。真实步骤3（模式1）还原 **12** | 我用夹具**独立枚举**：5 / **12** / 12 / 80.00% / 15；缺目录条目 2 / 缺文件名条目 2；未命中目录去重 1 / 未命中文件去重 2 —— **与面板逐项相等**（详见 §4） | `verifier-dryrun-old.json`（我自己复跑的干跑）、`extension-mode1.json`、`extension-mode1-StaticHash.lst` | **PASS**（但 E-3 从不判 ②，见 S-3） |
| **E-4** 坏档案必须被拒 | CRC / 截断 / 版本过高 / 缺 IDENTITY / 空档案 **5/5 明确拒绝且不产出 lst**；「盐不匹配」子用例**也是"被拒"，但拒的理由是错的** | 我逐条读对话框原文（§5.2）；另**自造**一份真·盐不匹配档案，验证真路径可用（§5.3） | `extension-bad-*.json`、`verifier-step1-salt-mismatch-real.json` | **PASS（带假通过）** |
| **E-5** 不伪造名字 | 负例名 `nosuch_999_9999` 出现 **0** 次；输出 16 条全部可追溯到声明来源（15 资源 + `RestoreReport.txt`） | 我自己数：模式1 输出目录 = 15 个资源 + 1 份报告 = 16 | `extension-mode1.json` | **PASS** |
| **E-6** 源目录只读 | 6 组前后摘要全等：旧作解包目录 20 文件、**旧作"生成来源目录" 21 文件**、raw 夹具 1 文件 ×2、模式1 解包 20、传统解包 20 | 我复核了那个"21 文件"：`OldGame\Restored_Extractor_Output` 由夹具生成时确实是 21 个（15 哈希名 + 4 明文 + 2 日志），不是我数错（§5.5） | 各 run JSON 的 `*DigestBefore/After` | **PASS** |
| **E-7** 幂等 | 两次生成 **734 字节 / `452A9291C806FC82`**，sha256 相同；库清单只有 **1** 条数据行 | 我 dump 了档案：734 B、`452A9291C806FC82`、META 里 `generatedAt=2026-10-13 21:25:52`。**但这个"字节一致"有前提**（§6.2 S-4）：第二次生成继承了第一次的 `generatedAt` | `extension-old.json`、`extension-old-repeat.json`、`verifier-e7-fresh1/2.json` | **PASS（口径需修正）** |
| **E-8** 体积 | 档案 **734 字节（0.7 KiB）**；DIRS 5 / FILES 15 / EXCEPTIONS 0；36.7 字节/条；容器结构自洽（magic/CRC/偏移） | 我用脚手架自带 `lib/rules-int.py` 独立 dump：734 B、`452A9291C806FC82`、chunkCount 6、CRC 实算=存储、偏移全在范围内 | `archive-dump-old-draft.json` | **PASS**（但鉴别力弱，见 §7） |
| **E-9** 大小写与分隔符 | `StaticHash.lst`：16 位目录 **5** 条 / 64 位文件 **15** 条，不合格 **0**；档案 DIRS 5 / FILES 15，不合格 0；归一化单测 6 例 0 失败 | 我按 §3 口径逐行正则复核了 `StaticHash.lst` 的 20 行：5 行 16 位、15 行 64 位、无大写目录、无带斜杠文件名 | `extension-mode1-StaticHash.lst` | **PASS** |
| **E-10** 模式1 ≡ 传统动态模式 | 模式1 15 / 传统 15；只在模式1 0、只在传统 0、内容不同 0；报告里写明映射来源 | 两个模式的输入是同一份内容摘要（`EE33C484411DE148`），差异只可能来自映射目录；我用故障注入另行证明了映射目录确实按模式分派（§5.6） | 同上 E-1 | **PASS** |
| **X-6** 脚本自身无静默空转 | 整脚本 **0.6 秒**；最长单条断言 227 ms；无超预算段 | 我复核了耗时表：最长 227 ms（StaticHash.lst 20 项枚举），预算 120 秒 | `checks-extension.json` | **PASS** |

---

## 4. 【裁决】口径分歧：面板"文件名可解"应是 12 还是 13

### 4.1 结论先说

| 说法 | 值 | 口径 | 判定 |
|---|---|---|---|
| task-2 实测的面板值 | **12** | 命中的 **64 位文件哈希去重** | ✅ **正确**（= 设计稿 §5 = 内核 `DryRunResult::resolvedFiles`） |
| task-3 主张的值 | 13 | 命中的**文件条目数**（重复哈希计两次） | ❌ 与 §5/内核不一致；它是**脚手架自己的独立枚举**算出来的（见 S-3） |

**面板 5 / 12 / 12 正确**；设计稿 §5 的**规则**自洽，但 §5 里那句**理由**不适用于本夹具（见 4.4）。

### 4.2 我自己在夹具上的独立枚举（不读任何报告）

**枚举对象**：`Run\ExtensionFixture\NewUnpacked`（20 个文件）
**映射表来源**：只用**原始采集成果**（`DirectoryHash.log` / `FileNameHash.log` / `MinedNames.lst` / `HashRestore_RecoveredNames.lst` / `ExtraManual.lst`），**不读产品产物**

```
TOTAL-FILES = 20    HASH-NAMED = 15    PLAINTEXT = 3    LOGS = 2
映射表（原始来源）：DIRS = 5 个不同 16 位哈希 / FILES = 15 个不同 64 位哈希
```

**15 个哈希名文件 → 64 位文件哈希去重 = 14 个不同哈希**（有 1 个哈希出现两次）：

| # | 相对路径 | 16 位目录命中 | 64 位文件命中 | 整路径可还原 |
|---|---|---|---|---|
| 1 | `bgimage\71307BFDBC8EE25F\928E0FD3…A1E5` | ✗ `71307BFDBC8EE25F` 不在映射表 | ✓ | ✗（**目录未知但文件名已知**） |
| 2 | `bgimage\A35EF79C550C3DB9\928E0FD3…A1E5` | ✓ | ✓ | ✓ |
| 3 | `bgimage\A35EF79C550C3DB9\A1EABA40…B132` | ✓ | ✓ | ✓ |
| 4 | `bgimage\AEA3B033A006D5C1\AC4CD10A…342D` | ✓ | ✓ | ✓ |
| 5 | `data\71307BFDBC8EE25F\A6B35530…43D7` | ✗ | ✗ | ✗（两缺） |
| 6 | `data\94D4A97C61498621\C3E5A5B1…D87B` | ✓ | ✓ | ✓ |
| 7 | `data\A35EF79C550C3DB9\23AD7C55…B6E4` | ✓ | ✗ | ✗（缺文件名） |
| 8 | `evimage\AC4AF8CB007B6FCC\707C9BCB…A07E` | ✓ | ✓ | ✓ |
| 9 | `evimage\AC4AF8CB007B6FCC\7C2DDFF4…D726` | ✓ | ✓ | ✓ |
| 10 | `extra\AC4AF8CB007B6FCC\D22973C1…A0BE` | ✓ | ✓ | ✓ |
| 11 | `extra\AEA3B033A006D5C1\EF520DAC…CAA8` | ✓ | ✓ | ✓ |
| 12 | `scn\44B0624B2446C200\CFD0D77A…8D15` | ✓ | ✓ | ✓ |
| 13 | `scn\A35EF79C550C3DB9\4CBA3BA5…DF28` | ✓ | ✓ | ✓ |
| 14 | `scn\A35EF79C550C3DB9\A51CBFB8…5986` | ✓ | ✓ | ✓ |
| 15 | `scn\A35EF79C550C3DB9\F9D43280…0402` | ✓ | ✓ | ✓ |

我算出来的五个数（**与面板逐项相等**）：

```
hashNamedFiles          = 15
resolvedDirectories     = 5     （不同 16 位目录哈希命中数）
resolvedFiles（去重）    = 12    （不同 64 位文件哈希命中数）   ← ② 的正确值
文件【条目】命中数       = 13    （重复哈希计两次）            ← 13 的来源
fullyResolvedFiles      = 12
successRate             = 80%（12 ÷ 15）
缺目录条目 2 / 缺文件名条目 2；未命中目录去重 1 / 未命中文件去重 2
```

**12 与 13 的差在哪，一句话**：

> 15 个哈希名条目里，**13 个**的 64 位文件哈希能在映射表里查到（只有 `A6B35530…` 与 `23AD7C55…` 两个查不到）；
> 但这 13 个条目里，`928E0FD31426902B2E90128E8A2DC78DFB1D7D9283E6FCEAA5E884190679A1E5` 出现了**两次**
> （`bgimage\71307BFDBC8EE25F\` 目录未知、`bgimage\A35EF79C550C3DB9\` 目录已知），**按哈希去重后 = 12**。

### 4.3 内核口径（权威依据）

`CxdecExtractorLoader\ExtensionSet.cpp`：

```cpp
std::set<std::wstring> hitFiles;                     // 2774：set ⇒ 天然去重
if (fileResolved) { hitFiles.insert(fileHash); }     // 2842：按【哈希】插入，不是按条目
result.resolvedFiles = static_cast<unsigned int>(hitFiles.size());   // 2879
if (directoryResolved && fileResolved) { ++result.fullyResolvedFiles; }  // 2852：按【条目】计数
result.successRate = fullyResolvedFiles * 100 / hashNamedFiles;      // 2882-2886
```

`ExtensionSet.h:344` 的注释也写死了：「`resolvedFiles` // 命中的文件哈希数（**去重**）」。
面板直接打印 `job.DryRun.resolvedFiles`（`YuaExtractMainWindow.cpp:2185`），所以我复跑干跑拿到的 `文件名可解（去重）：12` 就是内核值。

### 4.4 设计稿 §5 是否自洽

§5 的**规则**自洽：① 目录按 16 位去重、② 文件按 64 位去重、③ 整路径按条目、成功率 = ③ ÷ 哈希名数 —— 与内核逐条对得上。

§5 的**理由**不自洽（文档精度问题，**不是产品缺陷**）：

> §5 原文：「所以在"每个可解文件名都恰好有可解目录"的夹具里 ② 与 ③ 会相等 —— 那是正常的，不是缺陷。」

本夹具**恰恰违反**这个前提：`928E0FD3…` 有一个"目录未知但文件名已知"的中间态出现。
按 §5 的理由推，本该 ② > ③；实测 ② = ③ = 12。真正的原因是**另外两条**：

1. 去重把重复的 `928E0FD3…` 折成一条；
2. 12 个命中的文件哈希，**每一个都至少有一次出现在已知目录下**。

也就是说：§5 括号里那句「可能含'目录未知但文件名已知'的中间态」**描述对了**（夹具里确实有），
但紧接着给的理由**不能推出** ② = ③。**结论对、理由错**，建议改成"去重后两者相等当且仅当每个命中哈希都有已知目录的落点"。

> 附带：这个夹具**恰好不能**区分"② = 去重哈希数"与"② = ③"（两者都是 12），
> 但它**能**区分 12（去重）与 13（条目）—— 这正是本次分歧的判别力所在。

---

## 5. 主动找的反例

### 5.1 坏档案（E-4 覆盖的 5 类 + 我自造的 3 类）

| 反例 | 做法 | 实测结果 | 证据 |
|---|---|---|---|
| CRC 错 | 脚手架 `rules-int.py mutate` | 拒绝：`CRC32 校验失败：档案内容与头部记录不一致（已损坏或被改动）。`，无 lst | `extension-bad-crc-error.json` |
| 截断 | 同上 | 拒绝：`payload 解压失败（zlib status=18446744073709551613）：档案已损坏。`，无 lst（**状态码是垃圾数字**，见 P-4） | `extension-bad-truncated.json` |
| 版本过高 | 同上（version=0xFFFF，CRC 合法） | 拒绝：`档案格式版本过高：文件是 65535，本工具只认 1。请升级 YuaExtract。`，无 lst | `extension-bad-version-high.json` |
| 缺 IDENTITY | 同上（摘掉 IDENTITY，重算表与 CRC） | 拒绝，但**报成了"别的游戏"**（见 P-3） | `extension-bad-missing-identity.json` |
| 空名字档案 | 同上（只留 META/IDENTITY/STATS） | 拒绝：`档案里没有任何名字（DIRS/FILES 都是空的），生成的静态 Hash 表会是空的，按设计明确报错而不是写一个空表出去。`，无 lst | `extension-bad-empty-names.json` |
| **盐不匹配（脚手架的用例）** | 把 `OtherGame\ExtensionDraft\rules.int` 装进库 | **拒绝，但理由不是盐**：报的是**空名字档案**那条消息。该档案实测 `DIRS 0 / FILES 0 / EXCEPTIONS 2`、`salt = xp3hnp`（**与当前游戏同盐**）→ **假通过**，见 §6.2 S-1 | `extension-bad-salt-mismatch.json` |
| **盐不匹配（我自造的真档案）** | 拿好档案改 IDENTITY：`salt=othersalt123`、`target.sha256=451BA16F…`（名字层保持非空 5/15），装进库 | **拒绝且理由正确**：对话框 `【扩展集与当前游戏盐不同】…该扩展集属于 YuaHarness/otomeki_ckr，与当前游戏的盐不同，拒绝加载。（哈希不可跨作移植：盐变了档案里一条都不会中。）`；无 lst；按钮只有 `是(&Y),否(&N)`，**没有"强行使用"这一项** | `verifier-step1-salt-mismatch-real.json`、`craft-report.json` |
| **只有目录、没有文件** | 我自造 `dirs-only.rules.int`（META/IDENTITY/DIRS/STATS，5 目录 0 文件，结构自洽） | 步骤1 **接受**并写出 `StaticHash.lst` **5 行**（只有 16 位目录）；步骤3 结果：哈希名 15 / **已还原 0** / 未还原被丢弃 15 / 明文名原样 3 —— **0% 且如实报数，不崩、不伪造** | `verifier-step1-dirs-only.json`、`verifier-mode1-dirs-only.json` |
| **只有文件、没有目录** | 我自造 `files-only.rules.int`（0 目录 15 文件） | 步骤1 接受并写出 **15 行**（只有 64 位文件）；步骤3：哈希名 15 / **已还原 0** / 丢弃 15 / 明文 3 | `verifier-step1-files-only.json`、`verifier-mode1-files-only.json` |
| **空档案** | 见上（empty-names） | 明确报错，不写空表 | 同上 |

### 5.2 归属校验被拒时的出口（设计稿 §13.12）—— **两条路都在，且实测走得通**

构造：库档案 = 我自造的盐不匹配档案（exe 摘要 `451BA16F…` ≠ 当前 `5D7DE330…`），并把 `<工具目录>\Log\Universal.log` 移开 → **拿不到本作盐**。

| 步骤 | 实测对话框 | 按钮 |
|---|---|---|
| 点步骤1 | `【扩展集未能校验归属】这份扩展集无法在当前游戏上校验归属：…推荐出路：先做一次「静态 Key 提取」（离线，不用启动游戏），拿到本作的盐之后再自动重试。点「确定」= 现在做静态 Key 提取并重试；点「取消」= 再问一次要不要「不校验、强行使用」。` | `是(&Y),否(&N)` |
| 答「否」后 | `【强行使用扩展集（未校验）】不校验归属、强行使用这份扩展集？强行使用不会报错，但展开出的静态 Hash 表可能一条都不中（看起来有数据、实际静默失效）。点「确定」= 强行使用（allowUnverified=true）；点「取消」= 放弃本次步骤1。` | `是(&Y),否(&N)` |
| 答「是」后 | 任务跑完，写出 `StaticHash.lst` **20 行**；成功框 `扩展集加载成功：YuaHarness / otomeki_ckr　摘要不中、盐一致` + `来源：扩展集 YuaHarness/otomeki_ckr（**未能校验归属**：exe 摘要不中且没有可用的当前游戏盐）` | `确定` |

**"未能校验归属"这句话到底写在哪（三处逐一查）**：

| 位置 | 是否写明 | 实测 |
|---|---|---|
| 成功**模态框** | ✅ 写了 | `（**未能校验归属**：exe 摘要不中且没有可用的当前游戏盐）` |
| **概览面板**（`IDC_YUA_OVERVIEW`） | ❌ **没写** | 全文搜 `未能校验归属` / `未校验` / `归属` / `摘要不中` / `盐` → **各 0 次**；只有 `状态：已用扩展集生成静态 Hash 表（…）` |
| **日志**（`<工具目录>\Log\`） | ⚠️ 写了但**不是设计稿要求的措辞** | `YuaExtract.log: [YuaExtract] 模式1 步骤1：用户选择强行使用未校验的扩展集`（Warn）；全文搜 `未能校验归属` → **0 次** |

→ 设计稿 §13.12 要求「**概览与日志**写明"未能校验归属"」：**概览没写**，日志写了同义但不同字的话。这是产品侧问题 **P-1**。

**盐不同（别的游戏）时"强行使用"救不救得了**：

- 内核层面**救不了**，且是硬编码的：`ExtensionSet.cpp:2575-2591` 在 `currentSaltKnown` 为真且盐不同时**直接 return Rejected**，`allowUnverified` 根本不在这个分支里读（它只在 2593-2601「摘要不中且拿不到盐」那条路上被读）。
- UI 层面也没有给这条出路：盐不同时的对话框只给「是 = 打开日志目录去处理 / 否 = 关掉」，**没有"强行使用"按钮**（实测按钮列表只有 `是(&Y),否(&N)`）。
- 与设计稿 §13.12 一致。**PASS**。

### 5.3 把 A 作的扩展集用在"别的游戏"上

| 做法 | 实测 | 判定 |
|---|---|---|
| 好档案（OldGame，盐 `xp3hnp`）放到 **OtherGame 工作区**（exe = `othergame.exe`，摘要 `451BA16F…` ≠ 档案的 `5D7DE330…`），工具 Log 里是 `Hash Seed:xp3hnp` | **被接受**！对话框：`扩展集加载成功：YuaHarness / otomeki_ckr　摘要不中、盐一致` / `来源：扩展集 …（盐一致、exe 不同：可能打过补丁或换过版本）`，写出 20 行 | ⚠️ **见 S-1** |
| 真·盐不同（我自造档案，`salt=othersalt123`）+ 当前盐 `xp3hnp` | **拒绝**，理由正确，无 lst（§5.1） | ✅ |
| 强行使用（allowUnverified=true）+ 盐不同 | 内核无条件拒绝（源码 2575-2591 + UI 无该按钮） | ✅ |

> 上表第 1 行**不是产品缺陷**：产品此时拿到的"当前游戏盐"确实是 `xp3hnp`（因为 `Universal.log` 里就是它），
> 按 §2.1 的规则「摘要不中、盐一致 → 可用，但标注"盐一致、exe 不同"」它**判对了**。
> 问题在于**夹具无法表达"别的游戏"**：`run-extension-ui.ps1` 不管 `-Game` 是谁，都往**同一个共享** `Log\Universal.log` 写**主夹具的盐**，
> 于是"别的游戏"的当前盐永远等于档案的盐。这正是 E-4 假通过的根因（S-1）。

### 5.4 模式切换：文案 **与** 点击分支（不只看文字）

**文案与互斥勾选**（我直接读运行中的控件，不是读源码）：

| 点下的模式按钮 | `BM_GETCHECK`（4030/4031/4032） | 4050 文案 | 4051 文案 | 4052 文案 |
|---|---|---|---|---|
| `该作扩展集提取`(4030) | **1 / 0 / 0** | 步骤1：根据该作扩展集生成静态 Hash 表 | 步骤2：提取动态 XP3 资源 | 步骤3：用静态 Hash 还原资源名 |
| `该会社合集撞新作`(4031) | 0 / **1** / 0 | 步骤1：待定（二期预留） | 步骤2：待定（二期预留） | 步骤3：待定（二期预留） |
| `传统动态模式`(4032) | 0 / 0 / **1** | 步骤1：提取动态 XP3 资源 | 步骤2：加载动态Hash收集模块 | 步骤3：用动态Hash还原资源名 |

三组文案互不相同、勾选严格互斥（每次恰好一个 = 1）。证据：`Run\out\verifier-ui-probe.json`。

**点击分支的行为**（故障注入：把 `CxdecExtractorUI.dll` 与 `CxdecStringDumper.dll` 从**快照副本** `Run\AppProbe\` 挪走，副本 exe 与被测 exe 同 sha256）：

| 点击 | 该分支要求的模块 DLL（对话框原文） | 说明 |
|---|---|---|
| 模式1 步骤1 | **不需要任何模块** → 直接写出 `StaticHash.lst` 20 行，状态 `已用扩展集生成静态 Hash 表` | 走 `OnMode1Step1`（离线展开），与"提取"无关 |
| 模式1 步骤2 | 缺 `CxdecExtractorUI.dll` | = 传统模式的步骤1（提取 XP3） |
| 传统模式 步骤1 | 缺 `CxdecExtractorUI.dll` | **与"模式1 步骤2"逐字相同** ⇒ 两者是同一件事 |
| 传统模式 步骤2 | 缺 `CxdecStringDumper.dll` | 动态 Hash 收集模块，与模式1 步骤2 **不同** |

这四条一起构成"三个按钮的**点击分支真的按 `ActiveMode` 分派**"的行为证据（不是只看按钮文字）。
证据：`Run\out\verifier-ui-probe3.json`（`dispatch` 段）。步骤3 的分派另由 E-1/E-10 证明（模式1 用 `StaticHash_Output`、传统用 `StringHashDumper_Output`，输出逐条一致）。

### 5.5 干跑来源目录的一个陷阱（记录）

夹具生成的 `OldGame\Restored_Extractor_Output` 是 **21 个文件**（15 哈希名 + 4 明文 + 2 日志）→ 干跑扫到的非日志文件 = **19**、哈希名 = 15，与面板的 `哈希名文件：15 / 扫描文件总数：19` 完全对得上。
但**传统动态模式的步骤3 会覆写同一个目录**（§9 的 `Restored_Extractor_Output`），全量跑完后它变成 16 个已还原文件。
我在全量跑完后单独复跑一次干跑，面板给的是 **`可解目录 0 / 文件名可解 0 / 整路径可还原 0 / 成功率 0%（0 ÷ 0）`、扫描 16** —— **不是产品回归，是目录被上一次用例改了**。
证据：`verifier-dryrun-old.json`（干净夹具，5/12/12）对 `verifier-dryrun-after-classic.json`（被 classic 覆写后，0/0/0）。

---

## 6. 暴露的问题

### 6.1 产品侧缺陷（4 条）

> 定级依据：**P-1** 违反设计稿明文要求且影响可追溯性；**P-2/P-3** 是对用户说错话；**P-4** 是诊断信息质量。
> **没有一条会让 E-1..E-10 的实质结论失效**（闭环、拒绝、幂等、只读、体积都成立）。

**P-1（中）「不校验、强行使用」后，概览面板没有写明"未能校验归属"（违反 §13.12）**

- 现场：用户在「强行使用扩展集（未校验）」上点「是」→ 档案被采用、`StaticHash.lst` 写出 20 行。
- 实测：`IDC_YUA_OVERVIEW` 全文**不含** `未能校验归属` / `未校验` / `归属` / `摘要不中` / `盐`（各 0 次）；只有 `状态：已用扩展集生成静态 Hash 表（2026-10-13 21:37:31）`。
- 日志只有 `[YuaExtract] 模式1 步骤1：用户选择强行使用未校验的扩展集`（Warn），也没有那句措辞。
- 根因（代码）：`YuaExtractMainWindow.cpp:2126` 设了 `context.StageNote = L"扩展集未能校验归属（用户已确认强行使用）"`，
  但任务成功分支在 `2080` 把 `StageNote` 覆盖成 `已用扩展集生成静态 Hash 表（…）`；概览只渲染 `StageNote`/`LastStaticNote`，**不渲染内核的 `matchNote`**（那句 `未能校验归属` 只在成功**模态框**里出现，一闪而过）。
- 影响：设计稿把"写明未能校验归属"当作静默失效的防线，现在这条防线只剩一个必须点掉的弹框，**事后翻概览/日志查不出来**。
- 建议（由实现者定）：在概览的「扩展集状态」里持久保留 `LoadResult::matchNote`（或加一行"归属校验：未校验（用户强行使用）"），并把 Warn 日志改成含"未能校验归属"字样。

**P-2（轻）强行使用成功框首行自相矛盾：「摘要不中、盐一致」，而此时根本没有盐**

- 实测成功框：`扩展集加载成功：YuaHarness / otomeki_ckr　摘要不中、盐一致`，下一行却是 `（**未能校验归属**：exe 摘要不中且**没有可用的当前游戏盐**）`。
- 根因：`ExtensionSet.cpp:2597` 在 `allowUnverified` 分支把 `result.match` 设成 `IdentityMatch::SaltMatch`，
  而 `IdentityMatchText`（`1219`）把 `SaltMatch` 一律渲染成 `"摘要不中、盐一致"`。语义上这是"放行"，不是"盐一致"。
- 建议：给这条路径一个独立标签（如 `未校验（用户强制）`），或让 `IdentityMatchText` 依据 `matchNote` 区分。

**P-3（轻）缺 IDENTITY 的档案被报成"这份扩展集属于别的游戏，与当前游戏的盐不同"**

- 实测对话框：标题 `【扩展集与当前游戏盐不同】`，正文第一句 `这份扩展集属于别的游戏，与当前游戏的盐不同，已拒绝加载：` 后面才是真实原因 `档案缺少 IDENTITY chunk（0x0002）：没有盐与目标 exe 摘要，无法确认它属于哪部游戏，拒绝加载。`
- 根因：`YuaExtractMainWindow.cpp:2131` 的 `else if (job.IdentityRejected)` 把"**结构缺失**（没有 IDENTITY）"与"**盐确实不同**"合并成同一个出口；
  它给的两条出路（重跑步骤2 覆写日志 / 移开 `Universal.log`）对一个**损坏档案**毫无用处。
- 影响：用户被引导去折腾 `Universal.log`，而真正该做的是重新生成/换一份档案。内层 `error` 说对了，所以不是静默错误，但标题与首句是**事实错误**。
- 建议：按"缺 IDENTITY / 解码失败"与"盐不同"分成两个出口。

**P-4（极轻）截断档案把 zlib 内部状态码打印成无符号 64 位垃圾数字**

- 实测：`payload 解压失败（zlib status=18446744073709551613）：档案已损坏。` —— 该数是 `(uint64)(-3)`（`Z_DATA_ERROR`）。
- 影响：纯诊断可读性。建议打印有符号值或映射成人话（"数据损坏"）。

### 6.2 脚手架侧问题（6 条）

**S-1（重）E-4 的「盐不匹配」用例是假通过：它根本没测到盐不匹配**

- 事实链（全部我自己枚举/读取，不采信报告）：
  1. 用例档案 `Run\ExtensionFixture\OtherGame\ExtensionDraft\rules.int` 实测：**`DIRS 0 条 / FILES 0 条 / EXCEPTIONS 2 条`**、`IDENTITY.salt = "xp3hnp"`。
     —— 也就是说这份"别的游戏"的档案**一个名字都没有**，而且**盐与主夹具完全相同**（`othersalt123` 只存在于夹具的采集日志里，没进档案）。
  2. 于是模式1 步骤1 命中的是**空名字防线**：`档案里没有任何名字（DIRS/FILES 都是空的），生成的静态 Hash 表会是空的…`（与 `empty-names` 用例**逐字相同**）。
  3. 为什么名字是空的：`run-extension-ui.ps1:591-604` 无条件把**主夹具的盐**写进共享的 `<工具目录>\Log\Universal.log`（不看 `-Game`）；
     OtherGame 的采集日志是用 `othersalt123` 算的哈希，产品用 `xp3hnp` 重算 → §3.2 生成期自校验把两条**如实**判为例外 → `DIRS/FILES` 空。
  4. 断言为什么还是绿的：`assert-extension.ps1:726` 的 `Get-EvidenceText` 把概览/状态/日志拼成一个大 blob，
     `salt-mismatch` 的关键词是 `@('盐','归属','会社','作品','不一致','属于','不匹配')`。
     我在该 run JSON 上复算：blob 里 `盐/归属/不一致/属于/不匹配` **各 0 次**，只有 `会社`×2、`作品`×3 —— 全部来自**页面套话**（`先在左侧「扩展集选择」里选会社，再选作品。`）与 `会社：YuaHarness`。**关键词是靠 UI 套话命中的。**
- 我另外证明**真路径是好用的**：自造 `salt-mismatch-real.rules.int`（5 目录/15 文件、`salt=othersalt123`、`target.sha256=451BA16F…`）→ 正确拒绝、正确对话框、无 lst（§5.1/§5.2）。
- 影响：E-4 名义上覆盖"盐不匹配"，实际覆盖不到；**这是本期最需要修的一条**（且属脚手架，产品无责）。
- 建议（task-3）：① 让"别的游戏"档案**有名字**（生成前把工具 Log 的盐换成 `otherSalt`，或改用 `-NoToolSalt` 并预置正确盐）；
  ② 把 E-4 的关键词从"大 blob 里出现过"收紧为"**拒绝原因字段/对话框正文**里出现"，并加一条**负例**（把"因别的原因被拒"的 run JSON 喂给断言，要求它炸）。

**S-2（中）扩展集断言自检对"被拒但拒错理由"没有鉴别力**

- `self-test-extension.ps1` 的 E-4 场景只有两个：`e4-accepts-bad`（产品**接受**了 CRC 坏档案）、`e4-accepts-empty-names`（产品**接受**了空名字档案）。
  两者都是"该拒不拒"，**没有任何场景是"拒了、但理由不是本用例声称的那个"** —— 所以它**测不出 S-1**。
- 结论：**19/19 是真绿，但它证明不了 E-4 有鉴别力**。S-1 恰好落在这个盲区里。
- 建议：补 `e4-rejects-for-wrong-reason` 场景（把 `salt-mismatch` 的 run JSON 换成一个因空名字被拒的 JSON，要求 E-4 炸）。

**S-3（轻）E-3 的"独立枚举"里 ② 用的是文件**条目数**（13），与 §5/内核（64 位哈希去重 = 12）口径不一致**

- 实测断言输出：`独立枚举 [NewUnpacked]：可解目录 5 / **可解文件 13** / 完整路径可还原 12 / 哈希名总数 15（80.00%）` —— 面板同项是 **12**。
- 这条**不影响结论**（§5 明确"② 只记录不判"，E-3 只判 ③ 与成功率），但它正是 12 vs 13 分歧的出处，
  而且 **E-3 从头到尾没有读取/比对过面板的 ②**（它只读 `整路径可还原`、`成功率`、`可解目录`）。
- 建议：独立枚举 ② 改为按 64 位哈希去重；并把面板的 `文件名可解` 一并记入证据（即使不判）。

**S-4（轻，但会误导）E-7 的"档案字节可复现"依赖 meta 预填，`-IdempotenceMode auto` 才是真判据；README 踩坑 #24 的解释不成立**

- 事实：run-all 的两次生成相隔 **12 秒**（21:25:52 → 21:26:04），sha256 **仍然相同**（`452A9291C806FC82`）。
  README #24 说"两次生成若落在同一秒才会一致"—— 与实测不符。
- 真实机制：`-Step draft` 会**自动补跑 `meta`**（`run-extension-ui.ps1:717-723`），而 `OnSetEditInfo` 会**从已存在的档案预填 `context.Meta`**（`YuaExtractMainWindow.cpp:2940-2958`，含 `generatedAt`）；
  内核只在 `generatedAt` 为空时才盖时间戳（`ExtensionSet.cpp:2333: stampGeneratedAt && generatedAt.empty()`）→ 第二次生成**继承**了第一次的时间戳 → 字节相同。
- 我做了对照实验：**删掉档案后全新生成** → sha256 `FDE2EE93481794F1…`（`generatedAt=21:41:35`），**与基线不同**；紧接着再生成一次（档案已存在）→ 又变回 `FDE2EE93…`（继承）。证据：`verifier-e7-fresh1.json` / `verifier-e7-fresh2.json`。
- 也就是说：E-7 的标题「档案**字节**可复现（同名同盐同内容 → 同一摘要）」**比实际验证到的性质更强**；实际成立的是"**除易变 META 字段外逐 chunk 一致**"（auto 模式）。
- 附带：设计稿 §13.4 明说「**验证脚本要测 E-7 就保持默认，别开这个开关**（`stampGeneratedAt`）」，
  但脚手架走的是 UI 生成按钮（`OnSetGenerate` 硬编码 `stampGeneratedAt = true`，`YuaExtractMainWindow.cpp:2922`）—— **没有按设计稿的指示测 E-7**，而是用 auto 模式绕过去。
- 建议：E-7 的证据里显式记录"两次生成的 `generatedAt` 是否相同 + 是否走 auto 模式"，并把 README #24 改成真实机制。

**S-5（轻，易误判）干跑默认测试来源目录与传统模式步骤3 的输出目录是同一个**

- `EffectiveTestDirectory` 默认 = `EffectiveSourceDirectory` 默认 = `<工作区>\Restored_Extractor_Output`（`YuaExtractMainWindow.cpp:419-436`），
  而传统模式的步骤3 正是往这个目录写结果。
- 后果：**全量跑完后单独复跑一次干跑**，面板会显示 `0/0/0、成功率 0%（0 ÷ 0）`（目录已是 16 个真名文件）—— 看起来像"干跑坏了"，其实是目录被改过。
- 我实测到了这个现象（`verifier-dryrun-after-classic.json`），并在干净夹具上复跑得到正确的 5/12/12（`verifier-dryrun-old.json`）。
- 建议：干跑用例把测试来源目录显式覆盖到 `NewUnpacked`（或断言里记录"来源目录的内容摘要"），别依赖会变的默认目录。

**S-6（轻，鉴别力弱）E-8 的"体积同量级"在 20 条档案上说明不了 §3.3**

- 实测 734 字节里，**元数据占了大头**：20 字节头 + 6×12 字节 chunk 表 + `META` 204 字节 + `IDENTITY` 326 字节 ≈ **622 字节**，真正的名字层压缩后只占一小部分。
- 于是"36.7 字节/条"完全被固定开销主导，拿它去比 §3.3 的"≈22 字节/条"（60,133 条量级）**没有鉴别力**。
- E-8 真正有价值的部分是"容器结构自洽（magic/尺寸/CRC/偏移）"—— 那由脚手架的独立读取器 `lib/rules-int.py` 复核，是真独立检查。
- 建议：把"体积量级"断言与"结构自洽"断言分开写，或用一个**大档案**（几千条以上，可用 `-PlantFakeDup` 之类手段生成）来测每条约数。

### 6.3 我不认为是缺陷、但值得记录的两点

1. **"只有目录/只有文件"的档案被接受**：设计稿 §13.6 只要求"**一个名字都没有**时报错"，所以 5 目录 0 文件 / 0 目录 15 文件被接受是**符合设计**的；
   下游结果是**已还原 0、丢弃 15**，报告如实写 `已还原 0 / 未还原被丢弃 15`，不崩、不伪造。风险是用户可能不理解"为什么 0%"——属产品体验建议，不算缺陷。
2. **`-Scope all` 里扩展集组与既有组共用 `Run\`**：两组用不同夹具目录（`ExtensionFixture` vs `FakeWorkspace`）互不重建，实测没有互相污染；但**共享 `Run\App\Log\Universal.log`** 这件事确实带来了 S-1，值得在 README 里点明。

---

## 7. 断言自检的鉴别力评估（我自己判断，不采信"19/19"）

跑了 `self-test-extension.ps1`：**19/19 通过**（`Run\out\checks-selftest-extension.json`、`verifier-run-all-extension.log`）。
场景覆盖：E-1×4、E-2×2、E-3×1、**E-4×2**、E-5×1、E-6×1、E-7×4、E-8×1、E-9×1、E-10×1 + `correct` 基线。

**鉴别力够不够：部分够，关键处不够。**

- 够的地方：每个断言都有"把结果做坏 → 该断言必须炸"的场景，且要求"**只有对应的断言炸**"（例如 `e7-volatile-only-strict` 与 `e7-volatile-only-auto` 成对，证明 auto/strict 两种口径真的可区分）。
  `correct` 基线也在，能防"永远炸"。
- 不够的地方（**这是本次最重要的自检结论**）：
  1. **拒绝类断言只有"该拒不拒"的方向**，没有"**拒了但拒错理由**"的方向 → 测不出 S-1（E-4 假通过）。
  2. E-4 的判据本身依赖 `Get-EvidenceText` 大 blob + 宽松关键词，**"命中关键词"这一步几乎没有约束力**（S-1 就是靠页面套话命中的）。
  3. E-8 的场景只改"档案大小/条数"这类数字，测不出"体积断言其实没有鉴别力"（S-6）。
- 结论：**19/19 不能推出"E-1..E-10 的断言都可信"**；它只证明了"这 19 个特定坏味道会被抓到"。S-1 恰好落在盲区，而它是我这轮唯一发现的"绿但没测到"的情况。

---

## 8. 覆盖不到的地方（如实写）

1. **真实游戏数据 / 真实 XP3 解包**：全部合成；模式1 与传统模式的"步骤2 提取动态 XP3 资源"在脚手架里是**预置目录**（脚本自己声明为"替代"）。
   所以"免跑游戏"证到的是"**映射来源不同、输出逐条一致**"，**没有真的跑过一次 XP3 解包**。
2. **真实"别的游戏"**：夹具无法表达（S-1）。我用**自造档案**替换了"盐/摘要"两个字段来验证拒绝路径，
   但"另一部真游戏跑过步骤2 → `Universal.log` 换成它的盐 → 模式1 被拒"这条**完整端到端链路没跑过**。
3. **`allowUnverified` 之后的后果**：我只验证到"步骤1 写表成功 + 成功框写明未能校验归属"。
   "表里可能一条都不中"的**量化**（真跑步骤3 看还原率）没测。
4. **库的其他面**：多会社/多作品列表、`manifest.txt` upsert 的**跨会社**去重、库根不可写（如装在 `Program Files`）时 §9 要求的"明确报错"—— 都没测（只测了同 exe 摘要重复安装不产生重复行）。
5. **未驱动的按钮**：`打开当前扩展集目录`(4105) / `打开扩展集库目录`(4107)（会拉起 Explorer，不适合自动化）；
   图2 弹窗的 `DATETIMEPICK_CLASS`(4124) 日期控件（脚手架只填了会社/名称/贡献者/版本/留言，没动日期）。
6. **取消**：`IDC_YUA_TASK_CANCEL`(4088) 在生成/干跑/写表**中途**的行为（§13.11 说"取消在阶段间生效"）未测。
7. **规模**：真实量级（`MinedNames.lst` 5.19 万条、6 万哈希名文件）的生成/干跑**耗时与内存**未测；
   E-8 的体积只在 20 条档案上量过（S-6）。
8. **干跑"未命中明细（可导出）"**：设计稿 §5 提到"未命中明细（可导出）"，导出功能未测（只读到了面板上的计数）。
9. **E-5 的强版本**：只在合成夹具上验证"负例名 0 次 + 输出可追溯"；沿用既有安全不变式，未做对抗性构造。
10. **像素级界面**：没做截图布局越界检查（本轮不涉及新增控件布局断言）。
11. **X-6 的语义**：它只量"断言脚本自身"的耗时预算，**不是产品性能护栏**。

---

## 9. 裁决

### 9.1 产品侧：4 条问题，**无阻塞缺陷**

- **P-1（中）**：`不校验、强行使用` 之后**概览面板没有写明"未能校验归属"**，日志也只有同义措辞 —— 违反设计稿 §13.12。
- **P-2（轻）**：强行使用的成功框首行 `摘要不中、盐一致` 与实际（拿不到盐）矛盾。
- **P-3（轻）**：缺 IDENTITY 的档案被报成 `这份扩展集属于别的游戏，与当前游戏的盐不同`，并给出对损坏档案无效的两条出路。
- **P-4（极轻）**：截断档案把 zlib 状态码打成 `18446744073709551613`。

**一期闭环本身成立**：E-1（免跑游戏逐条一致）、E-2（生成期自校验大面积报例外）、E-3（干跑 == 实测）、E-5（不伪造名字）、E-6（源目录只读）、E-7（除易变字段外可复现 + upsert 不重复）、E-8（容器结构自洽）、E-9（归一化）、E-10（模式1 ≡ 传统模式）在我自己的复核下**全部成立**；
E-4 的**真实拒绝行为**（我自造的 5 类坏档案 + 1 类真盐不匹配）也成立。**不建议回退或阻止发布**，建议把 P-1 排进一期收尾。

### 9.2 脚手架侧：6 条问题，**1 条必须先修**

- **S-1（重）**：E-4 的「盐不匹配」用例**假通过**——档案 0 名字 + 与当前同盐，实际命中的是空名字防线；断言关键词靠页面套话命中。**E-4 对"盐不匹配"的覆盖当前为 0**，必须由 task-3 修（修法见 §6.2）。
- **S-2（中）**：断言自检对"被拒但拒错理由"无鉴别力，所以 19/19 掩盖了 S-1。
- **S-3（轻）**：E-3 的 ② 独立枚举按条目数（13），与 §5/内核（12）不一致；且从不读取面板的 ②。
- **S-4（轻）**：E-7 的"字节可复现"依赖 meta 预填（`generatedAt` 继承），全新生成摘要不同；README #24 的"同秒"解释不成立；且未按 §13.4 的指示关闭 `stampGeneratedAt`。
- **S-5（轻）**：干跑默认来源目录会被传统模式覆写，单独复跑会得到 0/0/0，易误判。
- **S-6（轻）**：E-8 的体积断言在 20 条档案上无鉴别力。

> 我一律**没有改**这些脚本（源码树摘要与脚本 mtime 可核对，见 §1）；以上只回报，由 task-3 决定修法。

### 9.3 口径分歧裁决（本次重点）

- **面板 `可解目录 5 / 文件名可解 12 / 整路径可还原 12`、成功率 `80%`（12 ÷ 15）正确**，与内核 `DryRunResult`、与我的独立枚举**逐项相等**。
- **`13` 是"命中映射的文件条目数"**（13 = 15 条目 − 2 个文件哈希查不到的条目；其中 `928E0FD3…` 因出现两次而多算 1）。
- **`12` 是"命中的 64 位文件哈希去重数"**（§5 与 `std::set hitFiles` 的口径）。
- **设计稿 §5 的规则自洽**；但 §5 里"每个可解文件名都恰好有可解目录 ⇒ ② = ③"这句**理由不适用于本夹具**（本夹具恰恰有一个"目录未知但文件名已知"的中间态），
  ② = ③ = 12 是靠"去重 + 每个命中哈希都有已知目录落点"成立的 —— **结论对、理由错**，建议修文档措辞（不改产品）。

---

## 附：本轮新增的证据文件（均在 `_verify\restore-harness\Run\out\`）

| 文件 | 内容 |
|---|---|
| `build-release-x86-verifier.log` | 我自己的全解决方案构建日志（exit 0 / 0 error / 0 warning） |
| `verifier-run-all-extension.log` | `-Scope extension` 全量控制台输出（30/30，exit 0） |
| `verifier-run-all-all.log` | `-Scope all` 全量控制台输出（83/83，exit 0） |
| `verifier-dryrun-old.json` | 我在**干净夹具**上复跑的干跑（面板 5/12/12/80%，扫描 19） |
| `verifier-dryrun-after-classic.json` | 全量跑完后单独复跑干跑（0/0/0，证明 S-5） |
| `verifier-step1-dirs-only.json` / `verifier-step1-files-only.json` | 自造"只有目录/只有文件"档案的步骤1 结果 |
| `verifier-mode1-dirs-only.json` / `verifier-mode1-files-only.json` | 上述档案跑完整模式1（已还原 0 / 丢弃 15） |
| `verifier-step1-salt-mismatch-real.json` | **真·盐不匹配**档案被正确拒绝的完整证据 |
| `verifier-caseA-no-salt.json` | 拿不到本作盐时的**两个**出口对话框 |
| `verifier-forced-use.json` | 「强行使用」走到底：成功框含"未能校验归属"、概览不含 |
| `verifier-othergame-with-good-archive.json` | 好档案用在 OtherGame 工作区**被接受**（S-1 的现场） |
| `verifier-ui-probe.json` | 三个模式的按钮文案 + 互斥勾选 |
| `verifier-ui-probe2.json` / `verifier-ui-probe3.json` | 故障注入下的**点击分支**行为分派（v1/v2 有模态框未排空的脚手架自身 bug，v3 是干净结果） |
| `verifier-e7-fresh1.json` / `verifier-e7-fresh2.json` | E-7 的对照实验：全新生成 ≠ 继承式再生成（S-4） |
| `verifier-edge-cases.json` | 三个边界档案的汇总 |
| `Run\ExtensionFixture\VerifierArchives\` | 我自造的 3 份边界档案 + `craft-report.json` + `good-backup.rules.int` |

> 说明：`Run\out\verifier-ui-probe*.json` 与 `verifier-*.json` 是**我的**证据文件（不是脚手架产物）；
> 我在 `%TEMP%\yua-verify\` 下放了 4 个一次性探针脚本，**没有落进仓库**。
> 收尾状态：无 `YuaExtract` 残留进程；仓库根无 `Run\` 残留；`Run\AppProbe\` 已删除；夹具档案与库档案均已还原为 `452A9291C806FC82`。

---
---

# 附录：修复轮复核（task-5）

- 验证者：`extension-verifier`（同一人；**不采信 task-1/2/3 的"已修"说法**，只认自己跑出来的证据）
- 复核时间：2026-10-13 22:29 ~ 22:42
- 冻结条件核对：产品源码最新 mtime **21:54:03**（< 构建 21:54:26）；脚手架脚本最新 mtime **22:27:05**；我的第一条证据 **22:29:00**
  → **整个复核过程里产品代码与脚手架脚本都没有被动过**（这是本轮结论可信的前提，可核对）。
- **修复轮裁决：P-1 / P-2 / P-3 / P-4 / S-1 / S-2 / S-3 / S-4 / S-5 / S-6 —— 十条全部 CLOSED。**
  新增 **1 条产品侧轻微缺陷 P-5** + **7 条残留/提示（R-1..R-7，均不阻塞）**。
  §1~§9 的原裁决对应旧指纹 `9CC65F90…22C78`，**保留不动**。

## A1 新指纹与构建（我自己跑的）

| 产物 | 时间戳 | 大小 | SHA256 |
|---|---|---|---|
| `Release\YuaExtract.exe` | 2026-10-13 **21:54:26** | **1,003,008** | **`6B664840DD6EF1BB116A5A0979A1779AB72C4B89F08E128F8568306A180A1AE9`** |
| `Release\CxdecExtractordll\CxdecStringDumper.dll` | 2026-10-12 02:38:37 | 342,528 | `F3F1E9371F07D881…` |

```
MSBuild YuaExtract.sln /p:Configuration=Release /p:Platform=x86 /m /v:minimal /nologo
→ 退出码 0；error 0 行、warning 0 行
→ 构建前/后：size 1,003,008 不变、mtime 21:54:26 不变、sha256 6B664840… 不变（与 lead 给的新指纹逐字节一致）
→ 日志：Run\out\build-release-x86-verifier-fixround.log
```

**与旧指纹 `9CC65F90…` 的差异原因（可核对）**：修复轮改了 3 个产品源文件 ——
`ExtensionSet.h`（21:48:10）、`ExtensionSet.cpp`（21:50:29）、`YuaExtractMainWindow.cpp`（21:54:03），全部早于构建 21:54:26。
改动是新增 `IdentityMatch::Unverified` / `LoadFailure` / `LoadResult::failure`、zlib 状态可读化、以及 UI 的四个出口 + 概览「归属校验」行
→ 代码段与字符串表变大 → **998,400 → 1,003,008 字节（+4,608 B）**。

| 项 | 我进场时 | 复核结束时 | 判定 |
|---|---|---|---|
| 源码树摘要（379 个源文件） | `91EBE68C123D08138F927BA91D18D6FD6FAFF6EEE6854E83BA5105736C473337` | 同左 | 我**没碰源码**（旧轮是 `45F09561…6140`，差异来自修复轮本身） |
| `Release\YuaExtract.exe` | `6B664840…` | `6B664840…` | 未变 |

## A2 命令与结果表（修复轮）

| 命令 | 结果 | 证据 |
|---|---|---|
| MSBuild 全解决方案 Release\|x86 | **exit 0；0 error / 0 warning；指纹不变** | `Run\out\build-release-x86-verifier-fixround.log` |
| `run-all.ps1 -Scope extension` | **exit 0；33/33**（E 组 11 + 扩展集断言自检 **22**） | `Run\out\verifier-fix-run-all-extension.log`、`checks-extension.json`（11 项 0 失败）、`checks-selftest-extension.json`（22 项 0 失败） |
| `run-all.ps1 -Scope all` | **exit 0；86/86**（A 21 + B 10 + C/E 14 + 自检 8 + E 11 + 扩展集自检 22） | `Run\out\verifier-fix-run-all-all.log` |
| `self-proof-s1-salt.ps1` | **exit 0；旧写法必炸 / 新写法必过** | `Run\out\verifier-selfproof-s1.log`、`s1-selfproof-oldstyle-checks.json`、`s1-selfproof-newstyle-checks.json` |

## A3 P-1..P-4 逐条复核（实测，不是读代码猜）

**P-1（概览/日志必须写明"未能校验归属"）—— CLOSED**

我自己重跑了「拿不到本作盐 → 强行使用」全流程（`verifier-fix-forced-use.json`）：

| 位置 | 修复前（task-4 实测） | 修复后（我本轮实测） |
|---|---|---|
| 概览「扩展集状态」 | 全文 **0 次**"未能校验归属" | `归属校验：未能校验归属（用户已确认强行使用）` + 下一行附内核 matchNote `来源：…（未能校验归属：exe 摘要不中，且拿不到当前游戏的盐，无法做任何盐比对；本条是用户强制放行，未做归属校验）` |
| 概览「当前任务 → 状态」 | `已用扩展集生成静态 Hash 表（…）`（StageNote 被覆盖） | `已生成静态 Hash 表，但未能校验归属（用户已确认强行使用）（2026-10-13 22:39:32）` |
| 工具日志 | 只有 `用户选择强行使用未校验的扩展集` | `[YuaExtract] 模式1 步骤1：未能校验归属（用户已确认强行使用）—— 来源：…`（Warn，含原词） |

程序化判定：`overviewHasPhrase('未能校验归属') = True`（task-4 为 `False`）；日志命中的文件列表非空。
另外确认五个确认框的正文都改成了「点「是」/点「否」」，与 `Host::Confirm` 的 `MB_YESNO` 一致（源码里已无 `点「确定」`/`点「取消」` 的确认框文案）。

**P-2（强行使用成功框不得再说"盐一致"）—— CLOSED**

成功框实测：

```
扩展集加载成功：YuaHarness / otomeki_ckr　未校验（用户强制放行）
来源：扩展集 YuaHarness/otomeki_ckr（未能校验归属：exe 摘要不中，且拿不到当前游戏的盐，无法做任何盐比对；本条是用户强制放行，未做归属校验）
目录条数 5 / 文件条数 15 / 例外 0
```

程序化判定：`successDialog 含 '盐一致' = False`、`含 '未校验（用户强制放行）' = True`。内核侧 `IdentityMatchText` 新增
`case IdentityMatch::Unverified: return L"未校验（用户强制放行）"`，`allowUnverified` 放行时 `match = Unverified`（不再是 `SaltMatch`）。

**P-3（缺 IDENTITY 必须报"结构缺失/损坏"并给对损坏档案有效的出路）—— CLOSED**

缺 IDENTITY 实测（`extension-bad-missing-identity.json`）：

```
标题：扩展集档案结构缺失或已损坏
正文：这份扩展集档案结构缺失或已损坏（读不出来 / 版本不符 / 截断 / CRC 错 / 缺 META 或 IDENTITY）：
      档案结构缺失（IDENTITY）：档案缺少 IDENTITY chunk（0x0002），没有盐与目标 exe 摘要，无法确认它属于哪部游戏，拒绝加载。
      出路：用当前游戏重新执行一次「生成当前游戏扩展集」，再「安装到扩展集库」覆盖掉库里这份档案；
            如果这份档案是从别处拿来的，换一份重新导出的。
      点「是」= 打开扩展集库目录去处理；点「否」= 关掉。
```

对照 task-4 的旧行为（`【扩展集与当前游戏盐不同】这份扩展集属于别的游戏…`）：**标题与出路都换了**，
**不含**"别的游戏"、**不含**"Universal.log"、出路是"重新生成/重装/换档案"。同类四份结构坏档案（CRC / 截断 / 版本过高 / 缺 IDENTITY）
现在都走同一条 `Structure` 出口，具体原因在正文第二行。✓

**P-4（zlib 状态号必须可读）—— CLOSED**

截断档案实测：`档案结构损坏：payload 解压失败（数据损坏(Z_DATA_ERROR)，status=-3）。`
—— `18446744073709551613` 消失，改为有符号 `-3` + 名称。源码 `ExtensionSet.cpp` 新增把 zlib status 映射成
`数据损坏(Z_DATA_ERROR)` 一类的可读名。✓

## A4 S-1 重点自证（含我对"证明力"的独立判断）

**我跑了 `self-proof-s1-salt.ps1`（exit 0），结果：**

| 半场 | 档案实测 | 断言结果 |
|---|---|---|
| ① 故意改回旧写法（`-ToolGeneration other` 也写主夹具盐 `xp3hnp`） | `salt='xp3hnp' DIRS=0 FILES=0`（正是 S-1 现场：**同盐 + 没有名字**） | **E-4 = False** ✔ 该炸就炸 |
| ② 改回新写法（按 `-Game` 取盐 `othersalt123`） | `salt='othersalt123' DIRS=1 FILES=1` | **E-4 = True** ✔ |

**证明力判断（lead 的点 C 成立，我独立确认）：**

旧写法那一轮，断言明细是：

```
前置=False（用例档案的盐 = 'xp3hnp'，与当前游戏**同盐** —— 这样测到的是别的防线（S-1 的假通过），不是盐不匹配）
专属措辞命中 [无法校验扩展集归属, 归属, 摘要与档案记录不符, 没有可用的当前游戏盐]
他类防线措辞 []    拒绝痕迹=True    StaticHash.lst 存在=False 行数=0
```

**四个关键词全部命中、没有命中任何"他类防线"措辞、也确实被拒了、也没有产出 lst** ——
如果判据只有"关键词 + 被拒 + 无 lst"（task-4 的旧写法），这条用例**仍然会绿**。
拦住它的是 `Test-CasePrecondition('salt-differs-and-has-names')`：它拿 summary 里记的档案路径，
用**脚手架自己的 `lib\rules-int.py` 独立解析**，要求 `salt ≠ gameSalt` **且** `DIRS+FILES > 0`；
旧写法档案两条都不满足 → `前置=False` → E-4 必炸。

所以：**"独立解析档案 + 前置复核"这条防线是真的有效，而且它是这两条用例唯一的承重结构**；
关键词收紧对它们几乎不贡献鉴别力（见 R-2）。自证还有两个优点：它同时要求"旧必炸 + 新必过"（不会因"永远炸"而假绿），
并会自检新写法档案的属性、还原现场（重装好档案 + 重跑坏档案编排器），可重复执行。

**S-1 另有独立旁证**：修复轮把 E-4 从 6 条扩到 **7 条**（新增 `salt-mismatch-known`），
两条盐用例的 `前置` 都实测 `True（盐 'othersalt123' ≠ 当前游戏 'xp3hnp'，且档案有 1 目录 / 1 文件）`，
而且两者走了**不同出口**：`salt-mismatch`（拿不到当前盐）→`【扩展集未能校验归属】`；
`salt-mismatch-known`（提供工具盐）→`【扩展集与当前游戏盐不同】`（含两条可操作出路）。✓

## A5 S-2..S-6 逐条复核

**S-2（自检要有"被拒但拒错理由"的鉴别力）—— CLOSED**

自检场景 19 → **22**，新增三条（我读了实现，确认不是凑数）：

| 场景 | 构造 | 期望 | 实测 |
|---|---|---|---|
| `e4-s1-false-pass` | 两条盐用例的档案 dump 换成 `DIRS0/FILES0/Exception2/salt=gameSalt` | E-4 炸 | PASS |
| `e4-wrong-reason-salt` | 前置仍真（`realSaltDump`），但把 `salt-mismatch` 的拒绝文本换成 `empty-names` 的话 | E-4 炸 | PASS |
| `e4-wrong-reason-crc` | 前置仍真（crcOk=false），但把 `crc-error` 的文本换成 `version-high` 的话 | E-4 炸 | PASS |

后两条正好补上我在 §7 指出的盲区（"拒了、但拒错理由"）。

**S-3（E-3 的 ② 要按 64 位哈希去重，并读面板）—— CLOSED**

实测断言输出：`独立枚举 [NewUnpacked]：① 可解目录 5（去重）/ ② 文件名可解 12（哈希去重，条目命中 13）/ ③ 整路径可还原 12 / 哈希名总数 15（80.00%）`；
并新增读面板：`干跑报的 ② 文件名可解数 = 12` → `[OK] 产品的「可解文件」= 12 与②「文件名可解（去重）」独立枚举一致`。
（`13` 仍如实作为"条目命中"记录，但已不当作 ② 的口径。）✓

**S-4（E-7 要如实记录是"继承"而非"生成时刻"）—— CLOSED**

实测：`META.generatedAt：第一次='2026-10-13 22:29:38' 第二次='2026-10-13 22:29:38' 相同=True（相同说明第二次是从已有档案**继承**的，不是「生成时刻」）`；
判据写明 `-IdempotenceMode auto（判据 = 除 META.generatedAt 外逐 chunk 一致）`；
`[OK] 两次生成字节相同（… 注意这是"继承"的结果，不作为稳定性判据）`。
自检里 `e7-volatile-only-auto`（两份 dump 的 `generatedAt` 不同、archive sha 也不同 → 走 chunk 比对）必过、
`e7-volatile-only-strict`（`-StrictBytes`）必炸 —— 成对场景真的区分了两种口径。✓

**S-5（干跑前重铺来源目录）—— CLOSED**

全量跑完后 `OldGame\Restored_Extractor_Output` 是 classic 留下的 **16** 个文件；
我单独复跑一次干跑（`-Step dryrun`）：`dryRunSourceReseeded = True`、重铺后 **21 个文件**、内容摘要 `AF161217A69D`，
面板给出 **可解目录 5 / 文件名可解 12 / 成功率 80% / 哈希名 15 / 扫描 19** ——
**不再是 task-4 那个误导性的 0/0/0**。证据：`verifier-fix-dryrun-standalone.json`。✓

**S-6（E-8 不该在 20 条档案上比 §3.3 量级）—— CLOSED**

E-8 改名为「档案结构自洽 + 条数 + 名字层每条平均字节（小夹具不比 §3.3 量级）」，实测输出把结构拆开：
`头 20 + chunk 表 6×12 = 92；META 204 + IDENTITY 475（元数据 679）；名字层 253；其他 162`，
并明确 `每条平均：名字层 12.7 字节/条 —— 这是本断言唯一看的量级指标；整档 36.7 字节/条**不作判据**`
+ `说明：本夹具只有 20 条，量级结论不成立；要量 §3.3 请用 make-extension-fixture.ps1 -ExtraFiles 3000 放大后再看`。
另加了"名字层每条 > 128 字节 → 失败"的失控护栏（但我发现它没有自检覆盖，见 R-3）。✓

## A6 关键数字的独立复枚举（我自己的脚本，不读报告）

夹具与映射表仍用 task-4 的独立枚举口径（只读 `NewUnpacked` + 原始采集成果）：

```
TOTAL=20  HASHNAMED=15   RAW-SOURCE mapping: DIRS=5 FILES=15
① resolvedDirectories(去重)   = 5
② resolvedFiles(64 位去重)     = 12     [文件条目命中 = 13]
③ fullyResolvedFiles          = 12
successRate                   = 80%（12 ÷ 15）
缺目录条目 2 / 缺文件名条目 2 ; 未命中目录去重 1 / 未命中文件去重 2
```

与修复轮面板**逐项一致**（§4 的 12-vs-13 裁决在修复轮后**不变**：② = 12，13 是条目数）。

**档案**：`-Scope all` 那次 734 字节 / `22E460A6B77BE519`；`-Scope extension` 那次 735 字节 / `71C833C36F7762B9`
—— 两次只差 1 字节，原因是 `META.generatedAt` 文本长度不同导致 deflate 输出差一字节，**符合 S-4 修正后的口径**（不要求跨次字节一致）。
**容器格式未变**：`magic 'YXS1' / version 1 / headerSize 20 / chunkCount 6`，chunk id 集合（META/IDENTITY/DIRS/FILES/EXCEPTIONS/STATS）与 task-4 相同。
（我顺手核了 `IDENTITY` 从 326 → 475 的疑问：不是异常内容，而是它按 §3 用**重复的 `collectionSource=` 行**列了 5 个采集来源，见 R-4。）

## A7 修复轮新暴露 / 残留的问题

### 产品侧（1 条，轻微）

**P-5（新，轻）加载成功、但写表失败时，概览把归属校验误报成"拒绝加载"**

- 现场（`extension-bad-empty-names.json`，空名字档案：IDENTITY/META 合法、exe 摘要命中 → **加载成功**，随后 `WriteStaticHashMap` 报"没有名字"）：

```
状态：生成静态 Hash 表失败。
静态 Hash 表：未生成
归属校验：拒绝加载                                  ← 错：归属校验其实通过了
来源：扩展集 YuaHarness/otomeki_ckr（exe 摘要命中）  ← 同一段里又证明加载成功（自相矛盾）
```

- 根因：`YuaExtractMainWindow.cpp:2173` 在 `JobKind::StaticHash` 的**失败**分支里无条件
  `context.LastLoadVerdict = LoadVerdict::Rejected;`，没有区分"**加载**失败"与"加载成功、**写表**失败"。
  （该分支里 `switch (job.Failure)` 走 `default` 兜底弹 `生成静态 Hash 表失败：…`，那条弹框是对的。）
- 影响：P-1 的初衷是"归属结论长期留在面板上、事后翻得出来"；这条路径上留痕是**错的**，会让人以为档案归属有问题。
- 建议：只在 `job.Load.ok == false` 时置 `Rejected`；加载成功而写表失败时，`归属校验` 保持 `Verified`（并让"静态 Hash 表：未生成"承担失败信息）。

### 脚手架侧 / 口径（7 条，均为残留或提示，不阻塞）

| # | 级别 | 问题 | 建议 |
|---|---|---|---|
| **R-1** | 低 | E-4 的前置复核读的是 summary 里记的**路径**，而 summary **不记每用例档案摘要**（我实测 `hasSha=False`）→ 校验的是"断言时刻该路径上的文件"，不是"那次运行真正用的文件"。当前 run-all 顺序下二者一致，且 S-1 自证正是靠这个机制成立 | 在 summary 里记 per-case `archiveSha256`，断言时先核对摘要再复核前置 |
| **R-2** | 低 | 两条盐用例的 `MustHit` 词表被**错误防线**的正文完全满足（自证 ① 实测：4 个关键词全中、他类措辞 0），所以关键词层对这两条几乎不贡献鉴别力，**前置复核是唯一承重**；`salt-mismatch` 的 `Forbidden` 也没列 `盐不同` | 把 `盐不同` 加进 `salt-mismatch` 的 `Forbidden`；并在脚本里注明"前置是承重的、别删" |
| **R-3** | 低 | E-8 新增的"名字层每条 > 128 字节"护栏**没有自检场景**；且 self-test 的合成 dump 里 DIRS/FILES chunk 只给 `count` 不给 `size` → `nameLayerSize` 恒为 0，护栏在自检里永远不会触发（`e8-volume` 只变 ArchiveSize/DumpDirCount，测的是条数） | 给 `New-ArchiveDump` 的 DIRS/FILES 补 `size`，并加一条"名字层超胖"的自检场景 |
| **R-4** | 提示 | `lib/rules-int.py` 的 `enrich()` 用 `dict()` 解析 IDENTITY → **重复键被折叠**；档案合法地写了 5 行 `collectionSource=`，解析结果只剩最后一行（`ExtraManual.lst（1 条）`） | 重复键收成数组（`collectionSource[]`），免得将来核对"采集来源清单"时漏 4 条 |
| **R-5** | 提示（产品） | Verified 分支把 `归属校验：` 与 matchNote 拼在一起，而 matchNote 本身以 `来源：` 开头 → 面板读作 `归属校验：来源：扩展集 …（exe 摘要命中）` | 该行只放结论（`已通过`），matchNote 另起一行 |
| **R-6** | 低（产品/口径） | `LastLoadVerdict` 只由模式1 步骤1（**库**档案）设置，干跑（**草案**档案）不设 → 只跑干跑时该行一直是"（还没有加载过扩展集）"；步骤1 与干跑目标不同文件时，该行描述的是库档案而非刚测的草案 | 让干跑也更新该行（或把标签写成"库档案归属校验"）。**我没构造"先步骤1 再干跑另一份档案"的混合场景** |
| **R-7** | 提示 | E-6 对 `生成来源目录` 允许"仅内容一致（mtime 变化不算）"，因为 S-5 的修法让**脚手架自己**在干跑前重铺了该目录；`解包目录` 仍走严格摘要（含 mtime）。改动范围窄、断言标题已写明，属可接受的口径调整 | 保留；若要恢复 mtime 保护，可在重铺时记录"这是脚手架动作"的时间窗 |

## A8 覆盖不到的地方（修复轮）

§8 的全部条目仍然适用（真实游戏 / 真实 XP3 解包、真实"别的游戏"端到端、库的其他面、取消、真实规模、未命中导出、像素布局）。
修复轮**新增**的盲区：

1. `allowUnverified` 之后"展开出的表一条都不中"的**量化**仍未测 —— 我只验证到写表成功 + 面板/日志留痕。
2. **R-6 的混合场景**（步骤1 用库档案 + 干跑用草案档案）没有构造，`归属校验` 行的语义边界只做了静态推导。
3. E-8 的 `-ExtraFiles 3000` 放大档**没跑** —— §3.3 的 0.6 MB / 22 字节每条这一档仍然只有 20 条档案上的间接证据。
4. `e4-wrong-reason-*` 三个场景我读了实现并看它们在 22/22 里"该炸就炸"，但没有手工再造变体喂一遍（自检脚本本身在跑）。
5. **P-5 只在 empty-names 这一条路径上复现**；其他"加载成功但后续阶段失败"的路径（如 STATS 异常、写盘失败）没有穷举。
6. 我没有对**新增的 `LoadFailure` 分类本身**做单元级穷举（只通过 7 条 E-4 用例 + 我自造的真盐不匹配档案间接覆盖）。

## A9 修复轮裁决

| 编号 | 结论 | 我的关键证据 |
|---|---|---|
| **P-1** | ✅ **CLOSED** | 概览 `归属校验：未能校验归属（用户已确认强行使用）` + 状态行 + Warn 日志都含原词（task-4 为 0 次） |
| **P-2** | ✅ **CLOSED** | 成功框 `未校验（用户强制放行）`，`含"盐一致" = False` |
| **P-3** | ✅ **CLOSED** | `【扩展集档案结构缺失或已损坏】` + 重新生成/重装的出路，不含"别的游戏"/"Universal.log" |
| **P-4** | ✅ **CLOSED** | `数据损坏(Z_DATA_ERROR)，status=-3` |
| **S-1** | ✅ **CLOSED** | E-4 7 条用例、两条盐用例 `前置=True`；自证：旧写法 E-4=False（且**关键词全中、只有前置拦住**）、新写法 E-4=True |
| **S-2** | ✅ **CLOSED** | 自检 19→22，新增 `e4-s1-false-pass` / `e4-wrong-reason-salt` / `e4-wrong-reason-crc` |
| **S-3** | ✅ **CLOSED** | E-3 的 ② 按 64 位哈希去重 = 12（条目 13 另记），并读面板比对 → `[OK]` |
| **S-4** | ✅ **CLOSED** | E-7 记录 `generatedAt 相同=True（继承）`，判据改为"除易变字段外逐 chunk"；auto/strict 成对自检 |
| **S-5** | ✅ **CLOSED** | 干跑前重铺（21 文件 / `AF161217A69D`），全量跑完单独复跑仍得 5/12/80% |
| **S-6** | ✅ **CLOSED** | E-8 改名并只把"名字层 12.7 字节/条"当量级指标，明确"20 条不支持量级结论" |

**新增**：产品侧 **P-5（轻）**；脚手架/口径 **R-1..R-7**（低/提示，不阻塞）。

**总评**：修复轮**实质有效** —— 我原来指出的 10 条全部真的关闭（不是"改了口径让它变绿"：S-1 是**先让旧写法炸**再让新写法过，
S-4 是**把过强的判据改弱并如实标注**，S-6 是**把没有鉴别力的结论撤掉**，方向都是"让断言更诚实"）。
一期闭环与 E-1..E-10 的实质结论在修复轮后**仍然成立**（33/33 + 86/86 都是我自己跑的）。
建议：**P-5 排进下一轮**（它恰好落在 P-1 想修好的那条"面板留痕"上）；R-2/R-3 是"防御强度"建议，R-2 尤其值得处理 ——
它说明 S-1 的防线目前是**单点**（前置复核），关键词层对这两条用例不设防。

### 修复轮新增证据文件（均在 `_verify\restore-harness\Run\out\`）

| 文件 | 内容 |
|---|---|
| `build-release-x86-verifier-fixround.log` | 修复轮全解决方案构建（exit 0 / 0 error / 0 warning / 指纹不变） |
| `verifier-fix-run-all-extension.log` | `-Scope extension` 全量（33/33，exit 0） |
| `verifier-fix-run-all-all.log` | `-Scope all` 全量（86/86，exit 0） |
| `verifier-selfproof-s1.log` | 我自己跑的 S-1 自证（旧必炸 / 新必过） |
| `s1-selfproof-oldstyle-checks.json` / `s1-selfproof-newstyle-checks.json` | 两个半场的 E-4 明细（含"关键词全中但前置=False"的铁证） |
| `verifier-fix-forced-use.json` | P-1 / P-2 实测（概览 + 状态行 + 日志 + 成功框全文） |
| `verifier-fix-dryrun-standalone.json` | S-5 实测（重铺 21 文件，面板 5/12/80%） |
| `extension-bad-salt-mismatch.json` / `extension-bad-salt-mismatch-known.json` | 两条盐用例各自的出口（Unverified / SaltMismatch） |
| `extension-bad-empty-names.json` | **P-5 的现场**（`归属校验：拒绝加载` 与 `来源：…exe 摘要命中` 并存） |
| `Run\ExtensionFixture\VerifierArchives\` | 我自造的边界档案 + `good-backup.rules.int`（复跑探针用） |

> 收尾（修复轮）：无 `YuaExtract` 残留进程；仓库根无 `Run\` 残留；`Run\AppProbe\` 未生成；
> 库档案已还原为 `22E460A6B77BE519`（与 `good-backup.rules.int` 一致）；产品源码树摘要与脚手架脚本 mtime 均未被我改动。

---

# 附录：幽灵修复轮复核（task-8）

> 复核者：**verifier-delta**（全新上下文，接替一个失败退出的成员）。复核对象：产品指纹 **`492522BD…` / 1,006,592 B**。
> 本轮**只在本文件末尾新增本节**；上面 §1–§9（task-4 裁决）与「附录：修复轮复核（task-5）」**原文未动**。
> 所有 UI 用例都在 lead 明确授权的时间窗内独占运行（同一时刻只有我一个人驱动界面），每一步跑完都收尾了 `YuaExtract` 进程。
> 下面每个数字都是**我自己跑出来的**：脚手架我只读、不改（探针脚本写在 `%TEMP%`）。

## B0 先把口径钉死：可见性看不见幽灵

用户实测的缺陷是**像素**残留：切到「制作扩展集」后首页控件的旧像素留在画面上（叠字）。
缺陷存在时首页控件的 `IsWindowVisible` **全是 False** —— 也就是说，任何只看窗口状态的断言在缺陷面前**照样全绿**。

这件事我**自己复现了一遍**（不是引用别人的结论）：

| 口径 | 修复前构建（`6B664840`）实测 | 结论 |
|---|---|---|
| `E-11-e` 可见性对照（4101 可见 / 4050 不可见；首页专属控件仍可见 0 个） | **PASS（全绿）** | 缺陷存在时可见性口径**没有鉴别力** |
| `E-11-a` 制作页签四路抓图两两比对 | **FAIL：最小 11,248 px / 1.230%** | 只有逐像素判据能看见幽灵 |

所以本轮的裁决一律以**抓图逐像素**为准；`E-11-e` 只当不变量记录。

## B1 产物来源可信：我自己重跑的构建

| 步骤 | 结果 |
|---|---|
| ① 任务给的命令：`& '…\MSBuild.exe' 'YuaExtract.sln' /p:Configuration=Release /p:Platform=x86 /m /v:minimal /nologo`（先杀进程） | **exit 0**；日志 31 行里 `warning`/`error` 匹配 **0 条**；`Release\YuaExtract.exe` **未变** = 1,006,592 B / mtime 2026-10-14 12:12:03 / sha256 `492522BD…E7A8`（增量判定为最新，说明**源码不比产物新**） |
| ② 为拿到"真编译"证据：把本轮唯一改动的工程 `CxdecExtractorLoader.vcxproj` **`/t:Rebuild`** 到 `%TEMP%` 的独立 `OutDir` | **exit 0**；**零 C++ 编译告警**（唯一一条是我自己把 `OutDir` 指到 `%TEMP%` 触发的 MSBuild `MSB8029`，与代码无关）；产物同尺寸 **1,006,592 B**，sha256 `23ABA4A0…C446` |
| ③ ②的产物与冻结产物**逐字节比对** | 长度相同，**只差 3 处 / 共 6 字节**：`0x110`（PE `TimeDateStamp`）与它在调试目录里的两份副本 `0xBF234` / `0xBF250`。解码：冻结 = **2026-10-14 12:12:03**（正好等于 exe 的 mtime），我的重编 = 13:00:56（= 我的编译时刻） |

**结论：冻结的 `492522BD…` 确实是当前源码树的产物**，两次构建只在"内嵌构建时间戳"上不同（同一份源码重编是**逐字节可复现**的，除了这 6 个字节）。
本轮产品侧改动范围我也核对过：`CxdecExtractorLoader\YuaExtractMainWindow.cpp`（`EraseBehindTabControls` / `RedrawAllControls` / `WM_SIZE` 分支 / 构建指纹）。仓库**不是 git 仓库**（`git status` 报 not a repository），所以没法用 diff 佐证改动面，这点如实记下。

> **过程说明（我自己的操作，不是产品/脚手架缺陷）**：`/t:Rebuild` 的 `Clean` 按**工程记录的真实输出路径**把 `Release\YuaExtract.exe` 删掉了。
> 我在动手前已把该文件备份到 `%TEMP%`，随后**原地恢复**：长度、mtime（12:12:03）、sha256 **三项都还原到与冻结值一致**（lead 也独立复核过）。
> 教训：要把工程重编到别处，要么先复制解决方案，要么确保备份在手并事后逐字节核对 —— 不能让 `Release\` 处于"被删且无备份"的状态。

## B2 修复后：E-11 判据（我**自己抓**的 12 张图 + 我自己跑的两条脚本）

`run-tab-pixels.ps1`（exit 0）：12 张抓图，客户区 **1150x795**，指纹 `492522BD23280617`，**全部是 `bitblt` 抓的**（`PrintWindow` 会无限期阻塞，只作兜底，本轮一次都没用上）。
`assert-tab-pixels.ps1`（exit 0）：**10 项全过**。关键实测值：

| 判据 | 我实测 | 阈值 |
|---|---|---|
| `E-11-a` 制作页签 **四路两两**（首页→制作 / 扩展集→制作 / 日志展开收起→制作 vs 基线） | **6/6 组合全部 0 px / 0.000%** | 0.10% |
| `E-11-C` 同状态连拍（噪声地板） | **0 px / 0.0000%** | 0.010% |
| `E-11-b` 首页历史无关性 | 启动 vs 回首页 = **1,698 px / 0.186%**（两对）；visit1 vs visit2 = **0 px** | 1.00% |
| `E-11-d` 跨进程（另一进程走同一条路） | **0 px / 0.000%** | 0.10% |
| `E-11-F` 正对照（首页图 ≠ 制作页图） | **26.83%** | ≥ 5.0% |
| `E-11-E` 快照完整性 | 12 张图都在、尺寸一致、驱动没失败 | — |
| `E-11-e` 可见性对照 | 全绿（**如前所述，这不是幽灵判据**） | — |

首页那 1,698 px 我自己也拆过：**1,510 px 是 ±1 通道 LSB 的 RichEdit 重绘噪声 + 188 px 是勾焦点框**（见 B4 的分类方法），与 README 里的标定一致。

## B3 反例：同一套判据在**修复前构建**上必须不为 0（我**自己抓的现场**）

我没有只信归档的 PNG，而是**用同一条驱动**对旧构建重新抓了一遍：把 `Run\App` 快照整体复制到 `%TEMP%\yua-verifier-delta\prefix-app\`，再把旧 exe（**1,003,008 B / `6B664840DD6EF1BB…`**，取自 `%TEMP%\yua-smoke-tool\`）盖进去，然后
`run-tab-pixels.ps1 -AppExe <旧构建>` → 12 张图（driver exit 0）→ `assert-tab-pixels.ps1`（**exit 1**）。

| 判据 | 旧构建实测 | 结论 |
|---|---|---|
| `E-11-a` | 6/6 组合**全部超阈值**：11,248 px/1.230%、**52,434 px/5.735%**、25,345/2.772%、41,856/4.578%、27,685/3.028%、50,014/5.470% | **必须炸 → 炸了** |
| `E-11-b` | 82,862 px/**9.063%**、98,681 px/**10.794%**、18,153 px/1.986% | **必须炸 → 炸了** |
| `E-11-d` | 16,666 px/1.823% | **必须炸 → 炸了** |
| `E-11-e` 可见性 | **PASS** | **缺陷现场可见性全绿** ← 判据盲区的又一次实测 |
| `E-11-C` 噪声地板 | 0 px | 测量本身可判读 |
| `E-11-F` 正对照 | 26.46% | 抓图有效（页签真的切过去了） |

断言还把 144 条差异热区连同**疑似旧位置控件**打了出来，例如
`make-baseline vs make-after-set ← (8,216) 552x96，16,708 px ← 旧位置:set-visit1 4071«扩展集…» / 旧位置:home-startup 4080«扩展集选择»`
—— 这正是"旧页签控件像素印在分组框里"的形状。

> 与归档值的关系：归档的修复前抓图是 **1164x801** 客户区（最小信号 12,762 px / 1.369%、首页 11.569%），
> 我这批是驱动强制的 **1150x795**，所以**百分比不能逐位对比**，但量级与结构完全一致（都属于"远超阈值"）。

**结论：这套判据不是"永远绿"的假绿判据 —— 它在真缺陷面前会响，而且响得很响（最差 5.7% / 10.8%）。**

## B4 E-11 **覆盖不到**的路径：WM_SIZE（我自己写探针抓的）

脚手架的 E-11 只在**固定客户区 1150x795** 下抓图，因此**完全不覆盖**"改窗口大小"这条路径。
本轮修复的第 2 条恰恰是 WM_SIZE（日志按钮旧像素印进「辅助操作」分组框）。所以我自己写了探针
（`%TEMP%\yua-verifier-delta\probe-wm-size.ps1`，只读脚手架 lib，产物全在 `%TEMP%`）：

场景：新启动 → 直接进制作（`make-a`）→ 连拍（噪声地板）→ **缩到最小再拉回 1150x795**（`make-b`）→ 回首页（`home-b`）→ **放大到 1500x1000 再还原**（`home-c`）→ 再进制作（`make-c`）。
（`WM_GETMINMAXINFO` 把最小窗口限制在 960x800，所以"缩到 900x560"实际被钳制为**客户区 944x761** —— 探针把钳制当合法 WM_SIZE 记账。）

| 对比 | 修复前 `6B664840` | 修复后 `492522BD` | 判读 |
|---|---|---|---|
| make 页：**小窗→拉大**（`make-a` vs `make-b`） | 18,132 px / **1.983%** | 2,437 px / **0.267%** | 降 7.4 倍，但**不为 0** |
| make 页：离开再回来（`make-a` vs `make-c`） | 16,143 px / 1.766% | **0 px** | 全清 |
| home 页：去制作后回首页（`home-a` vs `home-b`） | 89,436 px / 9.782% | **1,698 px / 0.186%** | 全清（只剩合法重绘） |
| home 页：放大还原（`home-a` vs `home-c`） | 88,245 px / 9.652% | **188 px / 0.021%** | 全清（只剩焦点框） |
| 同状态连拍 | 0 px | 0 px | 测量可判读 |

**那 2,437 px 到底是什么？我没有靠"看起来差不多"下结论，而是做了量级分类**（`classify-diff.py`，逐像素统计三通道最大差值）：

| 对比 | 差异像素 | 其中 `|Δ|=1`（纯色阶抖动） | 其中 `|Δ|>1`（内容级差异） | 最大通道差 |
|---|---|---|---|---|
| **修复后** make-a vs make-b | 2,437 | **2,437（100.0%）** | **0** | **1** |
| **修复后** make-a vs make-c | 0 | — | 0 | 0 |
| **修复后** home-a vs home-b | 1,698 | 1,510（88.9%） | 188（焦点框） | 240 |
| **修复前** make-a vs make-b | 18,132 | 2,247（12.4%） | **15,885** | 240 |
| **修复前** make-a vs make-c | 16,143 | 6（0.0%） | **16,137** | 255 |

补充证据：
* 2,437 个差异像素**全部落在两个 12~13 行高的文字带**里（y=82–93、y=456–468），热区坐标 `(912,80) 232x16`、`(792,80) 112x16`、`(648,456) 488x16` —— 用快照里的控件矩形表一查，**三处全部落在同一个控件内部**：`id=4111 RICHEDIT50W«功能介绍…» rect=575,48 575x707`（制作页自己的富文本框），**没有一处落在 `BS_GROUPBOX` 内部**（幽灵的典型形状是"别的控件在分组框里留下的旧内容"）。
* 把两块带子 3 倍放大人工比对：**上下两张图是同一段文字、同一位置**（`名字 ↔ 哈希」成果整理成一份扩展集档案（rules.int）…` / `E:\…\Run\App`）。
* 位移搜索：`make-b` 相对 `make-a` 的最佳位移就是 **(dx=0, dy=0)** —— 不是文字整体挪位，是同一批字形边缘的**ClearType 亚像素重算**（差异颜色对清一色是 `(255,255,234)→(255,255,233)`、`(90,160,225)→(89,159,224)` 这种单通道 ±1）。

**我的判读：这 2,437 px / 0.267% 是富文本框重新光栅化的合法噪声，不是幽灵**——任何像素的任一通道都没差过 1/255，文字内容与位置都一样，人眼不可见；
而真正的幽灵（旧构建同一条路径）是 **15,885 px 的内容级差异**（最大通道差 240）。两者的性质完全不同。

## B5 全量 `-Scope all`：**总计：通过 104 / 失败 0 / 提示 0**

命令：`pwsh -File '_verify\restore-harness\run-all.ps1' -Scope all` → **exit 0**。

| 断言组 | 失败 / 提示 / 共 |
|---|---|
| 断言 A（步骤3 restore，default） | 0 / 0 / **21** |
| 断言 B（步骤3 restore，keep） | 0 / 0 / **10** |
| 断言 C + E（步骤2 只收集 / 环境变量端到端） | 0 / 0 / **14** |
| 脚手架自检（断言脚本鉴别力） | 0 / 0 / **8** |
| 扩展集一期断言 **E-1..E-11** | 0 / 0 / **20** |
| 扩展集断言自检（鉴别力，含 9 个 E-11 场景） | 0 / 0 / **31** |
| **总计** | **通过 104 / 失败 0 / 提示 0** |

假绿护栏的状态我也照单读了：**本轮新写的断言组 6 个、被判为"上一次残留"而跳过的 0 个、文件不存在 3 个**
（`checks-supplement.json` / `checks-selftest-supplement.json` / `checks-pressure.json` —— `-Scope all` 本来就不跑 supplement 与 pressure，属于预期），
实际执行项数 **104 ≥ 下限 80**，因此 `结论：全部通过` 这次是**有内容的绿**，不是"0 项通过也叫通过"。
21+10+14+8+20+31 = 104，与打印的总计**对得上**。lead 亲跑同样是 104/0/0，我这次独立复跑**一致**。

E-11 在**全量路径里**的表现与我单独跑的一致（`checks-extension.json` 里 E-11 的 **9 项** `E-11-a/b/C/d/E/F/T/e/g` 全 PASS；`E-11-P`（性能护栏）只在独立入口 `assert-tab-pixels.ps1` 里，那次也是 PASS）：
制作页四路 **0 px**、噪声地板 **0 px**、首页 **1,698 px / 0.186%**、跨进程 **0 px**、正对照 **26.83%**。

E-11 的 9 个**自检场景**（`e11-ghost` / `e11-home-ghost` / `e11-noise-unstable` / `e11-blank-capture` / `e11-frozen-tab` / `e11-cross-process` / `e11-threshold-tampered` / `e11-missing-snapshot` / `e11-prefix-real-capture`）全部 `PASS 断言响应符合预期`
—— 其中 `e11-prefix-real-capture` 用的就是归档的修复前现场图，**要求 `E-11-a`+`E-11-b` 必须炸**，与我 B3 的实抓反例互相印证。

## B6 无回归：既有断言与"标题栏改了会不会打断 UI 驱动"

* **既有 A/B/C/D/P 组**：A 21 项、B 10 项、C+E 14 项、自检 8 项**全绿**（B5 表）。
* **E-1..E-10 + X-6**（共 11 项）：全部 **PASS**（`checks-extension.json`；同一组里另有 E-11 的 9 项，见上）。
  > 口径说明：这一组我验的是"**跑通了、全绿、指纹是本轮的**"，**没有**像 task-4 那样对每条 E 的数字做独立复枚举 —— 见 B10。
* **UI 驱动没被标题栏变化打断**：驱动**按窗口类名**找主窗口（`run-step3-restore.ps1:349`、`run-extension-ui.ps1:720` 都是 `Class -eq 'YuaExtractMainWindow'`），
  标题只用于"目标游戏有没有设上"的判定（`-like '*otomeki_ckr.exe*'`，`run-extension-ui.ps1:738`）。
  本轮 run 的实测日志：`主窗口就绪：YuaExtract 1.0.0 [492522BD] - otomeki_ckr.exe`、`目标编辑框回读：'…\Mode1Game\otomeki_ckr.exe'；标题：'… [492522BD] - otomeki_ckr.exe'`
  —— 指纹后缀 `[492522BD]` 落在目标名**之前**，`-like` 匹配照旧成立（E 组 20 项全绿就是它的行为证据）。

## B7 「关于」页构建指纹 + 键盘焦点（我自己实读 / 实测）

**关于页（我自己点开截的图 + 读的文本）**：截图 1150x795（`%TEMP%\yua-verifier-delta\about-shot\about-page.png`），控件 4071 的正文（`SmartText` 读到 965 字）里确有：

```
构建指纹
程序文件： YuaExtract.exe
文件大小： 1006592 字节
修改时间： 2026-10-14 12:12:03
SHA-256： 492522BD23280617（前 16 位，标题栏取前 8 位）
```

四项**逐项等于冻结值**（大小 1,006,592 B、mtime 2026-10-14 12:12:03、sha256 前 16 位 `492522BD23280617`），标题栏 `YuaExtract 1.0.0 [492522BD]` 也当场读到。
代码侧对得上：`SelfFingerprint()` 用 `static` **每进程只读一次**自身 exe（`GetModuleFileNameW` + `GetFileAttributesExW` + `SHA-256`），页面只报"读到的这个 exe"，不做推断。

**键盘焦点（修复的附带要求）**：我用 `BM_GETSTATE`(`0x00F2`) 直接量了页签按钮的 `BST_FOCUS`(`0x8`)：

| 步骤 | BM_GETSTATE |
|---|---|
| 点「制作扩展集」之后 | **0x0008**（拿到焦点） |
| **WM_SIZE（缩到 944x761 再拉回 1150x795）之后** | **0x0008**（焦点**还在** —— "全藏→擦除→布局→显示"这一趟没有把焦点丢掉） |
| 切到首页之后 | 首页 **0x0008**、制作页签 0x0000（焦点跟着走） |
| 再一次 WM_SIZE 之后 | 首页仍 **0x0008** |

代码侧一致：`LayoutWindow` 开头 `HWND focused = ::GetFocus()`，末尾 `if (focused && ::IsWindow(focused) && ::IsWindowVisible(focused)) ::SetFocus(focused);`（`YuaExtractMainWindow.cpp:1639 / 1740`）。

> 探针踩的坑（留给后人）：脚手架 `lib\HarnessNative.ps1` 的 `CheckState()` 是 **`BM_GETCHECK`(0x00F0)**，不是 `BM_GETSTATE`；用它量焦点**永远是 0**。我第一版探针就是这么错的，改成 `SendTimed(hwnd, 0x00F2, …)` 才量到真值。
> 另外：这个构建的 4071（`RICHEDIT50W`）用 `RemoteText` 读回 **`null`**，用 `SmartText` 才读到 965 字 —— 写探针时注意换读法。

## B8 我发现的问题（脚手架 / 口径侧，均不阻塞）

| 编号 | 严重度 | 问题 | 证据 / 建议 |
|---|---|---|---|
| **R-8** | **中（口径缺口，建议补文档或补判据）** | **E-11 不覆盖 WM_SIZE**，而它的制作页阈值 **0.10%（≈932 px）比这条路径上的合法噪声（2,437 px / 0.267%）还紧**。谁要是把"小窗→拉大"当第 5 路塞进状态矩阵，**修复后的构建也会炸**，而且炸的是 ClearType 亚像素噪声 | 要么在 README/断言里写明"E-11 是**固定几何**判据，WM_SIZE 不在口径内"，要么给这条路径单独的口径（独立阈值，或把"仅 ±1 通道 LSB 的差异"排除）。我这次没改脚手架（按约束只回报） |
| **R-9** | 低（易踩坑） | `lib\HarnessNative.ps1` 没有 `BM_GETSTATE` 读取器，`CheckState()` 名字容易被当成"窗口状态"实际是 `BM_GETCHECK` | 我在 B7 里记了正确用法（`SendTimed` + `0x00F2`）。建议加个 `ButtonState()` 或至少在注释里点明 |
| **R-10** | 低 | 本轮构建的 4071 `RICHEDIT50W`：`RemoteText` 返回 `null`，`SmartText` 正常 | 影响的是探针写法，不是断言正确性（E-11 不读文本） |
| **R-11** | 提示 | `assert-tab-pixels.ps1` 只校验"抓图文件在不在 + 尺寸对不对 + 驱动有没有失败"，**不重算**快照里记着的 `sha256` | 若有人把同名同尺寸的 BMP 换掉，断言不会发现。真要做防篡改，重算一次 `sha256` 与快照比对即可（当前不是阻塞项） |

## B9 产品侧：我的判断

| 项 | 结论 | 我的关键证据 |
|---|---|---|
| 用户报的"切页签后旧像素叠成两层" | ✅ **已修** | 制作页签四路抓图（首页→制作 / 扩展集→制作 / 日志展开收起→制作 vs 基线）**6/6 组合 0 px**；首页 11.57% → **0.186%**（且已拆清是 RichEdit ±1 LSB + 焦点框）；跨进程 0 px |
| 同一套判据的**反例** | ✅ **有效** | 旧构建（我自己重抓）最差 **5.735% / 10.794%**，`E-11-a/b/d` 三项炸，而 `E-11-e` 可见性**全绿** |
| WM_SIZE 路径残留 | ✅ **已修（残留为纯亚像素噪声）** | 1.983% → 0.267%，且残留 **100% 是 ±1 通道 LSB**（最大通道差 **1**）、文字内容与位置不变；同类路径 1.766% → **0 px**、9.78% → **0.186%**、9.65% → **0.021%** |
| 键盘焦点 | ✅ 保住 | 重排/擦除后页签按钮 `BST_FOCUS` 仍为 `0x0008` |
| 「关于」页构建指纹 | ✅ 正确且自洽 | 实读四项 == 冻结值（大小/mtime/sha256 前 16 位），标题栏前 8 位 |
| **P-6（新，轻）** | ⚠️ **代码卫生 / 地雷** | `YuaExtractMainWindow.cpp:1655`：`MoveControl(…, context.ActiveTab == tabIds[index] || true);` —— 这个表达式**恒为真**（`|| true` 把前半句变成了死代码）。**当前行为是对的**（四个页签按钮本来就该一直可见），所以**不是功能缺陷**；但它是编辑器残留：谁哪天顺手把 `|| true` 删掉"修干净"，四个页签就会变成"只有当前页签可见"，**界面直接坏掉**。建议改成直接传 `true` 并写一行注释说明"页签按钮恒可见" |

**没有发现新的产品侧功能缺陷。**本轮修复在**用户可感知的层面**（叠字幽灵）确实闭合，且我找不到"改了口径让它变绿"的痕迹 ——
相反：`E-11-a` 在修复后是 **0 px**（不是"压着阈值过"），空白/单色图、页签没切过去、阈值被调大、快照缺失这些假通过路径都有自检场景守着且全过。

## B10 覆盖不到的地方（如实写）

1. **E-1..E-10 我验的是"全绿 + 指纹是本轮的"，没有独立复枚举其数字**（task-4 那一轮做过逐条复核）。本轮我复算的是 E-11 与 WM_SIZE 这两条**新/未覆盖面**。
2. **焦点只验了页签按钮**（`BS_OWNERDRAW` 按钮的 `BST_FOCUS`）。**没有**验"在目标游戏输入框里改到一半、动窗口大小后光标还在不在"（那需要 `GetGUIThreadInfo` 读 `hwndFocus`/`rcCaret`，脚手架 lib 没有这个 API，我没有为它再写 Add-Type 探针）。
3. **WM_SIZE 只用 `SetWindowPos` 改客户区**（944x761 ↔ 1150x795 ↔ 1500x1000）。**没测**：鼠标拖边框的真实 `WM_SIZING` 过程、最小化/还原、最大化、多显示器/DPI 变化（E-11 本身要求几何完全一致，跨 DPI 根本跑不了）。
4. **本机 DPI/显示配置只有一种**，所以"高 DPI 下会不会又出现残留"没有数据。
5. **`R-8`（E-11 口径缺口）我只做了标定与回报，没有改脚手架**（按约束不动 `_verify\**` 里除本报告以外的文件）。
6. 我**没有**去改/验 `Run\out` 里归档的历史证据；B3 的反例是我**新抓**的一批（`Run\out\verifier-tab-pixels-prefix*`），与归档的 1164x801 那批**几何不同**，两者百分比不可逐位对齐。
7. "构建可复现"的结论只覆盖 **Release|x86 + 本机 v143 工具集**；换工具集/换机器没有数据。
8. 我**没有**验证"每进程只读一次指纹"在"不重启换 exe"场景下的行为（代码注释已说明这是有意取舍，自更新时要去掉 `static`）—— 当前无自更新功能，属于设计说明而非缺陷。

## B11 幽灵修复轮裁决

| 结论 | 判定 |
|---|---|
| **产品侧** | ✅ **通过 —— 用户报的幽灵缺陷已修，且判据可证伪（旧构建必炸、修复后 0 px）；无新的阻塞缺陷。** 仅 **P-6（轻，代码卫生）**：`|| true` 死表达式，建议清掉并留注释 |
| **脚手架侧** | ✅ **通过 —— 无假绿**：`-Scope all` 104/0/0 有内容（新写 6 组 / 跳过 0 组 / 执行 104 ≥ 下限 80）；E-11 的 10 项判据 + 9 个鉴别力自检全部有效，且我在**真缺陷**上实测它会炸。新增 **R-8（中，口径缺口：E-11 不覆盖 WM_SIZE，其 0.10% 阈值比该路径的合法噪声 0.267% 更紧）**、R-9/R-10/R-11（低/提示） |
| **交付物指纹** | ✅ **1,006,592 B / mtime 2026-10-14 12:12:03 / sha256 `492522BD23280617911D001027CEF678AB03BF82C0E096791CFA9F2D19E1E7A8`**，标题栏 `YuaExtract 1.0.0 [492522BD]`，「关于」页四项自证一致；`Release\` 里没有被删/无备份的窗口期（我全程有备份并逐字节核对） |
| **`-Scope all`** | ✅ **总计：通过 104 / 失败 0 / 提示 0**（exit 0），与 lead 亲跑一致 |

### 幽灵修复轮新增证据文件

> 约束要求我**只写本报告**，所以上面所有新证据都落在 `%TEMP%`（不在仓库里）；下表是我自己造的证据及其复算口径。

| 路径 | 内容 |
|---|---|
| `Run\out\tab-pixels.json` + `Run\out\tab-pixels\tab-*.bmp/png` | **我自己抓**的修复后 12 张抓图（1150x795，bitblt）与快照 |
| `Run\out\checks-tab-pixels.json` | 我自己跑的 `assert-tab-pixels.ps1` 结果（10/10 PASS） |
| `Run\out\verifier-tab-pixels-prefix.json` + `Run\out\verifier-tab-pixels-prefix\` | **我自己抓**的修复前（`6B664840`）12 张抓图与差异可视化 |
| `Run\out\verifier-checks-tab-pixels-prefix.json` | 旧构建上的断言结果（**exit 1 / 3 项失败**，含 144 条热区归属） |
| `%TEMP%\yua-verifier-delta\build-incremental.log` | 任务指定命令的构建日志（exit 0、0 warning / 0 error） |
| `%TEMP%\yua-verifier-delta\build-forced-loader.log` | 改动的工程 `/t:Rebuild` 到独立 OutDir 的日志（exit 0、零编译告警） |
| `%TEMP%\yua-verifier-delta\rebuild\YuaExtract.exe` | 强制重编产物（与冻结版只差 6 字节时间戳，逐字节对照的原始文件） |
| `%TEMP%\yua-verifier-delta\wm-fixed\` / `wm-prefix\` | WM_SIZE 探针的抓图与 JSON（两代构建各一套） |
| `%TEMP%\yua-verifier-delta\crops\` | 差异带 3 倍放大对照图 + 差异涂蓝图（`diffmap-*.png`） |
| `%TEMP%\yua-verifier-delta\classify-diff.py` / `analyze-bands.py` / `crop-bands.py` | 量级分类（±1 LSB vs 内容级）、位移搜索、带子放大 |
| `%TEMP%\yua-verifier-delta\about-shot\about-page.png` | 「关于」页截图（构建指纹栏位） |
| `%TEMP%\yua-verifier-delta\run-all-scope-all.log` | `-Scope all` 全量日志（含汇总 104/0/0） |
| `%TEMP%\yua-verifier-delta\probe-wm-size.ps1` / `probe-about-focus.ps1` / `probe-about-shot.ps1` | 我写的探针（只读脚手架 lib；不修改 `_verify\**`） |

> **收尾（本轮）**：`Get-Process YuaExtract` **无残留**；仓库**根目录无任何新产物**（探针一律 `Set-Location %TEMP%` 后运行）；`_verify\**` 里除本报告与脚手架自己该写的结果文件外**未被我改动**；未触碰 `E:\GAME`；
> `Release\YuaExtract.exe` 经"备份 → 被 Clean 删除 → 原地恢复"后，**长度 / mtime / sha256 三项重新核对为冻结值**。
