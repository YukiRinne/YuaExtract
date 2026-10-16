# 独立验证报告 —— 「扩展集」页签与在线更新（task-5）

> **复核者身份**：本轮**没有参与任何实现**。全部数字都是我自己跑出来的；凡引用别人的数字都注明来源。
> 复核期间**没有改动** `CxdecExtractorLoader\**`、`docs\**` 与 `_verify\restore-harness\` 下任何既有脚本
> （脚手架脚本只读、只跑）。我新增的脚本全部落在 `_verify\restore-harness\Run\verify-task5\`。

## 0. 验证对象与我用的机器 / 命令

| 项 | 值 |
|---|---|
| 被测产物 | `Release\YuaExtract.exe` = **1,173,504 B**，sha256 `FAEB67605A77DF3F2E7CDA23156018019C13F458E2348D8F73A16AFBA386889E`（短指纹 `FAEB6760`），mtime 2026-10-15 23:45:57 |
| 复核后复查 | 仍然是 `FAEB6760` / 1,173,504 B / mtime 未变（我全程只**拷贝**它，从不写它） |
| 机器 | Windows 11 Pro 10.0.26200，x86 目标，Administrator |
| 工作目录 | `E:\Harness&Agent Project\YuaToolkit插件\YuaExtract` |
| 代码库 | MSBuild 17.14.60（VS 2022 BuildTools），pwsh 7 |

主要命令（全部由我本人执行）：

```powershell
# ① 全量脚手架（自己重跑，不引用别人的数字）
pwsh -NoProfile -File '_verify\restore-harness\run-all.ps1' -Scope all      # → exit 0

# ② 同源码重编到独立 OutDir（不覆盖 Release\）
MSBuild.exe CxdecExtractorLoader\CxdecExtractorLoader.vcxproj -p:Configuration=Release -p:Platform=Win32 `
  "-p:OutDir=...\Run\verify-task5\Out\" "-p:IntDir=...\Run\verify-task5\Int\" "-p:SolutionDir=<repo>\"

# ③ 我自己的"敌对远端仓库"用例台（新增脚本，见 §6）
pwsh -NoProfile -File '_verify\restore-harness\Run\verify-task5\forge.py' <base rules.int> .\forged
pwsh -NoProfile -File '_verify\restore-harness\Run\verify-task5\make-repo.ps1'
pwsh -NoProfile -File '_verify\restore-harness\Run\verify-task5\run-case.ps1' -Case <case>
pwsh -NoProfile -File '_verify\restore-harness\Run\verify-task5\break-manifest.ps1'
pwsh -NoProfile -File '_verify\restore-harness\Run\verify-task5\serve-redirect.ps1' -InfoFile ...
```

---

## 1. 第一件事：`run-all.ps1 -Scope all` 的自跑结果

```
断言 A（步骤3 还原，模式 default）    [失败 0 / 提示 0 / 共 21]
断言 B（步骤3 还原，模式 keep）       [失败 0 / 提示 0 / 共 10]
断言 C + E（步骤2 只收集）            [失败 0 / 提示 0 / 共 14]
脚手架自检（断言脚本的鉴别力）        [失败 0 / 提示 0 / 共 8]
扩展集一期断言 E-1..E-19              [失败 0 / 提示 0 / 共 33]
扩展集断言自检（鉴别力）              [失败 0 / 提示 0 / 共 55]

总计：通过 141 / 失败 0 / 提示 0
本轮新写的断言组 6 个；被判为'上一次残留'而跳过的 0 个；文件不存在 3 个
结论：全部通过。            exit = 0
```

**与声明一致**：`总计 141 / 失败 0 / 提示 0`、`通过 141`。两条必须说清的注解：

- **`文件不存在 3 个` 是结构性的，不是缺失**：这 3 份是 `checks-supplement.json`、`checks-selftest-supplement.json`、
  `checks-pressure.json`，只有 `-Scope supplement` / `-Scope pressure` 才会产出（`run-all.ps1` 里那两个分支是
  `if ($Scope -eq 'supplement')` / `-eq 'pressure'`）。我逐个确认过这三个文件确实不存在（`stale = 0` 也印证了这一点）。
  **但要注意它的含义**：`-Scope all` 这个名字容易让人以为"全跑了"，实际上它**不覆盖** supplement 与 pressure 两组。
- **141 里有 63 项是脚手架自检**（8 + 55），产品侧断言只有 **78 项**（21+10+14+33）。
  数字本身没错（汇总就是逐 check 数的），但引用时别把 63 项鉴别力自检当成产品断言。

运行时序：`Run\out\extension-online-summary.json` 写于 03:04，`checks-extension.json` 03:05，
`checks-selftest-extension.json` 03:07，汇总 03:08 —— 全部是**本轮**新写的（`stale = 0`）。

---

## 2. E-12..E-19 的 `-Details` 逐条读 + 判据真假判断

从我自己这轮的 `Run\out\checks-extension.json` 取（不是抄别人的）：

| 断言 | 我读到的详情要点 | 判据是真的吗 |
|---|---|---|
| E-12 | `三路取样：set-1 / set-2 / home-back`；`set-1 可见 18 个：4001 4002 4003 4004 4071 4072 4080 4081 4082 4083 4084 4130 4131 4132 4133 4134 4135 4136`；`set-2 可见 18 个；home-back 可见 36 个` | **真**。逐控件枚举 `IsWindowVisible`，要求 set-1 与 set-2 集合完全相等、该可见的都在、该隐的都不在、切回首页后本页签控件全隐、且 homeVisible 不得为空。没有"靠页面套话"的空间。我自己另去 run JSON 里把三路的 id 全列出来核对过（见 §5.2）。 |
| E-12-T / -C / -E / -F / -a | 阈值可信性 `≤0.10%`、连拍 `0 px / 0.0000%`、`5 张 set 抓图；客户区 1150x795；构建指纹 FAEB67605A77DF3F`、`set-baseline vs make-baseline 42.56%` / `vs home-startup 37.71%`、六组两两比对全 `0 px / 0.000%` | **真**。像素口径可复算（`lib\HarnessPixels.ps1`），且判据里含"阈值必须在可信区间""抓图必须非空白""与别的页必须显著不同"三条反退化护栏。 |
| E-13 | `空库：清单独立解析 0 行 / 0 个会社；界面报 会社 0 个、作品 0 个`；`1 条：…列表文字 [YuaHarness] / [otomeki_ckr]`；`多会社多作品：… 界面报 会社 2 个、作品 1 个；列表文字 [YuaHarness,YuaHarnessOnline] / [otomeki_ckr]` | **真**。期望值来自断言**自己解析**的清单快照（不是产品自述），并且列表**文字**也要逐项相等 —— 这条正好挡住"计数正常、文字读不到"那类假通过。 |
| E-14 | `逐项比对完成：YuaHarness / otomeki_ckr；档案 735 字节 sha256 491D443DB214F92A…；DIRS 5 / FILES 15` | **真**。面板字段 ↔ 清单行 ↔ 档案本体（`rules-int.py` 独立 dump）三方交叉，还要求库内目录与档案路径真的出现在面板里。 |
| E-15 | `期望目录（界面选中的会社/作品算出）：…\ExtensionLibrary\YuaHarness\otomeki_ckr`；并**禁止**"无选中/目录缺失"两路出现「已打开 」 | **真**，且明确声明"只判路径文本，不判资源管理器窗口"。 |
| E-16 | 7 个变体各自"报出了该报的"，改法分别为：换坏档案 / 改第 441 字节 / 追加 9 列坏行 / 追加非 hex 摘要 / 给库根加 Deny Write ACL / 挪走 DLL；`自检前后库根与工具目录（除 Log）的树摘要逐字节相同` | **真**。我另做了独立复核（手工坏行，见 §5.3），结论一致。**唯一弱点**：needle 偏宽（`找不到`、`不可写` 这种短词），但我逐变体做了交叉计数检查：每个变体的面板里**只**出现自己那套关键词，`clean` 面板里一个坏变体关键词都没有（`解码失败/不合规/跳过/格式不对/解析失败/不可写/找不到` 全部 count = 0）。 |
| E-17 | `档案 META.comment（独立解析）："YuaExtract 验证脚手架合成档案"；弹窗 1 个` | **真**。comment 由 `rules-int.py` 独立解析取得；另要求弹窗出现会社/作品与「无法校验/无法验证」的如实说明。 |
| E-18 | `基址一致：https://127.0.0.1:54636/`；`正例：库清单 1 → 2 行；索引 2 行；第二轮幂等（仍 2 行）`；11 条坏用例**逐条**"已按预期拒，本地未动" | **真，判据质量最高**。索引由断言自己解析、档案自己 sha256+dump、库清单自己解析；另有"夹具自身一致性复核""（本地已有→）无需下载 前置复核""前后库摘要逐字节""暂存目录为空"。 |
| E-19 | `端点不可达…2.9 秒`；`404…0.8 秒`；`超时…32.5 秒`；`基址是明文 http：措辞与用时都对（0.8 秒），本地未动` | **有时间/本地两条是真的**（上限 20s / 下限 25s / 上游 300s、本地逐字节、暂存为空）。**但 http 那一条的措辞判据是弱判据 —— 见 S-1，缺陷就藏在"基址是明文 http：措辞与用时都对"这句结论里。** |

---

## 3. 产品侧发现

### P-1（中）索引行的「会社 / 作品」与档案内嵌 META 的「会社 / 作品」不一致时，界面说的是索引那套，实际落库进的是 META 那套

**现象**：远端索引是攻击者可控输入，它提供两个字符串用来拼下载 URL，但**落库目录**来自档案自己的 `META`。
两条路都没有"二者必须一致"的校验（`§14.4` 的 8 条里也没有这一条）。后果：

1. 列表与结论文字说「已新增/已更新：<索引里的会社> / <索引里的作品>」，磁盘上却落在 `<档案 META 的会社>/<作品>\`。
2. 写进库清单的那一行**自相矛盾**：`会社/作品` 列来自档案 META，`来源` 列是**用索引名拼出来的**暂存路径。
3. **加重版（代码已确认，未实跑两槽位现场）**：覆盖确认框里的"本地那份"是用**索引名**查的
   （`YuaExtractMainWindow.cpp:5003` `FindLibraryEntry(local, remote.publisher, remote.title)`），
   而真正被覆盖的是 `InstallToLibrary`（`ExtensionSet.cpp:3443`）按**档案 META** 算出来的目录。
   于是用户看到并确认"覆盖本地 A/B"，实际被替换的可能是 C/D —— 一个标准的 confused-deputy。
   （`Online::Classify` 同样按索引名/索引 exe 摘要判定，所以连"该不该问"都可能问错对象。）

**复现**（我实际跑过）：

```powershell
# 夹具：索引行写 IndexSaysThis/AndThis，位置上的 rules.int 内嵌 META 写 VictimPublisher/VictimTitle，
#       档案摘要 = 该档案实际 sha256，目标 exe 摘要 = 档案内嵌 target.sha256（所以 2/3/4 条都能过）
pwsh -File _verify\restore-harness\Run\verify-task5\run-case.ps1 -Case meta-mismatch
```

**判定依据（我跑出来的原文）**：

- 远端列表：`IndexSaysThis / AndThis / 2 条 / 新增（本地还没有）`
- 安装后状态：`已新增：IndexSaysThis / AndThis（2 条）`，而**同一段文字**下面的校验明细写着
  `会社：VictimPublisher / 作品：VictimTitle` —— 产品自己把两个不一致的名字并排印了出来，却没报一句异常。
- 库内实际文件：`App\ExtensionLibrary\VictimPublisher\VictimTitle\rules.int`（我按整个沙箱前后文件清单 diff 得到）
- 同一次落下来的清单行：`VictimPublisher<TAB>VictimTitle<TAB>84F17B…<TAB><TAB>4E7736…<TAB>D002CA…<TAB>2<TS>…<TAB>来源=E:\…\Log\OnlineStaging\Online_IndexSaysThis_AndThis.rules.int`

**建议修法**：在 `Online::VerifyArchive` 里加一条（对应 §14.4 第 4 条扩写为"索引是唯一命名来源"）：
`MakeSafePathComponent(archive.meta.publisher/title)` 必须等于 `MakeSafePathComponent(entry.publisher/title)`，
不等就拒并给出"索引自称 X，档案自述 Y"的措辞；设计文档 §14.4 补第 9 条、§14.6 补这一种失败措辞。

### P-2（低-中）基址不是 https 时，弹窗「仓库地址」是空的，错误里把地址也印成空串，还给了与事实相反的"地址也不能为空"

**现象**：`Online::NormalizeBase` 把"非 https"折成**空串**，于是 §14.5 的"弹窗必须显示当前基址"落空，
§14.6 的"每一种失败各有各的措辞"也变成"空地址"这种没法行动的措辞。

**复现**（我实际跑了 http 与 ftp 两种）：

```powershell
pwsh -File Run\verify-task5\run-case.ps1 -Case control-good -EndpointOverride 'ftp://127.0.0.1:21/pub/'
```

**判定依据**：状态文字逐字为

```
索引拉取失败：
基址不可用：""。
本工具只在 https 上拉扩展集（http 是明文，内容中途谁都能改），地址也不能为空。
…
当前基址：
```

而驱动记下的弹窗 4140 文本 `endpointShown = ''`（`YUAEXT_ENDPOINT` 确实传进了子进程，
`endpointInChildEnv = True`）。用户填的是 `http://127.0.0.1:62283/`，产品回显的是 `""`，还说"地址也不能为空"。

**注**：脚手架 README（E-19 行）**已自行披露**这条"已知小瑕疵…已报 lead，未按缺陷拦断言"。
我独立复现并确认它**仍然存在**；同时发现了 README 没提的另一半（见 **S-1**：E-19 的判据其实也抓不到它）。

**建议修法**：`ResolveUsableBase` 报错时回显"用户实际填的原文"（`TrimSpaces` 后、归一化前），
并把"空地址"与"非 https"分成两句；弹窗显示基址时也回显原文并旁注"（不可用：只走 https）"。

### P-3（低）在线更新入库后，库清单的「来源」列指向一个已经被删掉的暂存文件

**现象**：`InstallToLibrary` 把 `row.source = archivePath` 写进清单；在线这条路传进来的 `archivePath`
是 `Log\OnlineStaging\Online_*.rules.int`，而这个文件在入库成功后的 `CleanupStaging` 里被删掉了。
详情面板「来源」因此永远指向不存在的文件（我在 `meta-mismatch` 的清单行里拿到实证，且 `Log\OnlineStaging` 事后为空）。

**建议修法**：在线路径把「来源」写成远端 URL（`<基址><会社>/<作品>/rules.int`），或加一列"来路"区分"本地制作"与"在线拉取"。

### P-4（低）敌对/非法名字能走到"入库失败"，但界面从不显示 sanitize 之后的实际目录名

**现象**：`..\..\Windows\Temp / T` 在界面上原样显示，实际落在 `ExtensionLibrary\WindowsTemp\T\`。
用户看到的名字和磁盘上的目录名不同，没法据此去"打开选中扩展集"里找。

**判定依据**（我跑出来的）：状态文字 `已新增：..\..\Windows\Temp / T（2 条）`，而库内是 `WindowsTemp\T\rules.int`。
同类：`  padded  /  T  ` → `padded\T`；`a:b / c:d` → `ab\cd`；`C:\absolute / T` → `Cabsolute\T`；`\\server\share / T` → `servershare\T`。

**建议修法**：详情面板加一行"库内目录"，或状态文字里同时给"原名 → 实际目录名"。

### P-5（低）`EnsureDirectoryTree` 把 `ERROR_ALREADY_EXISTS` 一律当成功，保留设备名会让错误推迟并说得不准

**现象**：会社/作品 = `CON` / `NUL` 时，`CreateDirectoryW` 对 `…\ExtensionLibrary\CON` 与 `…\CON\NUL`
返回失败但 `GetLastError() == ERROR_ALREADY_EXISTS`（这两个名字**是**设备），于是被当成"目录已存在"放过，
真正报错推迟到写文件：`安装失败：写库里的档案失败：…\ExtensionLibrary\CON\NUL\rules.int（Win32 错误码 3）`。

**判定依据**：我实测的原文如上；事后用 `[System.IO.Directory]::Exists('\\?\' + … + '\CON')` 确认**目录并不存在**，
库根 0 个条目、暂存目录为空、整个沙箱零新增文件。**后果仅是措辞不准**（不是安全缺陷）。

**建议修法**：`InstallToLibrary` 在 `ValidateMeta` 里显式拒绝保留设备名（`CON/PRN/AUX/NUL/COM1-9/LPT1-9`，
含带扩展名与尾随空格的写法）；或在 Ensure 之后复核 `DirectoryExists`。

### P-6（信息）索引里会社分量带 `%` 时，URL 路径里的 `%2f` 会被**服务端**解码成 `/`

**现象**：会社 `..%2f..` 经 `MakeSafePathComponent` 后是 `%2f`（去掉了前后点、`\` 已被剥掉），
`EscapeUrlPath` **故意保留 `%`**（为了不弄坏带 `%20` 的镜像基址），于是请求行里出现 `/%2f/T/rules.int`。

**判定依据**：夹具服务端日志逐字为 `#2 | T/rules.int | mode=404` —— 服务端把 `%2f` 解码成 `/` 并去掉了前导斜杠，
所以它请求到的对象**不是**索引行所指的那一个。**本地无任何影响**（这条路 404 收场，前后沙箱零新增文件、库未改）。

**建议修法**：把两类字符串分开处理 —— 基址（配置来的，允许含转义，原样保留）与**会社/作品路径分量**
（索引来的，必须是字面量，`%` 一律转义成 `%25`，或在分量层面直接拒绝非 `[A-Za-z0-9._-]` 与常见文字字符之外的字节）。

---

## 4. 脚手架侧发现

### S-1（中）E-19 的 `online-http-base` 是**弱判据**：只有通用那一句 needle 会命中，用例支持不了它的标题

**现象**：`assert-extension.ps1` 第 2621 行：

```powershell
'online-http-base' = @{ Needles = @('只允许 https', '不是 https', '基址不可用'); MaxSeconds = 20; ... }
```

而产品的 `ResolveUsableBase` 对**任何**非 https 基址都走同一句 `基址不可用：…（http 是明文…）`。
前两个 needle 在这个分支里**永远不会出现**（`只允许 https`/`不是 https` 只存在于 `ParseHttpsUrl`，而
`NormalizeBase` 在它之前就把非 https 折成空串了）。于是这条断言实际只靠最泛的第三个 needle 撑着。

**我实测的假通过过程**（这是我本轮最有价值的产出）：

1. 先把基址换成 `ftp://127.0.0.1:21/pub/` —— **与 http 毫无关系**的一种不可用协议；
2. 产品回的状态文字与 http 那次**逐字一致**（`基址不可用：""。…`）；
3. 拿同一套 needle 判定：`只允许 https -> False`、`不是 https -> False`、**`基址不可用 -> True`**；
4. 也就是说：把这条用例的 `Endpoint` 换成任意非 https 串，E-19 照样会打印
   `基址是明文 http：措辞与用时都对（0.8 秒），本地未动`。

**结论**：产品的**行为**是安全的（一律拒非 https，实测也没有偷偷降级），但**断言声明它验的是「只允许 https 地址」这条措辞，实际没验**。
自检组里也没有对应场景（`e19-*` 三条是 `wrote-local-library` / `unreachable-slow` / `timeout-too-fast`），
所以这个弱点不会被 55 个自检场景抓到。

**建议修法**：needle 收紧为 `@('只在 https 上拉扩展集', '不是 https:// 开头的')`；
并补一条自检场景 `e19-http-base-generic-word`：造一份"只含 `基址不可用` 字样"的记录，要求 E-19 **必须失败**。

### S-2（中）库目录不存在时，`run-extension-ui.ps1` 直接**崩溃**（不是失败，是未捕获异常）

**现象**：`Get-LibraryInfo`（`run-extension-ui.ps1:796`）在 `<工具目录>\ExtensionLibrary` 不存在时返回 `Root = ''`；
紧接着 `setupdate` 第 1755 行与 `setcheck` 第 1710 行的 `Get-TreeDigestExcluding -Path $libInfo.Root`
撞上 `lib\HarnessUtil.ps1` 里 `[Parameter(Mandatory)][string]$Path` 的**空串绑定校验**，抛
`ParameterBindingValidationException: 无法将自变量绑定到参数 'Path'，因为它是空字符串`，
驱动在**写任何结果文件之前**就死了。

**判定依据**：我自己踩到的原文（`$_.ScriptStackTrace` 指到 `run-extension-ui.ps1: 行 1755`）；
换成"先手工建一个空 `ExtensionLibrary` 目录"就立刻恢复正常。README 里**没有**记录这条。

**为什么这条要紧**：**首次安装**（还没有 `ExtensionLibrary\`）正好就是这个状态，而那正是
"在线更新把库建出来"这条产品路径唯一需要被测的现场 —— 现在它测不到，而且失败形态是"驱动崩了"，
不是"断言红了"，很容易被当成环境问题忽略掉。

**建议修法**：`Get-TreeDigestExcluding` 的 `-Path` 加 `[AllowEmptyString()]`（函数体里已有 `Test-Path` 兜底，
会返回 `Exists = $false`），或调用点用 `if ($libInfo.Root)` 包住；再补一条"库目录不存在时在线更新照样能装进去"的用例。

### S-3（低）「关于」页签（4004）在断言里**完全没有覆盖**

**现象**：`TabAbout = 4004` 只在 `run-extension-ui.ps1` 的 `$script:UiIds` 里定义，
`assert-extension.ps1` / `assert-restore.ps1` / `assert-collect-only.ps1` 里**没有任何一处**用它。
（`grep -rn "TabAbout\|4004"` 的结果只有那两处定义与注释。）

**为什么现在更要紧**：本轮 `LayoutWindow` 的分派刚被改过（新增 `setTab` 分支，About 落到
`if (!homeTab && !makeTab && !setTab)` 那一路）。若有人把这几个分支改错，`PAGE` 的宽度/可见性会坏，而不会有任何断言响。

**建议修法**：补一条最便宜的 E-20：切到 4004 → `4071` 可见且宽度铺满（`GetWindowRect` 比一下 `Set` 页签那一路的宽度）、
`4130-4136`/`4080-4084`/`4070` 全隐。

### S-4（低，记账）「总计：通过 141」里有 63 项是脚手架自检，产品侧只有 78 项

8（`checks-selftest.json`）+ 55（`checks-selftest-extension.json`）= 63。数字本身没错（汇总是逐 check 数的），
但对外引用"141 通过"很容易被读成"产品过了 141 条"。建议汇总行加一句拆分：
`总计：通过 141（其中产品断言 78 / 脚手架自检 63）`。

### S-5（低）`-Scope all` 的绿**不覆盖** supplement 与 pressure

见 §1 第一条注解：`文件不存在 3 个` 正是这两组的 4 个结果文件中的 3 个（supplement 2 个 + pressure 1 个）。
不是缺失、不是残留，但 `-Scope all` 这个名字会让人以为"全部"。

### S-6（低，文档）README 的历史标定行相对当前产物已经过时

`README.md:297` 写"别和最终构建（`492522BD…` / 1,006,592 B）混淆"，而当前产物是 `FAEB6760` / 1,173,504 B。
**我不建议改这一行**（它属于 E-11 的历史来源记录，任务也要求"来源指纹与历史行不能被改动"）；
建议在 `# E-12..E-19` 那一节追加一行"当前最终构建 = `FAEB6760` / 1,173,504 B"，而不是改动旧行。
同理 `docs\YuaExtract-UI升级说明.md` §7.6 已把这一项显式留给 lead 填 —— 那种做法是对的。

### S-7（信息）E-12-E 只**记录**被测构建指纹、不比对期望值

`run-tab-pixels.ps1:460-464` 只把 `appFingerprint` 记进记录，没有任何 `ExpectedFingerprint` 断言。
这是**正确的**（脚手架该测 `Release\` 里那一份，而不是测某个固定指纹），但它意味着"这轮的绿属于哪个构建"
完全依赖读者去看结果里的指纹字段。我把这一条算作**已满足**：我这轮的
`Run\out\checks-extension.json` → `E-12-E` 详情逐字为 `5 张 set 抓图；客户区 1150x795；构建指纹 FAEB67605A77DF3F`。

### S-8（信息，自查）我自己也踩了 README 踩坑 #52

我写的 `make-repo.ps1` 最初把两段代码用裸 `{ … }` 包着当"块"用 —— PowerShell 里那是**脚本块字面量**，
既不执行还会被打印到 stdout（于是我的夹具只在 foreach 里建成了 11 个 case，`meta-mismatch` 与 `control-good` 静默丢失）。
改成 `& { … }` 后正常。记录在此，因为它与 README 踩坑 #52 完全同因。

---

## 5. 我自己重跑的独立复核（数字全部出自我这一轮）

### 5.1 产物来源可信：同源码重编到独立 OutDir

```
MSBuild.exe CxdecExtractorLoader\CxdecExtractorLoader.vcxproj -p:Configuration=Release -p:Platform=Win32 `
  -p:OutDir=...\Run\verify-task5\Out\ -p:IntDir=...\Run\verify-task5\Int\ -p:SolutionDir=<repo>\
→ CxdecExtractorLoader.vcxproj -> ...\Run\verify-task5\Out\YuaExtract.exe     （0 error / 0 warning）
```

| | 大小 | sha256（前 16 位） |
|---|---|---|
| 冻结产物 `Release\YuaExtract.exe` | 1,173,504 | `FAEB67605A77DF3F…` |
| 我重编的 `Out\YuaExtract.exe` | **1,173,504** | `26640536162E11F7…` |

**逐字节 diff（我自己写的 `diff-pe.ps1`）：整份 1,173,504 字节里只有 9 个字节不同，集中在 3 段：**

| 偏移 | 长度 | 落在哪 | A（冻结） | B（我重编） |
|---|---|---|---|---|
| `0x00000110` | 3 | PE 头（`TimeDateStamp`） | `35 F5 D0` | `55 22 D1` |
| `0x000E55F4` | 3 | `.rdata`（调试目录项的时间戳） | `35 F5 D0` | `55 22 D1` |
| `0x000E5610` | 3 | `.rdata`（同上） | `35 F5 D0` | `55 22 D1` |

三段的值都等于 PE TimeDateStamp 的低 3 字节（`1792079157` = 2026-10-15 15:45:57Z vs
`1792090709` = 2026-10-15 18:58:29Z，差 11,552 秒 = `0x2D20`，`0xD0F535 + 0x2D20 = 0xD12255`，与实测字节吻合）。
**`.text` 段零差异**。

**结论**：冻结产物 `FAEB6760` **确实是当前源码编出来的** —— 除链接时间戳外逐字节相同，
不存在"源码比产物新"或"产物来路不明"的情况。另外：

- `check-source-newer.ps1`（我自己写的）比较了仓库里 **471 个** `*.cpp/*.c/*.h/*.rc/*.vcxproj/*.filters/*.ico/*.def/*.asm`
  （排除 `Release/Debug/obj/x64`），**没有一个**比 `Release\YuaExtract.exe` 新 → 与"源码在 `FAEB6760` 构建之后没有任何改动"一致。
- 我的重编把 `IntDir` 也重定向了，事后复查 `CxdecExtractorLoader\**` 下**没有任何**文件被我的构建写过 → 没有污染源码树。

### 5.2 P-6 真的修了吗 + 没引入"只有当前页签可见"的新行为

读 `YuaExtractMainWindow.cpp:2238-2245`：四路 `MoveControl(..., true)`，最后一个是**字面 `true`**
（注释说明了原来那个恒真表达式与"谁清理掉它就会坏"的来历）；选中态由 `WM_DRAWITEM → DrawTab`
（`DrawTab` 用 `selected` 画白底 + 强调色下划条）决定。行为与"恒真"等价，没有新语义。

**实测佐证**（我自己从 `Run\out\extension-set-one.json` 里把三路取样逐 id 列出来）：

```
set-1     : 18 个 = 4001 4002 4003 4004 4071 4072 4080 4081 4082 4083 4084 4130 4131 4132 4133 4134 4135 4136
set-2     : 18 个（与 set-1 完全相同）
home-back : 36 个 = 4001 4002 4003 4004 4010…4014 4020 4021 4022 4030 4031 4032 4040 4050 4051 4052
                    4060…4064 4070 4072 4074 4080 4081 4082 4083 4084 4085 4086 4087 4088
```

→ **四个页签按钮 4001-4004 在每一路上都可见**（首页那一路也在），所以"只有当前页签可见"那种新行为没有被引入。

### 5.3 「不静默」三处真的都报了吗（我用**自己的手**改的 manifest.txt）

`break-manifest.ps1`（我写的）：先跑一次正常安装把库建好，然后**手工**在 `manifest.txt` 末尾追加
一行只有 9 列的坏行（`BogusPublisher<TAB>BogusTitle<TAB>AAAA…<TAB><TAB>BBBB…<TAB>CCCC…<TAB>3<TAB><TAB>`），
再驱动 `settab,setbrowse,setcheck`。我跑出来的原文：

| 处 | 我读到的原文 | 结论 |
|---|---|---|
| ① 详情面板「清单提示」 | `清单提示：扩展集库清单有不合规的行（已跳过）：<…>\ExtensionLibrary\manifest.txt` / `有 1 行不合规，已跳过；其余 1 行照收：` / `  第 3 行：列数不对（应有 10 列，实际 9 列）。…` | **报出来了，且点名行号** |
| ② Warn 日志 | `2026-10-16 03:32:13 \| W \| T6EDC \| [YuaExtract] 扩展集清单提示：扩展集库清单有不合规的行（已跳过）：…` | **报出来了（Level = W）** |
| ③ 自检（Severity=1） | 弹窗 `YuaExtract :: 运行环境自检：1 项问题`，其中 `· 库清单 manifest.txt：可解析，共 1 条 —— …` / `  但清单里有不合规的行，已被跳过：` / `  有 1 行不合规，已跳过；其余 1 行照收：` / `  第 3 行：列数不对（应有 10 列，实际 9 列）。` | **报出来了，结论计为"1 项问题"（Severity=1）** |

**并且产品没有因此改动任何文件**：库根树摘要前后一致 = True；工具目录（除 Log）摘要前后一致 = True（我自己的清单+摘要比较）。
（副观察：自检这一路是"**失败才弹**"，本次因为有 1 项问题所以弹了框 —— 与 §7.4 "成功不弹" 的口径一致。）

### 5.4 信任链第 3 / 5 条（脚手架在线用例**没覆盖**的那几条）

`rules-int.py mutate` 只产 CRC 错 / 截断 / 版本过高 / 缺 IDENTITY / 空名字层，**不产 EXCEPTIONS 非空**；
在线编排也没有 magic 坏、版本高这两条坏法。我用 `forge.py` 从真档案派生，把索引的
`档案摘要` 与 `目标exe摘要` 都写成**能对上的值**（所以第 2、4 条必然通过），专打第 3、5 条：

| 我的用例 | 我造的东西 | 产品行为（我读到的原文） | 本地 |
|---|---|---|---|
| `trust-exceptions` | 结构/CRC/尺寸全合法、`IDENTITY` 一字未动、只把 `EXCEPTIONS` 填 1 条且 `STATS` 同步改成 1 | `档案自洽性检查不过：EXCEPTIONS 有 1 条、STATS 自称 1 条，两者都必须为 0。有例外的档案本工具不接受。` | 库未写、暂存为空、沙箱零新增 |
| `trust-magic` | 只把 magic 改成 `ZZZZ`（长度与 CRC 字段不动） | `档案结构校验不通过：magic 不对：期望 'YXS1'（0x31535859），实际 0x1515870810。这不是扩展集档案。` | 同上 |
| `trust-version` | `version = 0xFFFF`，CRC 合法 | `档案结构校验不通过：档案格式版本过高：文件是 65535，本工具只认 1。请升级 YuaExtract。` | 同上 |

→ **§14.4 第 3 条与第 5(a) 条在在线这条路上真的生效**，且三种措辞互不相同、都能指导用户。

### 5.5 https 门禁：302 跳明文 http 会不会被跟随（脚手架**没有**这条坏法）

我自己写了 `serve-redirect.ps1`（https 侧一律回 `302` + `Location: http://127.0.0.1:<明文端口>/index.txt`；
明文侧一旦被连上就记账并回一份**看起来合法**的索引）。结果：

- https 侧日志：`TCP-REQUEST 03:26:01.239 GET /index.txt HTTP/1.1`（只此一条）
- 明文侧命中文件：**空**（一个连接都没收到）→ **没有降级**
- 产品状态文字：`拉取索引失败：服务器要求重定向（HTTP 302）。本工具不跟随重定向（只走 https），就此停止。地址：https://127.0.0.1:60014/index.txt / 对方指向 http://127.0.0.1:60015/index.txt`
- 本地：沙箱零新增文件、库未改、暂存目录为空

→ **`WINHTTP_OPTION_REDIRECT_POLICY_NEVER` 确实设上了、也确实生效**（而且失败措辞把对方要跳去的 http 地址也印了出来，很好）。

### 5.6 「绝不后台联网」（§14.4 第 8 条）

让 `YUAEXT_ENDPOINT` 指向一个**活着的**本地夹具服务，然后只跑 `-Step settab`（打开「扩展集」页签、
刷新本地库，**不点**在线更新）：夹具服务的日志**只有启动那一行**，整个会话**零请求**。
`OnSetOnlineUpdate` 读代码也只做"开弹窗 + 重扫本地库"，没有任何 `Online::` 调用。
→ 启动与切页签都不联网；`FetchIndex` 只有"拉取索引"按钮那一条调用路径。

### 5.7 路径穿越（我自己构造的 10 组敌对会社/作品）

夹具：索引行与档案 META 都写成敌对串，索引里的两个摘要都按**实际字节**算（所以能走到落库那一步）。
每次跑完对比**整个沙箱**（工具目录 + 库 + 夹具，除 `Log`/`out`）的前后文件清单：

| 会社 / 作品 | 产品结果 | 库内实际落点 | 库外有没有东西 |
|---|---|---|---|
| `..\..\Windows\Temp` / `T` | 已新增 | `WindowsTemp\T\rules.int` | **没有** |
| `..%2f..` / `T` | 下载失败（404，URL 里是 `/%2f/T/rules.int`） | 无 | **没有**（暂存也为空） |
| `C:\absolute` / `T` | 已新增 | `Cabsolute\T\rules.int` | **没有** |
| `\\server\share` / `T` | 已新增 | `servershare\T\rules.int` | **没有** |
| `  padded  ` / `  T  ` | 已新增 | `padded\T\rules.int` | **没有** |
| `a:b` / `c:d` | 已新增 | `ab\cd\rules.int` | **没有** |
| `CON` / `NUL` | 安装失败（Win32 3，见 P-5） | 无 | **没有**（`\\?\…\CON` 实测不存在） |
| `L`×240 / `M`×240 | 写临时档案失败（Win32 3） | 无 | **没有** |
| `...` / `..` | **未跑**（未验）：我只造了档案 `forged\meta-dots-only.rules.int`，没为它建仓库夹具、也没跑 UI —— 按代码它应在 `ValidateMeta` / 索引解析那一步被判"做不出库目录名"而拒，但这是**推断，不是我实测的** | — | — |
| `IndexSaysThis` / `AndThis`（档案 META 是另一套） | 已新增，**落进档案 META 的槽位** | `VictimPublisher\VictimTitle\` | **没有**（但见 P-1） |

**下载路径与落库路径都没有被带出预期目录**：落库目录只由 `MakeSafePathComponent` 之后的
`会社/作品` 拼成（`/ \ : " < > | ? *` 与控制字符被剥掉、前后空格与点被裁掉），
暂存文件名固定是 `Online_<会社>_<作品>.rules.int`（`Online_` 前缀还顺手排除了"文件名本身是 `..` 或设备名"）。
我逐例确认了失败路径一律"本地库未被改动"+ `Log\OnlineStaging` 为空。
**唯一真正的设计缺口是 P-1（名字来源不一致），不是路径穿越。**

---

## 6. 绿是真绿还是假绿

**总的说：产品侧这轮的绿是真的**（我按上表逐条独立复现，行为都对），
**脚手架侧有一条真·弱判据（S-1），我把它打出来了，并且成功让一条断言在"它自称验的东西没发生"的情况下通过。**

### 我让断言在不该通过的情况下通过过 —— 完整过程

- **目标断言**：`E-19` 的 `online-http-base`，标题 `基址是明文 http（§14.4 第 1 条：只走 https）`，
  `Expected = '只允许 https 地址'`。
- **本该被验的东西**：产品那句「**只允许 https 地址**：… 不是 https:// 开头的」。
- **我做的**：把基址从 `http://127.0.0.1:<port>/` 换成 **`ftp://127.0.0.1:21/pub/`**
  （一个与"明文 http"无关的协议），其余不动，用同一套 needle 判定。
- **结果**：`只允许 https -> False`、`不是 https -> False`、**`基址不可用 -> True`** ⇒ 断言通过，
  并打印 `基址是明文 http：措辞与用时都对（0.8 秒），本地未动`。
- **为什么能过**：needle 表里放了一个**通用兜底词**（`基址不可用` 是 `ResolveUsableBase` 对任何非 https
  基址的统一措辞），而它要证明的那句具体措辞（`只允许 https`）在这条代码路径上根本不可能出现。
- **顺带说明它为什么没被自检抓到**：`self-test-extension.ps1` 给 E-19 的三条鉴别力场景是
  `e19-wrote-local-library` / `e19-unreachable-slow` / `e19-timeout-too-fast`，没有一条针对"通用词误命中"。
- **修法与复验**：见 **S-1**（收紧 needle + 补自检场景）。收紧之后，当前的 http 用例会**立刻变红**，
  因为产品那句措辞确实不含 `只允许 https` —— 也就是说收紧 needle 会**同时**逼出 **P-2** 的措辞问题。

### 我还试过、但**没能**打出假绿的地方（如实记录）

- E-12 的可见性矩阵：我试过从"三路取样不全 / homeVisible 为空 / 该隐藏却可见"三条去绕，都绕不过（判据自己就拦）。
- E-13 的"列表读不到文字"：我核对了 `publisherCount`/`titleCount`/列表文字三者都比对独立解析结果，
  只改计数不改文字过不了。
- E-14：面板字段必须同时等于"清单行"和"档案本体"（`rules-int.py` 独立解析），只让面板撒谎过不了。
- E-16：我逐个变体做了关键词交叉计数，`clean` 面板里一个坏变体关键词都没有 → 不存在"靠上一条残留的话蒙对"。
- E-18：有"无需下载 前置复核""夹具自洽复核""前后库摘要逐字节""暂存为空"四道，我没找到绕法。
- 我自己**尝试**过但**没能**验证的部分，见 §7。

---

## 7. 我没验到的（不用推测填）

1. **「库目录压根不存在」时的产品自助建库那一路** —— 被 S-2（脚手架崩溃）挡住，我只好先手工建空目录再跑；
   因此 `OnOnlineInstall` 里 `EnsureDirectory(context.LibraryRoot)` 那一段的现场我**没测到**。
2. **`AutoProxy` / PAC** —— 只看代码（`WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY`；https 下代理只能做 CONNECT 隧道，
   理论上不能伪造内容）。我没有在设了 PAC 或系统代理的机器上实测，也没测"代理返回自签证书"那一路。
3. **「关于」页签** —— 没有任何断言（S-3），我只做了代码阅读（`LayoutWindow` 的 `!home && !make && !set` 分支
   把 `PAGE` 铺满整宽），**没有跑 UI 去抓图或量宽度**。
4. **`-Scope supplement` / `-Scope pressure` 两组** —— `-Scope all` 不含（S-5），本轮我没跑。
5. **P-1 的"加重版"**（确认框点名 A/B、实际覆盖 C/D）—— 我用代码把路径确认到底了
   （`FindLibraryEntry` 用索引名 vs `InstallToLibrary` 用档案 META），但**没有**构造"两个本地槽位 + 弹确认框 + 点是"
   的现场实跑；我实跑的是"新增"那一半。
6. **`%2f` 在真实静态服务器上的行为** —— 我只在脚手架自带的夹具服务（.NET `UnescapeDataString`）上实测；
   不同服务器的 `%2f` 处理不一致（nginx 一般不解码），**我没有**在真实 GitHub raw / nginx 上验证。
7. **`WINDOWS` 保留设备名的完整清单**（`COM1-9`/`LPT1-9`/`AUX`/`PRN`/带扩展名与尾随空格的各种写法）
   —— 我只实测了 `CON` / `NUL` 两个。

---

## 8. 顺带报备：本地夹具的自签证书（任务点名要求确认作用域）

- 位置：`Cert:\LocalMachine\Root` 与 `Cert:\LocalMachine\My` **各一张**，Subject `CN=YuaExtract Harness Fixture`，
  指纹 **`DB3141E97F5416A323EB1DA1D498AEB5D8FF9D86`**，`NotAfter = 2031-10-15 23:41:38`，
  `My` 那张 `HasPrivateKey = True`。（与任务里报的 `DB3141E9…`、2031-10-15 一致。）
- 扩展（我读了 RawData）：SAN 原始字节 `30 11 82 09 6C6F63616C686F7374 87 04 7F000001`
  = **`DNS=localhost` + `IP=127.0.0.1`，仅此二者**；EKU 只有 **Server Authentication**；
  **没有 basicConstraints**（既没有 `CA:TRUE` 也没有 `CA:FALSE`）。
- **作用域结论（要点）**：这张证书**自己**只能给 `localhost` / `127.0.0.1` 用（SAN 决定的），
  但它装在 **机器级 Root**（`LocalMachine`，不是 `CurrentUser`），私钥也在机器级 `My` 上，
  而且没有 basicConstraints —— 作为**信任锚**，SAN 只约束它自己、**不约束"用它的私钥签出来的叶证书"**。
  也就是说：**它的信任作用域不是"只影响 localhost"**，而是"本机所有走 Windows 证书库的 TLS 客户端
  （WinHTTP / .NET / Edge / Chrome）都把它当根"。
  （"能用它签出任意主机名且被信任"这一句是**按 X.509 链构建规则推断**的，我**没有**实际签一张叶证书去验证。）
- 现实影响：不构成新增提权（本机管理员本来就能改机器证书库），但"在一台机器上长期留一张自签信任根"
  本身就有安全含义 —— 收尾时应当卸掉。
- 卸载手段：`lib\serve-fixtures.ps1 -UninstallCertificate`（我读了代码：会同时遍历 `Root` 与 `My` 两个机器库、
  按 Subject 删）。**我没有执行卸载**，因为复核过程中还要用它跑 https 用例。

---

## 9. 总结论

| 侧 | 结论 | 主要理由 |
|---|---|---|
| **产品侧** | **通过（带 1 条中等缺陷 + 4 条低/信息项）** | 声明的功能都真做到了：页签骨架/可见性/像素、刷新/详情/打开/留言/自检、在线更新闭环与幂等、11+3 条坏输入全部按各自理由被拒且本地逐字节不动、https 不降级、不后台联网、路径不穿越。**但 P-1（索引名与档案 META 名不一致 → 界面说一套、落库落另一套，且确认框可能问错对象）是一条真实的中等缺陷**，设计文档 §14.4 的 8 条里没有这一条，建议在下一轮补第 9 条并要求 `VerifyArchive` 校验二者一致。P-2 的措辞/回显问题仍未修（脚手架 README 已披露但未拦断言）。 |
| **脚手架侧** | **通过（带 1 条已被我打出的真·弱判据 + 1 条崩溃缺陷）** | `run-all.ps1 -Scope all` 自跑 `通过 141 / 失败 0 / 提示 0`、exit 0，且 `stale = 0`（全是本轮新写）；E-12..E-19 的判据绝大多数是真的（期望值独立算、本地逐字节、暂存为空、含多条反退化护栏），55 个自检场景的扰动也确实是真的扰动。**但**：(a) `E-19/online-http-base` 是弱判据，我实测用一个 `ftp://` 基址就能在"它自称验的措辞没出现"的情况下通过（S-1）；(b) 库目录不存在时驱动会**崩溃**而不是失败，直接导致"首次安装 + 在线更新建库"那一路测不到（S-2）；(c) 「关于」页完全无覆盖（S-3）；(d) 141 里有 63 项是自检、`-Scope all` 不含 supplement/pressure（S-4/S-5）。 |

**一句话**：这轮的"绿"在**产品行为层面是真的**、产物来源也**已用逐字节重编钉死**；
但它**不是全绿**——`E-19` 那一条断言在它自述要验的措辞根本没出现的情况下也会打印"措辞与用时都对"，
而它掩盖的正是 P-2 那个"用户看不见自己填的地址"的毛病。按任务的要求，这就是本轮"『全绿』里藏的假通过"。

---
---

# 修复轮增量复核（2026-10-16）

> **被复核的指纹从 `FAEB6760` 变为 `71BF4A17`**。上一节（第一轮复核）的正文**一字未改**，本节只追加。
> 复核者仍是同一个人：本轮**没有参与任何实现**，下面每个数字都是我自己跑出来的；引用别人的数字都注明来源。
> 全程**没有改动** `CxdecExtractorLoader\**`、`docs\**` 以及 `_verify\restore-harness\` 下任何既有脚本。

## A0. 本轮对象 / 环境 / 命令

| 项 | 值（我自己核的） |
|---|---|
| 被测产物 | `Release\YuaExtract.exe` = **1,182,720 B**，sha256 `71BF4A1738182F47F452A90E41E1E70B247BEE07D16F26F5821F60590C20E5EE`（短指纹 `71BF4A17`），mtime 2026-10-16 03:43:49 —— 与 lead 报的一致 |
| 复核后复查 | 仍是 `71BF4A17` / 1,182,720 B / mtime 未变（我全程只拷不写） |
| 源码新鲜度 | `check-source-newer.ps1`（我的脚本）比较仓库里 **471** 个源文件（排除 Release/Debug/obj/x64）→ **没有一个比产物新** |
| 机器 | Windows 11 Pro 10.0.26200 / x86 / Administrator / pwsh 7 / MSBuild 17.14.60 |

```powershell
pwsh -NoProfile -File '_verify\restore-harness\run-all.ps1' -Scope all          # 我自己重跑 → exit 0
MSBuild.exe CxdecExtractorLoader\CxdecExtractorLoader.vcxproj -p:Configuration=Release -p:Platform=Win32 `
  "-p:OutDir=...\Run\verify-task5\Out2\" "-p:IntDir=...\Run\verify-task5\Int2\" "-p:SolutionDir=<repo>"
python -I mkcase.py .\base-template.rules.int .\forged2 .\repo2                  # 本轮新增：名字擦边 / 设备名夹具
pwsh -NoProfile -File run-case.ps1 -Case <case> [-RepoDir repo2] [-KeepAppDir] [-SkipLibraryReset] `
  [-NoLibraryPreCreate] [-BlockDirectoryWithFile AB] [-DenyLibraryWrite] [-EndpointOverride <url>]
pwsh -NoProfile -File probe-e18.ps1          # 用**真实的** assert-extension.ps1 判"被拒的理由对不对"
pwsh -NoProfile -File break-manifest.ps1     # 手工改坏清单 → 三处是否都报
pwsh -NoProfile -File inspect-library.ps1 -Library App\ExtensionLibrary   # 查半截目录（\\?\ 前缀）
```

**我自己重跑的汇总**（不是引用 lead 的）：

```
断言 A（步骤3 default）      [失败 0 / 提示 0 / 共 21]
断言 B（步骤3 keep）         [失败 0 / 提示 0 / 共 10]
断言 C + E（步骤2 只收集）    [失败 0 / 提示 0 / 共 14]
脚手架自检（断言脚本鉴别力）  [失败 0 / 提示 0 / 共 8]
扩展集一期断言 E-1..E-19     [失败 0 / 提示 0 / 共 35]
扩展集断言自检（鉴别力）      [失败 0 / 提示 0 / 共 63]
总计：通过 151 / 失败 0 / 提示 0
      其中：产品断言 通过 80 / 失败 0；脚手架自检 通过 71 / 失败 0
本轮新写的断言组 6 个；被判为'上一次残留'而跳过的 0 个；文件不存在 3 个
结论：全部通过。            exit = 0
```

与 lead 报的 `151 / 0 / 0（产品 80 / 自检 71）`**逐项一致**。`文件不存在 3 个` 与第一轮同因
（`-Scope all` 不含 supplement / pressure 两组，S-5 的处理见下）。

## A1. 上一轮每一条的结案（逐条，不合并）

| 编号 | 严重度 | 结论 | 我的判定依据（本轮实测） |
|---|---|---|---|
| **P-1** 索引名 ≠ 档案 META 名 | 中 | **CLOSED** | 5 组"名字擦边"输入全部按预期：全角 `Ｙｕａ…`、西里尔 `О(U+041E)`、无点 `ı(U+0131)`、META 带尾随空格 → 全部**被拒且点名两边**；第一轮那个 `IndexSaysThis/AndThis` vs `VictimPublisher/VictimTitle` 的现场 → 现在被拒。见 A2。 |
| **P-2** 非 https 基址措辞/回显 | 低-中 | **部分关闭** | 措辞与回显这一半 **CLOSED**：`基址不可用：只允许 https:// 开头的地址，你填的是 "http://127.0.0.1:63633/" —— 它不是 https 的地址。` + `出处：环境变量 YUAEXT_ENDPOINT`；`地址也不能为空` 这句已从产品里消失（只剩注释）。**UI 那半仍开着**：弹窗 4140 仍是空串（实测 `endpointShown = ''`），见 A3/N-P2。 |
| **P-3** 清单「来源」指向必被删的暂存文件 | 低 | **CLOSED** | 在线：`来源 = https://127.0.0.1:59743/AB/T/rules.int`（远端 URL）；本地制作：`来源 = …\ExtensionFixture\OldGame\ExtensionDraft\rules.int`（**行为未变**）。两条路都实测过。 |
| **P-4** 不显示 sanitize 后的实际目录 | 低 | **CLOSED** | 状态文字里多了 `实际落库目录：…\ExtensionLibrary\YuaHarnessOnline\OnlineWork`。 |
| **P-5** `ERROR_ALREADY_EXISTS` 一律当成功 | 低 | **部分关闭** | 诊断这一半 **CLOSED**：`CON/NUL`、同名**文件**挡路、库根 Deny-Write 三种都**明确报错**（代码 183 / 3 / 5），设备名那句提示也到位。**但会留下半截空目录**（N-P1），且"本地库未被改动"与事实不符。 |
| **P-6** 页签布局 `\|\| true` | 信息 | **维持（无缺陷）** | 上一轮已按代码+实测确认等价；本轮 E-12/E-12-b 全绿、四路页签按钮在每一路采样里都可见，无回归。 |
| **S-1** E-19 http 弱判据 | 中 | **CLOSED** | 新判据是结构化的：**回显我们传进去的原址** + 点到 https 形状 + `Forbid` 网络类原因。我拿 4 种基址实测（http / ftp / `https://` / `httpcs://`）见 A4。残留两处小问题（死判据 / `https://` 的解释与事实相反）记在 N-S3 / N-P2。 |
| **S-2** 库目录不存在时驱动崩溃 | 中 | **CLOSED** | `-NoLibraryPreCreate`（库目录**压根不建**）跑完 setupdate：驱动退出码 0、stderr **空**、无 `driverFailure`；产品把库建出来、装进去、清单 1 行且 `来源` 是远端 URL。脚手架侧另有 `online-first-install` 用例（把库整个挪开）。 |
| **S-3** 「关于」页零覆盖 | 低 | **CLOSED** | 新增 `E-12-b`，判据比我建议的强：它比对的是**这次跑的那个 exe** 的指纹与字节数（driver 记 `appFingerprint`/`appExeSize`，断言要求它们在页面正文里出现）+ 可见性矩阵（只 4001-4004/4071/4072 可见）。本轮实测 `关于页签可见 6 个：4001 4002 4003 4004 4071 4072 / 正文 1062 字，含构建指纹 71BF4A1738182F47 与免责声明`。另有 3 条新自检（缺指纹 / 缺分节 / 泄漏 set 控件）。 |
| **S-4** 141 的构成没分开报 | 低 | **CLOSED** | 汇总现在多打一行 `其中：产品断言 通过 80 / 失败 0；脚手架自检 通过 71 / 失败 0`。 |
| **S-5** `-Scope all` 不含 supplement/pressure | 低 | **维持（部分改善）** | `文件不存在 3 个` 与第一轮相同、仍由 scope 决定；"all 不等于全部"这一点没变（只是汇总多了一行拆分）。建议要么改 scope 名，要么在那一行旁注"这两组只有 `-Scope supplement` / `-Scope pressure` 才跑"。 |
| **S-6** README 历史标定行过时 | 低 | **CLOSED（按"不改写历史"处理）** | `evidence/tab-pixels-prefix/`（README.txt sha256 前 16 位 `adcef38cbe21c3c5`、7 张 PNG 全是 2026-10-14 的时间戳）**一字未动**；主 README 的 L248/297/312/317 历史行也未被改写，反而多了 L645 一句「上面那些 `6B664840` / `492522BD` / `FAEB6760` 是**历史取证与标定依据**，一律保留不改」——正是我建议的处理方式。 |
| **S-7** 指纹只记录不比对 | 信息 | **CLOSED（被 E-12-b 顺带补上）** | 现在"这轮绿属于哪个构建"在**页面上被断言**（见 S-3 那条），不再只靠读 checks json。 |
| **S-8** 我自己踩了 README 踩坑 #52 | 信息 | 维持 | 我这轮的 `mkcase.py` 没再犯；记录保留。 |

## A2. 重点：`salt-mismatch` 与 `index-name-mismatch` 是不是"因为正确的原因"

### ① `salt-mismatch`：拒它的确实是内嵌 exe 摘要那条，不是名字那条

我读到的**产品原文**（`Run\out\extension-online-salt-mismatch.json`，本轮新跑）：

```
下载 / 校验失败：
档案内嵌的目标 exe 摘要与索引不一致：这一份不是索引里那一条描述的东西。
档案里是：84F17BA8D563DC53C04971F60840009612B8700FCEC4558F36832ED6793FC783
索引写的是：69B031A2F4BF79FABB5AEE1638A094CAFAF4560768754C1D757020FBB808AC7D

本地库未被改动（档案没写进库）。
```

我逐词核过断言里 `Forbid` 的那三个措辞：`索引说` = **False**、`META 说` = **False**、
`会社 / 作品与档案` = **False**；而该用例要求的 `目标 exe 摘要与索引不一致` = **True**。
→ **结论：这条用例确实走在"内嵌 exe 摘要"那条防线上，名字校验没有抢答。**（这一点也印证了改夹具的必要性：
若索引名仍用主档案那套，新的名字校验会先一步拦下，这条用例就变成"因为错误的原因通过"。）

### ② `index-name-mismatch`：措辞确实同时点了两边

```
下载 / 校验失败：
索引与档案不是同一条，已丢弃：两边的会社 / 作品对不上。
索引说：IndexSaysThis / AndThis
档案自己的 META 说：YuaHarnessOnline / OnlineWork
索引是信任根，装进库里放哪个目录必须可预测 —— 不一致意味着索引写错了，或者这个位置上的档案被换成了别的东西，本工具不装。
```

索引那行（断言自己解析仓库 `index.txt` 得到 `IndexSaysThis` / `AndThis`）与档案 META（断言自己
用 `rules-int.py` dump 得到 `YuaHarnessOnline` / `OnlineWork`）**四个值全部出现在错误文字里**，
且 `目标 exe 摘要与索引不一致` = False（不是被摘要那条拦下的）。→ **因为正确的原因。**

### ③"两条防线同时会被触发"的输入 —— 我用**真实的断言脚本**判过

我造了一条 `both-defects`（索引名错**且** `目标exe摘要` 也错），实跑得到的是**摘要那条**的措辞。
然后把 `Run\out\extension-online-summary.json` **拷一份**，只把 `online-index-name-mismatch` 那一条的
`RunJson`/`ServerRoot` 换成这条实跑记录，再拿真 `assert-extension.ps1` 跑（`probe-e18.ps1`）：

| 探针 | 期望 | 实测 `E-18.Ok` | 说明 |
|---|---|---|---|
| `baseline`（原样汇总） | 绿 | **True** | 对照组：证明"替换法"本身不破坏断言 |
| `both-defects-real` | **红** | **False** | 断言打印：`没有被按预期理由拒（要出现 [索引说/META 说/会社/作品]）` + `被拒的理由**没有同时点名两边**（索引名…命中 True/True；档案 META 名 VictimPublisher/VictimTitle 命中 False/False）` |
| `meta-only-synthetic`（我合成的"只点档案那一边"的假措辞） | 红 | **True** ← **假通过** | 见 N-S2 |
| `salt-forbid-synthetic`（给 salt 那条**追加**一句名字不一致的措辞） | 红 | **False** | 断言打印 `被拒的理由里出现了**别的防线**的措辞「索引说」/「META 说」` → **Forbid 不是摆设** |

→ **两条新用例都"因为正确的原因"通过**；`Forbid` 也确实会响。
但 `meta-only-synthetic` 暴露了 `NameMismatch` 判据的一个弱点（**N-S2**）：它并没有真的验"索引那一边被点名"。

## A3. 重点：不匹配的档案是不是在**覆盖确认框之前**就被拒（行为证明，不是看代码位置）

我先把 `IndexSaysThis / AndThis` 这个槽位**真的装进本地库**（`seed-ok`，自洽档案、安装成功），
再用**同名槽位、摘要不同**的 `regress-name-mismatch`（名字错）去打它：

| 证据 | 我读到的东西 |
|---|---|
| 产品的分类（列表行） | `IndexSaysThis / AndThis / 2 条 / **可更新（档案摘要不同，且条数不少于本地）**` ← 这个状态按设计**必须**先弹覆盖确认框 |
| 弹窗记录 | **没有任何确认框**（只有一个 `(None,'')` 占位，是驱动"没找到弹窗"写下的） |
| 安装后状态 | `下载 / 校验失败：索引与档案不是同一条，已丢弃：两边的会社 / 作品对不上。…` |
| 本地库 | 清单与档案**逐字节未变**（还是 `seed-ok` 那一行；`toolsDigest`/清单文本前后相同） |

→ **行为证明：不匹配的档案在"该弹确认框"的状态下就已经被拒了，确认框根本没出现。**
同理，`online-salt-mismatch` 的分类是 `本地更全（条数比远端多）—— 覆盖会减少覆盖度，默认不覆盖`
（同样必须确认），而它也是**零确认框**、直接以校验失败收场。

## A4. 重点：新 E-19 判据的判别力（我自己喂 4 种基址）

同一套判据（原址回显 + https 形状 + 禁网络类措辞）喂给四种基址，我实跑的产品文字**逐字**如下（都只有 `请求基址/当前生效基址：（无：配置里的地址没通过 https 校验）` 那一行不同）：

| 我传的 `YUAEXT_ENDPOINT` | 原址回显 | 形状命中 | 禁词 | 判据结论 | 我的评价 |
|---|---|---|---|---|---|
| `http://127.0.0.1:9/x/` | ✔ | 3 条 | 无 | 通过 | **正确**（这就是它要考的） |
| `ftp://127.0.0.1:21/pub/` | ✔ | 3 条 | 无 | 通过 | **也是正确的**：§14.4 第 1 条要求的是"仅 https"，ftp 确实不是 https；而且判据要求"回显它拒的那个地址"，所以它不再是"只要形状像就绿" |
| `https://`（非空、看着像 https、但没有主机名） | ✔ | 3 条 | 无 | 通过 | **通过是对的，但产品那句解释是错的** → N-P2 |
| `httpcs://127.0.0.1:9/x/` | ✔ | 3 条 | 无 | 通过 | **正确**（`httpcs://` 确实不是 https） |

四种的用时都是 0.8 秒、本地零新增文件、暂存目录为空 → 全部在**发请求之前**就拦住了。
**判别力结论**：判据已经不再是"只要是某个形状就绿" —— 它把"**拒的是这个地址**"作为必要条件，
第一轮那种"地址被折成空串也能过"的现场**已经堵死**（我实测第一轮的证据形态：
`基址不可用：""。` + `地址也不能为空` 在这个构建里**不再产生**）。

## A5. 重点：P-1 的绕过尝试（我构造输入，逐条问产品）

| 我构造的输入（索引名 vs 档案 META 名） | 目的 | 产品行为 | 绕过？ |
|---|---|---|---|
| `YuaHarnessOnline` vs `yuaharnessonline` | `_wcsicmp` 只折大小写，而目录名保留大小写 | **接受**，落进 `ExtensionLibrary\yuaharnessonline\onlinework\` | **否**：NTFS 默认大小写不敏感，这就是同一个目录；`EnumerateLibrary` 也按 META 名 sanitize 拼路径，仍然自洽。残留仅是"显示名与目录名大小写不同"（N-P3） |
| `ＹｕａＨａｒｎｅｓｓＯｎｌｉｎｅ` vs `YuaHarnessOnline` | 全角兼容字符会不会被折成 ASCII | **拒**（点名两边） | 否 |
| `ОnlineWork`（西里尔 O U+041E） vs `OnlineWork` | 同形异码 | **拒** | 否 |
| `Onlınework`（无点 ı U+0131） vs `OnlineWork` | 同上 | **拒** | 否 |
| META `YuaHarnessOnline `（尾随空格） vs 索引不带 | 空白差异 | **拒** | 否（比 `_wcsicmp` 更严，这是好事） |
| 索引 `C:\absolute` / `\\server\share` / `..\..\Windows\Temp` / `a:b` / 前后空格 | 敌对路径串 | 前四个 sanitize 后落在**库内**（`Cabsolute\T`、`servershare\T`、`WindowsTemp\T`、`ab\cd`），整沙箱**库外零新增** | 否 |
| 两条**不同**的索引名 sanitize 后落到同一目录：先装 `A:B/T`（→`AB\T`）再装 `AB/T`（→`AB\T`） | 槽位冲突 | 库清单**恰好 1 行**（upsert 按 sanitize 后的名字判同槽，旧的 `A:B` 行被替换），目录只有 `AB\T` 一份 | 否（且这正是期望行为） |
| `CON` / `NUL` | 保留设备名 | **拒**（Win32 183 + 设备名提示），但留下空目录 `CON` | 否（残留见 N-P1） |

→ **P-1 的交叉校验没有被绕过**；`_wcsicmp` 的唯一松动（大小写）不产生"界面说 A、磁盘落 B"的实质后果。

## A6. 重点：P-5 的复核逻辑（我自己造四种）

| 我造的局面 | 产品原文 | 有没有留下半截东西 |
|---|---|---|
| `CON` / `NUL` 当会社/作品 | `安装失败：建不出库目录：…\ExtensionLibrary\CON\NUL（Win32 错误码 183）` + `这个名字不能当目录用：它可能占用了 Windows 的保留设备名（CON / PRN / AUX / NUL / COM1-9 / LPT1-9），或者位置上一份同名文件挡着。把会社 / 作品改个名字再试。` | **有**：`ExtensionLibrary\CON` 这个空目录被建出来了（见 N-P1） |
| 用一个**文件** `AB` 挡住 `<库>\AB` | `安装失败：建不出库目录：…\ExtensionLibrary\AB\T（Win32 错误码 3）` | **没有**（挡路文件是我预置的，前后条目一致） |
| 库根加 Deny-Write ACL（Administrator） | `安装失败：写库里的档案失败：…\ExtensionLibrary\AB\T\rules.int（Win32 错误码 5）` | **有**：`AB` 与 `AB\T` 两个空目录被建出来了（报错的代码 5 是对的，但目录已经建了） |
| 库目录压根不存在 | 成功：`已新增：…`（见 S-2） | 无 |

三种失败**都明确报错、都点名了具体路径**（P-5 诊断半边确实修好了）；但**两种会留下空目录**，而
同一句结论里写着「本地库未被改动（档案没写进库）」—— 见 **N-P1**。
另外"文件挡路"这一路拿到的是代码 **3**（`ERROR_PATH_NOT_FOUND`），所以设备名/挡路那句提示**没被打印**
（提示只挂在代码 183 上）——措辞只是不够贴，不影响判定。

## A7. 本轮新发现

### 产品侧

#### N-P1（低）失败路径会留下**空目录**，而结论同时写着"本地库未被改动"

- **现象**：`InstallToLibrary` 先建目录再写文件。目录建到一半失败、或目录建好了但写文件失败时，
  已经建出来的那一级/两级**留在库里**；而状态文字统一带一句
  `本地库未被改动（档案没写进库）。`（"没写进库"对，"本地库未被改动"与事实不符）。
- **复现（两种，互相独立）**：
  1. `run-case.ps1 -Case reserved-con -RepoDir repo2` → 库根多出一个空目录 `CON`；
     `inspect-library.ps1` 用 `\\?\` 前缀核实：`DirExists(…\ExtensionLibrary\CON)=True`、创建时间 `05:16:52`（正是该用例跑的时刻）、
     里面 0 个条目；`CON\NUL` 不存在。
  2. `run-case.ps1 -Case collision-plain -RepoDir repo2 -DenyLibraryWrite` → 库根多出 `AB` 与 `AB\T` 两个空目录。
- **判定依据**：我这条是把清单改成**含目录**之后才看见的（新增条目 `D|App\ExtensionLibrary\CON|0` /
  `D|…\AB|0`、`D|…\AB\T|0`）——**只列文件的前后对比是查不出这件事的**（我第一轮的清单就是只列文件）。
- **严重度**：低。目录在库内、内容是空、`Remove-Item -LiteralPath …\CON -Recurse -Force` 实测**删得掉**，
  不构成安全或可用性问题；问题在"结论说没改、实际改了"这句不准确。
- **建议修法**：要么在失败清理里回滚刚建出来的目录（记住第一个成功创建的层级，失败时逐级删到它为止），
  要么把那句结论改成实情（`档案没写进库；库目录可能已新建，空着不影响使用`）。

#### N-P2（低）三态措辞的第 ② 态对"看着像 https 但没有主机名"的地址给出**与事实相反**的解释

- **现象**：基址 `https://`（非空、确实以 `https://` 开头，只是没有主机名）会落到第 ② 态，于是报
  `基址不可用：只允许 https:// 开头的地址，你填的是 "https://" —— 它不是 https 的地址。`
  —— 对 `https://` 而言"它不是 https 的地址"是**错的**（它恰恰是）。地址回显本身是对的。
- **复现**：`run-case.ps1 -Case control-good -EndpointOverride 'https://'`（我实跑，4 种基址之一）。
- **判定依据**：上面的原文；同一次里 `httpcs://…` 拿到同样的话，那句是**对**的 —— 说明两种输入被合进了同一个状态。
- **严重度**：低（措辞误导，不影响安全性：`https://` 本来也该被拒）。
- **建议修法**：把第 ② 态再拆一条"看着是 https 但缺少主机名"（`https://` / `https:///x`），或者直接复用
  `ParseHttpsUrl` 里那句更准的措辞（`StartsWithHttps` 已经区分了 `size<=8` 与 `url[8]=='/'` 两种情况）。
- **顺带**：这条也说明新 E-19 判据仍有"形状对了但话不对"的缝（见 A4 表），只是缝里现在是产品措辞的问题，不是判据能判的。

#### N-P3（信息）`_wcsicmp` 放行"只差大小写"，落库目录用的是 META 那套大小写

实测：索引 `YuaHarnessOnline` / META `yuaharnessonline` → 通过校验，状态文字写 `已新增：YuaHarnessOnline / OnlineWork`，
磁盘上是 `ExtensionLibrary\yuaharnessonline\onlinework\`。NTFS 默认大小写不敏感 → 同一个目录，
`EnumerateLibrary` 也按 META 名 sanitize 拼路径 → 自洽。**不构成缺陷**，只是"索引名 = 落库名"不字面成立。
若哪天要字面相等，把比较改成 `CompareStringOrdinal(..., TRUE)` 之外再加一步"大小写也相同"，或反过来统一按 META 名回写清单。

#### N-P4（信息）META 层是**转义**存储、索引/清单 TSV 层**不转义** —— 手写索引里名字含 `\` 时要以转义形态写

`ExtensionSet.cpp` 的 `EscapeValue`/`UnescapeValue` 会把 `\`→`\\`、`\n`、`\r`，而且**只作用在 META 读取那条路**
（`ForEachKeyValue`）；`ParseManifestLine` 拿到的索引字段是**逐字**的。所以对**产品自己生成**的名字两边永远一致
（生成时转义、读回时还原；清单侧原样写原样读）；但**手写 / 第三方**的索引行若要和档案 META 对齐，
名字里的 `\` 必须写成双写形态。

> **同时纠正我第一轮的一处夹具问题**（不是产品缺陷）：第一轮我的 `repo\meta-unc` 夹具，索引行写的是
> **逐字**的 `\\server\share`，而档案 META 里也写了**未转义**的 `\\server\share` ——
> 产品读 META 时把 `\\` 折成一个 `\`，于是两边变成 `\\server\share` vs `\server\share`。
> 本轮这条用例因此被新的名字校验拒掉，**这是正确行为**，不是"误拒"。
> （第一轮的 `meta-dotdot-backslash` 之所以仍然通过，是因为 `..\..\` 里每个 `\` 后面不是 `\`/`n`/`r`，不会被折。）

#### N-P5（信息）「来源」列的 URL 与落库目录的耦合依赖 P-1

`YuaExtractMainWindow.cpp:5158` 传的 `sourceLabel` 是 `Online::ArchiveUrl(job.Base, remote.publisher, remote.title)`
（**索引名** sanitize 后拼），落库目录是 `InstallToLibrary` 按**档案 META 名**算的。现在两者一致，**纯粹因为 P-1 那条新校验**。
建议在 `VerifyArchive` 那一段注释里点明"这里的相等是 `sourceLabel` 可用的前提"，
免得将来有人为了兼容性放宽 P-1 时，来源列与目录悄悄分叉。

### 脚手架侧

#### N-S1（低）"本地一个字节都不变 / 自检是只读"的判据只看**文件**，对"新建空目录"是盲的

`lib\HarnessUtil.ps1:628` 的 `Get-TreeDigestExcluding` 用的是 `Get-ChildItem … -Recurse **-File**`，
所以 E-16 的"自检前后树摘要逐字节相同"、E-18/E-19 的"前后库摘要相等"、S-2 新加的 `online-first-install`
对"产品在库里新建了一个空目录"**一律看不见**。本轮的两处残留（`ExtensionLibrary\CON`、
Deny-Write 下的 `AB` + `AB\T`）用脚手架原口径都查不出来 —— 我是把**自己的**清单改成含目录之后才发现的。
建议：把目录条目也算进摘要（`{kind}|{rel}|{size}`），或在"只读"类判据里额外断言"目录数不变"。
（成本很低：目录项通常远少于文件项。）

#### N-S2（低）E-18 的 `NameMismatch` 判据里，"索引那一边"是**自动满足**的（假通过，我已复现）

- **判据的样子**（`assert-extension.ps1:2852-2856`）：在 `$evidence` 里分别找
  `索引会社`、`索引作品`、`档案 META 会社`、`档案 META 作品` 四个子串，四个都在才算"两边都点了名"。
- **问题**：`$evidence` 来自 `Get-OnlineCaseText`，里面包含 `listItemsAfterRefresh` ——
  而列表行就是 `<索引会社> / <索引作品> / N 条 / <状态>`（**索引名必然在里面**）。
  所以 `needIdxPub`/`needIdxTitle` 恒为 True，判据实际退化成"档案 META 的两个名字出现在文字里"。
- **我的复现**（`probe-e18.ps1` 的 `meta-only-synthetic`，**用真实断言脚本**跑的）：
  把该用例的状态文字换成
  `下载 / 校验失败：\r\n档案自述的会社 / 作品是 YuaHarnessOnline / OnlineWork，索引那一条用不了。`
  （**只点档案那一边**，完全不点索引那一边，ServerRoot 保持不动）→ **`E-18.Ok = True`**，
  并且断言打印了 `按"索引名 ≠ 档案 META 名"被拒，且两边都点了名（索引 IndexSaysThis/AndThis vs 档案 YuaHarnessOnline/OnlineWork）`。
- **为什么仍然值得保留这条用例**：`both-defects-real` 探针证明它在"被别的防线拒了"时会红（`E-18.Ok = False`）——
  也就是说它并不是完全无效；问题是"**同时点名两边**"这个声明**比实际做的事强**。
- **建议修法**：把 NameMismatch 的证据范围收窄到**状态文字 + 弹窗**（不含远端列表行），
  或者要求出现形如 `索引说：<索引会社> / <索引作品>` 的**成对**子串（而不是两个孤立的名字）。

#### N-S3（信息）E-19 的 `GenericShape = '地址也不能为空'` 已经是**死判据**

产品里那句"地址也不能为空"只剩两处**注释**（`OnlineUpdate.cpp:871`、`YuaExtractMainWindow.cpp:4738`），
运行时不再产生（4 种基址实测 `地址也不能为空` 全为 False）。所以断言里那段
`只命中了泛化兜底措辞…（S-1 的假绿现场）` 的提示**永远不会打出来**。
建议把那句 `GenericShape` 换成产品现在的兜底措辞（例如 `基址不可用：` 后面**没有引用任何地址**这种**形态**判断），
或者直接删掉 `GenericShape` 分支免得读者以为它还在守着一道门。

#### N-S4（信息）`salt-mismatch` 的用例名/描述已经与它实际考的东西不一致

它现在只错 `目标exe摘要` 一列（名字、档案摘要、条数全部正确）—— 考的是 §14.4 第 4 条，
但名字仍叫 `盐不一致`、描述仍写"位置上摆的是别的游戏的档案（别的盐）"。判据本身没问题（我按 A2① 核过原因），
只是名字会让后来的人以为在考盐列。建议改名 `index-exe-digest-mismatch`（`make-online-repo.ps1` /
`run-extension-online.ps1` / `assert-extension.ps1` 三处的键名一起改）。

#### N-S5（信息，记账）脚手架自身的"本地未动"口径也受 N-S1 影响

E-16 的 `libraryDigestBefore/After` 与 E-18/E-19 的 `before/after.libraryDigest` 都走同一个函数，
所以它们对空目录盲；这跟 N-P1 叠起来就是"产品的空目录残留既没被产品承认、也没被脚手架抓到"。
这条与 N-S1 是同一件事的两个面，列在这里是为了把"为什么这一轮没红"说清楚。

## A8. 回归复核（第一轮验过的东西，这轮重跑）

| 回归项 | 本轮结果（我实跑） |
|---|---|
| 302 → 明文 http 重定向 | **仍然不跟随**：我的 `serve-redirect.ps1` 明文端口**零连接**；产品报 `服务器要求重定向（HTTP 302），本工具不跟随重定向，已中止。…对方指向：http://127.0.0.1:53771/index.txt`；本地零新增、暂存为空 |
| 绝不后台联网 | **仍然成立**：`YUAEXT_ENDPOINT` 指向活着的夹具、只跑 `settab` → 夹具日志**只有启动那一行**（整个会话零请求） |
| 路径穿越（7 例：`..\..\Windows\Temp`、`..%2f..`、`C:\absolute`、`\\server\share`、前后空格、`a:b`、240 字） | **仍然全部关在库里**：逐例对整沙箱做前后 diff → **库外零新增**；失败路径库未被写、暂存为空 |
| 手工改坏 `manifest.txt`（9 列坏行） | **三处都报**：面板 `清单提示：…第 3 行：列数不对（应有 10 列，实际 9 列）`；Warn 日志 `[YuaExtract] 扩展集清单提示：…`；自检弹窗 `运行环境自检：1 项问题`。库根/工具目录（除 Log）树摘要前后全等 |
| A/B/C/D 组 | 全绿（21 / 10 / 14，失败 0） |
| E-11 像素基线 | `evidence/tab-pixels-prefix/` **一字未动**（7 张 PNG + README 都是 2026-10-14）；E-11-a/b/d/e/g + E-11-C/E/F/T 全绿；自检 `e11-prefix-real-capture` 绿（基线**仍然有牙**）；主 README 历史标定行未改写 |
| E-12 系列 | E-12 / E-12-a / E-12-b / E-12-C/E/F/T 全绿；set 页签 set-1 = set-2 = 18 个控件 |
| 产物来源可信（新指纹） | 见 A9 |

## A9. 产物来源可信（对 `71BF4A17` 重做一遍）

```
MSBuild.exe CxdecExtractorLoader\CxdecExtractorLoader.vcxproj -p:Configuration=Release -p:Platform=Win32 `
  -p:OutDir=...\Run\verify-task5\Out2\ -p:IntDir=...\Run\verify-task5\Int2\ -p:SolutionDir=<repo>
→ 0 error / 0 warning；产物 ...\Out2\YuaExtract.exe
```

| | 大小 | sha256（前 16 位） |
|---|---|---|
| 冻结产物 `Release\YuaExtract.exe` | 1,182,720 | `71BF4A1738182F47…` |
| 我重编的 `Out2\YuaExtract.exe` | **1,182,720** | `D2738C0BCFCCBD9C…`（不同，但只差链接时间戳，见下） |

**逐字节 diff：整份 1,182,720 字节里只有 6 个字节不同**，落在 3 处、
每处都是**同一个 32 位链接时间戳**的不同字节：

| 偏移 | 字节数 | 落点 | 冻结 | 我的重编 |
|---|---|---|---|---|
| `0x00000110` | 2 | PE 头 `TimeDateStamp` | `F5 2C` | `F0 3F` |
| `0x000E7894` | 2 | `.rdata`（调试目录时间戳） | `F5 2C` | `F0 3F` |
| `0x000E78B0` | 2 | `.rdata`（同上） | `F5 2C` | `F0 3F` |

即 `0x6AD12CF5`（1792093429 = 2026-10-15 19:43:49Z）→ `0x6AD13FF0`（1792098288 = 2026-10-15 21:04:48Z），
差 4859 秒；第 3、第 4 字节（`D1 6A`）两边相同，所以只差 2 个字节 × 3 处。**`.text` 段零差异。**

**结论**：`71BF4A17` **确实是当前源码编出来的**，除链接时间戳外逐字节相同；配合"471 个源文件没有一个比产物新"，
不存在"源码比产物新"或"产物来路不明"。我的重编把 `IntDir` 也重定向了，事后复查 `CxdecExtractorLoader\**`
下没有任何文件被我的构建写过。

## A10. 本轮**没验到**的（不用推测填）

1. **`AutoProxy` / PAC 环境下的行为** —— 与第一轮相同，仍只看代码，没在设了 PAC 的机器上实测。
2. **`%2f` 在真实静态服务器（nginx / GitHub raw）上的解码行为** —— 仍只在脚手架夹具（.NET `UnescapeDataString`）上验过。
3. **`CON` 这类残留会不会在别的路径上变成"删不掉的死结"** —— 我实测 `Remove-Item -LiteralPath …\CON -Recurse -Force`
   **删得掉**（第一轮我删不掉的是"含 `CON\NUL` 的那棵树"，两者不同）；但没有穷举所有组合（例如 `CON\NUL` 真被建出来的情形）。
4. **`-Scope supplement` / `-Scope pressure` 两组** —— 本轮仍未跑（S-5 维持）。
5. **`salt-mismatch` 的新夹具在"本地库已有同槽位"时的分类** —— 我验的是 `本地更全` 那种形态（有确认框却不弹），
   没有把它的每一种分类（新增 / 可更新 / 本地更全）都覆盖一遍。
6. **`_wcsicmp` 在非 C locale 下的行为** —— 程序没调 `setlocale`，所以按 C locale 推断只折 ASCII；
   全角 / 西里尔 / 无点 i 我都用**行为**证明了会被拒，但没有直接调用 `_wcsicmp` 去列它的等价类。

## A11. 总结论（修复轮）

| 侧 | 结论 | 理由 |
|---|---|---|
| **产品侧** | **通过**（上一轮 6 条里 4 条 CLOSED、2 条部分关闭；本轮新发现 2 低 + 3 信息） | P-1 **真的修好了**（含 8 组绕过尝试与"确认框之前就被拒"的行为证明）；P-3/P-4 CLOSED；P-2 的措辞与回显 CLOSED（但弹窗那个框仍是空的）；P-5 的诊断 CLOSED 但**留下空目录**（N-P1）。没有发现新的安全类缺陷。 |
| **脚手架侧** | **通过**（上一轮 8 条里 6 条 CLOSED、2 条维持；本轮新发现 1 低 + 4 信息） | S-1/S-2/S-3 三条**都真的修好了**，而且都带着新的鉴别力自检（`e19-http-base-generic-word` / `online-first-install` / E-12-b 三条 + `e16-selfcheck-drops-item` 等）。**但** `NameMismatch` 判据的"索引那一边"是自动满足的 —— 我用**真实的断言脚本**让它接受了一段"只点档案那一边"的状态文字（N-S2，假通过已复现）；"本地未动"类判据对空目录是盲的（N-S1）。 |

**一句话**：这一轮把上一轮报的**根因**基本都治了（我自己重跑 151/0/0，产品 80 / 自检 71，产物来源逐字节钉死），
两条"新证据"也确实是**因为正确的原因**通过的（我按 A2/A3 逐条核过，包括构造"两条防线同时触发"的输入去喂真断言）；
但仍然留有两条**判据层面的缝**：一条是脚手架新加的 `NameMismatch` 只验了半边（N-S2，已复现假通过），
一条是"本地一个字节都不变"只看文件（N-S1），而产品侧正好有两次操作会在这条缝里留下空目录（N-P1）。


---
---

# 终版定向确认（`9995400F`，2026-10-16）

> **接替者**：task-5b。全新上下文、未参与任何实现。前一节（第一轮复核 + 修复轮增量复核）的正文**一字未改**，本节只追加。
> **被测产物**：`Release\YuaExtract.exe` = **1,188,352 B**，sha256 `9995400F7709515C…`（短指纹 `9995400F`），mtime 2026-10-16 05:39:16 —— 与 lead 报的一致。全程只拷贝、从不写它；跑完 UI 用例已 `Stop-Process`，夹具服务已停，无残留监听。
> 我自己的临时脚本 / 产物全部落在 `_verify\restore-harness\Run\verify-task5b\`（重编）与 `Run\verify-task5\out\`（用例复现，复用前任的 run-case.ps1 等脚本，**没有改任何既有脚手架脚本**）。
> 没有改动 `CxdecExtractorLoader\**`、`docs\**`、`evidence\**` 与任何既有脚本。

## B0. 本轮自跑的基线

```
pwsh -NoProfile -File run-all.ps1 -Scope all      （我自己跑，2026-10-16 08:0x–08:27）
总计：通过 157 / 失败 0 / 提示 0
      其中：产品断言 通过 82 / 失败 0；脚手架自检 通过 75 / 失败 0
结论：全部通过。            exit = 0
```

与 lead 报的 `157 / 0 / 0（82 / 75）`逐项一致；`文件不存在 3 个` 与前两轮同因（`-Scope all` 不含 supplement / pressure）。
67 个自检场景全绿，其中与本轮直接相关的四条：`e16-touches-empty-dir` / `e18-name-mismatch-meta-only` / `e14b-raw-join` / `e19-https-no-host-wrong-layer` 全部 PASS。

## B1. 逐条确认清单

### 1. N-P1（失败后留空目录 + 谎称本地库未被改动）→ **CLOSED**

全部现场由我用 `Run\verify-task5\run-case.ps1`（前任脚本，未改）独立重跑，被测版本 `9995400F`：

| 现场 | 产品错误原文（要点） | 磁盘事实（沙箱逐项 diff） | 错误文字里的结论与事实相符？ |
|---|---|---|---|
| 会社 `CON` / 作品 `NUL` | `建不出库目录：…\CON\NUL（Win32 错误码 183）` + 保留设备名提示 + **`注意：本次已经新建了库目录，但没能全部收回：…\CON\NUL 删不掉…本地库**不是**与操作前完全一致` | 库里多了空目录 `CON`（`D|App\ExtensionLibrary\CON|0`），`CON\NUL` 不存在 | **相符**（如实承认有残留，不再谎称"未被改动"） |
| `A:B` 撞 `AB` 同一目录（库根 Deny-Write） | `写库里的档案失败：…\AB\T\rules.int（Win32 错误码 5）` + **`本次没有改动本地库。`** | 库列表空、沙箱零新增/零消失（`AB`、`AB\T` 已被回滚收回） | **相符** |
| 同名**文件**挡路（`AB` 是文件） | `建不出库目录：…\AB\T（Win32 错误码 3）` + **`本次没有改动本地库。`** | 沙箱零 diff（挡路文件是我预置的，保持原样） | **相符** |
| **`rules.int` 已写进库之后才失败**（清单那步失败）——构造：先正常装一份 → 把 `manifest.txt` 重置为只剩表头并加 `Deny-WriteData` ACL → 再在线装第二份（新增路径，无确认框） | `写扩展集库清单失败：…\manifest.txt（Win32 错误码 5）` + **两条注意**：`…没能全部收回：…\yuaharnessonline\onlinework…` 与 **`这份 rules.int 已经写进库了（…），只是清单没更新 —— 本地库与操作前**不一致**。` | 库里确实有 `yuaharnessonline\onlinework\rules.int + info.txt`、清单 0 行 | **相符**（构造得出来，且说的正是"不一致"） |

另外读代码核对：`ExtensionSet.cpp` 的 `failInstall` 现在统一给**每条失败路径**追加"本地库状态"结论 —— 只删自己新建的空目录、删不掉或档案已写进去就如实说"不一致"，都收干净了才说「本次没有改动本地库」；UI 侧（`YuaExtractMainWindow.cpp:5167` 起）不再自己无条件下"本地库未被改动"。

### 2. N-P2（基址措辞各层各真）→ **CLOSED**

同一驱动、四种 `YUAEXT_ENDPOINT`，我逐字读回的产品状态文字：

| 输入 | 产品措辞（首两句） | 与任务要求的对应 |
|---|---|---|
| `https://` | `基址不完整："https://" 确实是 https 的地址，但缺少主机名，拼不出能用的 URL。` | **"基址不完整、缺少主机名"层** ✓（且不再说"它不是 https 的地址"） |
| `http://127.0.0.1:9/x/` | `基址不可用：只允许 https:// 开头的地址，你填的是 "http://127.0.0.1:9/x/" —— 它不是 https 的地址。` | **"不是 https"层** ✓，回显原址 |
| `ftp://127.0.0.1:21/pub/` | 同上（回显 `ftp://…`） | ✓ |
| `httpcs://x` | 同上（回显 `httpcs://x`） | ✓ |

四种全部约 0.8 秒失败（发请求之前）、本地零改动（沙箱零 diff、库空、暂存空）。四层措辞互不顶替；断言侧的 `e19-https-no-host-wrong-layer` 自检（② 的输入喂 ③ 的措辞必须炸）本轮也 PASS。
**残留（低，只记录）**：弹窗 4140「仓库地址」在这四种失败输入下仍显示空串（`endpointShown = ''`）——P-2 的 UI 回显那半边从增量复核起就一直开着，本轮实测**仍开着**，属 P-2 遗留，不是新发现。

### 3. N-S1（树摘要对空目录盲）→ **CLOSED**

- **没有靠"默认弱化"过关**：扫了全部脚手架脚本，`-FilesOnly` 全仓库只出现在 `assert-extension.ps1` 的 X-8 自证里（注释 + 两次显式调用）；`Get-TreeDigestExcluding` 本身默认**含目录**（`Digest` = 文件行 + `D:` 目录行），`FileDigest` 只是留作对照的旧口径。E-16 / E-18 / E-19 的"本地零改动"判据**默认**比的是 `Digest` + `Count` + **`DirCount`**（`assert-extension.ps1` 约 2507 / 2989 / 3163 行，实测详情里也印了"目录数"），没有任何一条"零改动"判据默认走只比文件的旧口径。
- **前任那个"文件全等但多一个空目录"的输入现在必须炸**：自检场景 `e16-touches-empty-dir`（合成"文件全等、只多一个空目录、摘要字符串故意相同、只有目录数变了"的记录）本轮 **PASS**（即 E-16 确实炸了）；函数级自证 X-8 也 PASS（`-FilesOnly` 对两棵树给同一摘要 = 旧口径盲区被复现，新口径摘要必须不同）。
- **标题与口径一致**：E-16 的标题现为「…自检不改任何文件**或目录**」（`assert-extension.ps1:2519`），与实际比对的口径一致。
- 产品行为侧闭环印证：本轮我自己重跑的 N-P1 现场（§B1.1）里，"Deny-Write 留 `AB\T`"那种残留已不再发生（回滚了），"CON 残留"那一种现在会被产品自己的错误文字如实承认 —— 脚手架（含目录维度）与产品（如实结论）两边口径对上了。

### 4. N-S2（NameMismatch 索引侧恒真）→ **CLOSED**

- **复现前任的 `meta-only-synthetic` 形状必须炸**：自检场景 `e18-name-mismatch-meta-only`（状态文字**只点名档案那一侧**）本轮 **PASS**（E-18 必须炸的判据成立）。本轮 run-all 里 E-18 的详情也打印了 `online-index-name-mismatch：按"索引名 ≠ 档案 META 名"被拒，且两边都点了名`，且来自真实产品输出（索引 `IndexSaysThis/AndThis` vs 档案 `YuaHarnessOnline/OnlineWork`）。
- **`Get-OnlineCaseText` 现在只取结论文字**（`assert-extension.ps1:1714`）：只拼 `statusAfterRefresh` / `statusAfterInstall` / 弹窗，注释明确写明不掺远端列表行、不掺累积日志；E-18 正例要列表内容时直接读 `listItemsAfterRefresh` 字段。
- **扫了一遍还有没有别的判据读"掺了远端列表行/累积日志"的文本**：`Get-OnlineCaseText` 的两处调用（E-18 坏用例、E-19）都只判结论文字；E-13/E-14 走字段级比对、E-15 走路径文本 + 本次日志尾部（`toolLogTails`）、E-16 已收窄到"由该项自己那一段报出来"、E-17 只读弹窗正文 —— 没有发现别的同类问题（与 README 第三轮"顺带扫过"的结论一致，且是我独立扫的）。

### 5. E-14b（改名条目的详情面板）→ **CLOSED**

本轮 run-all 的 `hostile-install` + `set-hostile` 现场（会社 `A:B` / 作品 `Yuzu/Soft`，按**文字**选中）：

- 面板「库内目录」= `…\ExtensionLibrary\AB\YuzuSoft`，与磁盘上真实存在的那一级**逐字符相同**（我另在磁盘上核了：`AB\YuzuSoft\rules.int + info.txt` 存在，raw 名 `A:B` 目录不存在）；
- 面板带说明「库里的目录名是改写过的：名字里的 \ / : * ? " < > | 之类字符被去掉了…」；
- 面板里**没有**出现 raw 名拼出的路径（`…\A:B\Yuzu\Soft` 不存在）。
- E-14-b 本轮 Ok=True，详情：`选中项：原始名「A:B / Yuzu/Soft」→ 落库名「AB / YuzuSoft」；面板的「库内目录」= 磁盘上真实存在的那一级`；自检 `e14b-raw-join`（面板退回 raw 拼接必须炸）PASS，且带"sanitize 前后必须真的不同，否则用例无效"的前置复核。

### 6. 产物来源可信（对 `9995400F` 重做）→ **CLOSED**

```
MSBuild.exe "CxdecExtractorLoader\CxdecExtractorLoader.vcxproj" -p:Configuration=Release -p:Platform=Win32
  -p:OutDir=...\Run\verify-task5b\Out\  -p:IntDir=...\Run\verify-task5b\Int\  -p:SolutionDir=<repo>\   → 0 error / 0 warning
```

| | 大小 | sha256（前 16 位） |
|---|---|---|
| 冻结产物 `Release\YuaExtract.exe` | 1,188,352 | `9995400F7709515C…` |
| 我重编的 `Out\YuaExtract.exe` | 1,188,352 | `FED5AF0D4195D8A0…`（不同，见下） |

逐字节 diff（沿用前任 `diff-pe.ps1`，未改）：**只有 6 个字节不同，3 段**，全是同一个链接时间戳的不同字节：

| 偏移 | 字节数 | 落点 | 冻结 | 我的重编 |
|---|---|---|---|---|
| `0x00000110` | 2 | PE 头 `TimeDateStamp` | `04 48` | `C9 6B` |
| `0x000E8D14` | 2 | `.rdata`（调试目录时间戳） | `04 48` | `C9 6B` |
| `0x000E8D30` | 2 | `.rdata`（同上） | `04 48` | `C9 6B` |

即 `0x6AD14804`（1792100356 = 2026-10-15 21:39:16Z，与产物 mtime 一致）→ `0x6AD16BC9`（1792109513 = 2026-10-16 00:11:53Z）；高 2 字节相同，所以每处只差 2 字节。**`.text` 段零差异**。构建前已 `Stop-Process YuaExtract`；`IntDir` 已重定向，事后复查 `CxdecExtractorLoader\**` 下没有任何源文件比产物新 —— 源码树未被我的构建污染。**结论：`9995400F` 确实是当前源码编出来的。**

### 7. 无回归 → **CLOSED**

| 项 | 本轮我实测 |
|---|---|
| A/B/C/D 组 | 我自跑 `run-all -Scope all` 全绿（21/10/14，失败 0）；总计 157/0/0、产品 82、自检 75，与 lead 一致 |
| E-11 基线 | `evidence/tab-pixels-prefix/` **一字未动**（8 个文件全是 2026-10-14 时间戳，README.txt sha256 前 16 位 `ADCEF38CBE21C3C5`）；E-11-a/b/C/d/e/F/T/g 本轮全绿；自检 `e11-prefix-real-capture` PASS（基线仍有牙）；E-11-e 详情记录本次构建指纹 `9995400F7709515C`；主 README 历史行（`6B664840`/`492522BD`/`FAEB6760`/`71BF4A17`）保持原样 |
| 302→http 不降级 | 我的 `serve-redirect.ps1`（前任脚本，未改）：https 侧回 302 → 产品报 `服务器要求重定向（HTTP 302），本工具不跟随重定向，已中止` 并印出对方要跳的 `http://…` 地址；**明文侧零命中**（HitFile 不存在）；暂存空 |
| 不后台联网 | `YUAEXT_ENDPOINT` 指向**活着的**本地夹具，只跑 `-Step settab`：夹具日志**只有启动那一行**，整个会话零请求 |
| 路径穿越 | 7 组敌对输入（`..\..\Windows\Temp`、`C:\absolute`、`\\server\share`、`a:b`、前后空格、240 字、`..%2f..`）：前 4 组落在**库内**（`WindowsTemp\T`、`Cabsolute\T`、`servershare\T`、`ab\cd`），后 3 组分别被"索引≠META 名 / 写临时档案失败 / HTTP 404"拒；**逐例沙箱前后 diff：库外零新增**，失败路径暂存空 |
| 手改坏 manifest | 我的手工坏行（9 列）→ **三处都报**：① 面板`清单提示：…第 1 行：列数不对（应有 10 列，实际 9 列）`；② Warn 日志 `[YuaExtract] 扩展集清单提示：…`（Level=W）；③ 自检弹窗 `运行环境自检：1 项问题`。产品零改动（我改的行我自己还原） |

## B2. 本轮新发现

### 产品侧

- **（低，只记录）弹窗 4140「仓库地址」对非 https 基址仍显示空串**：四种非法基址（`https://` / `http://` / `ftp://` / `httpcs://`）下 `endpointShown` 均为 `''`，错误文字里倒是逐字回显了原址。这是 P-2 增量复核时就已记录的"UI 那半边仍开着"，本轮实测**维持原状**；错误文字本身已能指导用户，不新开缺陷编号。
- 其余无新发现。所有定向现场的产品行为（回滚结论、四层基址措辞、名字校验、拒绝措辞）都与上一节增量复核的结论一致。

### 脚手架侧

- **无新发现**。两项"工具说假话 / 判据比它声称的弱"重点排查结果：① E-16/E-18/E-19 的"本地零改动"判据默认含目录维度、标题与口径一致（§B1.3）——**不属于**这两类；② `Get-OnlineCaseText` 只取结论文字、E-18 的 NameMismatch 判据本轮在真实输出上同时点了两边、`meta-only` 自检会炸（§B1.4）——也**不属于**这两类。`-Scope all` 不含 supplement/pressure（S-5）维持已知记账项。

## B3. 总结论（终版定向确认）

| 侧 | 结论 |
|---|---|
| **产品侧** | **通过**。1–7 条全部 CLOSED：N-P1 的回滚与"本地库状态"结论逐现场与磁盘事实相符（含"rules.int 已入库才失败 → 说不一致"这一最难构造的现场）；N-P2 四层措辞各自为真且互不顶替；E-14-b 面板路径 = 磁盘实况；产物来源逐字节钉死（只差链接时间戳）；无回归。唯一残留是 P-2 的弹窗回显空串（已知、低危、只记录）。 |
| **脚手架侧** | **通过**。自跑 157/0/0（产品 82 / 自检 75，exit 0）；树摘要默认含目录、`-FilesOnly` 只在自证里出现；`meta-only` / `empty-dir` / `raw-join` / `wrong-layer` 四条自证场景全部"该炸就炸"；`Get-OnlineCaseText` 口径收窄落实且无别的同类；历史取证一字未改。没有新发现。 |

**一句话**：前任报的最后两条判据缝（N-S1 / N-S2）与两条产品侧尾巴（N-P1 / N-P2 第二层）在这一版里**都真的修掉了**，且每一处我都能用自己的输入把它打出来或验出与磁盘一致；`9995400F` 的"全绿"里我没有再找到假通过。
