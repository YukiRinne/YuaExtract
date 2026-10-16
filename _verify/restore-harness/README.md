# YuaExtract 验证脚手架（步骤2 只收集 / 步骤3 只留已还原）

独立于实现的**黑盒**验证：不调用产品内部函数，只通过真实界面点击 + 文件系统观测下结论。
本目录是 task-3 的交付物，也是 task-5 / task-14 独立验证用的全部工具。

## 一条命令

```powershell
pwsh -File '_verify\restore-harness\run-all.ps1'                   # 全量（小用例）
pwsh -File '_verify\restore-harness\run-all.ps1' -Scope restore    # 只跑步骤3（断言 A/B/D + P）
pwsh -File '_verify\restore-harness\run-all.ps1' -Scope collect    # 只跑步骤2（断言 C/E）
pwsh -File '_verify\restore-harness\run-all.ps1' -Scope supplement # 后缀识别 + 推理补充还原 + R/N/T（6 轮 UI 用例）
pwsh -File '_verify\restore-harness\run-all.ps1' -Scope pressure   # 压力用例：12 封包 × 5000 = 6 万文件
# 把挖掘拉到分钟级（T-1 的规模档）：噪声候选从 2 万抬到 30 万
pwsh -File '_verify\restore-harness\run-all.ps1' -Scope supplement -NoiseCandidates 300000
```

它会依次做完：构建验证用空宿主 → 复制 `Release\` 快照 → 生成合成工作区 →
跑步骤3「默认」与「勾选保留」两种模式 → 跑步骤2「只收集 / 对照组」两种环境 →
汇总所有断言，并最后自检断言脚本本身的鉴别力。

退出码：`0` = 全部通过；`1` = 有断言失败；`2` = 前置条件没满足（没编译等）。

当前结果（2026-10-12，指纹 `D04DA2560CA3D00D`）：小用例 **53 / 53**、
压力用例（6 万文件）**21 / 21**，详见 [VERIFICATION.md](VERIFICATION.md) 附录。

## 目录结构

| 路径 | 作用 |
|---|---|
| `run-all.ps1` | 一键入口（`-Scope all\|restore\|collect\|supplement\|pressure`）。UI 步骤带**硬超时**，卡住记 124 而不是拖几十分钟 |
| `build-harness.ps1` + `src/EmptyHost.cpp` | 构建 32 位「空宿主」EXE（`LoadLibrary` 注入型 DLL 用） |
| `make-workspace.ps1` | 生成合成工作区 + 期望值清单（`manifest.json` + 逐条 `manifest.tsv`；`-Packages/-FilesPerPackage` 放大到 6 万文件）。**自带夹具自检**：类别与映射表不一致就直接抛错 |
| `run-step3-restore.ps1` | 驱动产品界面跑一次资源名还原（默认 / `-KeepUnrestored`） |
| `assert-restore.ps1` | 断言 A（默认语义）/ B（勾选保留）/ D（源目录只读）/ P（多线程与节流） |
| `assert-supplement.ps1` | 断言 S（后缀 + 补充还原）/ R（还原率）/ N（文案）/ T（后台线程与关窗） |
| `run-host-load.ps1` | 空宿主加载 DLL，观测窗口/日志/环境变量/「清空目录文件」 |
| `assert-collect-only.ps1` | 断言 C（只收集 vs 对照组）+ 断言 E（环境变量端到端） |
| `self-test-assertions.ps1` | 用故意做错的合成结果自检断言脚本的鉴别力 |
| `lib/HarnessUtil.ps1` | 通用工具（UTF-8/PE 位数/快照/进程/Release 快照/断言结果收集） |
| `lib/HarnessNative.ps1` | 跨进程窗口与控件访问（Add-Type 内联 C#） |
| `make-extension-fixture.ps1` | **扩展集一期夹具**：旧作采集成果 + 新解包目录 + 独立期望清单（`Run\extension-manifest.*`） |
| `run-extension-ui.ps1` | **扩展集 UI 驱动**：制作页签（生成/编辑/干跑/安装）+ 首页模式1 三步 + 传统模式对照（控件 ID 见 §8.4） |
| `run-extension-bad-archives.ps1` | **E-4 编排**：把坏档案（CRC/截断/版本/缺 IDENTITY/盐不匹配/空名字）装进库逐个驱动步骤1 |
| `assert-extension.ps1` | **断言 E-1..E-19**（扩展集一期验收，docs\扩展集设计.md §11） |
| `self-test-extension.ps1` | 扩展集断言的鉴别力自检（**55 个场景**，含"拒了但拒错理由"、S-1 假通过回归、E-11 的 9 个像素场景、以及 E-12..E-19 的 24 个新场景） |
| `run-tab-pixels.ps1` | **E-11 UI 驱动**：抓图（新启动基线 + 首页/扩展集/日志三路 + 连拍 + 跨进程）→ `Run\out\tab-pixels\*.bmp/png` + `tab-pixels.json` |
| `assert-tab-pixels.ps1` | **断言 E-11 独立入口**：只读快照做逐像素比对（也可由 `assert-extension.ps1 -TabPixelsJson` 内联调用） |
| `run-extension-online.ps1` | **E-12..E-19 编排**：搭远端仓库 → 逐个用例起本地 HTTPS 夹具 → 驱动「扩展集」页签与在线更新弹窗 → 汇总 |
| `make-online-repo.ps1` | 把**产品真档案**摆成"远端仓库"（索引列全部独立算出）+ 造坏法（404 / 截断 / 短读 / 乱字节 / 坏 sha / 盐不一致 / 索引格式错 / 慢响应） |
| `lib/serve-fixtures.ps1` | 本地夹具服务：TcpListener + SslStream（自签名证书装进机器信任库，**产品只认 https**）+ `_faults.json` 坏法 |
| `lib/HarnessPixels.ps1` | 像素工具：BMP 读写、逐像素比对、差异热区、热区→控件归属、合成图夹具（自检用） |
| `lib/HarnessSummary.ps1` | run-all 的汇总 + **假绿护栏**（本轮无新结果 / 项数低于下限 → 非 0） |
| `self-test-runall-guard.ps1` | **假绿护栏自检**：造"全是残留 / 项数不够 / 有失败"三种局面，要求该炸就炸 |
| `evidence/tab-pixels-prefix/` | **修复前旧构建的真实抓图**（PNG，含 `README.txt` 写明来源 exe 指纹）：E-11 的"真缺陷面前必须炸"证据 |
| `self-proof-s1-salt.ps1` | **S-1 自证**：故意改回旧写法（共享盐）→ E-4 必须炸；改回新写法 → 必须通过 |
| `build-hashprobe.ps1` + `src/HashProbe.cpp` | 夹具哈希的**产品侧真值探针**（直接编 `resource_hash.cpp`，只做哈希、不参与判分） |
| `lib/hash-extension.py` | 夹具侧 file/domain 哈希计算器（BLAKE2s-256 / SipHash-2-4+bswap + 归一化，带自检） |
| `lib/rules-int.py` | `rules.int` 的**独立读取器/破坏器**（§3 布局：dump 校验 + 造坏档案，带自检） |
| `Run/` | 运行产物：`FakeWorkspace`（合成工作区）、`App`（Release 快照）、`Host`、`out`（结果 JSON/截图/日志） |

## 断言清单

| 组 | 断言 | 判什么 |
|---|---|---|
| A | A-1 | 已还原文件按期望路径逐字节一致 |
| A | A-2 | **本来就是明文名的文件（`.alst`/readme）必须还在**（防"丢得太多"） |
| A | A-3 | **不允许"明文文件名躺在 Hash 目录下"的条目**（防"丢得太少"） |
| A | A-4 | 报告里的「未还原被丢弃」计数 = 脚手架独立枚举的数（不信报告，两边各算一遍） |
| A | A-5 | 报告其它计数（已还原/缺目录/缺文件/明文/总数/重名改名/失败）对照 |
| A | A-6 | 输出里不存在 64 位十六进制文件名 |
| A | A-7 | 输出条数不多不少 |
| A | A-8 | 未还原文件的内容没有以任何名字混进输出 |
| A | A-9 | 工具日志目录 `Extractor_Log` 被跳过 |
| A | A-10 | 重名冲突落 `__dupN`，两份数据都留下（不互相覆盖） |
| A | A-11 | 所有封包任务都到终态（无"永远处理中"，含空封包） |
| B | B-1..B-6 | 勾选保留后一个不丢、未还原文件确实被保留、已还原/明文名仍正确 |
| C | C-0..C-9 | 只收集：无还原窗口、无 `RuntimeHashRestore.log`、有收集控制台、清空确认框默认「否」、布局不越界、进度分母来自目标目录扫描 |
| E | E-1..E-3 | **父进程设开关 → 子进程继承 → DllMain 读到**；对照组走上游路径；两种模式下纯 Hash 目录零改动 |
| D | D-1 | 源解包目录只读（小用例比逐条明细；大用例比"排序行摘要"，含文件数/大小/mtime） |
| A | A-12 | 报告里的「跳过日志文件」== 独立枚举的跳过条数（根目录 + 封包内部两处，防重复计数） |
| P | P-1..P-5 | 多线程收尾（全部终态、任务数与封包数一致）、标题统计与列表一致、输出语义、**进度上报节流**（界面日志行数 vs 文件数）、耗时/吞吐/界面响应 |
| S | S-1..S-6b | ① 文件头补后缀（6 种文件头样本，未知落 `.bin`）；② 推理补充还原（脚本里的名字 → 真名；默认语义下也算"已还原"；不勾时不还原）；**S-4 安全底线**（负例名绝不出现 + 报告命中数与磁盘对账 + 名字形态审计）；`MinedNames.lst` 格式与条数；盐不可用 / 带密钥盐两种降级 |
| R | R-1, R-2 | 详细日志两行还原率（第一遍 + 最终），百分比与 X/Y 自洽；**与磁盘独立对账**（不采信日志） |
| N | N-1 | 用户可见文案一律「推理补充还原」，不残留孤立的「补充还原」（复选框 / 日志 / 报告章节 / 名字表头部） |
| T | T-1, T-1b, T-2 | **挖掘进行中**界面往返延迟 < 500 ms（采样 ≥5 次）；挖掘窗口内采样 ≥5 次；挖掘中关窗 → 确认框弹出、窗口 < 5 s 消失、线程回收、进程干净退出无残留 |

## 跑之前需要知道的几件事（都是踩过的坑）

1. **32 位 DLL 只能进 32 位进程**：pwsh 是 64 位，所以专门编了 x86 的 `EmptyHost.exe` 来当宿主。
   `build-harness.ps1` 会检查产物位数，不对直接报错。
2. **本仓库路径含 `&`**：不能在 `cmd` 里拼路径。构建脚本的做法是——用一个放在无空格/无 `&`
   目录下的临时批处理把 `vcvars32.bat` 的环境导进 pwsh，再由 pwsh 直接调 `cl.exe`。
3. **不直接跑 `Release\YuaExtract.exe`**：那会锁住文件、把队友正在链接的构建搞失败。
   每次都先复制成 `Run\App\` 快照再跑（先写 `App.staging` 再原子替换，避免留下半份快照）。
4. **环境变量必须由父进程设、子进程继承**：断言 E 的要点就是复现 Loader 的
   `SetEnvironmentVariableW` + `CreateProcessW(lpEnvironment=NULL)`，而不是替子进程拼环境块。
5. **跨进程读控件文字要走编组的系统消息**：`WM_GETTEXT`/`WM_SETTEXT` 传**本进程**缓冲区
   （系统负责搬），而 `LVM_*` 这类 ≥ `WM_USER` 的自定义消息不编组，必须在目标进程里
   `VirtualAllocEx` 一块内存（x86 的 `LVITEMW.pszText` 在偏移 **20**，写错会让对方崩在 `0xC000041D`）。
6. **别用 OutputDataReceived 事件里的 PowerShell 脚本块**：回调在 .NET 线程池线程上跑、
   没有 runspace，会把整个 pwsh 带崩。用 `ReadToEndAsync` 纯 .NET 异步排空管道。
7. **子进程会继承 stdout 句柄**：不排空管道，父脚本退出后调用方会一直等不到 EOF（后台任务表现为"永不返回"）。
8. **`PrintWindow` 抓图时要正高度**：写负高度（自上而下）时这台机器的驱动仍按自下而上回填，
   图会上下颠倒（文字看着像镜像）。截图固定输出 `Run\out\host-<mode>-<窗口类>.png`。
9. **报告解析别用大范围关键词**：`Get-ReportNumberMap` 里曾用 `目录` 做排除词，
   结果把 `缺目录 Hash 3` 一起排掉了 —— 只排除"以目录/路径结尾"的表头行。
10. **`pwsh -File` 传数组参数会被拆**：多余的数组元素会被绑到下一个**位置参数**上
    （实测把第二个窗口类名绑到了 `-DllPath`）。数组一律用逗号分隔的字符串（`*Csv`）传。
11. **对话框按钮文案带加速键**：产品的「中止还原」确认框按钮是 `是(&Y)` / `否(&N)`。
    拿 `'是'` 去精确比较**永远匹配不上** —— 结果是"框识别到了、一次都没点"，
    表现为"关窗关不掉"。现在先把 `&` 和 `(...)` 归一化再比，匹配不上再用
    `PostCommand(dlg, IDYES=6/IDNO=7)` 兜底。
12. **定长数组不能 `.Add()`**：`$record.saltRemoved` 这类字段在 schema 里是 `@()`，
    对它调 `.Add()` 会抛 `Collection was of a fixed size.`。要么用 `+=`，要么显式
    `New-Object System.Collections.ArrayList`（两条 UI 用例都栽在这上面过）。
13. **产品的名字表是持久的**：`StringHashDumper_Output\MinedNames.lst` 会被下一条用例读到，
    于是"上一条挖到的名字"被当成"这一条自己挖的"（S-6b 的假阳性就是这么来的）。
    现在每条用例前把旧名字表挪开、跑完再放回，并把本轮结果拷成
    `Run\out\step3-<mode>-MinedNames.lst` 留证（断言读快照，不读现场）。
14. **`Universal.log` 三个字段含义不同**：`Hash Seed:` 才是参与哈希的种子；
    `FileNameHasherSalt:` / `PathNameHasherSalt:` **留空**才是"不带密钥哈希"。
    一开始把三个字段都填成种子，产品读成"带密钥哈希"从而**正确地**拒绝挖掘 ——
    夹具写错会让好实现看起来像没实现。
15. **T-2 的语义**：关掉还原窗口 = 中止还原并**回到主窗口**，进程本来就该继续活着。
    要量的是"确认框弹了没、窗口多久消失、线程回收了没"；最后再关主窗口量进程退出（无残留）。
    按"进程必须立刻退出"判会冤枉实现。
16. **跑 UI 用例务必收尾进程**：`Get-Process YuaExtract -ErrorAction SilentlyContinue | Stop-Process -Force`
    —— 残留实例会让下一条用例认到旧窗口，也会让别人构建时撞 LNK1104。
    `run-all.ps1` 每步结束只会杀**自己快照**（`Run\App\`）起的实例，不误伤别人的手工验证。

## 自检（证明断言不是"永远通过"）

```powershell
pwsh -File '_verify\restore-harness\self-test-assertions.ps1'    # A/B/C/D/P 的鉴别力（8 个场景）
pwsh -File '_verify\restore-harness\self-test-supplement.ps1'    # S/R/N/T 的鉴别力（13 个场景）
pwsh -File '_verify\restore-harness\self-test-extension.ps1'     # 扩展集断言（E-1..E-11）的鉴别力（31 个场景）
pwsh -File '_verify\restore-harness\self-test-runall-guard.ps1'  # run-all 的假绿护栏必须会炸（5 个场景）
```

给断言脚本喂**故意做错**的合成结果，要求"该炸的必须炸"：

- `self-test-assertions.ps1`（8 场景）：明文名文件被丢掉、只认文件哈希就落盘、
  报告计数撒谎、还原过程改了源目录、勾了保留却没保留、步骤3 还是旧的内联还原。
- `self-test-supplement.ps1`（13 场景）：6 个样本没补后缀、该还原的名字没还原、对照组却还原了、
  **负例名落进输出**、输出里出现没埋过的"资源名形态"文件名、报告命中数撒谎、
  还原率那两行缺失、百分比与 X/Y 不自洽、文案残留旧词、界面被挖掘卡住（超时 / 700 ms）、
  关窗卡住不退出 —— 每个场景都要求"只有对应的断言炸，别的不能跟着炸"。

---

# 扩展集一期（task-3：夹具 + 驱动 + 断言 E-1..E-10）

```powershell
pwsh -File '_verify\restore-harness\run-all.ps1' -Scope extension    # 只跑扩展集
pwsh -File '_verify\restore-harness\run-all.ps1' -Scope all          # 既有 A/B/C/D/E 组 + 扩展集 E 组（总计里含）

> 扩展集组**也挂在 `-Scope all` 下**（lead 要求总计里含新增 E 组）；它与 `FakeWorkspace`
> 那几组用不同的夹具目录（`Run\ExtensionFixture` vs `Run\FakeWorkspace`），互不重建。
> 改了 `lib\HarnessUtil.ps1` / `lib\HarnessNative.ps1` 之后请至少跑一次 `-Scope restore`（实测 31/31）。

# 单独跑（调试用）
pwsh -File '_verify\restore-harness\make-extension-fixture.ps1'
pwsh -File '_verify\restore-harness\run-extension-ui.ps1' -Step meta,draft,dryrun,install -Game old
pwsh -File '_verify\restore-harness\run-extension-ui.ps1' -Step mode1   -Game mode1 -OutJson Run\out\extension-mode1.json
pwsh -File '_verify\restore-harness\run-extension-ui.ps1' -Step classic -Game old -OutJson Run\out\extension-classic.json
pwsh -File '_verify\restore-harness\run-extension-ui.ps1' -Step draft   -Game raw   -OutJson Run\out\extension-raw.json
pwsh -File '_verify\restore-harness\run-extension-bad-archives.ps1'
pwsh -File '_verify\restore-harness\assert-extension.ps1' -BadArchiveRunJsonCsv '<从 summary.json 读>'
pwsh -File '_verify\restore-harness\self-test-extension.ps1'

# E-11（页签像素幽灵）：抓图 → 判分（见本文「E-11」一节）
pwsh -File '_verify\restore-harness\run-tab-pixels.ps1'
pwsh -File '_verify\restore-harness\assert-tab-pixels.ps1'
pwsh -File '_verify\restore-harness\assert-tab-pixels.ps1' -SnapshotJson Run\out\tab-pixels--firstpostfix-run.json   # 复算旧快照
pwsh -File '_verify\restore-harness\self-test-runall-guard.ps1'
```

夹具（`make-extension-fixture.ps1`）造 4 部"旧作" + 1 份新解包目录：

| 目录 | 用途 |
|---|---|
| `OldGame\` | 主夹具：`StringHashDumper_Output`（DirectoryHash.log / FileNameHash.log / MinedNames.lst / HashRestore_RecoveredNames.lst / ExtraManual.lst）+ 旧解包目录 + 盐 |
| `RawGame\` | **E-2**：采集日志里的目录名故意没归一化、哈希按原样算 → 生成期自校验必须大面积报例外 |
| `OtherGame\` | **E-4 盐不匹配**：另一个盐、**exe 摘要也不同**（否则"exe 摘要命中"会短路掉盐判定） |
| `EmptyGame\` | 预留：采集成果全空的旧作（空名字档案另由 `rules-int.py mutate` 造） |
| `NewUnpacked\` | 新解包目录（纯哈希名：12 可还原 / 3 不可还原 / 3 明文名 / 2 工具日志） |

哈希真值链：`lib/hash-extension.py`（BLAKE2s-256 / SipHash-2-4+bswap）**与** `Run\Host\HashProbe.exe`
（直接编产品自己的 `CxdecRepacker\core\resource_hash.cpp`，**不跑 MSBuild**）逐条对账，不一致直接抛错。
根域哈希 `94D4A97C61498621` 与 `resource_hash.h` 注释里的真实样本一致（锚点）。

断言清单（`docs\扩展集设计.md` §11）：E-1 免跑游戏闭环 / E-2 生成期自校验 /
E-3 干跑 == 实测 / E-4 坏档案必须被拒（含空名字档案）/ E-5 不伪造名字 / E-6 源目录只读 /
E-7 幂等 / E-8 体积 / E-9 归一化 / E-10 模式1 ≡ 传统动态模式，
外加 **E-11 页签像素幽灵**（task-7，用户实测的缺陷，见下节）与 X-6 性能护栏。

---

## E-11：页签像素幽灵（可见性 ≠ 像素干净）

**用户实测的缺陷**：切到「制作扩展集」页签后，**首页控件的旧像素还留在画面上**（叠字）。
最初以为"控件没藏干净"，实测却相反 —— 切过去之后首页控件的 `IsWindowVisible` **全是 False**
（可见性完全正确），旧像素却留在窗口表面上。

> **这就是既有 86 项断言的盲区**：所有只看窗口状态的断言（可见性 / enable / 文字 / 布局）
> 在缺陷存在时**照样全绿**。像素残留只有**抓图逐像素比对**才看得见。
> 所以 E-11 把两条线分开写：
> - `E-11-e` 可见性对照（保留，作为不变量）；
> - `E-11-a/b` **像素判据**（唯一的幽灵判据）。
>
> 实测证据：缺陷现场 `E-11-e` 全绿、`E-11-a` 报 12,762 px 差异 —— 这条盲区是**跑出来的**，不是推理。

```powershell
# 抓图（起一次真实界面，走四路状态；全程 bitblt 抓客户区，见踩坑 #31）
pwsh -File '_verify\restore-harness\run-tab-pixels.ps1'
# 判分（只读快照，不启动产品）
pwsh -File '_verify\restore-harness\assert-tab-pixels.ps1'
# 假绿护栏自检
pwsh -File '_verify\restore-harness\self-test-runall-guard.ps1'
```

`-Scope extension` / `-Scope all` 里 E-11 已挂在 `assert-extension.ps1` 内（`-TabPixelsJson`），
所以断言的**总数里含 E-11**。

### 判据：画面必须与"你怎么到达这个页签"无关

| 断言 | 判什么 |
|---|---|
| `E-11-a` | **制作页签四路抓图两两必须一致**：新启动→直接点（基线）/ 首页→制作 / 扩展集→制作 / 日志展开收起→制作 |
| `E-11-b` | 首页页签也要历史无关（幽灵是**双向**的：制作页的像素也会留在首页上） |
| `E-11-C` | 同一状态**连拍两张必须逐像素一致**（噪声地板；不然本次测量不可判读） |
| `E-11-d` | 跨进程可复现：另一个进程走"新启动→直接点"必须与进程1 的基线一致 |
| `E-11-E` | 快照完整性：抓图文件都在、尺寸与快照声明一致、驱动没失败 |
| `E-11-F` | 抓图有效性：不能是空白/单色图，且**首页图与制作页图必须显著不同**（防"页签没切过去，两边都是同一张图"这种假通过） |
| `E-11-T` | 阈值可信性：阈值必须落在可信区间（防"把阈值调到 50% 让断言永远通过"） |
| `E-11-e` | 可见性对照（**不是**幽灵判据，见上面的盲区说明） |
| `E-11-g` | 失败时输出**差异热区**（客户区坐标 + 疑似旧位置控件 + 重叠像素数）与差异可视化 PNG |

基线口径就按任务要求：**新启动 → 第一次点击就是「制作扩展集」**（`make-baseline`）；
其余三路都是"先去过别的页签再切回来"。做成**两两比对**而不是只跟基线比，是因为缺陷是**累积**的
（修复前实测 `make-after-set` 与 `make-after-log` 之间也有 3.51% 差异）。

### 阈值标定（实测，不是拍脑袋）

客户区 1150x795 = 914,250 px（修复前那批历史抓图是 1164x801 = 932,364 px，口径同一套）：

| 量 | 修复前（`6B664840`，1,003,008 B） | 修复后（`492522BD`，1,006,592 B） |
|---|---|---|
| 同一状态连拍两张（噪声地板） | **0 px / 0.0000%** | **0 px / 0.0000%** |
| 制作页签：新启动→制作 vs 首页→制作 | 12,762 px / **1.369%** | **0 px** |
| 制作页签：其余两两组合 | 3.51% / 5.76% / 7.13% / 9.28% / 10.59% | **全部 0 px** |
| 首页：启动时 vs 去过制作页后回首页 | 107,863 px / **11.569%** | 1,698 px / **0.186%**（合法重绘，见下） |
| 首页图 vs 制作页图（正对照） | 18.01% | 26.83% |

**阈值取 0.10%（制作页签）**：噪声地板实测 0，最小缺陷信号实测 1.369% → 距信号 **13.7 倍**、
距噪声地板有近千像素余量。**取 1%（首页）**的理由是"合法重绘噪声"：

- 首页修复后仍有 **1,698 px（0.186%）** 差异，现场判读过，两块都不是幽灵：
  ① 概览 RichEdit（4070）**同一段文字被重新写一遍之后的重绘差异**（约 1,510 px；用 `SmartText`
  逐字比对：启动时与回首页后都是 464 字、正文**完全相同**）；
  ② 点过的页签按钮上的**焦点框**（约 188 px；`BM_GETSTATE` 实测启动 `0x0000` → 点击后 `0x0008` = `BST_FOCUS`）。
- 三者放在一起：合法重绘 0.186% ← **阈值 1%** → 缺陷信号 11.569%（相距 5.4 倍 / 11.6 倍）。

断言详情里会**同时打印**"本次实测差异像素数/比例"和"阈值"，所以阈值有没有被偷偷放大一眼能看出来
（`E-11-T` 还会拦住超出可信区间的阈值）。

### ⚠️ 几何边界：E-11 是**固定几何**判据（R-8，2026-10-14 独立复核发现）

E-11 只覆盖"**不改窗口大小**"的路径。`WM_SIZE`（小窗 → 拉大）路径上存在**合法的 ClearType 重光栅化噪声**：

- 实测 **2,437 px / 0.267%**（客户区 1150×795），而制作页阈值是 **0.10%（≈932 px）** ⇒ **已经超过阈值**；
- 该残差经逐像素量级分类：**100% 是单通道 ±1 LSB**（最大通道差 = 1）、最佳位移 (0,0)、文字内容与位置不变、
  **全部落在控件 4111（`RICHEDIT50W`）内部，没有一处落在 GROUPBOX 里**；旧构建同路径是 **15,885 px 内容级差异（最大通道差 240）**
  ⇒ 判为亚像素噪声，**不是幽灵**。
- 所以：**不要把"小窗 → 拉大"直接当作第 5 路塞进 `E-11-a` 的状态矩阵** —— 那样连**修复后的构建也会炸**。
  要覆盖这条路径，必须先给它**单独口径**（例如容忍 ±1 LSB，或按控件区域分别取阈值）。

### 自检（"幽灵存在时必须炸"）

`self-test-extension.ps1` 里有 9 个 E-11 场景，每个都对应一种"断言可能退化"的方式：

| 场景 | 做法 | 期望 |
|---|---|---|
| `e11-ghost` | 制作页某几路叠上首页的旧像素 | **只有 `E-11-a` 炸**；`E-11-e`（可见性）**必须不炸** ← 盲区的回归测试 |
| `e11-home-ghost` | 首页回去后不干净 | 只有 `E-11-b` 炸 |
| `e11-noise-unstable` | 同状态连拍不一致 | 只有 `E-11-C` 炸 |
| `e11-blank-capture` | 所有抓图都是单色（两张图差异 0，最容易假通过） | `E-11-F` 炸 |
| `e11-frozen-tab` | 页签没切过去（"制作页图"其实是首页图，且驱动谎报切成功） | `E-11-F` 炸 |
| `e11-cross-process` | 另一个进程的同一路径不一致 | 只有 `E-11-d` 炸 |
| `e11-threshold-tampered` | 把阈值调到 50% 想让断言永远通过 | 只有 `E-11-T` 炸 |
| `e11-missing-snapshot` | 快照文件不存在 | `E-11` 炸（不许静默跳过） |
| `e11-prefix-real-capture` | **修复前旧构建的真实抓图**当输入 | `E-11-a` + `E-11-b` 炸 |

最后一条用的是 `evidence/tab-pixels-prefix/` 里的现场抓图（PNG 存证，比对前用 Pillow 无损转 BMP；
往返自检 0 差异像素）。**来源 exe 指纹写在那个目录的 `README.txt` 里：旧构建 1,003,008 B /
sha256 `6B664840DD6EF1BB…`**，别和最终构建（`492522BD…` / 1,006,592 B）混淆。

### 存证与产出

| 路径 | 内容 |
|---|---|
| `Run\out\tab-pixels.json` | 抓图快照（每张图的路径/尺寸/颜色数/主色占比/抓图方法/页签标志可见性 + 控件矩形表 + 阈值与标定说明） |
| `Run\out\tab-pixels\tab-*.bmp` | 逐像素权威证据（断言只读它） |
| `Run\out\tab-pixels\tab-*.png` | 同图的人眼看版本 |
| `Run\out\tab-pixels\diff-*.png` | 超阈值组合的**差异可视化**（差异涂蓝） |
| `Run\out\checks-tab-pixels.json` | `assert-tab-pixels.ps1` 单独跑时的结果 |

抓图方法记在每张图的 `captureMethod` 字段里（`bitblt` / `printwindow1`）—— 兜底抓的图必须标出来，
不能让人以为全是 PrintWindow 抓的（原因见踩坑 #31）。

**最近一次全量结果（2026-10-14 E-11 轮后，产品指纹 `492522BD23280617` / 1,006,592 B）**：
`-Scope extension` = **51 / 51**（E-1..E-11 + X-6 共 **20 项** + 断言自检 **31 项**）；
其中 E-11 的实测见本文「E-11」一节（制作页签四路抓图 **全部 0 px**、首页 0.186%、跨进程 0 px）。
`-Scope all` 由 lead 在本轮改动后复跑（本 README 记录的是最近一次实测值）。

> 上一轮（2026-10-13，指纹 `6B664840DD6EF1BB` / 1,003,008 B，**修复前**）：
> `-Scope extension` = **33 / 33**（E-1..E-10 + X-6 共 11 项 + 自检 22 项），`-Scope all` = **86 / 86**。
> 那一轮的 E-11 是**缺失的**（用户实测到幽灵、既有断言看不见），这正是本轮补上它的原因。
> 注意：本轮改了 `lib\HarnessNative.ps1`（抓图内核）与 `lib\HarnessUtil.ps1` 之外的 `lib\HarnessPixels.ps1`、
> `lib\HarnessSummary.ps1`，按下面的老规矩应至少复跑 `-Scope restore` 与 `-Scope collect`（`-Scope all` 覆盖两者）。

关键实测（E-1..E-10 沿用上一轮结论，未复算的以 run JSON 为准）：

| 断言 | 实测 | 证据 |
|---|---|---|
| E-1 | 模式1 在**无盐、无动态采集成果**的工作区里跑通：输出 15 个资源、与"原始映射直接跑"逐条一致（0 差异）；两棵 `Extractor_Output` 内容摘要（不含 mtime）相同；`StaticHash.lst` 20 行 | `Run\out\extension-mode1.json`、`extension-classic.json` |
| E-2 | 未归一化夹具：EXCEPTIONS **5** 条（5 个未归一化目录全被点名）；正常夹具 EXCEPTIONS **0** 条 | `Run\out\extension-raw.json`、`extension-old.json` 的 `archiveDump` |
| E-3 | 面板四行逐项对上：① 5 / ② 12（哈希去重）/ ③ 12 / 成功率 80%；独立枚举 5 / 12（条目 13）/ 12 / 80.00% == 真实步骤3 还原 12；面板扫描数 19/15 自证 | 同上 + `extension-mode1-StaticHash.lst` |
| E-4 | **7/7 全部按该用例专属理由被拒**（盐不匹配两种出口 / CRC / 截断解压 / 版本过高 / 缺 IDENTITY / 空名字），每条先做**前置复核**（独立解析档案确认"真的是这一类的坏档案"） | `Run\out\extension-bad-*.json`、`extension-bad-archives-summary.json`、`self-proof-s1-salt.ps1` |
| E-5 | 负例名 0 次；16 条输出全部可追溯到声明来源 | `extension-mode1.json` |
| E-6 | 解包目录前后摘要全等；"生成来源目录"按**内容**判（mtime 变化来自脚手架自己的干跑重铺） | 各 run JSON 的 `*Digest*` |
| E-7 | 除 `META.generatedAt` 外逐 chunk 一致（本次两轮 generatedAt 也相同 = 继承）；重复安装后库清单仍只有 1 条数据行 | `extension-old.json`、`extension-old-repeat.json`、`extension-old-install2.json` |
| E-8 | 结构自洽（独立解析）；DIRS 5 / FILES 15 / EXCEPTIONS 0；名字层 12.7 字节/条（整档 36.7 字节/条被元数据主导，**不作判据**） | `archive-dump-old-draft.json` |
| E-9 | `StaticHash.lst` 16 位目录 5 条、64 位文件 15 条，不合格 0；档案 DIRS/FILES 同口径 | `extension-mode1-StaticHash.lst` |
| E-10 | 模式1 与传统动态模式各 15 个资源、0 差异；报告里写明映射来源 | 同上 |

**S-1 自证**（`pwsh -File self-proof-s1-salt.ps1`，退出码 0）：
故意改回旧写法（`-Game other -ToolSaltOverride xp3hnp`）→ 档案变成 `salt=xp3hnp / DIRS 0 / FILES 0`
→ **E-4 必须炸**（前置复核：`用例档案的盐 = 'xp3hnp'，与当前游戏同盐 —— 这样测到的是别的防线`）；
改回新写法（按 `-Game` 取盐 `othersalt123`）→ E-4 通过。证据：
`Run\out\s1-selfproof-oldstyle-checks.json`、`s1-selfproof-newstyle-checks.json`。

扩展集侧的踩坑（都是本脚手架自己踩的，记下来以免重犯）：

1. **HashProbe 只做哈希、不做归一化**：把未归一化的目录名喂给它，算的是另一个串
   （`characters/yuki` vs `characters/yuki/`），对账会全红。现在先由 Python 导出"最终要哈希的串"再喂它。
2. **spec 文件的 CRLF**：两列的行里 `parts[1]` 会带上 `\r`，哈希算的是 `"base.png\r"`，
   而且**只在两列的行上出错**（三列的行把 `\r` 留在第三列）—— 极难查。所有读入字段一律 Trim。
3. **窄字符路径打不开**：本仓库路径含中文，C 运行时的窄 `fopen` 走 ANSI 代码页 → 用宽字符路径打开。
4. **根域目录哈希要取 `ROOT` 行**：写成 `DIR|''` 会得到空串，根域文件于是落到 `<包>\<64位哈希>`
   （包根裸哈希文件），口径完全变了。
5. **`1..0` 在 PowerShell 里是 `@(1,0)`**：自检里"0 条例外"的合成档案凭空多出 2 条，E-2 假失败。
6. **干跑数字可能一行多项**（`可解目录数 5 / … / 完整路径可还原数 12 / 成功率 80.00%`）：
   按行只取一对 label/number 的旧解析会漏项。扩展集断言改用整段扫所有 `label number` 对。
7. **E-4 的坏档案要在库里换**：`run-extension-bad-archives.ps1` 用 try/finally 保证
   无论成败都把库档案还原成好档案，否则后面的模式1 对照会被污染。
8. **步骤2 是替代**：合成工作区里没有真游戏，"提取动态 XP3 资源"这一步用**预置 Extractor_Output**
   代替（两个模式用同一份），报告与断言里都按"替代"记录。
9. **`@{}` 是大小写不敏感的**：夹具的哈希表键是 `KIND|输入`，于是 `DIR|ui`（正常夹具）被
   `DIR|UI`（E-2 的未归一化变体）覆盖 → 采集日志里写了个"重算不回去"的哈希 →
   产品生成期自校验如实报成例外（**看起来像产品缺陷，其实是夹具的锅**）。
   夹具的哈希表一律用 `Dictionary[string,string]` + `StringComparer::Ordinal`；
   并且夹具自检里加了一条"每条采集映射都必须按文档口径重算得回自己那个哈希"。
10. **页签是自绘 BUTTON，不是 `SysTabControl32`**（4001..4004）：`TCM_SETCURSEL` / `TCM_*`
    一律无效 —— 结果是页签从没切过去、制作页签的控件全是 `visible=False`，后面点按钮全落空。
    现在 `Select-Tab` 一律 `BM_CLICK`，并用"该页标志控件必须变可见"来**校验**切换成功。
11. **4083/4084「会社/作品」是 `ListBox`，不是 `ListView`**：`LVM_GETITEMCOUNT` 问 ListBox 只会得到 0
    → "列表是空的"假失败。现在按类名分派：`LB_GETCOUNT`(0x018B) / `LB_GETTEXT`(0x0189，
    **远端缓冲区**，lParam 是指针不编组) / `LB_SETCURSEL`(0x0186)，并照 §8.4 补发 `LBN_SELCHANGE`。
12. **盐在 `<工具目录>\Log\Universal.log`，不在工作区**：生成扩展集时产品读的是**工具目录**那份
    （"由步骤2 的收集模块写出"）。`run-extension-ui.ps1` 在**快照同步之后**往 `Run\App\Log\` 补一份
    （同步会把 `Run\App` 整个换掉）；**模式1 故意不补** —— 它必须只靠档案里的盐，这正是 E-1 的证据。
13. **`-Step meta,draft` 传进来是一个字符串**：`pwsh -File x.ps1 -Step a,b` 不会拆成数组，
    所以 `-Step` 不能挂 `ValidateSet`（它会在拆分前先拒绝）。脚本里手工拆逗号 + 校验。
14. **`$Title` 参数被局部 `$title` 覆盖**：PowerShell 变量名不区分大小写，`$title = 窗口标题`
    把用户传的 `-Title` 冲掉了（图2 的"游戏名称"于是填成了整条窗口标题）。局部变量改名 `$windowTitle`。
15. **native 命令分两次 splat 参数会把开关拆成字符**：`& pwsh -File x.ps1 -A 1 @extra`
    实测把 `-StrictBytes` 当成 `-`、`S`、`t`… 逐个传（报"参数 '-' 不属于 ValidateSet"）。
    现在把所有参数拼成一个数组，**一次性** `& pwsh @args`。
16. **`1..0` 是 `@(1,0)`**：自检里"0 条例外"的合成档案凭空多出 2 条，E-2 假失败。
17. **干跑数字可能一行多项**：`可解目录数 5 / … / 完整路径可还原数 12 / 成功率 80.00%` ——
    按行只取一对 label/number 的旧解析会漏项。扩展集断言改用整段扫所有 `label number` 对。
18. **E-7 的判据不是"字节一致"**（2026-10-13 修复轮按 lead 裁决改写）：UI 路径下 `META.generatedAt`
    会被**继承** —— `-Step draft` 会先自动跑 `meta`，而"编辑扩展集信息"会从**已存在的档案**预填
    `Meta`（含 `generatedAt`），内核只在它为空时才盖时间戳。所以第二次生成常常与第一次字节相同，
    但**这不是稳定性证据**（删掉档案全新生成就会不同；验证者实测 `FDE2EE93…` vs `452A9291…`）。
    判据 = **除 `META.generatedAt` 外逐 chunk 一致**（`-IdempotenceMode auto`，默认）；
    `-StrictBytes` 只用于内核级 `stampGeneratedAt=false` 的场景，**不作为 UI 路径的通过条件**。
    断言还会校验 `generatedAt` 存在时格式合法（`yyyy-MM-dd HH:mm:ss`）。
19. **E-4 的坏档案要在库里换**：`run-extension-bad-archives.ps1` 用 try/finally 保证
    无论成败都把库档案还原成好档案，否则后面的模式1 对照会被污染。
20. **路径分隔符**：档案里的目录名是 `characters/yuki/`（正斜杠，§3 规范化形态），落盘后是
    `characters\yuki\`。期望值/比较键必须统一成 Windows 形态，否则"文件明明在"却报缺少。
21. **`@(...)` 包住单元素返回值**：目录里恰好一个文件时 `[Harness.Batch]::Rows` 会被解包成
    单个字符串，紧接着 `$rows.Count` 在 StrictMode 下直接抛（raw/other 夹具的 Extractor_Output 就一个文件）。
    同理 `$x = if (...) { $单元素数组 } else { ... }` 也会解包成标量 —— **外面一律套 `@(...)`**。
22. **证据要留快照**：后续用例（E-4 的坏档案 step1）会清掉 `StaticHash_Output` 重建，
    断言再去读现场就会"文件不存在"。驱动现在把 `StaticHash.lst` 拷成
    `Run\out\extension-<game>-StaticHash.lst`，断言读快照。
23. **模式1 要跑在专用工作区**：`-Game mode1`（Mode1Game：没有盐、没有 `StringHashDumper_Output`）。
    在旧作工作区里跑模式1，E-1 的"免跑游戏"证据会被旧作自带的盐污染（假失败）。
24. **共享工具目录的盐会串味（task-4 的 S-1，最重的一条）**：`<工具目录>\Log\Universal.log` 是
    **所有用例共用**的。旧写法不看 `-Game`、无条件写主夹具的盐 `xp3hnp`，于是"别的游戏"（OtherGame）
    的采集日志被用 `xp3hnp` 重算 → 名字层全进例外 → 那份档案 `DIRS 0 / FILES 0`、`salt=xp3hnp`
    → 模式1 命中的是**空名字防线**，而断言的关键词（`会社/作品`）又命中了**页面套话** → **假通过**。
    现在：① 盐按 `-Game` 取（`other` → `othersalt123`），并用 `-ToolSaltOverride` 可复现旧写法；
    ② 不提供盐时**删掉**残留的那份；③ E-4 每条用例先做**前置复核**（自己用 `lib\rules-int.py`
    解析该用例的档案：盐是否真的不同、是否真的有名字、CRC 是否真的坏、版本是否真的 0xFFFF…），
    前置不成立直接判失败；④ 拒绝理由只从"失败原因 / 对话框正文 / 步骤原因"里取（**不含**概览与
    制作状态面板 —— 那是套话），并且要求命中**该用例专属**措辞、**排除**其他防线的措辞。
25. **"拒了但拒错理由"必须有自检场景**（S-2）：只测"该拒不拒"会漏掉 S-1 这类盲区。
    现在自检有 `e4-s1-false-pass`（同盐+无名字的假档案）、`e4-wrong-reason-salt`、
    `e4-wrong-reason-crc` 三个场景，专打"理由不对也算通过"。
26. **干跑默认测试来源目录会被改（S-5）**：它默认 = 采集来源目录 = `<工作区>\Restored_Extractor_Output`，
    而传统模式步骤3 正是往那儿写结果 → 全量跑完单独复跑干跑会得 `0/0/0`。现在驱动在点「测试」之前
    把夹具内容**重新铺回去**，断言还会核对面板的"测试来源目录 + 文件总数（哈希名 N）"，哈希名为 0 判失败。
27. **E-8 在小夹具上别下量级结论（S-6）**：20 条档案里固定开销（头 + chunk 表 + META + IDENTITY）
    占大头，"整档 ÷ 条数"没有鉴别力。现在 E-8 = **结构自洽（独立解析）+ 条数 + 名字层每条平均**，
    量级结论只在 `-ExtraFiles 3000` 那种规模档上才谈。
28. **② 「文件名可解」按 64 位哈希去重**（S-3）：内核 `hitFiles` 是 `set`，条目数（同一哈希在两个
    目录下出现两次）是 13、去重后是 12。独立枚举与面板对账都用去重口径；③ 整路径按条目、成功率 = ③ ÷ 哈希名数。

E-11（页签像素幽灵）这一轮踩的坑 —— **前两条是脚手架自己的缺陷，与产品无关**，写清楚免得后人误判：

29. **`GetDIBits` 的行跨距会踩烂 CLR 堆（脚手架缺陷，产品无责）**：DIB 每一行按 **4 字节对齐**，
    而抓图缓冲区原先按 `width*height*3` 分配 —— 只要 `width*3 % 4 != 0`（例如客户区宽度 1150：
    `1150*3 = 3450` → 对齐后 3452），`GetDIBits` 每行就多写 2 字节，整幅溢出 1,590 字节，
    **直接踩烂 CLR 堆**：表现为抓几张之后 `Fatal error. Internal CLR error. (0x80131506)`、
    pwsh 挂死不退出、CPU 烧着。宽度 1164（窗口默认尺寸）时 `1164*3 = 3492` 恰好对齐，
    所以这个 bug 潜伏了很久 —— 只要谁把客户区调成非 4 对齐宽度就会中。
    现在：`stride = ((width*3 + 3) / 4) * 4`，缓冲区 `stride*height`，`biSizeImage = stride*height`，
    写 BMP 时整块拷 `stride` 行（BMP 的行布局与 DIB 一致）。
30. **`GetDIBits` 取像素前必须把位图从 DC 里摘出来 + `GdiFlush()`**：MSDN 明确要求 `hbmp`
    **没有被选入任何 DC**，否则会**间歇性**返回 0（实测"改尺寸/切页签后连抓 3 次失败 2 次"，
    报出来就是"GetDIBits 失败"，看起来像抓图坏了）。只摘出来还不够，`PrintWindow` 之后立刻
    `GetDIBits` 会跟挂起的 GDI 批处理赛跑，所以补一发 `GdiFlush()`。
31. **`PrintWindow` 会无限期阻塞 —— 抓图首选"窗口 DC 的 BitBlt"（脚手架经验）**：
    `PrintWindow` 要目标窗口的界面线程配合渲染，实测有一次**再也不返回**（脚本挂死、CPU 烧着，
    而且带线程超时也只是把阻塞线程丢掉、它还在烧 CPU）。现在 `CaptureClientToBmp` /
    `CaptureWindowToBmp` 默认顺序 = **先从窗口 DC 直接 `BitBlt`**（纯同步 GDI、不依赖目标消息循环、
    不会阻塞），失败才退回 `PrintWindow`（放在工作线程上跑 `Join(2500ms)`，超时即作废、产物一律
    走临时文件，超时那次绝不采信）。**两种口径的像素实测一致**：同一现场下差异像素数
    `12,762 / 86,487 / 53,716` 三组**分毫不差**（pw0 / pw2 / dc 三种抓法结果相同）。
32. **可见性 ≠ 像素干净（这条是 E-11 存在的理由）**：切到「制作扩展集」后首页控件的
    `IsWindowVisible` 全是 `False`，旧像素却留在表面上（用户看到的叠字）。根因是**大号
    `BS_GROUPBOX` 按钮只画边框+标题、从不擦内部**，父窗口又被 `WS_CLIPCHILDREN` 挡在外面，
    于是被隐藏控件画过的像素永远留着。**任何只看窗口状态的断言在缺陷存在时都全绿** ——
    判幽灵只能用抓图逐像素比对（`E-11-a/b`），可见性断言只能当辅助（`E-11-e`）。
33. **差异热区要按"旧位置"归属才有用**：热区坐标（客户区左上角原点）必须和**当时可见控件的矩形**
    对上，否则"差异 12,762 px"这种数字没法定位。所以驱动在每个抓图时刻记录一份可见控件矩形表
    （客户区坐标），断言再把热区映射成"疑似旧位置：`4014«辅助操作»`（重叠 5,600 px）"。
    注意隐藏控件会被布局挪到 `(0,0,0,0)`（`MoveControl(..., false)` 带 `SWP_HIDEWINDOW`），
    **所以"旧位置"只能取它可见时的矩形表**。
34. **首页的合法重绘噪声别当成幽灵**：修复后首页仍差 1,698 px，逐字比对正文完全相同、`BM_GETSTATE`
    显示焦点位变化 —— 是"同一段 RichEdit 文字被重设一次的重绘差异 + 点过的页签按钮焦点框"。
    这两块与"旧控件像素残留"是两回事（后者修复前 11.57%），所以首页阈值用 1%（噪声 5.4 倍 /
    缺陷 11.6 倍），而不是硬套制作页签的 0.10%。
35. **汇总会假绿**（脚手架缺陷，产品无责）：如果某一步在写出 `checks-*.json` 之前就失败了
    （例如共享 lib 编译不过 → 所有 UI 用例全挂），旧版汇总会把上一轮的 checks 文件统统当成
    "残留"跳过，然后打印"总计：通过 0 / 失败 0 + 结论：全部通过" —— 这一轮**一个断言都没跑**。
    现在汇总抽到 `lib\HarnessSummary.ps1`，三条护栏（① 本轮一个新高结果都没有 ② 实际执行项数
    低于下限 ③ 有失败项）任何一条成立都非 0 退出，并由 `self-test-runall-guard.ps1`
    **自证**（造"全是残留 / 项数不够 / 有失败"三种局面，要求该炸就炸，且"全是残留"那一次
    **不许出现"全部通过"字样**）。
36. **改共享 lib 的流程**：`_verify\restore-harness\lib\**` 是别的成员也在跑的运行时。
    这一轮曾经分两步编辑 `HarnessNative.ps1`，中间态被队友的 `run-all` 撞上 → `Add-Type`
    编译不过 → E-4 全线误判。现在的流程是：**先在 `%TEMP%` 的副本上 `Add-Type` 编译自测、
    通过才写回，写回后再用真实路径编译一次**（`run-tab-pixels.ps1` 之类的调用方也因此多了一层
    "lib 起不来就直接报错"的兜底）。

---

# E-12..E-19：扩展集页签 + 在线更新（2026-10-15 轮，构建指纹 `FAEB6760`）

本轮把「扩展集」页签（`IDC_YUA_TAB_SET` = 4003，设计稿 §8.5）与**在线更新**（§14）纳进验证面。
新增 **12 项断言**（E-12 骨架、E-12-a/C/E/F/T 像素五条、E-13..E-19）与 **24 个自检场景**。

两条口径先说清：

1. **在线更新一律走本地夹具**（`YUAEXT_ENDPOINT` 指向 `lib\serve-fixtures.ps1` 起的本地服务），
   绝不访问外网、绝不依赖 GitHub。断言里也没有任何"访问某个真域名"的分支。
2. **扩展集页签的像素判据与 E-11 同一套**（逐像素比对、阈值必须在可信区间、失败给热区与控件归属），
   只是矩阵换成了 set 页签那四路；两页的结论因此可以横向对照。

## 驱动约定（§8.5 第 4–5 条）

| 约定 | 脚手架怎么落 |
|---|---|
| 第 4 条：页签 4003 的**标志控件是 `IDC_YUA_SETM_REFRESH`(4131)**，切过去它必须变可见 | `Select-Tab 4003 -ExpectVisible 4131`：切不过去就直接失败（自绘按钮页签必须 `BM_CLICK`，见踩坑 #10） |
| 第 5 条：在线更新弹窗的耗时动作（拉索引 / 下载安装）轮询 **4145 文本 + 4143 enable** | `Wait-OnlineIdle`：忙碌 = 4141 被禁用；结束 = 4141 恢复可用 **且** 4145 文本连续两次采样不变（防"刚恢复可用、结论还没写进去"） |
| 弹窗是**模态**的（主窗口被 `EnableWindow(FALSE)`） | 按顶层窗口类 `YuaExtractOnlineDialog` 找（与图2 的 `YuaExtractMetaDialog` 同一手法），用 `GetDlgItem(dialog, id)` 取件 |

页签骨架的期望值**不是从设计稿抄的**，是实测出来的（`run-extension-ui.ps1 -Step settab` 记三路取样）：

- 切到 4003：可见 = 页签 4001-4004 + `4071`（右栏详情）+ `4072`（日志按钮）+ `4080-4084`（扩展集选择，与首页共用）+ `4130-4136`（本页签自己那组）
- 首页/制作页签的控件（`4010/4020-4022/4030-4032/4050-4052/4085-4088/4070/4073/4100-4111`）必须**全隐**
- 切回首页后 `4130-4136` 必须**全隐**（`4080-4084` 是共用组，本来就该可见）

## 本地 HTTPS 夹具（`lib\serve-fixtures.ps1`）

```powershell
# 手工起一个夹具（正常用法是由 run-extension-online.ps1 自动起停）
pwsh -File '_verify\restore-harness\lib\serve-fixtures.ps1' -Root Run\ExtensionRepo\good -PortFile Run\out\port.json -IdleTimeoutSec 60
# 卸掉夹具证书（本机信任库里留一张自签 CA 是有安全含义的）
pwsh -File '_verify\restore-harness\lib\serve-fixtures.ps1' -Root Run\ExtensionRepo\good -UninstallCertificate
```

- **端口**：`-Port 0`（默认）= 让系统挑空闲端口；实际端口写进 `-PortFile`（JSON）并打印一行 `PORT=<n>`。
  **不要写死端口**，否则会和别的进程/别的用例撞。
- **必须是 https**：产品只接受 `https://`（`OnlineUpdate.cpp` 的 `StartsWithHttps`/`ParseHttpsUrl`），
  明文 http 会在"基址不可用"上直接失败。所以夹具自带一张自签名证书
  （`Run\certs\yuaextract-fixture.cer`，SAN = `localhost` + `127.0.0.1`）并把它装进
  **`Cert:\LocalMachine\Root`** —— **WinHTTP 只认机器证书库，不读用户库**（只导 CurrentUser 会报"TLS 握手没通过"）。
  证书跨轮复用；要清掉用 `-UninstallCertificate`。
- **怎么造坏**：坏法写在夹具目录里的 `_faults.json`（服务器只负责照做）：

  ```json
  {
    "index.txt":                      { "mode": "404" },
    "YuaHarnessOnline/OnlineWork/rules.int": { "mode": "truncate", "ratio": 0.5 },
    "*":                              { "mode": "hang", "seconds": 40 }
  }
  ```

  `mode`：`404` / `status`（指定状态码）/ `hang`（收下请求什么都不发，打接收超时 30s）/
  `truncate`（只发前 ratio，`Content-Length` 也按截断后的长度报）/ `short-body`（`Content-Length` 报全长、只发一半就断）/
  `garbage`（200 + 随机字节）/ `slow`（分段慢慢发）。`*` 是兜底。
- **请求日志**：每个请求一行（路径 / 坏法 / 状态 / 字节数 / 耗时），断言用它证明"产品到底请求了哪些地址"，
  也是"没有访问外网"的旁证。
- **收尾**：编排脚本在 `finally` 里停服务，并额外给服务加了 `-IdleTimeoutSec` / `-ParentPid` 两道自杀保险。

`make-online-repo.ps1` 负责把**产品真档案**摆成远端仓库（一个用例一个仓库根）：

| 用例 | 造法 |
|---|---|
| `good` | 2 行索引：① 第三个游戏的档案（本地没有 → 新增）② 主档案（本地已有同一份 → 已是同一份） |
| `bad-sha256` | 索引里的档案摘要改开头一位 |
| `truncate` | 档案只发前一半（索引摘要仍是**完整**档案的 → 被"字节≠索引摘要"拦下） |
| `truncate-declared` | 截断 **且索引摘要按截断后的字节算** → sha256 过得去，必须靠**结构解码**拦下 |
| `short-body` / `garbage` | 短读 / 随机字节 |
| `salt-mismatch` | URL 位置上摆**别的游戏**的档案，索引那行仍写主档案的目标 exe 摘要 |
| `index-cols` / `index-empty-publisher` / `index-bad-digest` / `index-empty-digest` | 索引行少一列 / 会社为空 / 摘要不是 hex / 档案摘要为空 |
| `missing-archive` / `no-index` | 档案不存在 / 连索引都没有（真 404） |
| `http-base`（不需要服务） | 基址给成 `http://…`：产品只走 https，必须在**发请求之前**就拒（连都不会连） |
| `slow` | `index.txt` 拖住 40 秒不回应（打接收超时） |

索引的每一列都由 `make-online-repo.ps1` **独立算出来**（会社/作品 与目标 exe 摘要取自档案 META/IDENTITY、
盐摘要 = `sha256(salt)`、条数 = STATS 的 `directories+files`、档案摘要 = 文件 sha256），
断言那侧再自己算一遍（`rules-int.py dump` + 文件 sha256），两边不共用一份结果。

## 用例与判据

| 断言 | 判什么 | 期望值来自哪 |
|---|---|---|
| `E-12` | 页签骨架：4130-4136 与共用列表/详情可见、首页与制作页签控件全隐、**切回去同样全隐**（三路取样，set-1/set-2 可见集合必须一致） | 实测可见性矩阵（driver 逐控件记录） |
| `E-12-a` | **扩展集页签的像素幽灵主判据**：4 路 set 抓图两两一致（阈值 0.10%，与 E-11 同口径） | `run-tab-pixels.ps1` 的抓图快照 |
| `E-12-C/E/F/T` | 噪声地板（连拍）/ 抓图有效性（非空白 + 与首页/制作页 ≥5% 不同 + 4131 可见）/ 阈值可信性 | 同上 |
| `E-13` | 刷新列表：空库 / 1 条 / 多会社多作品 → 4083/4084 的**内容与计数**与独立解析的库清单一致；面板的「共 N 个会社 / M 份扩展集」也要一致 | 断言自己解析 driver 当场拷的 `manifest.txt` 快照 |
| `E-14` | 选中项的每个字段（会社/作品/条数/目标 exe 摘要/盐摘要/档案摘要/生成时间/安装时间/来源/库内目录/档案路径）与 **清单行 + 独立解析的档案** 逐项一致；两边还要互相印证 | 清单（断言解析）+ `rules-int.py dump` |
| `E-15` | 打开选中扩展集：无选中给提示、目录缺失明确报错、正常打开时面板与**本次日志尾部**都写出「已打开 <路径>」且路径逐字符相等 | 界面选中的会社/作品算出的目录 |
| `E-16` | 运行环境自检：正常全绿；**故意造**（坏档案 / 档案改一字节 / 清单少一列 / 清单摘要非 hex / 库根不可写 / 缺 DLL）每一项都必须报出；自检前后库根与工具目录（除 Log）的树摘要**逐字节相同** | 变体期望对照产品真实措辞 |
| `E-17` | 扩展集留言只读弹窗里出现选中档案 `META.comment`（独立解析核对）+ 会社/作品 + 「无法校验名字对不对」的如实说明 | `rules-int.py dump` 的 META |
| `E-18` | 在线更新闭环：索引 → 差异分类 → 下载 → 校验 → 入库 → 库清单多一条；**重复更新幂等**（不再产生条目）；坏 sha256 / 截断 / 截断且索引自洽 / 短读 / 乱字节 / 盐不一致 / 索引格式错 / 缺摘要 / 档案 404 **一律拒**，且本地库与库清单**逐字节不变**、暂存目录不留东西 | 断言的独立解析（索引 TSV + 档案）+ 库清单前后对比 |
| `E-19` | 端点不可达（要**快**：≤20s）/ 404 / 超时（要真的等到接收超时：≥25s）/ **明文 http 基址必须被拒**（§14.4 第 1 条）各自明确措辞；本地一个文件都不动 | 同上 + 用时上下界 |

**E-18 里"坏用例"有一条前置复核**（S-1 的教训）：如果本地已经有索引里那一份，产品会在下载**之前**
就短路成「本地已经是同一份，无需下载」，那条用例就什么都没测到 —— 这类记录直接判**用例无效**（失败），
不当作"被拒了"。编排里也据此把**负例排在正例之前**（正例会把远端那条装进库）。

## 实测（构建 `FAEB6760`，2026-10-16）

| 断言 | 实测 |
|---|---|
| E-12 | set-1/set-2 各 18 个控件可见、集合完全一致；切回首页后 4130-4136 全隐 |
| E-12-a | **六组两两比对全部 0 px（0.000%）**，阈值 0.10% —— 扩展集页签没有幽灵，也没有像首页那样的"合法重绘"噪声（4071 每次到达都是同一段文字） |
| E-12-F | set 图 vs 制作页 **42.56%**、vs 首页 **37.71%**（远高于 5% 的正对照下限） |
| E-12-C | 同状态连拍 **0 px** |
| E-13 | 空库 0/0；1 条 1 会社/1 作品；多会社多作品 2 会社（`YuaHarness` / `YuaHarnessOnline`）/ 2 作品；列表文字与独立解析的清单逐项一致 |
| E-14 | 面板每一项都等于「清单行」且等于「档案本体」（735 字节 / DIRS 5 / FILES 15 / sha256 `A610B9440A4E6952…`） |
| E-15 | 面板与日志都写出 `已打开 …\ExtensionLibrary\YuaHarness\otomeki_ckr`；无选中/目录缺失两条路径的提示都对 |
| E-16 | 7 个变体全报出；库根与工具目录（除 Log）的树摘要前后全等（自检确实只读） |
| E-17 | 弹窗里出现 `META.comment`（独立解析得到）+ 会社/作品 + 「无法校验」说明 |
| E-18 | 库清单 **1 → 2 行**，新增那一条与远端索引/档案逐项一致；第二轮「已是同一份」→ 仍 2 行；**11 个坏用例全部按各自理由被拒，本地库与清单逐字节不变、暂存目录为空** |
| E-19 | 不可达 **2.9 秒**报"连不上服务器"；404 **0.8 秒**报"服务器上没有这个文件（HTTP 404）"；慢响应 **32.6 秒**报"等不到对方的回应（超时：连接 10000 / 发送 15000 / 接收 30000 ms）"；http 基址立刻报"基址不可用…本工具只在 https 上拉扩展集"——四种措辞各不相同（§14.6）。**观察**：http 那条的措辞里基址显示成 `""`（空的），弹窗的「仓库地址」也是空的 —— `NormalizeBase` 把非 https 折成空串，用户看不到自己填的地址（已知小瑕疵，已报 lead，未按缺陷拦断言） |

自检：`self-test-extension.ps1` **55 个场景全部通过**（31 个既有 + 24 个新）。新场景里有六条是专门打
"最容易假通过"的：索引格式错被当成**远端为空**、坏 sha256 被放行、自检坏档案**不报**、
E-19 有人**偷偷写了本地库**、列表**文字读不到**（MSAA 退化）、面板撒谎。

## 本轮踩到的坑

37. **产品只接受 https**（`OnlineUpdate.cpp` 的 `StartsWithHttps`）：本地夹具**不能**是明文 http ——
    否则每个用例都会在"基址不可用"上炸，看起来像产品缺陷。夹具因此自带自签名证书并装进
    `LocalMachine\Root`（**WinHTTP 只认机器库**；只导 CurrentUser 会报"TLS 握手没通过"）。
38. **列表框文字只能走 MSAA**（脚手架缺陷，产品无责）：`LB_GETTEXT` 的 lParam 是**跨进程指针**，
    实测远端缓冲区**一个字节都收不到** —— `SendMessageW` 直接回 `LB_ERR`，
    `SendMessageTimeoutW` 回"复制了 N 个字符"但缓冲区原封不动（预填 `0xAA` 读回来还是 `0xAA`）。
    后果是"列表里明明有会社"却读到空串，**而计数（`LB_GETCOUNT`）完全正常** —— 只看计数会以为一切正常。
    现在统一走 `Get-MsaaListItems`（oleacc 的 `accName`，编组的 COM 调用；列表项是 `1..accChildCount`）。
39. **`'A' * 64` 不是"重复 64 次"**：PowerShell 里它是**64 个元素的数组**（Python 习惯会踩）。
    造"摘要不是 hex"的坏行时这么写，`-f` 直接抛"无法将 `Object[]` 转换为 `UInt32`"。
40. **RichEdit 的 `WM_GETTEXT` 可能只给 CR**：面板字段解析不能假定 `\n` 分行 ——
    `(?m)^` 只在整段开头匹配，结果所有字段都读成空（E-14 一开始全红就是这个）。改用 `(?:^|[\r\n])` 当锚点。
41. **`-join` 与 `-ne` 混在一个表达式里**（`@($a) -join "`n" -ne @($b) -join "`n"`）实测两边内容相同也报不一致。
    先落成变量再比。
42. **负例必须在库干净时跑**：正例会把远端那一条装进本地库，之后"截断/短读/乱字节"这些用例会命中
    「本地已经是同一份 → 无需下载」而**根本不走下载路径**（实测三条全变成"没测到"，而且看起来还是绿的）。
    现在编排把负例排在正例之前，断言另加一道前置复核。
43. **`@(...)` 又不能省**：在线更新列表里**恰好一项**时 `Get-DialogListItems` 的返回值被解包成标量，
    紧接着的 `.Count` 在 StrictMode 下抛"在此对象上找不到属性 Count" —— 六个"索引只有一行"的用例全栽在这上面。
44. **`Substring(0, 12)` 在短字符串上会抛**：合成世界里的摘要只有 10 个字符，一抛就**整支断言提前结束**，
    表现成"所有场景都失败"（55 个场景集体红）。统一走 `Get-ShortText`。
45. **工具日志是跨用例累积的**：拿整份 `Log\*.log` 去判"产品这次说了什么"，会把上一条用例写下的行
    当成本条的（S-1 那类假通过）。驱动现在记下启动时的日志偏移，收尾只取**本次新写的那一段**（`toolLogTails`）；
    在线更新的判据更是只用**本条用例自己的状态文字与弹窗**。
46. **自检的坏档案自己造**：`run-extension-bad-archives.ps1` 也会造一份 `rules-int.py mutate` 产物，
    但两条编排不该互相依赖 —— 在线更新编排自己造在 `Run\out\online-bad-archives\`。
47. **E-16 现在覆盖了"清单有坏行"**（2026-10-15 那轮补丁之后的行为）：塞一行 9 列的坏行 →
    面板「清单提示」写"清单中有不合规的行已被跳过：第 N 行：列数不对…"；塞一行"摘要不是 hex" →
    整份拒（"库清单格式不对…"）。两条都必须被自检报出来（之前是静默）。
48. **超时用例必须真的等到超时**：夹具"拖住不返回"要卡在产品的接收超时（30s）以外，
    断言同时要求**用时 ≥ 25s**（否则可能是别的原因报出来的"超时"）与 ≤ 300s；
    反过来"端点不可达"要求 ≤ 20s（实测 2.9s）—— 不可达却在等一整轮接收超时同样是缺陷。
49. **扩展集页签的像素矩阵不能混进 `set-visit1`**：那是进程 1 里 E-11 的"扩展集→制作"过渡状态，
    不属于本页签矩阵；混进来等于拿"经过制作页签之后"的图去比干净基线（自检里那张甚至就是首页图）。
50. **跨进程读模态框会撞上"窗口已存在、子控件还没建完"那一瞬**（脚手架缺陷，产品无责）：
    实测抓到过 —— 产品的「强行使用扩展集（未校验）」确认框（`是`/`否`）被读成"只有 1 个按钮、
    正文为空"，于是被当成信息框**直接点了「是」**，产品按"不校验、强行使用"继续走，
    E-4 的"盐不匹配"用例就变成了"没被拒"（还产出了 2 行的 `StaticHash.lst`）。
    现在读一次之后若"没有按钮"或"没有正文"，等 250ms 再读一次，取更完整的那一份；
    并且只有**确实只有一个按钮控件**时才走"信息框直接点"那条路（按"有文案的按钮"数会数错）。
51. **编排自己的前提步骤不许写别人的结果文件**：`run-extension-online.ps1` 的 setup 为了
    "库里得先有一份"会补跑一次安装 —— 原来写成 `-OutJson` 指到 run-all 的 `extension-old.json`，
    把带 `dryrun`（E-3 要的干跑文本）和幂等那一轮记录的文件覆盖掉了，E-3/E-7 直接变红。
    现在它写自己的 `extension-old-online-install.json`（只读别人的，不写别人的）。
52. **`{ ... }` 单独摆在语句位置不是"块"，是"脚本块字面量"**：既不执行、还会被打印到 stdout
    （驱动日志里突然出现一段自己的源码就是这么来的）。要就写成 `if (...) { ... }`。

---

## 修复轮：独立验证（task-5）打穿的两条 + P-1 回归（2026-10-16，构建 `71BF4A17`）

> **当前最终构建 = `71BF4A17` / 1,182,720 B**（标题栏 `YuaExtract 1.0.0 [71BF4A17]`）。
> 上面那些 `6B664840` / `492522BD` / `FAEB6760` 是**历史取证与标定依据**，一律保留不改。

独立验证报告 `VERIFICATION-扩展集页签与在线更新.md` 里，产品侧 6 条（P-1..P-6）与脚手架侧 8 条（S-1..S-8）。
下面只记**脚手架侧**已修的部分与它带来的新增覆盖。

### S-1（已修）E-19 的 http 用例是弱判据 —— 它打印过一次假绿

旧判据只有 `@('只允许 https','不是 https','基址不可用')` 三个 needle，而前两个在"基址不可用"那条分支里
**永远不会出现**（`NormalizeBase` 在 `ParseHttpsUrl` 之前就把非 https 折成空串），实际只靠最泛的第三个撑着；
验证者把 `Endpoint` 换成 `ftp://…` 后**照样报绿** —— 这条断言区分不了"拒绝明文 http"与"拒绝一切非 https"，
却在结论里声称验了前者。

**改法（结构化判据，不再靠单一措辞）**：

| 条件 | 含义 |
|---|---|
| ① **回显原址**：错误文字里必须出现我们传进 `YUAEXT_ENDPOINT` 的那串原文 | "它拒绝的是这个地址"的直接证据（承载判别力的是这一条） |
| ② 点到只接受 https（`SpecificShapes` 任一：`只允许 https:// 开头的地址` / `不是 https 的地址` / `只在 https 上拉扩展集` …） | "它把只接受 https 说出来了" |
| ③ `Forbid`：不许出现 DNS / 证书 / 连不上 / 超时 / 404 / 索引格式 | 证明它在**发请求之前**就拦了 |
| 补：若文字只是泛化兜底形态（含"地址也不能为空"）且没有原址 → 直接判失败并**点名**（S-1 的现场） | 泛化兜底单独不得通过 |

**自检场景 `e19-http-base-generic-word`**：喂一份"只有泛化兜底那一句、没有原址"的记录（= 旧构建对 http/ftp 的**实际输出**）
→ E-19 必须炸。这就是 S-1 的"凭什么能炸"，也是"旧判据会假绿"这件事被钉住的证据。
实测（`71BF4A17`）：`基址不可用：只允许 https:// 开头的地址，你填的是 "http://127.0.0.1:53092/" —— 它不是 https 的地址。`
两侧都命中（原址 + 形状），用时 0.8 秒。

### S-2（已修）库目录不存在时驱动**崩**（不是失败，是未捕获异常）

`Get-LibraryInfo` 在 `<工具目录>\ExtensionLibrary` 不存在时返回 `Root = ''`，随后
`Get-TreeDigestExcluding -Path ''` 撞上 `[Parameter(Mandatory)][string]$Path` 的空串绑定校验 →
`ParameterBindingValidationException` → **驱动在写任何结果文件之前就死了**。
而"库还不存在"正是**首次安装**（在线更新第一次把库建出来）唯一的现场 —— 这条产品路径以前**测不到**。

- 改在 `lib\HarnessUtil.ps1` 的 `Get-TreeDigestExcluding`（`[AllowEmptyString()]` + 空/不存在返回 `Exists=$false`）；
  `Get-FileListing` 同样容忍空路径。**驱动里那份重复实现已删掉**（同一件事两份实现、只修一份，正是这次崩的土壤）。
- 新用例 `online-first-install`（编排里 `-HideLibrary`：跑前把库整个挪开、跑完无论成败都挪回来）+
  E-18 里一段专门的判据：跑前 `libraryRoot='' / Exists=false / 清单 0 行`（**前置复核**：库真不在才算数）→
  跑后库被建出来、清单恰好 1 行、那一行与远端索引逐项一致。
- **自证**：断言里的 `X-7` 当面复现这条 —— 空串与不存在的路径都不得抛异常，**并且**证明
  `[Parameter(Mandatory)][string]` 的旧签名对空串确实会抛（否则这条自证没有意义）。
- 实测（`71BF4A17`）：`before: root='' rows=0 exists=False` → `after: root=<工具目录>\ExtensionLibrary rows=1`，
  装完的结论是 `已新增：YuaHarnessOnline / OnlineWork（2 条）`。

### S-3（已修）「关于」页签（4004）零覆盖 → 挂成 `E-12-b`

`settab` 步骤现在多取一份「关于」页签的骨架与正文（并在**去过关于页签之后**再回扩展集页签取一次样 `set-3`，
用来证明 S-3 没有引入新的残留）。E-12-b 判三件事：

1. 骨架：只有 `4001-4004`（页签）+ `4071`（内容视图）+ `4072`（日志按钮）可见，
   `4130-4136`/`4080-4084`/`4100-4111`/`4030-4032`/`4050-4052`/`4085-4088`/`4010,4020,4021,4022,4070` 全隐；
2. 正文里必须有 `构建指纹` / `SHA-256` / `免责声明` / `不要用于传播游戏资源`；
3. **构建指纹栏里必须是这次跑的那个 exe**（前 16 位 + 字节数）—— "这轮的绿属于哪个构建"要能在页面上读到。

自检场景三条（`e12-about-missing-fingerprint` / `-missing-section` / `-leaks-set-control`）。
实测（`71BF4A17`）：关于页签可见 6 个控件，正文 1062 字，含 `71BF4A1738182F47` 与免责声明。

### P-1 回归（已修）`salt-mismatch` 会被新加的名字校验**抢答**

task-1 给 `Online::VerifyArchive` 加了 §14.4 第 9 条：索引行的 会社/作品 必须等于档案内嵌 META 的会社/作品。
而夹具里 `salt-mismatch` 原本把索引行**覆盖成主档案的名字**、位置上却摆着对照档案（META 是它自己的名字）——
也就是"索引名 ≠ META 名" → 新校验会**先**拒它，那条用例想考的"内嵌 exe 摘要对索引"根本走不到
（= 因为错误的原因被拒，与 S-1 同类）。

- 修：`salt-mismatch` 的索引名与位置都改成**对照档案自己的**名字，只把 `目标exe摘要` 那一列换成本档案的 →
  唯一错的地方就是 exe 摘要。**实测被拒理由是 `档案内嵌的目标 exe 摘要与索引不一致…`** ✓（不是名字那条）。
  另加 `Forbid`：这条用例的文字里**不许**出现名字校验的措辞（`索引说` / `META 说`）。
- 新用例 `index-name-mismatch`（**只错名字**：索引写 `IndexSaysThis/AndThis`，档案 META 是它自己的名字；
  档案摘要 / 目标 exe 摘要 / 条数**全部与档案真实值一致**，所以别的防线都过得去）+ 结构化判据：
  错误文字必须**同时点名两边**（索引名 与 档案 META 名都从仓库独立解析出来），缺一边判失败。
- 自检场景 `e18-name-mismatch-wrong-reason`：把这条用例的被拒理由换成**别的防线**的措辞（只说"档案摘要不符"）
  → E-18 必须炸。这就是"只判'被拒'区分不了原因"的回归。
- 实测（`71BF4A17`）：`索引与档案不是同一条，已丢弃：两边的会社 / 作品对不上。索引说：IndexSaysThis / AndThis / 档案自己的 META 说：YuaHarnessOnline / OnlineWork`，
  本地库与清单逐字节不变、暂存目录为空 ✓。
- 其它用 `-PublisherOverride/-TitleOverride` 的地方：全仓库只有这一处（已改）；新用例是有意用
  `Copy-ArchiveInto -Publisher/-Title` 把档案摆到"索引名"的路径下 —— 那正是它的考点，已在注释里写明。

### P-2 / P-3 对断言的影响

- **P-2（三态措辞）**：E-19 的判据按**实测措辞**收紧（见 S-1 表），`ftp://` 这类同样非 https 的输入
  现在会逐字得到 ② 并**回显原文**；`ResolveUsableBase` 在匿名命名空间里、公共 API 未变，驱动没改。
- **P-3（清单「来源」列改成远端 URL）**：E-14 校验的「来源」是**本地安装**那条路径的档案来源（未变）；
  E-18 不校验「来源」列内容，所以没有期望值要改。**记在这里以免以后误判。**

### S-4（已修）汇总的"通过数"分开报

`总计：通过 N` 是逐 check 数的，里面既有产品断言也有**脚手架自检**（`checks-selftest*.json`）。
现在多打一行 `其中：产品断言 通过 X / 失败 Y；脚手架自检 通过 Z / 失败 W`（判据是文件名前缀 `checks-selftest`）。
护栏自检里钉了一句 needle（`其中：产品断言`），防这一行被悄悄改掉。

### 这一轮新增的自检场景（55 → 61）

| 场景 | 做错法 → 该炸 |
|---|---|
| `e19-http-base-generic-word` | 只给泛化兜底措辞（无原址）→ E-19（S-1 的自证） |
| `e12-about-missing-fingerprint` / `-missing-section` / `-leaks-set-control` | 关于页签缺指纹 / 缺构建指纹栏 / 泄漏扩展集控件 → E-12-b |
| `e18-first-install-lies` | 首次安装后库里还是 0 行 → E-18 |
| `e18-name-mismatch-wrong-reason` | 名字不一致那条被换成别的防线的措辞拒 → E-18 |

### 本轮踩到的坑（续）

53. **夹具的"索引名 vs 档案 META 名"必须当成契约**：产品新加了交叉校验（§14.4 第 9 条）之后，
    任何"把索引行名字覆盖成别的名字"的夹具都会被**那条校验先拒**，于是用例表面绿、实际考的是别的防线
    （与 S-1 同类的"因为错误的原因通过/失败"）。现在只有 `index-name-mismatch` **刻意**不一致，其余一律一致。
54. **判"因为某个原因被拒"必须要求"该原因特有的证据"**：只判"被拒"= 判据可以被任何别的防线满足。
    这一轮把两条这样的用例改成了结构化判据（http 那条要求**回显原址**；名字不一致那条要求**点名两边**），
    并各配一条"换成别的理由 → 必须炸"的自检场景。**照这个口径扫过的同类断言**：
    E-4 的坏档案用例（本来就有"专属措辞 + 排除他类防线"两道，未受影响）、E-16 的六个自检变体
    （每个变体有自己的期望措辞，且要求"库根与工具目录摘要前后全等"）、E-18/E-19 其余各条
    （各有专属措辞，且都要求"本地逐字节不变"）—— 没有发现别的同类漏洞。
55. **同一件事别留两份实现**：S-2 崩的土壤是"树摘要"在 `lib\HarnessUtil.ps1` 与驱动里各有一份，
    只有一份被改过。现在实现只留 lib 里那一份。
56. **跨进程读模态框可能读到"半成品"**：见前文踩坑 #50 —— 驱动现在是"读一次不够完整就等 250ms 再读"，
    并且只有**确实只有一个按钮控件**才走"信息框直接点"。

### 顺带收紧（同类风险扫描的结果）

按协调者要求把"靠关键词判'因为某个具体原因被拒'"的断言全扫了一遍，只有一处需要收紧：

**E-16 的自检变体**原来是在**整段结论里**找关键词 —— 那么"别的项目/别的原因凑巧带同一个词"就会
因为**错误的原因**通过。现在每个变体多记一条 `Owner`（该由**哪一项**报出来：`库内档案` /
`库清单 manifest.txt` / `扩展集库目录` / `Hash 收集模块 CxdecStringDumper.dll`），判据收在
**该项自己的那一段**（`· 项名：…` 到下一个 `· ` 之前）里；那一段根本不存在也判失败。

自检场景两条（`e16-selfcheck-wrong-owner`：把「解码失败」塞进"工具目录可写"那一项；
`e16-selfcheck-drops-item`：该报的那一项**根本没报**）—— 旧写法在这两种情况下都会假绿，现在必须炸。
实测（`71BF4A17`）：7 个变体全部"由它自己那一项报出来"。

**扫描结论（其余同类断言）**：

| 断言 | 判据形态 | 结论 |
|---|---|---|
| E-4（坏档案） | 专属措辞 + **排除他类防线措辞** + 前置复核（独立解析档案确认它真是那一类）+ 无 StaticHash.lst | 本来就有两道，未受影响（实测 7/7，`他类防线措辞 []`） |
| E-13 / E-14 / E-17 | 不靠关键词：与**独立解析的清单/档案**逐项比字段（E-14 连"数据层自洽"也一并验） | 无同类问题 |
| E-15 | 要求**路径文本逐字符相等**（面板 + 日志尾部），并**禁止**在失败路径出现「已打开」 | 正反两面都有，无同类问题 |
| E-16 | 见上（已收紧） | 已修 |
| E-18 / E-19 其余各条 | 每条有专属措辞 + **本地逐字节不变** + 用时上下界；http 与名字不一致两条另加结构化判据 | 无同类问题 |

### 这一轮的最终实测（构建 `71BF4A17`）

```
pwsh -File '_verify
estore-harness
un-all.ps1' -Scope extension
总计：通过 98 / 失败 0 / 提示 0
      其中：产品断言 通过 35 / 失败 0；脚手架自检 通过 63 / 失败 0
```

| 项 | 实测 |
|---|---|
| E-4 | 7/7 按各自理由被拒，`他类防线措辞 []`，`StaticHash.lst 存在=False` |
| E-12-b | 关于页签可见 6 个控件（`4001-4004` + `4071` + `4072`），正文 1062 字含 `71BF4A1738182F47` 与免责声明 |
| E-16 | 7 个变体全部**由各自那一项**报出；库根与工具目录（除 Log）摘要前后全等 |
| E-18 | 正例 1 → 2 行、第二轮幂等；**首次安装**（库从"不存在"被建出来、清单 1 行对得上索引）；12 条坏用例全拒、本地逐字节不变 |
| E-19 | 不可达 2.9s / 404 0.9s / 超时 32.6s / http 基址 0.9s（**回显原址** + 点明只接受 https，形状命中 3 处） |
| X-7 | 空路径与不存在的路径都不再抛；旧签名对空串确实会抛（S-2 的现场） |
| 自检 | **63 / 63** 场景全部通过 |

---

## 第三轮：判据"比它声称的弱"的三条（2026-10-16，构建 `9995400F`）

> **当前最终构建 = `9995400F` / 1,188,352 B**（标题栏 `YuaExtract 1.0.0 [9995400F]`）。
> 更早的 `6B664840` / `492522BD` / `FAEB6760` / `71BF4A17` 都是**历史标定/取证**，保留不改。

独立验证在「修复轮增量复核」里又挖出三条同一族的问题：**判据声称的比它实际能判的弱**。
三条都改了，并且每条都配了"喂一个**能骗过旧判据**的输入 → 必须炸"的自检场景。

### N-S1（已修）树摘要只枚举文件 → 对"新建/删除空目录"完全盲

`Get-TreeDigestExcluding` 原来只 `Get-ChildItem -File`，于是产品在失败路径上留下
`ExtensionLibrary\CON\`、`AB\T\` 这种**空目录**时，"本地一个字节都没改"照旧成立 ——
**一整族安全判据在"空目录"这一类改动上是假绿**。

**改法**：摘要同时枚举**目录**（含空目录；相对路径带 `D:` 前缀）并折进 `Digest`；
返回值现在是 `Count`（文件）/ `DirCount`（目录，含空）/ `Digest`（文件+目录，**默认口径**）/
`FileDigest`（旧的只比文件口径，留作对照）/ `Exists`。`-FilesOnly` 可以显式退回旧口径 ——
**谁要"只比文件"就必须自己说出来**，不许默认偷偷弱化。`Get-FileListing` 同样加了 `-IncludeDirectories`
（暂存目录"不留东西"也要看得见空目录）。

**哪些断言因此获得了目录维度**（这是 N-S1 的"影响面"清单）：

| 断言 | 判据 | 现在比什么 |
|---|---|---|
| E-16（7 个变体） | 自检是只读的 | 库根与工具目录（除 Log）的 `Digest` + **文件数** + **目录数**（措辞也从"不改任何文件"改成"**不改任何文件或目录**"） |
| E-18（12 条坏用例） | 被拒的同时本地没动 | 库摘要 + 清单文本 + 清单行数 + **目录数** |
| E-18（首次安装） | 库从"不存在"被建出来 | before `Exists=false` / after 真有那个库目录 |
| E-18（正例） | 暂存目录不留东西 | 暂存目录的**条目**（含空目录） |
| E-19（4 条） | 本地一个文件都不动 | 库摘要 + 清单行数 + **目录数** |
| X-8（新增，函数级自证） | 摘要必须看得见空目录 | 两棵"文件完全相同、只差 3 个空目录"的树：`Digest` 必须**不同**，`FileDigest` 必须**相同**，`-FilesOnly` 必须给**相同**摘要（旧口径的盲区被复现，自证才有意义） |

自检场景 `e16-touches-empty-dir`：合成一份"文件全等、只多一个空目录（摘要字符串故意相同、
只有目录数变了）"的记录 → E-16 必须炸。

> **回归风险（协调者点名）**：加了目录维度之后，如果产品在别处**合法地**新建目录，某些原本绿的
> "前后一致"判据可能变红。本轮的实测**没有出现**这种合法变红（E-16 七个变体 + E-18/E-19 全绿）。
> 一旦出现，**不许回退维度**，要在报告里点名交判。

### N-S2（已修）`NameMismatch` 的索引侧判据恒真

`Get-OnlineCaseText`（在线用例"本条自己的话"）原来**把远端列表行也拼进去**了
（`<索引会社> / <索引作品> / N 条 / 状态`）—— 索引那侧的会社/作品**永远在场**，
于是"必须点名两边"这条判据的索引侧**恒真**：验证者造 `meta-only-synthetic`
（状态文字只点名档案那一侧）时 E-18 **照样报绿**。

**改法**：`Get-OnlineCaseText` 只取**结论文字**（`statusAfterRefresh` / `statusAfterInstall` / 弹窗），
**不掺远端列表行、也不掺累积日志**。需要列表内容的判据（E-18 正例的"列表里能看到
`<会社> / <作品> / N 条`"）直接读 `listItemsAfterRefresh` 字段，不走这里。

自检场景 `e18-name-mismatch-meta-only`：状态文字只点名档案那一侧 → E-18 必须炸（旧判据会假绿）。

**顺带扫了一遍"判据读的文本里混进本来就会出现的行"**：E-13（读记录字段，不是文本）、
E-14（逐字段比）、E-15（路径文本 + 本次日志尾部）、E-16（已收紧成"由该项自己那一段报出来"）、
E-17（只读弹窗正文）、E-19（结论文字）—— **没有别的同类问题**。

### E-14 的路径判据口径与产品不一致（已修）

产品落库时走 `MakeSafePathComponent`（去掉 `< > : " / \ | ? *` 与控制字符、再去首尾空格/点），
而断言的期望路径原来拿 manifest 里的**原始名** `Join-Path` —— 夹具用普通名字时 raw == 改写后，
判据碰巧成立；名字一旦会被改写（`A:B` → `AB`）就指向一个**不存在的路径**（假红，或两边都错成一样而假绿）。

四步都做了：

1. **口径对齐**：`lib\HarnessUtil.ps1` 新增 `ConvertTo-SafePathComponent`（逐条镜像产品的规则），
   E-14 与驱动里所有"拼库内路径"的地方（`setopen` 的期望目录、`setcheck` 的库内档案路径）
   一律走它。产品侧 P-4 之后面板给的就是这个口径。
2. **真造一个会被改写的名字走了一遍详情面板**（用例 `hostile-install` + `set-hostile`：
   会社 `A:B`、作品 `Yuzu/Soft`，用 `-SetPublisherName` 按**文字**选中而不是写死序号）。
   实测（`9995400F`）：
   - 面板「库内目录」= `…\ExtensionLibrary\**AB\YuzuSoft**`（sanitize 后），**磁盘上真有这个目录**（逐字符 + `Test-Path` 都过）；
   - 面板同时给说明「**库里的目录名是改写过的**：名字里的 \ / : * ? " < > | 之类字符被去掉了…」；
   - 面板里**没有**出现"拿原始名拼出来"的那条路径（`…\A:B\Yuzu\Soft`，它不存在）。
   断言落在新的 **E-14-b**（含"用例无效"前置复核：sanitize 前后必须真的不同，否则这条没测到东西）。
3. **E-15 等同类扫过**：E-15 比的是"驱动器记下的期望目录"与"面板/日志里的文本"，
   而那个期望目录现在也走 sanitize ✓（否则产品显示 sanitized 目录、断言拿 raw 拼，同样会错）。
   其余拼库内路径的地方（`setcheck` 的库内档案）一并改了。**没有别的同类。**
4. **自检场景 `e14b-raw-join`**：把面板退回"原始名拼接"（且没有改写说明）→ E-14-b 必须炸。

### N-P2 的第二层措辞也纳进来了

基址措辞现在是三层（① 地址为空 / ② 前缀是 `https://` 但缺主机名 / ③ 前缀不是 `https://`）。
新增用例 `online-https-no-host`（基址 `https:///`）：判据要求点到"**缺少主机名 / 基址不完整**"，
并且**禁止**出现 ③ 那层的措辞（`不是 https 的地址`）—— 两层不许互相顶替。
自检场景 `e19-https-no-host-wrong-layer`：把 ② 的输入喂成 ③ 的措辞 → E-19 必须炸。

三处产品侧收窄的措辞（`只读检查` → 「只做检查、不在任何地方留下改动」、`本地一个字节都不改` →
「库与库清单一个字节都不改」、`没有写入任何东西` → 「没有写入本地库」）**没有断言 needle 压在上面**，
已核对过（断言压的是"无法校验 / 构建指纹 / 免责声明 / 已打开 / 解码失败 …"这些）。

### 本轮新增的自检场景（63 → 67）

| 场景 | 做错法 → 该炸 |
|---|---|
| `e16-touches-empty-dir` | 文件全等、只多一个空目录 → E-16（N-S1 的自证） |
| `e18-name-mismatch-meta-only` | 状态文字只点名档案那一侧 → E-18（N-S2 的自证） |
| `e14b-raw-join` | 面板给"拿原始名拼出来"的路径 → E-14-b |
| `e19-https-no-host-wrong-layer` | 缺主机名那条被喂成"非 https"那层的措辞 → E-19 |

### 本轮踩到的坑（续）

57. **`Get-ListItemsByHandle` 返回的是对象、不是字符串数组**：拿它当下标数组去比列表项文字，
   永远匹配不上（`[']`）。要列表文字就用 `Get-MsaaListItems` 的 `.Items`，或者逐项 `Get-ListItemText`。
58. **刷新之后列表可能还没填上**：库清单里有行、列表读到 0 项 → 那是"刚点完还没填"，
    不能当成空库（`setbrowse` 现在会等/重试，并且只在"清单有行"时才等）。
59. **同 exe 摘要换槽位时，产品只从清单里去掉旧行、不会删旧目录**（本轮观察，非缺陷判定）：
    夹具里"先装在线那份、再用同一游戏的不同名字装一份"就会看到库里多出一个
    "有目录、清单没行"的条目（界面上会补列出来，点它时面板会说"库清单里没有这一条"）。
    E-7 的幂等只判**清单**不产生重复条目 ✓ 不受影响；这条只是**记下来**，交 lead 判是否需要产品侧收口。
60. **MSAA 读列表要重试**：跨进程读偶发拿到空结果（同一控件前后两次调用一次 Ok 一次不 Ok），
    读一次空就判"找不到会社"会把偶发读失败当产品缺陷 —— 现在会重试 5 次（每次隔 400ms）。

### 第三轮的最终实测（构建 `9995400F`）

```
pwsh -File '_verifyestore-harnessun-all.ps1' -Scope extension
总计：通过 104 / 失败 0 / 提示 0
      其中：产品断言 通过 37 / 失败 0；脚手架自检 通过 67 / 失败 0
```

| 项 | 实测 |
|---|---|
| E-12-b | 关于页签可见 6 个控件；正文 1062 字含 `9995400F7709515C` 与免责声明 |
| E-14-b | 原始名 `A:B / Yuzu/Soft` → 落库名 `AB / YuzuSoft`；面板的「库内目录」= 磁盘上真实存在的那一级（逐字符 + Test-Path） |
| E-16 | 7 个变体全部由**各自那一项**报出；库根与工具目录（除 Log）的**摘要 + 文件数 + 目录数**前后全等 |
| E-18 | 正例 1 → 2 行、第二轮幂等；**首次安装**（库从"不存在"被建出来、清单 1 行对得上索引）；13 条坏用例全拒、本地逐字节不变 |
| E-19 | 不可达 2.9s / 404 0.8s / 超时 32.5s / **缺主机名 0.8s（与"非 https"两层措辞分得开）** / http 基址 0.8s（回显原址 + 形状命中 3 处） |
| X-7 | 空路径与不存在的路径都不再抛；旧签名对空串确实会抛（S-2 的现场） |
| X-8 | 只差 3 个空目录的两棵树：新口径摘要**不同**、`FileDigest` 相同、`-FilesOnly` 相同（旧口径盲区被复现） |
| 自检 | **67 / 67** 场景全部通过 |
