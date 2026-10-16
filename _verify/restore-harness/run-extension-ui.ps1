# 扩展集一期 UI 驱动（task-3）：通过**真实界面**完成
#   ① 「制作扩展集」页签：生成当前游戏扩展集 / 编辑信息 / 测试干跑 / 安装到扩展集库
#   ② 首页模式1 三步：根据该作扩展集生成静态 Hash 表 → 提取动态 XP3 资源 → 用静态 Hash 还原资源名
#   ③ 首页传统动态模式（对照组，供 E-1 / E-10 逐条对账）
#
#   pwsh -File run-extension-ui.ps1 -Step meta,draft,dryrun,install -Game old
#   pwsh -File run-extension-ui.ps1 -Step mode1 -Game old -OutJson Run\out\extension-mode1.json
#   pwsh -File run-extension-ui.ps1 -Step classic -Game old -OutJson Run\out\extension-classic.json
#   pwsh -File run-extension-ui.ps1 -Step draft -Game raw        # E-2：故意没归一化的采集日志
#   pwsh -File run-extension-ui.ps1 -Step draft,install -Game other   # E-4：另一个盐的档案
#   pwsh -File run-extension-ui.ps1 -Step mode1 -Game old -LibraryArchiveOverride <坏档案>
#
# 控件 ID 来自 docs\扩展集设计.md **§8.4（2026-10-13 冻结）**，全部集中在 $script:UiIds，
# 只有这一处有数字；`-IdsJson <file>` 可以在不改进代码的前提下覆盖任意一项（临时排查用）。
#
# 产物：Run\out\extension-<game>.json（步骤明细 + 时间线 + 磁盘清单 + 失败原因），
#       输出清单过大时落 TSV（沿用既有约定）。
#
# 收尾：默认把**所有** YuaExtract 实例结束掉（lead 的硬约束：残留实例会让下一条用例认到旧窗口、
#       也会让别人构建时撞 LNK1104）；-KeepForeignInstances 可只收自己起的那个。

[CmdletBinding()]
param(
    # 步骤列表。注意：`pwsh -File x.ps1 -Step a,b` 传进来的是**一个**字符串 "a,b"，
    # 所以这里不用 ValidateSet（它会在拆分之前先拒绝），改成在下面手工拆逗号 + 校验。
    [string[]]$Step = @('draft'),
    # 'mode1' = 专用的"免跑游戏"工作区（没有盐、没有动态采集成果）——
    # E-1 要的就是"模式1 在一个没有盐、没有动态映射的工作区里照样还原"。
    [ValidateSet('old', 'raw', 'other', 'empty', 'mode1')][string]$Game = 'old',
    [string]$AppExe = '',
    [string]$OutJson = '',
    [string]$ManifestPath = '',
    [string]$Publisher = 'YuaHarness',
    [string]$Title = '',
    [int]$TimeoutSec = 900,
    [int]$StartupTimeoutSec = 45,
    [int]$IdleTimeoutSec = 600,
    [int]$SettleMs = 3000,
    # 模式1 / 传统模式的"步骤2 提取动态 XP3 资源"：合成工作区里没有真游戏，
    # 这一步用**预置 Extractor_Output** 代替（报告里必须写明这是替代）。
    [switch]$NoSeedExtractor,
    # 生成类步骤默认会往 <工具目录>\Log\Universal.log 补一份盐（合成工作区没跑过游戏）；
    # -NoToolSalt 关掉它（用来复现"盐拿不到"的失败路径）。
    [switch]$NoToolSalt,
    # 自证/演示用：强制写指定盐（`-Game other -ToolSaltOverride xp3hnp` 可复现 S-1 那个
    # "盐不匹配用例其实没测到盐"的假通过，用来证明修好后的断言会炸）。
    [string]$ToolSaltOverride = '',
    # 模式1/步骤1 也提供工具目录的盐（默认不提供）。E-4 的 `salt-mismatch-known` 用例用它
    # 让产品能读到"当前游戏的盐"，从而走真正的"盐不同"判定路径（§2.1）。
    [switch]$ProvideToolSalt,
    [string]$SeedExtractorOutputFrom = '',
    # E-4：安装完成后，把库里那份 rules.int 换成指定档案（坏档案）再跑模式1 步骤1
    [string]$LibraryArchiveOverride = '',
    [string]$IdsJson = '',
    [switch]$KeepForeignInstances,
    [switch]$SkipSnapshotSync,

    # ---------- 扩展集页签 / 在线更新（§8.5，2026-10-15 冻结）----------
    # 在线更新的仓库基址：**只给子进程**设 YUAEXT_ENDPOINT（§14.5 的最高优先级）。
    # 不传这一项时会把 YUAEXT_ENDPOINT **从子进程环境里摘掉** —— 免得本脚本自己继承来的
    # 环境变量污染"不用夹具"的用例（用完不收拾是最容易犯的错）。
    [string]$Endpoint = '',
    # setbrowse：点「刷新列表」之后选第几个会社 / 第几个作品
    [int]$SetPublisherIndex = 0,
    [int]$SetTitleIndex = 0,
    [switch]$SetSkipRefresh,
    # setupdate：拉完索引之后要不要点「下载并安装选中」，以及重复几轮（幂等用例要 2 轮）
    [switch]$OnlineInstall,
    [int]$OnlineListIndex = 0,
    [int]$OnlineRuns = 1,
    # 联网动作的等待上限。**必须大于产品的接收超时（30s）**：超时用例要等它自己报错，
    # 上限给 120s 才不至于把"产品正确超时"记成"驱动等不及"。
    # 按**名字**选会社（列表里显示原始名；序号依产品排序而变，写死不可靠）
    [string]$SetPublisherName = '',
    [int]$OnlineJobTimeoutSec = 120,
    # 覆盖确认框默认点「否」（不覆盖本地）。要测"用远端覆盖本地"才传这个开关。
    [switch]$OnlinePreferYes,
    # setbrowse：
    #   -LibraryHide       —— 把扩展集库整个挪开再刷（E-13 的"空库"场景）
    #   -SetSkipSelection  —— 只点会社、不点作品（E-14/E-15 的"未选作品"路径）
    #   -SnapshotTag       —— 库清单 / 选中档案快照的文件名后缀（默认 <game>-<steps>）
    [switch]$LibraryHide,
    [switch]$SetSkipSelection,
    [string]$SnapshotTag = '',
    # setcheck：要跑哪些自检变体（逗号分隔，可重复多次自检；默认 clean）
    #   clean / bad-archive / byte-flip / manifest-malformed / manifest-bad-digest /
    #   library-readonly / missing-dll
    [string[]]$SetSelfCheckVariants = @('clean'),
    # bad-archive 变体要换进库里的那份坏档案（用 run-extension-bad-archives.ps1 造好的那份）
    [string]$BadArchivePath = '',
    # 工作区 / 目标 exe 覆盖：在线更新夹具需要一个"第三个游戏"（既不属于主档案、也不是
    # 库里已有的那份），做法是把夹具里的一个游戏目录整份拷出来、在这个副本里生成档案。
    # 不给这两项时行为完全不变。
    [string]$WorkspaceOverride = '',
    [string]$GameExeOverride = '',
    # setopen：ok（正常打开）/ none（什么都没选）/ missing（库内目录被删）
    [ValidateSet('ok', 'none', 'missing')][string]$SetOpenMode = 'ok'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessNative.ps1')

# -Step 归一化：`-Step meta,draft`（一个字符串）与 `-Step @('meta','draft')` 都要能用
$validSteps = @('meta', 'draft', 'dryrun', 'install', 'mode1', 'classic', 'step1', 'step3',
    'settab', 'setbrowse', 'setopen', 'setnote', 'setcheck', 'setupdate')
$stepList = New-Object System.Collections.ArrayList
foreach ($item in $Step)
{
    foreach ($part in ([string]$item -split ','))
    {
        $name = $part.Trim()
        if (-not $name) { continue }
        if ($name -notin $validSteps) { throw ("-Step 里有未知步骤：'{0}'（只认 {1}）" -f $name, ($validSteps -join '/')) }
        [void]$stepList.Add($name)
    }
}
$Step = $stepList.ToArray()
if ($Step.Count -eq 0) { throw '-Step 不能为空' }

# ==================================================================
# 控件 ID 表（唯一一处；docs\扩展集设计.md §8.4 冻结）
# ==================================================================
$script:UiIds = [ordered]@{
    # 主窗口 / 页签（沿用不动）
    MainWindowClass     = 'YuaExtractMainWindow'
    TabHome             = 4001
    TabMakeSet          = 4002      # 「制作扩展集」页签
    TabSet              = 4003
    TabAbout            = 4004
    ExeEdit             = 4021      # IDC_YUA_EXE_EDIT（首页与制作页签共用）
    ExeBrowse           = 4022
    # 首页模式按钮（互斥选择，BM_CLICK 后读 BM_GETCHECK）
    ModeExtract         = 4030      # 该作扩展集提取 = 模式1
    ModeCrack           = 4031      # 该会社合集撞新作（二期，不用）
    ModeClassic         = 4032      # 传统动态模式
    # 主流程三步（标题与行为随模式分派）
    Step1               = 4050
    Step2               = 4051
    Step3               = 4052
    # 概览
    Overview            = 4070      # IDC_YUA_OVERVIEW
    Page                = 4071      # IDC_YUA_PAGE
    # 扩展集选择 / 任务进度（4080-4088）
    GroupLib            = 4080
    LibPubLabel         = 4081
    LibTitleLabel       = 4082
    LibPublishers       = 4083      # LISTBOX 会社
    LibTitles           = 4084      # LISTBOX 作品
    GroupTask           = 4085
    TaskProgress        = 4086
    TaskText            = 4087      # STATIC 阶段文字，空闲含「（空闲）」
    TaskCancel          = 4088      # BUTTON 取消，空闲时 disable
    # 制作扩展集页签（4100-4111）
    SetGroupMake        = 4100
    SetGenerate         = 4101      # 生成当前游戏扩展集
    SetEditInfo         = 4102      # 编辑当前扩展集信息（开图2）
    SetTest             = 4103      # 测试当前扩展集（只读干跑）
    SetGroupDir         = 4104
    SetOpenDraft        = 4105      # 打开当前扩展集目录
    SetInstall          = 4106      # 安装到扩展集库
    SetOpenLib          = 4107      # 打开扩展集库目录
    SetGroupCustom      = 4108
    SetPickSource       = 4109      # 选择扩展集来源目录
    SetPickTest         = 4110      # 选择测试来源目录
    SetStatus           = 4111      # 右侧「制作状态」概览
    # 图2「编辑扩展集信息」（顶层窗口类 YuaExtractMetaDialog）
    MetaWindowClass     = 'YuaExtractMetaDialog'
    MetaWindowTitle     = '编辑扩展集信息'
    MetaPublisher       = 4120
    MetaTitle           = 4121
    MetaContributor     = 4122
    MetaVersion         = 4123
    MetaDate            = 4124
    MetaComment         = 4125
    MetaOk              = 4126
    MetaCancel          = 4127
    # 「扩展集」页签（§8.5，2026-10-15 冻结）：左列「扩展集管理与浏览」。
    # 4080-4084（扩展集选择）与 4071（扩展集详情）是**首页与本页签共用**的，不重复开一份。
    SetmGroupBrowse     = 4130
    SetmRefresh         = 4131      # **本页签的标志控件**（§8.5 驱动约定 4）
    SetmOpenRoot        = 4132
    SetmOpenEntry       = 4133
    SetmUpdate          = 4134      # 在线更新（开模态弹窗）
    SetmSelfCheck       = 4135      # 运行环境自检
    SetmNote            = 4136      # 扩展集留言（只读弹窗）
    SetmIds             = @(4130, 4131, 4132, 4133, 4134, 4135, 4136)
    # 「在线更新」弹窗（顶层窗口类 YuaExtractOnlineDialog，**模态**：主窗口被 EnableWindow(FALSE)）
    OnlineWindowClass   = 'YuaExtractOnlineDialog'
    OnlineWindowTitle   = '在线更新扩展集'
    OnlineEndpoint      = 4140      # EDIT 仓库地址（必须显示当前生效的基址）
    OnlineRefresh       = 4141      # 拉索引（忙碌时 disable → 驱动靠它判"还在跑"）
    OnlineList          = 4142      # LISTBOX 远端条目
    OnlineInstall       = 4143      # 下载并安装选中
    OnlineProgress      = 4144      # msctls_progress32
    OnlineStatus        = 4145      # EDIT 只读多行：状态 / 结果
    OnlineClose         = 4146      # 关闭
}
# 运行期覆盖（不改代码换 ID）：-IdsJson 指向一份 { "SetGenerate": 4101, ... } 的 JSON
if ($IdsJson)
{
    $override = Read-JsonFile -Path $IdsJson
    if (-not $override) { throw ("读不到 -IdsJson：{0}" -f $IdsJson) }
    foreach ($property in $override.PSObject.Properties)
    {
        if (-not $script:UiIds.Contains($property.Name)) { throw ("-IdsJson 里有未知键：{0}" -f $property.Name) }
        $script:UiIds[$property.Name] = $property.Value
    }
    Write-Host ("[ext-ui] 已用 {0} 覆盖 {1} 项控件 ID" -f $IdsJson, @($override.PSObject.Properties).Count)
}

$runRoot = Join-Path $root 'Run'
$outDir = Join-Path $runRoot 'out'
if (-not $ManifestPath) { $ManifestPath = Join-Path $runRoot 'extension-manifest.json' }
if (-not $OutJson) { $OutJson = Join-Path $outDir ("extension-{0}.json" -f $Game) }
$stepLabel = ($Step -join '-')
foreach ($directory in @($runRoot, $outDir))
{
    if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
}
Remove-Item -LiteralPath $OutJson -Force -ErrorAction SilentlyContinue

$fixture = Read-JsonFile -Path $ManifestPath
if (-not $fixture) { throw ("缺少扩展集夹具清单：{0}；请先运行 make-extension-fixture.ps1" -f $ManifestPath) }

$gameSpec = switch ($Game)
{
    'old' { $fixture.oldGame }
    'raw' { $fixture.rawGame }
    'other' { $fixture.otherGame }
    'empty' { $fixture.emptyGame }
    'mode1' { $fixture.mode1Game }
}
if (-not $gameSpec) { throw ("清单里没有 {0} 这一段" -f $Game) }
$workspace = [string]$gameSpec.workspace
$gameExe = [string]$gameSpec.gameExe
# 覆盖项（只有在线更新夹具用得到）：整份拷贝出来的工作区副本 + 它自己的 exe。
# 产品的"工作区"是从目标 exe 的路径推出来的，所以这两个必须成对给。
# 注意这里只能 Write-Host：Add-Event / $record 都在后面才定义（脚本是自上而下执行的）。
if ($WorkspaceOverride) { $workspace = [System.IO.Path]::GetFullPath($WorkspaceOverride); Write-Host ("[ext-ui] -WorkspaceOverride 生效：{0}" -f $workspace) }
if ($GameExeOverride) { $gameExe = [System.IO.Path]::GetFullPath($GameExeOverride); Write-Host ("[ext-ui] -GameExeOverride 生效：{0}" -f $gameExe) }
$draftDir = Join-Path $workspace 'ExtensionDraft'
$draftArchive = Join-Path $draftDir 'rules.int'
$staticHashDir = Join-Path $workspace 'StaticHash_Output'
$extractorOutput = Join-Path $workspace 'Extractor_Output'
$restoredOutput = Join-Path $workspace 'Restored_Extractor_Output'

$events = New-Object System.Collections.ArrayList
function Add-Event([string]$Text)
{
    if ($events.Count -lt 4000) { [void]$events.Add(("[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $Text)) }
}

$record = [ordered]@{
    case             = ('extension-{0}-{1}' -f $Game, $stepLabel)
    game             = $Game
    steps            = @($Step)
    mode             = $(if ($Step -contains 'classic') { 'classic' } elseif ($Step -contains 'mode1' -or $Step -contains 'step1' -or $Step -contains 'step3') { 'mode1' } elseif ($Step | Where-Object { $_ -in @('settab', 'setbrowse', 'setopen', 'setnote', 'setcheck', 'setupdate') }) { 'set' } else { 'make-set' })
    startedUtc       = (Get-Date).ToUniversalTime().ToString('o')
    workspace        = $workspace
    gameExe          = $gameExe
    appExe           = ''
    appExeSha256     = ''
    failure          = $null
    completed        = $false
    completionReason = ''
    notes            = @()
    events           = $events
    titleTimeline    = @()
    taskTimeline     = @()
    controls         = @()
    modalDialogs     = @()
    stepResults      = [ordered]@{}
    # 目录与产物
    draftDir         = $draftDir
    draftArchive     = $draftArchive
    draftArchiveSize = 0
    draftArchiveSha256 = ''
    draftArchiveDump = ''
    libraryRoot      = ''
    libraryArchive   = ''
    libraryArchiveSha256 = ''
    libraryManifest  = ''
    libraryManifestText = ''
    libraryArchiveOverride = $LibraryArchiveOverride
    staticHashDir    = $staticHashDir
    staticHashLst    = ''
    staticHashLstSize = 0
    staticHashLstSha256 = ''
    staticHashLineCount = 0
    staticHashText   = ''
    outputDir        = ''
    output           = @()
    outputTsv        = ''
    reportPath       = ''
    reportText       = ''
    overviewText     = ''
    setStatusText    = ''
    # 只读证据（E-6）
    extractorOutput  = $extractorOutput
    extractorDigestBefore = $null
    extractorDigestAfter  = $null
    sourceDir        = ''
    sourceDigestBefore = $null
    sourceDigestAfter  = $null
    # E-7：档案是否含"生成时间"这类不可复现字段
    archiveDump      = $null
    # 「扩展集」页签 / 在线更新（§8.5、§14）
    endpoint         = ''
    endpointInChildEnv = $false
    toolIniPath      = ''
    toolIniText      = ''
    setTab           = $null
    setBrowse        = $null
    setOpen          = $null
    setNote          = $null
    setCheck         = $null
    online           = $null
    processCleanup   = [ordered]@{ own = 0; foreign = 0; foreignKilled = 0 }
}

$script:appProcess = $null
$script:mainWindow = [IntPtr]::Zero

# ==================================================================
# 进程收尾（硬约束）
# ==================================================================
function Stop-AppProcess
{
    if ($script:appProcess)
    {
        try
        {
            if (-not $script:appProcess.HasExited)
            {
                if ($script:mainWindow -ne [IntPtr]::Zero) { [void][Harness.Win]::PostClose($script:mainWindow) }
                Start-Sleep -Milliseconds 600
                if (-not $script:appProcess.HasExited) { [void](Stop-HarnessProcess -Process $script:appProcess -GraceMs 2500) }
            }
        }
        catch { }
        $record.processCleanup.own = 1
        $script:appProcess = $null
    }

    # 残留实例会让下一条用例认到旧窗口（README 踩坑 #16）。
    $others = @(Get-Process YuaExtract -ErrorAction SilentlyContinue)
    if ($others.Count -gt 0)
    {
        $record.processCleanup.foreign = $others.Count
        if ($KeepForeignInstances)
        {
            Add-Event ("收尾：还有 {0} 个 YuaExtract 实例（-KeepForeignInstances，未结束）" -f $others.Count)
        }
        else
        {
            foreach ($process in $others)
            {
                try { $process.Kill(); $record.processCleanup.foreignKilled++ } catch { }
            }
            Add-Event ("收尾：结束 {0} 个残留 YuaExtract 实例（lead 硬约束；-KeepForeignInstances 可只收自己起的）" -f $record.processCleanup.foreignKilled)
        }
    }
}

function Save-And-Exit
{
    param([string]$Reason, [int]$Code = 2)

    Stop-AppProcess
    if ($Reason) { $record.failure = $Reason }
    $record.finishedUtc = (Get-Date).ToUniversalTime().ToString('o')
    Write-JsonFile -Path $OutJson -Object $record
    if ($Reason) { Write-Host ("[ext-ui] 失败：{0}" -f $Reason) }
    Write-Host ("[ext-ui] 结果写入 {0}" -f $OutJson)
    exit $Code
}

# ==================================================================
# 界面工具
# ==================================================================

function Get-ControlHandle([int]$Id)
{
    $handle = [Harness.Win]::GetDlgItem($script:mainWindow, $Id)
    if ($handle -eq [IntPtr]::Zero) { return $null }
    return $handle
}

function Get-ControlInfo([int]$Id)
{
    $handle = Get-ControlHandle $Id
    if (-not $handle) { return $null }
    return [pscustomobject]@{
        Id      = $Id
        Hwnd    = $handle
        HwndHex = ('0x{0:X8}' -f $handle.ToInt64())
        Class   = [Harness.Win]::ClassOf($handle)
        Visible = [Harness.Win]::IsWindowVisible($handle)
        Enabled = [Harness.Win]::IsWindowEnabled($handle)
        Text    = [Harness.Win]::SmartText($handle.ToInt64(), [uint32]$script:appProcess.Id, 4000)
    }
}

function Require-Control([int]$Id, [string]$What)
{
    $info = Get-ControlInfo $Id
    if (-not $info) { Save-And-Exit ("主窗口里找不到「{0}」（id={1}，见 docs\扩展集设计.md §8.4）—— 界面结构可能还没实现" -f $What, $Id) }
    return $info
}

function Show-ControlInventory
{
    $items = @()
    foreach ($child in [Harness.Win]::Children($script:mainWindow.ToInt64()))
    {
        $text = $child.Text
        if ($text -and $text.Length -gt 120) { $text = $text.Substring(0, 120) + '…' }
        $items += [pscustomobject]@{
            HwndHex = $child.HwndHex; Id = $child.CtrlId; Class = $child.Class
            Text = $text; Visible = $child.Visible; Enabled = $child.Enabled
        }
    }
    return $items
}

function Resolve-ModalDialogs([switch]$PreferYes)
{
    $found = @()
    foreach ($window in @([Harness.Win]::TopLevel([uint32]$script:appProcess.Id)))
    {
        if ($window.Hwnd -eq $script:mainWindow.ToInt64()) { continue }
        if ($window.Class -notin @('#32770')) { continue }

        # 读一次框里的控件；**看起来没建完就等 250ms 再读一次**。
        # 为什么：跨进程轮询有可能正好撞上"窗口已经存在、子控件还没建完"的那一瞬间 ——
        # 实测抓到过：产品的「强行使用扩展集（未校验）」确认框（是/否 两个按钮）被读成
        # "只有 1 个按钮、正文为空"，于是被当成信息框直接点「是」→ 产品按"不校验强行使用"
        # 继续走，E-4 的"盐不匹配"用例就变成了"没被拒"。
        $readChildren = {
            $texts = @()
            $buttons = @()
            $allButtons = @()
            foreach ($child in [Harness.Win]::Children($window.Hwnd))
            {
                if ($child.Class -eq 'Static' -and $child.Text) { $texts += $child.Text }
                if ($child.Class -eq 'Button')
                {
                    $allButtons += $child
                    if ($child.Text) { $buttons += $child }
                }
            }
            return [pscustomobject]@{ Texts = @($texts); Buttons = @($buttons); AllButtons = @($allButtons) }
        }
        $snapshot = & $readChildren
        $looksIncomplete = ($snapshot.AllButtons.Count -eq 0) -or ($snapshot.Texts.Count -eq 0)
        if ($looksIncomplete)
        {
            Start-Sleep -Milliseconds 250
            $again = & $readChildren
            if ($again.AllButtons.Count -gt $snapshot.AllButtons.Count -or $again.Texts.Count -gt $snapshot.Texts.Count) { $snapshot = $again }
        }
        $texts = @($snapshot.Texts)
        $buttons = @($snapshot.Buttons)
        $allButtons = @($snapshot.AllButtons)
        $entry = [pscustomobject]@{
            Title = $window.Text; Text = ($texts -join ' | ')
            Buttons = (@($buttons | ForEach-Object { $_.Text }) -join ',')
        }
        $found += $entry
        Add-Event ("出现模态框：title='{0}' text='{1}' buttons='{2}'" -f $entry.Title, $entry.Text, $entry.Buttons)

        # 按钮文案带加速键（「是(&Y)」/「否(&N)」），也可能写成「点「是」」这类带书名号的提示
        # （2026-10-13 修复轮把 Host::Confirm 的按钮文案改成了这种）—— 必须先归一化再比（README 踩坑 #11）。
        $normalize = { param($text) return (($text -replace '&', '') -replace '[\s「」『』]', '' -replace '\s*\([^)]*\)\s*', '').Trim() }
        $wanted = if ($PreferYes) { @('是', 'Yes', '确定', 'OK', '否', 'No', '取消') } else { @('确定', 'OK', '否', 'No', '取消', '关闭') }
        $choice = $null
        foreach ($label in $wanted)
        {
            $choice = $buttons | Where-Object { (& $normalize $_.Text) -eq $label } | Select-Object -First 1
            if ($choice) { break }
        }
        # 再退一步：按钮文字里**包含**目标词也算（「点「是」」这种写法）
        if (-not $choice)
        {
            foreach ($label in $wanted)
            {
                $choice = $buttons | Where-Object { (& $normalize $_.Text) -like ('*' + $label + '*') } | Select-Object -First 1
                if ($choice) { break }
            }
        }
        if ($choice)
        {
            [void][Harness.Win]::PostClick($choice.Hwnd)
            Add-Event ("已点模态框按钮「{0}」（归一化「{1}」，PreferYes={2}）" -f $choice.Text, (& $normalize $choice.Text), [bool]$PreferYes)
        }
        elseif ($buttons.Count -eq 1 -and $allButtons.Count -eq 1)
        {
            # **确实只有一个按钮控件**（信息框）才直接点它 —— 否则框一直挂着，等空闲会白等到超时。
            #
            # 为什么必须同时看 $allButtons（2026-10-15 实测踩到）：产品那个
            # 「强行使用扩展集（未校验）」确认框里，另一个按钮的文案是**空的**，
            # 于是"有文案的按钮"只剩「是」一个 → 旧写法把它当成信息框直接点 → 产品按
            # "不校验、强行使用"继续走了下去（E-4 的"盐不匹配"用例于是产出了 2 行的 StaticHash.lst，
            # 断言如实报"没被拒"）。这不是产品缺陷，是脚手架把"取消/否"那一下点丢了。
            [void][Harness.Win]::PostClick($buttons[0].Hwnd)
            Add-Event ("模态框只有一个按钮，直接点「{0}」" -f $buttons[0].Text)
        }
        else
        {
            # 兜底：标准 MessageBox 的按钮 id 固定是 IDYES=6 / IDNO=7 / IDOK=1
            [void][Harness.Win]::PostCommand($window.Hwnd, $(if ($PreferYes) { 6 } else { 7 }), 0)
            Add-Event ("按钮文案都没匹配上（实际：{0}；按钮控件共 {1} 个，其中 {2} 个没文案）；改用 PostCommand(ID{3})" -f `
                    $entry.Buttons, $allButtons.Count, ($allButtons.Count - $buttons.Count), $(if ($PreferYes) { 'YES' } else { 'NO' }))
        }
    }
    return $found
}

function Click-Control([int]$Id, [string]$What)
{
    $info = Require-Control $Id $What
    Add-Event ("点「{0}」（id={1}，class={2}，visible={3}，enabled={4}）" -f $What, $Id, $info.Class, $info.Visible, $info.Enabled)
    [void][Harness.Win]::PostClick($info.Hwnd)
    Start-Sleep -Milliseconds 400
    foreach ($dialog in (Resolve-ModalDialogs)) { $record.modalDialogs += $dialog }
}

function Read-ControlText([int]$Id, [int]$MaxChars = 200000)
{
    $handle = Get-ControlHandle $Id
    if (-not $handle) { return '' }
    $text = [Harness.Win]::SmartText($handle.ToInt64(), [uint32]$script:appProcess.Id, $MaxChars)
    if ($null -eq $text) { return '' }
    return $text
}

# 等后台动作结束：轮询 IDC_YUA_TASK_TEXT（空闲含「（空闲）」）或取消按钮的 enable（§8.4 驱动约定 2）
function Wait-TaskIdle
{
    param([string]$What, [int]$TimeoutSeconds = 0, [switch]$AlsoWaitOutput)

    if ($TimeoutSeconds -le 0) { $TimeoutSeconds = $IdleTimeoutSec }
    $textHandle = Get-ControlHandle ([int]$script:UiIds.TaskText)
    $cancelHandle = Get-ControlHandle ([int]$script:UiIds.TaskCancel)
    if (-not $textHandle -and -not $cancelHandle)
    {
        Save-And-Exit ("等「{0}」结束：既没有 IDC_YUA_TASK_TEXT(4087) 也没有 IDC_YUA_TASK_CANCEL(4088)，无法判断空闲（§8.4 的驱动约定）" -f $What)
    }

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $lastText = ''
    $samples = New-Object System.Collections.ArrayList
    $start = Get-Date
    while ((Get-Date) -lt $deadline)
    {
        if ($script:appProcess.HasExited) { Save-And-Exit ("等「{0}」时被测程序退出了（退出码 {1}）" -f $What, $script:appProcess.ExitCode) }
        foreach ($dialog in (Resolve-ModalDialogs)) { $record.modalDialogs += $dialog }

        $text = if ($textHandle) { [Harness.Win]::SmartText($textHandle.ToInt64(), [uint32]$script:appProcess.Id, 4000) } else { '' }
        if ($null -eq $text) { $text = '' }
        $cancelEnabled = if ($cancelHandle) { [Harness.Win]::IsWindowEnabled($cancelHandle) } else { $false }

        if ($text -ne $lastText)
        {
            $lastText = $text
            $ms = [int]((Get-Date) - $start).TotalMilliseconds
            [void]$samples.Add(('{0}|{1}|{2}' -f $ms, $(if ($cancelEnabled) { 'busy' } else { 'idle' }), $text))
            Add-Event ("任务文字：{0}" -f $text)
        }

        # 空闲判定：文字含「（空闲）」**或**取消按钮 disabled + 文字非空
        $idle = ($text -like '*（空闲）*') -or ($text -like '*(空闲)*')
        if (-not $idle -and $cancelHandle -and -not $cancelEnabled -and $text) { $idle = $true }
        if ($idle)
        {
            Start-Sleep -Milliseconds 500
            $record.taskTimeline = @($samples)
            return $text
        }
        Start-Sleep -Milliseconds 300
    }
    $record.taskTimeline = @($samples)
    Save-And-Exit ("等「{0}」超过 {1} 秒仍未空闲（最后的任务文字：'{2}'）" -f $What, $TimeoutSeconds, $lastText)
}

# 列表控件按**类名**分派消息：4083/4084 是 ListBox（LB_*），
# 若哪天改成 ListView 也照样能用（LVM_*）。混用会得到"列表是空的"这种假失败。
function Get-ListCount($Info)
{
    $msaa = Get-MsaaListItems -Hwnd $Info.Hwnd
    if ($msaa.Ok) { return $msaa.Count }
    if ($Info.Class -like 'ListBox*') { return [Harness.Win]::ListBoxCount($Info.Hwnd.ToInt64()) }
    return [Harness.Win]::ListCount($Info.Hwnd.ToInt64())
}

# 列表内容：**统一走 MSAA**（见 lib\HarnessNative.ps1 里 AccessibleClient 的说明：
# LB_GETTEXT 的跨进程指针实测收不到字节，"列表里明明有会社"却读到空串）。
# 读不到时返回 Ok=$false —— 调用方要如实记下来，不能当成"列表是空的"。
function Get-ListItemsByHandle($Info)
{
    $msaa = Get-MsaaListItems -Hwnd $Info.Hwnd
    if ($msaa.Ok)
    {
        return [pscustomobject]@{ Ok = $true; Count = $msaa.Count; Items = @($msaa.Items); Source = 'msaa'; Error = '' }
    }
    # 兜底：老办法逐项读（可能是 ListView，那 LVM_GETITEMTEXTW 才管用）。
    # 兜底也读不到就把原因带上，让断言看得见"这条读法不行"。
    $items = New-Object System.Collections.ArrayList
    $count = if ($Info.Class -like 'ListBox*') { [Harness.Win]::ListBoxCount($Info.Hwnd.ToInt64()) } else { [Harness.Win]::ListCount($Info.Hwnd.ToInt64()) }
    for ($index = 0; $index -lt $count; $index++)
    {
        $text = if ($Info.Class -like 'ListBox*')
        {
            [Harness.Win]::ListBoxText($Info.Hwnd.ToInt64(), [uint32]$script:appProcess.Id, $index, 512)
        }
        else { [Harness.Win]::RemoteListViewText($Info.Hwnd.ToInt64(), [uint32]$script:appProcess.Id, $index, 0) }
        [void]$items.Add($(if ($null -eq $text) { '' } else { [string]$text }))
    }
    return [pscustomobject]@{ Ok = ($count -eq 0); Count = $count; Items = @($items); Source = 'legacy'; Error = $msaa.Error }
}

function Get-ListItemText($Info, [int]$Index)
{
    # MSAA 的列表项是 1..accChildCount 的简单子元素（0 是控件自身），所以下标要 +1
    $msaa = Get-MsaaListItems -Hwnd $Info.Hwnd
    if ($msaa.Ok -and ($Index + 1) -le @($msaa.Items).Count) { return [string](@($msaa.Items)[$Index]) }
    if ($Info.Class -like 'ListBox*')
    {
        return [Harness.Win]::ListBoxText($Info.Hwnd.ToInt64(), [uint32]$script:appProcess.Id, $Index, 512)
    }
    return [Harness.Win]::RemoteListViewText($Info.Hwnd.ToInt64(), [uint32]$script:appProcess.Id, $Index, 0)
}

# 按**文字**找一个列表项的下标（列表里显示的是**原始名**，排序由产品决定，写死序号不可靠）。
# 找不到返回 -1。给"会社名会被改写成目录名"这类用例用（E-14-b）。
function Find-ListIndexByName([int]$ListId, [string]$Text, [string]$What)
{
    $info = Require-Control $ListId $What
    # 读列表**要重试**：MSAA 偶发拿不到（实测：刚刷新完那一下会读到 0 项，而过一会儿再读就有）——
    # 一次读空就判"找不到会社"会把偶发读失败当成产品缺陷（这条正是踩过的：set-hostile 曾因此失败）。
    for ($attempt = 1; $attempt -le 5; $attempt++)
    {
        # 注意用 Get-MsaaListItems 的 **.Items**（字符串数组）；Get-ListItemsByHandle 返回的是
        # 一个带 Items/Count/Ok 的对象，拿它当下标数组会永远匹配不上（踩过：这条判据一度静默找不到）。
        $msaa = Get-MsaaListItems -Hwnd $info.Hwnd
        $items = @()
        if ($msaa.Ok) { $items = @($msaa.Items) }
        else
        {
            $count = Get-ListCount $info
            for ($index = 0; $index -lt $count; $index++) { $items += (Get-ListItemText $info $index) }
        }
        for ($index = 0; $index -lt $items.Count; $index++)
        {
            if ([string]$items[$index] -eq $Text) { return $index }
        }
        if ($attempt -lt 5) { Start-Sleep -Milliseconds 400 }
    }
    $probe = Get-MsaaListItems -Hwnd $info.Hwnd
    Add-Event ("按名字找「{0}」失败（列表 {1} 项，读到的内容：[{2}]）" -f $Text, (Get-ListCount $info), ((@($probe.Items)) -join ', '))
    return -1
}

function Set-ListSelection([int]$ListId, [int]$Index, [string]$What)
{
    $info = Require-Control $ListId $What
    $count = Get-ListCount $info
    if ($count -le $Index) { Save-And-Exit ("{0} 列表（class={1}）只有 {2} 项，选不到第 {3} 项" -f $What, $info.Class, $count, $Index) }
    # §8.4 驱动约定 1：LB_SETCURSEL(0x0186) 之后必须补发 WM_COMMAND + LBN_SELCHANGE(1)，
    # 否则列表不会刷新（产品在 LBN_SELCHANGE 里才重建作品列表）。
    if ($info.Class -like 'ListBox*') { [void][Harness.Win]::SelectListBoxItem($info.Hwnd.ToInt64(), $Index) }
    else { [void][Harness.Win]::SendMessageW($info.Hwnd, 0x0186, [IntPtr]$Index, [IntPtr]::Zero) }
    [void][Harness.Win]::PostCommand($script:mainWindow.ToInt64(), $ListId, 1)
    Start-Sleep -Milliseconds 600
    $text = Get-ListItemText $info $Index
    Add-Event ("选中 {0} 第 {1} 项（共 {2} 项，class={3}，文字='{4}'）并补发 LBN_SELCHANGE" -f $What, $Index, $count, $info.Class, $text)
    return [pscustomobject]@{ Count = $count; Text = $text }
}

function Select-Tab([int]$TabId, [int]$Index, [string]$What, [int[]]$ExpectVisible = @())
{
    $tabHandle = Get-ControlHandle $TabId
    if (-not $tabHandle) { Save-And-Exit ("找不到页签控件（id={0}）" -f $TabId) }
    $className = [Harness.Win]::ClassOf($tabHandle)

    # 实测（2026-10-13 快照）：产品的页签是**自绘 BUTTON**（4001..4004，
    # 标题「首页/制作扩展集/扩展集/关于」），**不是** SysTabControl32。
    # 所以一律 BM_CLICK；TCM_* 那套对按钮页签完全无效（踩过：页签从没切过去，
    # 制作页签的控件全是 visible=False，后面点按钮全落空）。
    [void][Harness.Win]::PostClick($tabHandle)
    Start-Sleep -Milliseconds 700
    Add-Event ("已切到{0}（id={1} 是 {2}，按按钮点击；TCM 条目数={3}）" -f $What, $TabId, $className, [Harness.Win]::TabItemCount($tabHandle.ToInt64()))

    # 切换成功的判据：该页的标志性控件必须变可见（不能只看"点过了"）
    foreach ($id in $ExpectVisible)
    {
        $info = Get-ControlInfo $id
        if (-not $info) { Save-And-Exit ("切到{0}后找不到标志控件 {1}（界面结构可能已变）" -f $What, $id) }
        if (-not $info.Visible)
        {
            $pageInfo = Get-ControlInfo ([int]$script:UiIds.Page)
            Save-And-Exit ("切到{0}后控件 {1}（'{2}'）仍不可见 —— 页签没切过去（自绘按钮页签必须 BM_CLICK；IDC_YUA_PAGE(4071) 可见={3}）" -f `
                    $What, $id, $info.Text, $(if ($pageInfo) { $pageInfo.Visible } else { '<无>' }))
        }
    }
    Add-Event ("页签切换校验通过：{0} 的标志控件可见（{1}）" -f $What, ($ExpectVisible -join ','))
}

function Get-FileInfo([string]$Path)
{
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return [pscustomobject]@{ Exists = $false; Size = 0; Sha256 = ''; Mtime = $null } }
    $item = Get-Item -LiteralPath $Path
    return [pscustomobject]@{ Exists = $true; Size = [long]$item.Length; Sha256 = (Get-Sha256Hex -Path $Path); Mtime = $item.LastWriteTime }
}

# 档案的独立解析（lib\rules-int.py）：E-7（逐 chunk 比对）/ E-8（体积与条数）用它，不采信报告
function Get-ArchiveDump([string]$Path, [string]$Tag)
{
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $python = Get-HarnessPython
    $jsonPath = Join-Path $outDir ('archive-dump-{0}-{1}.json' -f $Game, $Tag)
    & $python (Join-Path $root 'lib\rules-int.py') dump --in $Path --json $jsonPath | Out-Null
    $dump = Read-JsonFile -Path $jsonPath
    if ($dump) { $dump | Add-Member -NotePropertyName 'exitCode' -NotePropertyValue $LASTEXITCODE -Force }
    return $dump
}

function Get-HarnessPython
{
    if ($script:pythonPath) { return $script:pythonPath }
    $candidates = @('C:\Users\Administrator\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe')
    foreach ($candidate in $candidates) { if (Test-Path -LiteralPath $candidate) { $script:pythonPath = $candidate; return $candidate } }
    $found = Get-Command python -ErrorAction SilentlyContinue
    if ($found) { $script:pythonPath = $found.Source; return $script:pythonPath }
    throw '找不到 Python 运行时（解析 rules.int 需要它）'
}
$script:pythonPath = ''

# 发现式兜底：安装之后到工具目录下找**最新的** rules.int / manifest.txt，
# 不硬编码库根（一期库根固定 <工具目录>\ExtensionLibrary，但用"发现"更抗实现细节变化）
function Find-LibraryArtifacts([string]$ToolDir, [datetime]$Since)
{
    $archives = @(Get-ChildItem -LiteralPath $ToolDir -Recurse -File -Filter 'rules.int' -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $Since.AddSeconds(-5) } | Sort-Object LastWriteTime -Descending)
    $manifests = @(Get-ChildItem -LiteralPath $ToolDir -Recurse -File -Filter 'manifest.txt' -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending)
    return [pscustomobject]@{ Archives = $archives; Manifests = $manifests }
}

# ==================================================================
# 「扩展集」页签 / 在线更新的取证小工具（§8.5、§14）
# ==================================================================

# 树摘要（可排除若干目录名）：E-15/E-16/E-18/E-19 判"本地一个文件都不动"用它。
#
# 为什么不能直接用 Get-DirectoryDigest：工具目录里带着 `Log\`，被测程序每做一件事就写一行
# 日志（`Host::LogLine`）——把 Log 一起算进去，"自检不改文件""失败不动本地"这两条永远不可能成立。
# 所以按**段名**排除（Log / OnlineStaging），其余逐条比 相对路径|大小|mtimeUtc。
# 树摘要（Get-TreeDigestExcluding）与「排除 Log」的口径都在 lib\HarnessUtil.ps1 ——
# **实现只留一处**（S-2 那次崩就是同一件事有两份实现、只有一份被修）。

# 目录清单（可含目录条目）：暂存目录"不留东西"这条判据也要看得见**空目录**（N-S1 的同一类盲区：
# 产品在失败路径上可能留下一个空目录，只列文件的话看起来"什么都没留"）。
function Get-FileListing
{
    param([AllowEmptyString()][string]$Path = '', [switch]$IncludeDirectories)

    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return @() }
    $root = [System.IO.Path]::GetFullPath($Path).TrimEnd('\')
    $items = New-Object System.Collections.ArrayList
    foreach ($file in @(Get-ChildItem -LiteralPath $root -Recurse -File -Force -ErrorAction SilentlyContinue | Sort-Object FullName))
    {
        [void]$items.Add([pscustomobject]@{ rel = $file.FullName.Substring($root.Length).TrimStart('\'); size = [long]$file.Length; kind = 'file' })
    }
    if ($IncludeDirectories)
    {
        foreach ($dir in @(Get-ChildItem -LiteralPath $root -Recurse -Directory -Force -ErrorAction SilentlyContinue | Sort-Object FullName))
        {
            [void]$items.Add([pscustomobject]@{ rel = $dir.FullName.Substring($root.Length).TrimStart('\'); size = -1; kind = 'dir' })
        }
    }
    return $items.ToArray()
}

# 把库清单读成"数据行"（跳过 # 注释与空行）——E-18 判"库清单多一条/不重复"用它，
# 断言那侧还会自己再数一遍（不采信这里的结果）。
function Read-ManifestRows([string]$Path)
{
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return @() }
    $text = Read-TextAutoShared -Path $Path -MaxBytes 2000000
    if (-not $text) { return @() }
    $rows = New-Object System.Collections.ArrayList
    foreach ($line in ($text -split "`r?`n"))
    {
        if (-not $line.Trim()) { continue }
        if ($line.TrimStart().StartsWith('#')) { continue }
        $fields = $line -split "`t"
        if ($fields.Count -lt 10) { continue }
        [void]$rows.Add([pscustomobject]@{
                publisher = $fields[0]; title = $fields[1]; exeDigest = $fields[2]; siblingDigest = $fields[3]
                saltDigest = $fields[4]; archiveDigest = $fields[5]; entryCount = $fields[6]
                generatedAt = $fields[7]; installedAt = $fields[8]; source = $fields[9]
            })
    }
    return $rows.ToArray()
}

# 找到本机扩展集库根（一期固定 <工具目录>\ExtensionLibrary，但用"发现"更抗细节变化）
function Get-LibraryInfo
{
    param([string]$ToolDir, $Manifest = $null)

    $root = ''
    if ($Manifest) { $root = [string](Get-ObjectValue -Object $Manifest -Name 'libraryRoot' -Default '') }
    if (-not $root)
    {
        $candidate = Join-Path $ToolDir 'ExtensionLibrary'
        if (Test-Path -LiteralPath $candidate) { $root = $candidate }
    }
    $manifestPath = if ($root) { Join-Path $root 'manifest.txt' } else { '' }
    $rows = @(Read-ManifestRows $manifestPath)
    return [pscustomobject]@{
        Root         = $root
        ManifestPath = $manifestPath
        ManifestText = $(if ($manifestPath -and (Test-Path -LiteralPath $manifestPath)) { [string](Read-TextAutoShared -Path $manifestPath -MaxBytes 2000000) } else { '' })
        Rows         = $rows
    }
}

# 弹窗（#32770）里的控件取件：主窗口之外的窗口要用窗口句柄自己 GetDlgItem
function Get-DialogControlHandle([IntPtr]$Window, [int]$Id)
{
    if ($Window -eq [IntPtr]::Zero) { return $null }
    $handle = [Harness.Win]::GetDlgItem($Window, $Id)
    if ($handle -eq [IntPtr]::Zero) { return $null }
    return $handle
}

function Get-DialogControlText([IntPtr]$Window, [int]$Id, [int]$MaxChars = 200000)
{
    $handle = Get-DialogControlHandle $Window $Id
    if (-not $handle) { return '' }
    $text = [Harness.Win]::SmartText($handle.ToInt64(), [uint32]$script:appProcess.Id, $MaxChars)
    if ($null -eq $text) { return '' }
    return $text
}

function Get-DialogControlEnabled([IntPtr]$Window, [int]$Id)
{
    $handle = Get-DialogControlHandle $Window $Id
    if (-not $handle) { return $null }
    return [Harness.Win]::IsWindowEnabled($handle)
}

function Click-DialogControl([IntPtr]$Window, [int]$Id, [string]$What)
{
    $handle = Get-DialogControlHandle $Window $Id
    if (-not $handle) { Save-And-Exit ("弹窗里找不到「{0}」（id={1}）—— 界面结构可能还没实现" -f $What, $Id) }
    Add-Event ("点弹窗控件「{0}」（id={1}）" -f $What, $Id)
    [void][Harness.Win]::PostClick($handle)
    Start-Sleep -Milliseconds 350
}

# 读弹窗里的列表框（远端条目列表）——走 MSAA，理由同 Get-ListItemsByHandle
function Get-DialogListItems([IntPtr]$Window, [int]$Id)
{
    $handle = Get-DialogControlHandle $Window $Id
    if (-not $handle) { return @() }
    $msaa = Get-MsaaListItems -Hwnd $handle
    if ($msaa.Ok) { return @($msaa.Items) }
    $items = New-Object System.Collections.ArrayList
    $class = [Harness.Win]::ClassOf($handle)
    if ($class -like 'ListBox*')
    {
        $count = [Harness.Win]::ListBoxCount($handle.ToInt64())
        for ($index = 0; $index -lt $count; $index++)
        {
            [void]$items.Add([Harness.Win]::ListBoxText($handle.ToInt64(), [uint32]$script:appProcess.Id, $index, 1024))
        }
    }
    else
    {
        $count = [Harness.Win]::ListCount($handle.ToInt64())
        for ($index = 0; $index -lt $count; $index++)
        {
            [void]$items.Add([Harness.Win]::RemoteListViewText($handle.ToInt64(), [uint32]$script:appProcess.Id, $index, 0))
        }
    }
    return $items.ToArray()
}

function Get-DialogListCount([IntPtr]$Window, [int]$Id)
{
    $handle = Get-DialogControlHandle $Window $Id
    if (-not $handle) { return 0 }
    $msaa = Get-MsaaListItems -Hwnd $handle
    if ($msaa.Ok) { return $msaa.Count }
    $class = [Harness.Win]::ClassOf($handle)
    if ($class -like 'ListBox*') { return [Harness.Win]::ListBoxCount($handle.ToInt64()) }
    return [Harness.Win]::ListCount($handle.ToInt64())
}

function Select-DialogListItem([IntPtr]$Window, [int]$Id, [int]$Index, [string]$What)
{
    $handle = Get-DialogControlHandle $Window $Id
    if (-not $handle) { Save-And-Exit ("弹窗里找不到列表 {0}（{1}）" -f $Id, $What) }
    $class = [Harness.Win]::ClassOf($handle)
    $count = Get-DialogListCount $Window $Id
    if ($count -le $Index) { Save-And-Exit ("{0} 列表只有 {1} 项，选不到第 {2} 项" -f $What, $count, $Index) }
    if ($class -like 'ListBox*') { [void][Harness.Win]::SelectListBoxItem($handle.ToInt64(), $Index) }
    else { [void][Harness.Win]::SendMessageW($handle, 0x0186, [IntPtr]$Index, [IntPtr]::Zero) }
    Start-Sleep -Milliseconds 300
    # `@(...)` 不能省：列表里**恰好一项**时 PowerShell 会把返回值解包成标量字符串，
    # 紧接着的 `.Count` 在 StrictMode 下直接抛"在此对象上找不到属性 Count"
    # （README 踩坑 #21 的同一条账；实测"索引只有一行"的坏用例全栽在这里）。
    $items = @(Get-DialogListItems $Window $Id)
    $picked = if ($items.Count -gt $Index) { [string]$items[$Index] } else { '' }
    Add-Event ("选中 {0} 第 {1} 项（共 {2} 项）：'{3}'" -f $What, $Index, $count, $picked)
    return [pscustomobject]@{ Count = $count; Text = $picked }
}

# 等在线更新弹窗里的联网动作结束（§8.5 驱动约定 5）：
# 忙碌 = 「拉取索引」(4141) 被禁用（产品在任务进行中把两个动作按钮都禁掉）。
# 结束 = 4141 恢复可用 **且** 状态文字连续两次采样不变（防"刚恢复可用但结论还没写进去"）。
function Wait-OnlineIdle
{
    param([IntPtr]$Window, [int]$TimeoutSeconds, [string]$What)

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $lastText = ''
    $stableSamples = 0
    $samples = New-Object System.Collections.ArrayList
    $start = Get-Date
    while ((Get-Date) -lt $deadline)
    {
        if ($script:appProcess.HasExited) { Save-And-Exit ("等「{0}」时被测程序退出了（退出码 {1}）" -f $What, $script:appProcess.ExitCode) }
        foreach ($dialog in (Resolve-ModalDialogs)) { $record.modalDialogs += $dialog }

        $text = Get-DialogControlText $Window ([int]$script:UiIds.OnlineStatus) 4000
        $busy = -not (Get-DialogControlEnabled $Window ([int]$script:UiIds.OnlineRefresh))
        $ms = [int]((Get-Date) - $start).TotalMilliseconds
        if ($text -ne $lastText)
        {
            $lastText = $text
            $stableSamples = 0
            [void]$samples.Add(('{0}|{1}' -f $ms, $text))
            Add-Event ("在线状态：{0}" -f ($text -replace "`r?`n", ' / '))
        }
        else { $stableSamples++ }

        if (-not $busy -and $stableSamples -ge 2)
        {
            return [pscustomobject]@{ Status = $text; Busy = $false; ElapsedMs = $ms; Samples = @($samples) }
        }
        Start-Sleep -Milliseconds 400
    }
    return [pscustomobject]@{ Status = $lastText; Busy = $true; ElapsedMs = [int]((Get-Date) - $start).TotalMilliseconds; Samples = @($samples) }
}

# 点一个"必然弹模态框"的按钮，把框的标题/正文/按钮**抓下来**再点掉。
# 为什么不用 Click-Control：它只在 400ms 后扫一次框，没扫到就漏了；而 E-15/E-16/E-17 的
# 证据正是框里的文字（"已打开 <路径>" / 自检结论 / 留言正文）。这里等到框出现为止。
function Invoke-ModalAction
{
    param([int]$Id, [string]$What, [int]$TimeoutMs = 10000, [switch]$PreferYes)

    $info = Require-Control $Id $What
    Add-Event ("点「{0}」（id={1}，期望弹模态框）" -f $What, $Id)
    [void][Harness.Win]::PostClick($info.Hwnd)

    $found = New-Object System.Collections.ArrayList
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline)
    {
        $dialogs = @(Resolve-ModalDialogs -PreferYes:$PreferYes)
        foreach ($dialog in $dialogs) { [void]$found.Add($dialog); $record.modalDialogs += $dialog }
        if ($found.Count -gt 0) { break }
        Start-Sleep -Milliseconds 250
    }
    return $found.ToArray()
}

# 主窗口可见性快照（E-12 的骨架判据）：可见 id / 隐藏 id / 逐控件明细。
# 为什么要逐控件明细：E-11 的教训是"可见性对了不等于画面干净"，所以这里记的是**状态**，
# 像素那一半交给 run-tab-pixels.ps1（两条线分开，谁也不替谁背书）。
function Get-VisibilitySnapshot([string]$Name)
{
    $visible = New-Object System.Collections.ArrayList
    $hidden = New-Object System.Collections.ArrayList
    $inventory = New-Object System.Collections.ArrayList
    foreach ($child in [Harness.Win]::Children($script:mainWindow.ToInt64()))
    {
        if ($child.CtrlId -le 0) { continue }
        $text = [string]$child.Text
        if ($text.Length -gt 40) { $text = $text.Substring(0, 40) + '…' }
        [void]$inventory.Add([pscustomobject]@{
                id = $child.CtrlId; class = $child.Class; visible = $child.Visible; enabled = $child.Enabled; text = $text
            })
        if ($child.Visible) { [void]$visible.Add([int]$child.CtrlId) } else { [void]$hidden.Add([int]$child.CtrlId) }
    }
    # `@(...)` 不能省：只有一个控件时 PowerShell 会把单元素集合解包成标量，
    # 后面 `.Count` 在 StrictMode 下会炸（README 踩坑 #21 的同一条账）。
    return [pscustomobject]@{
        Name      = $Name
        Visible   = @($visible | Sort-Object)
        Hidden    = @($hidden | Sort-Object)
        Inventory = @($inventory)
        TakenAt   = (Get-Date).ToUniversalTime().ToString('o')
    }
}

# ==================================================================
# 启动被测程序
# ==================================================================

if (-not $AppExe)
{
    if ($SkipSnapshotSync)
    {
        $AppExe = Join-Path $runRoot 'App\YuaExtract.exe'
    }
    else
    {
        try
        {
            $snapshot = Sync-ReleaseSnapshot -RepoRoot ([System.IO.Path]::GetFullPath((Join-Path $root '..\..'))) -RunRoot $runRoot
            $AppExe = $snapshot.Exe
            $record.appExeSha256 = $snapshot.ExeSha256
            Add-Event ("被测程序快照：{0}（{1}，sha256 {2}）" -f $snapshot.Exe, $snapshot.ExeMachine, $snapshot.ExeSha256.Substring(0, 12))
        }
        catch { Save-And-Exit $_.Exception.Message }
    }
}
if (-not (Test-Path -LiteralPath $AppExe)) { Save-And-Exit ("找不到被测程序：{0}；请先编译解决方案（Release|x86）" -f $AppExe) }
$AppExe = [System.IO.Path]::GetFullPath($AppExe)
$record.appExe = $AppExe
if (-not $record.appExeSha256) { $record.appExeSha256 = (Get-Sha256Hex -Path $AppExe) }
Add-Event ("被测程序：{0}" -f $AppExe)

# ---------- 在线更新的基址（§14.5：环境变量 → ini → 内置常量）----------
#
# 只给**子进程**设 YUAEXT_ENDPOINT。不传 -Endpoint 时把这个变量从子进程环境里**摘掉**：
# 本脚本自己继承来的环境变量会顺着 CreateProcess 传下去，那样"不用夹具"的用例会莫名其妙
# 打上一轮的夹具地址（还很难看出来）—— 用完不收拾是这一层最容易犯的错。
$childEnvironment = @{}
$removeEnvironment = @()
if ($Endpoint)
{
    $childEnvironment['YUAEXT_ENDPOINT'] = $Endpoint
    $record.endpointInChildEnv = $true
    $record.endpoint = $Endpoint
    Add-Event ("给子进程设 YUAEXT_ENDPOINT = {0}（§14.5 的最高优先级）" -f $Endpoint)
}
else
{
    $removeEnvironment += 'YUAEXT_ENDPOINT'
    Add-Event '本次不设 YUAEXT_ENDPOINT（并从子进程环境里摘掉，免得继承来的值影响结论）'
}
# ini 里可能也配了基址（§14.5 第 2 级）。环境变量优先，所以它不影响结论；
# 但"不设环境变量时产品还在打某个地址"只能靠它解释，所以记下来备查。
$toolDirForIni = Split-Path -Parent $AppExe
foreach ($candidate in @((Join-Path $toolDirForIni 'YuaExtract.ini'), (Join-Path $env:LOCALAPPDATA 'YuaExtract\YuaExtract.ini')))
{
    if (Test-Path -LiteralPath $candidate)
    {
        $record.toolIniPath = $candidate
        $record.toolIniText = Read-TextAutoShared -Path $candidate -MaxBytes 20000
        Add-Event ("发现配置文件 {0}（内容 {1} 字节）" -f $candidate, ([string]$record.toolIniText).Length)
        break
    }
}

# ---------- 工具目录的盐（<工具目录>\Log\Universal.log）----------
#
# 实测（2026-10-13 快照）：生成扩展集时产品读的是**工具目录**的 Log\Universal.log
# （"该文件由步骤2 的收集模块写出"），游戏目录里那份不读。合成工作区没有跑过游戏，
# 所以生成类步骤要在这里按夹具的盐补一份。
#
# 两个要点：
#   1) 必须在**快照同步之后**写：同步会把 Run\App 整个换掉，之前写的会被冲掉（README 踩坑）。
#   2) 模式1 默认**不写**，而且要把上一条用例可能留下的那份**删掉**：模式1 的盐只能来自档案
#      IDENTITY（§7），留着旧盐就没法证明"免跑游戏"（E-1 的证据）。
#   3) **盐按 -Game 取**（S-1 的根因）：`-Game other` 必须写 othersalt123，
#      否则"别的游戏"的采集日志会被用当前游戏的盐重算 → 名字层全进例外 →
#      档案一个名字都没有 → 命中的是"空名字防线"，盐不匹配用例变成假通过。
$gameSalt = if ($Game -eq 'other') { [string]$fixture.otherGame.salt } else { [string]$fixture.salt }
if ($ToolSaltOverride)
{
    $gameSalt = $ToolSaltOverride
    Add-Event ("-ToolSaltOverride 生效：强制写盐 '{0}'（自证/演示用，可复现 S-1 的假通过）" -f $gameSalt)
}
$provideToolSalt = ($Step -contains 'draft' -or $Step -contains 'dryrun' -or $Step -contains 'install' -or $ProvideToolSalt) -and -not $NoToolSalt
$record['toolSaltProvided'] = $provideToolSalt
$record['toolSaltValue'] = if ($provideToolSalt) { $gameSalt } else { '' }
$record['toolSaltOverride'] = $ToolSaltOverride
$toolSaltPath = Join-Path (Split-Path -Parent $AppExe) 'Log\Universal.log'
if ($provideToolSalt)
{
    $stampText = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    $saltText = (@(
            ('{0} | I | T1 | Hash Seed:{1}' -f $stampText, $gameSalt)
            ('{0} | I | T1 | FileNameHasherSalt:' -f $stampText)
            ('{0} | I | T1 | PathNameHasherSalt:' -f $stampText)
        ) -join "`r`n") + "`r`n"
    Write-Utf8File -Path $toolSaltPath -Text $saltText -NoNewline
    $record['toolSaltFile'] = $toolSaltPath
    $record['toolSaltSha256'] = (Get-Sha256Hex -Path $toolSaltPath)
    Add-Event ("已补工具目录的盐（-Game {0} → Hash Seed:{1}，两个 HasherSalt 留空）：{2}" -f $Game, $gameSalt, $toolSaltPath)
}
else
{
    if (Test-Path -LiteralPath $toolSaltPath)
    {
        Remove-Item -LiteralPath $toolSaltPath -Force
        $record['toolSaltRemoved'] = $true
        Add-Event ("已删除上一条用例留下的工具目录盐：{0}（模式1/对照：盐必须来自档案）" -f $toolSaltPath)
    }
    Add-Event '本次不提供工具目录的盐（模式1/对照：盐必须来自档案）'
}

# 模式1 / 传统模式：把"解包目录"预置好（合成工作区没有真游戏；报告里必须写明这是替代）
if ($Step -contains 'mode1' -or $Step -contains 'classic')
{
    if (-not $NoSeedExtractor)
    {
        $seedFrom = if ($SeedExtractorOutputFrom) { $SeedExtractorOutputFrom } else { [string]$fixture.newUnpacked.root }
        if (-not (Test-Path -LiteralPath $seedFrom)) { Save-And-Exit ("预置解包目录的来源不存在：{0}" -f $seedFrom) }
        [void](Remove-TreeUnder -Path $extractorOutput -MustBeUnder $runRoot)
        Copy-Item -LiteralPath $seedFrom -Destination $extractorOutput -Recurse -Force
        $note = ("步骤2「提取动态 XP3 资源」用**预置 Extractor_Output** 代替（来源 {0}）：合成工作区里没有真游戏，无法真的解包 —— 报告与断言里都按「替代」记录" -f $seedFrom)
        $record.notes += $note
        Add-Event $note
    }
    else
    {
        $record.notes += '步骤2 未预置（-NoSeedExtractor）：解包目录由上一次运行留下'
    }
}

# 只读证据（E-6）：源解包目录 + 生成来源目录
$record.extractorDigestBefore = Get-DirectoryDigest -Path $extractorOutput -WithHash
# 跨工作区"输入等价"的硬证据（E-1/E-10）：不含 mtime 的内容摘要 ——
# 模式1 跑在 Mode1Game、传统模式跑在 OldGame，两次复制出来的 mtime 必然不同，
# 只有内容摘要能证明"两者共用同一份预置 Extractor_Output"。
$record['extractorContentDigest'] = Get-DirectoryContentDigest -Path $extractorOutput
$record['extractorOutputPath'] = $extractorOutput
# **只有生成类步骤**才谈得上"生成来源目录"：模式1/传统模式的输出目录恰好就是
# `<工作区>\Restored_Extractor_Output`（§8.1 里它同时是"扩展集来源目录"的默认值），
# 把它们也算成"来源目录"会得出"源目录被还原改动了"的假失败（踩过）。
if ($Step -contains 'draft')
{
    $record.sourceDir = if ($Game -eq 'old') { [string]$fixture.oldGame.restoredDir } elseif ($Game -eq 'raw') { [string]$fixture.rawGame.sourceDir } else { '' }
    if ($record.sourceDir)
    {
        $record.sourceDigestBefore = Get-DirectoryDigest -Path $record.sourceDir -WithHash
        # 内容摘要（不含 mtime）：干跑步骤会把这份"来源目录"重铺一遍（S-5 的修法），
        # mtime 必然变；只读语义要看**内容**是否变（E-6 用它当主判据）。
        $record['sourceContentDigestBefore'] = Get-DirectoryContentDigest -Path $record.sourceDir
    }
}
Add-Event ("解包目录摘要（前）：{0} 个文件，digest {1}" -f $record.extractorDigestBefore.Count, ([string]$record.extractorDigestBefore.Digest).Substring(0, 12))

try
{
    $process = Start-HarnessProcess -FilePath $AppExe -WorkingDirectory (Split-Path -Parent $AppExe) -Arguments @($gameExe) `
        -Environment $childEnvironment -RemoveEnvironment $removeEnvironment
}
catch
{
    Save-And-Exit ("启动被测程序失败：{0}（正在编译或文件被占用时会出现）" -f $_.Exception.Message)
}
$script:appProcess = $process
$appPid = [uint32]$process.Id
Add-Event ("已启动 pid={0}，命令行目标：{1}" -f $appPid, $gameExe)

$deadline = (Get-Date).AddSeconds($StartupTimeoutSec)
while ((Get-Date) -lt $deadline -and $script:mainWindow -eq [IntPtr]::Zero)
{
    if ($process.HasExited) { Save-And-Exit ("被测程序启动后立刻退出，退出码 {0}" -f $process.ExitCode) }
    foreach ($window in [Harness.Win]::TopLevel($appPid))
    {
        if ($window.Class -eq [string]$script:UiIds.MainWindowClass) { $script:mainWindow = [IntPtr]$window.Hwnd; break }
    }
    if ($script:mainWindow -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 250 }
}
if ($script:mainWindow -eq [IntPtr]::Zero)
{
    $seen = ([Harness.Win]::TopLevel($appPid) | ForEach-Object { '{0}({1})' -f $_.Class, $_.Text }) -join '; '
    Save-And-Exit ("等不到主窗口（类名 {0}）。当前进程窗口：{1}" -f $script:UiIds.MainWindowClass, $seen)
}
Start-Sleep -Milliseconds 900
Add-Event ("主窗口就绪：{0}" -f [Harness.Win]::CaptionOf($script:mainWindow))
$record.controls = @(Show-ControlInventory)
Add-Event ("主窗口子控件 {0} 个" -f $record.controls.Count)

# 目标游戏是否已生效（命令行参数那条路）
$exeEdit = Get-ControlHandle ([int]$script:UiIds.ExeEdit)
$exeBaseName = Split-Path -Leaf $gameExe
$windowTitle = [Harness.Win]::CaptionOf($script:mainWindow)
$targetApplied = $windowTitle -like ('*' + $exeBaseName + '*')
if (-not $targetApplied -and $exeEdit)
{
    Add-Event '主窗口标题里没有目标名，改用"写编辑框 + EN_KILLFOCUS"兜底'
    [void][Harness.Win]::LocalSetText($exeEdit.ToInt64(), $gameExe)
    [void][Harness.Win]::PostCommand($script:mainWindow.ToInt64(), [int]$script:UiIds.ExeEdit, 0x0200)
    Start-Sleep -Milliseconds 1500
    $windowTitle = [Harness.Win]::CaptionOf($script:mainWindow)
    $targetApplied = $windowTitle -like ('*' + $exeBaseName + '*')
}
$echo = if ($exeEdit) { [Harness.Win]::SmartText($exeEdit.ToInt64(), $appPid, 2000) } else { '<没有 4021 编辑框>' }
Add-Event ("目标编辑框回读：'{0}'；标题：'{1}'" -f $echo, $windowTitle)
if (-not $targetApplied) { Save-And-Exit ("目标游戏没设上：期望标题出现 '{0}'，实际 '{1}'（编辑框='{2}'）" -f $exeBaseName, $windowTitle, $echo) }

# 工具目录的日志是**跨用例累积**的（同一个 Run\App 快照反复用）。如果断言去整份日志里找
# "产品这次说了什么"，就会把上一条用例写下的行当成这一条的（S-1 那类假通过就是这么来的）。
# 所以这里先记下每个日志文件的当前长度，收尾时只取"这次新写的那一段"（toolLogTails）。
$script:toolLogOffsetsBefore = [ordered]@{}
$logDirProbe = Join-Path (Split-Path -Parent $AppExe) 'Log'
if (Test-Path -LiteralPath $logDirProbe)
{
    foreach ($file in (Get-ChildItem -LiteralPath $logDirProbe -File -ErrorAction SilentlyContinue))
    {
        $script:toolLogOffsetsBefore[$file.Name] = [long]$file.Length
    }
}
$record['toolLogOffsetsBefore'] = $script:toolLogOffsetsBefore

$overviewId = [int]$script:UiIds.Overview
$taskId = [int]$script:UiIds.TaskText

# ==================================================================
# 步骤：制作扩展集页签
# ==================================================================

if ($Step -contains 'meta' -or $Step -contains 'draft' -or $Step -contains 'dryrun' -or $Step -contains 'install')
{
    Select-Tab ([int]$script:UiIds.TabMakeSet) 1 '「制作扩展集」页签' -ExpectVisible @([int]$script:UiIds.SetGenerate)
    $tabInfo = Get-ControlInfo ([int]$script:UiIds.TabMakeSet)
    if ($tabInfo) { Add-Event ("页签 4002：class={0} 标题='{1}'" -f $tabInfo.Class, $tabInfo.Text) }
    # 页签切好之后，制作页签的控件应当变成可见；不可见就说明切页失败（早点说出来，别等生成超时）
    $generateInfo = Get-ControlInfo ([int]$script:UiIds.SetGenerate)
    if ($generateInfo -and -not $generateInfo.Visible) { Add-Event '注意：切页后「生成当前游戏扩展集」仍不可见 —— 页签可能没切过去' }
}

# 产品要求生成前先填「会社」（否则弹框「还没有填「会社」」直接拒绝）。
# 调用方只写 -Step draft 时自动补一次 meta，避免"看起来是生成失败"的假失败。
$effectiveSteps = @($Step)
if ($Step -contains 'draft' -and $Step -notcontains 'meta')
{
    $effectiveSteps += 'meta'
    $record.notes += '生成前自动补跑「编辑当前扩展集信息」（产品要求先填会社，否则生成被弹框拒绝）'
    $record['effectiveSteps'] = @($effectiveSteps)
    Add-Event '自动补跑 meta（-Step 里有 draft 但没有 meta）'
}

if ($effectiveSteps -contains 'meta')
{
    $section = [ordered]@{ name = 'meta'; ok = $false; reason = '' }
    Click-Control ([int]$script:UiIds.SetEditInfo) '编辑当前扩展集信息'
    # 图2 是顶层窗口类 YuaExtractMetaDialog（不是 .rc 对话框）
    $metaWindow = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and $metaWindow -eq [IntPtr]::Zero)
    {
        foreach ($window in [Harness.Win]::TopLevel($appPid))
        {
            if ($window.Class -eq [string]$script:UiIds.MetaWindowClass) { $metaWindow = [IntPtr]$window.Hwnd; break }
        }
        if ($metaWindow -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 200 }
    }
    if ($metaWindow -eq [IntPtr]::Zero)
    {
        $seen = ([Harness.Win]::TopLevel($appPid) | ForEach-Object { '{0}({1})' -f $_.Class, $_.Text }) -join '; '
        $section.reason = ("等不到「编辑扩展集信息」窗口（类 {0}）。当前窗口：{1}" -f $script:UiIds.MetaWindowClass, $seen)
        $record.stepResults['meta'] = $section
        Save-And-Exit $section.reason
    }
    $metaTitle = if ($Title) { $Title } else { [System.IO.Path]::GetFileNameWithoutExtension($gameExe) }
    $fields = @(
        [pscustomobject]@{ Id = [int]$script:UiIds.MetaPublisher; Value = $Publisher; Name = '会社' }
        [pscustomobject]@{ Id = [int]$script:UiIds.MetaTitle; Value = $metaTitle; Name = '游戏名称' }
        [pscustomobject]@{ Id = [int]$script:UiIds.MetaContributor; Value = 'harness'; Name = '贡献者' }
        [pscustomobject]@{ Id = [int]$script:UiIds.MetaVersion; Value = 'v1'; Name = '版本' }
        [pscustomobject]@{ Id = [int]$script:UiIds.MetaComment; Value = 'YuaExtract 验证脚手架合成档案'; Name = '留言' }
    )
    foreach ($field in $fields)
    {
        $handle = [Harness.Win]::GetDlgItem($metaWindow, $field.Id)
        if ($handle -eq [IntPtr]::Zero) { Add-Event ("图2 里找不到「{0}」（id={1}）" -f $field.Name, $field.Id); continue }
        [void][Harness.Win]::LocalSetText($handle.ToInt64(), $field.Value)
        $readBack = [Harness.Win]::SmartText($handle.ToInt64(), $appPid, 400)
        Add-Event ("图2 「{0}」= '{1}'（回读 '{2}'）" -f $field.Name, $field.Value, $readBack)
    }
    $okHandle = [Harness.Win]::GetDlgItem($metaWindow, [int]$script:UiIds.MetaOk)
    if ($okHandle -eq [IntPtr]::Zero) { Save-And-Exit ("图2 里找不到「确定」按钮（id={0}）" -f $script:UiIds.MetaOk) }
    [void][Harness.Win]::PostClick($okHandle)
    Start-Sleep -Milliseconds 800
    $section.ok = $true
    $section.publisher = $Publisher
    $section.title = $metaTitle
    $record.stepResults['meta'] = $section
    Add-Event '已提交「编辑扩展集信息」'
}

if ($Step -contains 'draft')
{
    $section = [ordered]@{ name = 'draft'; ok = $false; reason = '' }
    $before = Get-FileInfo $draftArchive
    $started = Get-Date
    Click-Control ([int]$script:UiIds.SetGenerate) '生成当前游戏扩展集'
    $taskText = Wait-TaskIdle '生成当前游戏扩展集'
    Start-Sleep -Milliseconds $SettleMs
    $info = Get-FileInfo $draftArchive
    $section.taskText = $taskText
    $section.archiveExistedBefore = $before.Exists
    $section.beforeSha256 = $before.Sha256
    $section.size = $info.Size
    $section.sha256 = $info.Sha256
    $section.mtime = if ($info.Mtime) { ([datetime]$info.Mtime).ToString('o') } else { '' }
    $section.startedAt = $started.ToUniversalTime().ToString('o')
    $record.draftArchiveSize = $info.Size
    $record.draftArchiveSha256 = $info.Sha256
    $record.overviewText = Read-ControlText $overviewId
    $record.setStatusText = Read-ControlText ([int]$script:UiIds.SetStatus)
    if (-not $info.Exists)
    {
        $section.reason = ("生成结束后仍然没有草案档案：{0}" -f $draftArchive)
        $record.stepResults['draft'] = $section
        Save-And-Exit $section.reason
    }
    $section.ok = $true
    $record.stepResults['draft'] = $section
    Add-Event ("草案档案：{0} 字节 sha256 {1}" -f $info.Size, $info.Sha256.Substring(0, 16))
    $record.archiveDump = Get-ArchiveDump -Path $draftArchive -Tag 'draft'
}

if ($Step -contains 'dryrun')
{
    $section = [ordered]@{ name = 'dryrun'; ok = $false; reason = '' }

    # S-5：干跑的默认"测试来源目录" = 采集来源目录 = `<工作区>\Restored_Extractor_Output`，
    # 而传统模式的步骤3 正是往这个目录写结果 —— 全量跑完再单独复跑干跑就会得到 0/0/0
    # （目录里已经是 16 个真名文件），看起来像"干跑坏了"。
    # 所以点「测试」之前把夹具内容**重新铺回去**，并记录来源目录 + 内容摘要自证。
    $drySource = Join-Path $workspace 'Restored_Extractor_Output'
    $seedFrom = [string]$fixture.newUnpacked.root
    if (-not (Test-Path -LiteralPath $seedFrom)) { Save-And-Exit ("干跑来源目录的夹具源不存在：{0}" -f $seedFrom) }
    [void](Remove-TreeUnder -Path $drySource -MustBeUnder $runRoot)
    Copy-Item -LiteralPath $seedFrom -Destination $drySource -Recurse -Force
    if ($fixture.negativeName)
    {
        Write-Utf8File -Path (Join-Path $drySource 'scn_names.txt') -Text ("; YuaExtract harness fake script (extension fixture)`r`n*{0}`r`n" -f $fixture.negativeName) -NoNewline
    }
    $record['dryRunSourceReseeded'] = $true
    $record['dryRunSourceDir'] = $drySource
    $record['dryRunSourceContentDigest'] = Get-DirectoryContentDigest -Path $drySource
    Add-Event ("干跑前已把测试来源目录重铺为夹具内容：{0}（{1} 个文件，内容摘要 {2}）" -f `
            $drySource, $record.dryRunSourceContentDigest.Count, ([string]$record.dryRunSourceContentDigest.Digest).Substring(0, 16))

    Click-Control ([int]$script:UiIds.SetTest) '测试当前扩展集（只读干跑）'
    $taskText = Wait-TaskIdle '测试当前扩展集'
    Start-Sleep -Milliseconds 800
    $section.taskText = $taskText
    $section.overviewText = Read-ControlText $overviewId
    $section.setStatusText = Read-ControlText ([int]$script:UiIds.SetStatus)
    $record.stepResults['dryrun'] = $section
    if (-not $section.setStatusText -and -not $section.overviewText)
    {
        $section.reason = '干跑结束后既读不到概览（4070）也读不到制作状态（4111）—— 无法取干跑成功率'
        Save-And-Exit $section.reason
    }
    $section.ok = $true
    Add-Event '干跑完成，已抓取概览/制作状态文本'
}

if ($Step -contains 'install')
{
    $section = [ordered]@{ name = 'install'; ok = $false; reason = '' }
    $toolDir = Split-Path -Parent $AppExe
    $since = Get-Date
    Click-Control ([int]$script:UiIds.SetInstall) '安装到扩展集库'
    $taskText = Wait-TaskIdle '安装到扩展集库'
    Start-Sleep -Milliseconds 800
    $section.taskText = $taskText
    $found = Find-LibraryArtifacts -ToolDir $toolDir -Since $since
    $section.archiveCount = $found.Archives.Count
    if ($found.Archives.Count -eq 0)
    {
        $section.reason = ("安装之后在工具目录下没发现新的 rules.int（找过 {0}）—— 库根约定是 <工具目录>\ExtensionLibrary" -f $toolDir)
        $record.stepResults['install'] = $section
        Save-And-Exit $section.reason
    }
    $archive = $found.Archives[0]
    $record.libraryRoot = Split-Path -Parent (Split-Path -Parent $archive.FullName)
    $record.libraryArchive = $archive.FullName
    $record.libraryArchiveSha256 = (Get-Sha256Hex -Path $archive.FullName)
    if ($found.Manifests.Count -gt 0)
    {
        $record.libraryManifest = $found.Manifests[0].FullName
        $record.libraryManifestText = Read-TextAutoShared -Path $found.Manifests[0].FullName -MaxBytes 200000
    }
    $section.archive = $archive.FullName
    $section.archiveSize = [long]$archive.Length
    $section.archiveSha256 = $record.libraryArchiveSha256
    $section.manifest = $record.libraryManifest
    $section.manifestText = $record.libraryManifestText
    $section.ok = $true
    $record.stepResults['install'] = $section
    Add-Event ("已发现库档案：{0}（{1} 字节 sha256 {2}）" -f $archive.FullName, $archive.Length, $record.libraryArchiveSha256.Substring(0, 16))

    # E-4：把库里那份换成坏档案（或另一个盐的档案），供下一步"必须被拒"的用例使用
    if ($LibraryArchiveOverride)
    {
        if (-not (Test-Path -LiteralPath $LibraryArchiveOverride)) { Save-And-Exit ("-LibraryArchiveOverride 指向的文件不存在：{0}" -f $LibraryArchiveOverride) }
        Copy-Item -LiteralPath $LibraryArchiveOverride -Destination $archive.FullName -Force
        $record.notes += ("已把库档案替换为：{0}（E-4 坏档案用例；原始档案摘要 {1}）" -f $LibraryArchiveOverride, $record.libraryArchiveSha256)
        Add-Event ("库档案已被替换为 {0}" -f $LibraryArchiveOverride)
    }
}

# ==================================================================
# 步骤：「扩展集」页签（§8.5）+ 在线更新（§14）
# ==================================================================
#
# 这一组步骤的共同点：**只读页签状态 + 只读/受控地改本地库**，每一步都把"能不能炸"的证据
# 留在 run JSON 里（面板文字 / 弹窗文字 / 目录摘要前后 / 库清单前后），断言那侧不采信面板结论，
# 自己再解析一遍档案与清单。

$setStepNames = @('settab', 'setbrowse', 'setopen', 'setnote', 'setcheck', 'setupdate')
$setStepsActive = @($Step | Where-Object { $_ -in $setStepNames })
if ($setStepsActive.Count -gt 0)
{
    # 页签 4003 的标志控件是 4131（§8.5 驱动约定 4）：切过去之后它必须变可见，
    # 否则"页签压根没切过去"，后面每一条都会变成假失败。
    Select-Tab ([int]$script:UiIds.TabSet) 2 '「扩展集」页签' -ExpectVisible @([int]$script:UiIds.SetmRefresh)
    $record['activeTabAtSetSteps'] = 'set'
}

if ($Step -contains 'settab')
{
    $section = [ordered]@{ name = 'settab'; ok = $false; reason = '' }
    $passes = New-Object System.Collections.ArrayList

    # 三路取样：切到本页签 → 切回首页 → 再切回本页签。
    # 为什么必须切回去看：E-11 的教训是"切过去看着对、切回来残留"（幽灵是双向的），
    # 而 E-12 要的正是"骨架对了 + 画面没留旧像素"两条线分开验（可见性一条、像素一条）。
    [void]$passes.Add((Get-VisibilitySnapshot 'set-1'))
    Select-Tab ([int]$script:UiIds.TabHome) 0 '首页（校验切回去后本页签控件必须全隐）' -ExpectVisible @([int]$script:UiIds.ModeExtract)
    [void]$passes.Add((Get-VisibilitySnapshot 'home-back'))
    Select-Tab ([int]$script:UiIds.TabSet) 2 '「扩展集」页签（第二次）' -ExpectVisible @([int]$script:UiIds.SetmRefresh)
    [void]$passes.Add((Get-VisibilitySnapshot 'set-2'))

    # 「关于」页签（4004）：本轮 LayoutWindow 的分派刚被改过（新增 setTab 分支），
    # 顺手取一份骨架 + 正文（S-3：这条最便宜的覆盖挂在本组里，不单开一组）。
    Select-Tab ([int]$script:UiIds.TabAbout) 3 '「关于」页签' -ExpectVisible @([int]$script:UiIds.Page)
    $aboutSnapshot = Get-VisibilitySnapshot 'about'
    $aboutText = Read-ControlText ([int]$script:UiIds.Page) 200000
    # 再切回扩展集页签（后面的 setbrowse/setnote/... 都在这个页签上做），并**再取一次样**：
    # "去过关于页签再回来"和"直接切过来"必须一致（做完 S-3 不引入新残留）。
    Select-Tab ([int]$script:UiIds.TabSet) 2 '「扩展集」页签（去过关于页签之后）' -ExpectVisible @([int]$script:UiIds.SetmRefresh)
    [void]$passes.Add((Get-VisibilitySnapshot 'set-3'))

    $tabInfo = Get-ControlInfo ([int]$script:UiIds.TabSet)
    $record.setTab = [ordered]@{
        passes      = @($passes)
        pageText    = (Read-ControlText ([int]$script:UiIds.Page) 200000)
        overviewText = (Read-ControlText $overviewId 200000)
        tabClass    = $(if ($tabInfo) { $tabInfo.Class } else { '' })
        tabText     = $(if ($tabInfo) { $tabInfo.Text } else { '' })
        about       = [ordered]@{
            name        = 'about'
            visible     = @($aboutSnapshot.Visible)
            hidden      = @($aboutSnapshot.Hidden)
            inventory   = @($aboutSnapshot.Inventory)
            pageText    = $aboutText
            pageTextLength = $aboutText.Length
            takenAt     = $aboutSnapshot.TakenAt
            # 页面上的「构建指纹」栏必须报**这次跑的这一个 exe**（断言拿它比）
            appFingerprint = $record.appExeSha256.Substring(0, 16)
            appExeSize  = (Get-Item -LiteralPath $AppExe).Length
        }
    }
    $section.ok = $true
    $section.visibleIds = $passes[0].Visible
    $section.controlCount = @($passes[0].Inventory).Count
    $record.stepResults['settab'] = $section
    Add-Event ("页签骨架取样完成：set-1 可见 {0} 个控件，home-back 可见 {1} 个，set-2 可见 {2} 个，set-3 可见 {3} 个；" +
        "关于页签可见 {4} 个、正文 {5} 字（构建指纹 {6}）" -f `
            @($passes[0].Visible).Count, @($passes[1].Visible).Count, @($passes[2].Visible).Count, @($passes[3].Visible).Count,
            @($aboutSnapshot.Visible).Count, $aboutText.Length, $record.appExeSha256.Substring(0, 16))
}

if ($Step -contains 'setbrowse')
{
    $section = [ordered]@{ name = 'setbrowse'; ok = $false; reason = '' }
    $toolDir = Split-Path -Parent $AppExe
    $libraryHidden = ''
    $libraryOriginal = Join-Path $toolDir 'ExtensionLibrary'

    # E-13 的"空库"场景：把库整个挪开（**不是删**），刷完列表再挪回来。
    # 挪而不是删：断言要的是"产品面对空库时的行为"，删掉一旦中途失败就把库弄丢了。
    if ($LibraryHide)
    {
        if (-not (Test-Path -LiteralPath $libraryOriginal)) { Save-And-Exit ("-LibraryHide：库根本来就不存在：{0}" -f $libraryOriginal) }
        $libraryHidden = Join-Path $toolDir ('ExtensionLibrary.harness-hidden-{0}' -f $PID)
        if (Test-Path -LiteralPath $libraryHidden) { [void](Remove-TreeUnder -Path $libraryHidden -MustBeUnder $toolDir) }
        Move-Item -LiteralPath $libraryOriginal -Destination $libraryHidden
        Add-Event ("已把扩展集库暂时挪开（E-13 空库场景）：{0} → {1}" -f $libraryOriginal, $libraryHidden)
    }

    try
    {
        if (-not $SetSkipRefresh)
        {
            Click-Control ([int]$script:UiIds.SetmRefresh) '刷新列表'
            # 等列表真的出来：**库清单里有行**但列表读到 0 项 → 再等一会、还不行就再点一次刷新。
            # （踩过：set-hostile 那次刷新刚点完就读，列表还没填上，于是"会社列表里找不到 A:B"
            #   看起来像产品缺陷；而空库场景是真的 0 项，不能一律当成"没刷出来"。）
            $libraryRowsProbe = @((Get-LibraryInfo -ToolDir (Split-Path -Parent $AppExe)).Rows).Count
            $refreshDeadline = (Get-Date).AddSeconds(6)
            $refreshTries = 0
            while ((Get-Date) -lt $refreshDeadline -and $libraryRowsProbe -gt 0)
            {
                if ((Get-ListCount (Require-Control ([int]$script:UiIds.LibPublishers) '会社列表')) -gt 0) { break }
                Start-Sleep -Milliseconds 400
                if ((Get-Date) -lt $refreshDeadline)
                {
                    $refreshTries++
                    if ($refreshTries -le 2) { Click-Control ([int]$script:UiIds.SetmRefresh) '刷新列表（列表还是空的，重试）' }
                }
            }
            if ($refreshTries -gt 0) { Add-Event ("刷新列表重试了 {0} 次（库清单有 {1} 行、列表一开始读到 0 项）" -f $refreshTries, $libraryRowsProbe) }
        }

        $pubInfo = Require-Control ([int]$script:UiIds.LibPublishers) '会社列表'
        $pubCount = Get-ListCount $pubInfo
        $publishers = New-Object System.Collections.ArrayList
        for ($index = 0; $index -lt $pubCount; $index++) { [void]$publishers.Add((Get-ListItemText $pubInfo $index)) }
        $pageBeforeSelect = Read-ControlText ([int]$script:UiIds.Page) 200000

        $titles = New-Object System.Collections.ArrayList
        $titleCount = 0
        $selectedPublisher = ''
        $selectedTitle = ''
        if ($pubCount -gt 0)
        {
            $publisherIndexToUse = $SetPublisherIndex
            if ($SetPublisherName)
            {
                $publisherIndexToUse = Find-ListIndexByName ([int]$script:UiIds.LibPublishers) $SetPublisherName '会社'
                if ($publisherIndexToUse -lt 0)
                {
                    Save-And-Exit ("会社列表里找不到「{0}」（-SetPublisherName）—— 列表内容：[{1}]" -f $SetPublisherName, ((@(Get-ListItemsByHandle $pubInfo)) -join ', '))
                }
                Add-Event ("按名字选中会社「{0}」= 第 {1} 项（列表里显示的是原始名）" -f $SetPublisherName, $publisherIndexToUse)
            }
            $selectedPublisher = (Set-ListSelection ([int]$script:UiIds.LibPublishers) $publisherIndexToUse '会社').Text
            $titleInfo = Require-Control ([int]$script:UiIds.LibTitles) '作品列表'
            $titleCount = Get-ListCount $titleInfo
            for ($index = 0; $index -lt $titleCount; $index++) { [void]$titles.Add((Get-ListItemText $titleInfo $index)) }
            if ($titleCount -gt 0 -and -not $SetSkipSelection)
            {
                $selectedTitle = (Set-ListSelection ([int]$script:UiIds.LibTitles) $SetTitleIndex '作品').Text
            }
        }
        Start-Sleep -Milliseconds 600

        $pageText = Read-ControlText ([int]$script:UiIds.Page) 200000
        $section.publisherCount = $pubCount
        $section.titleCount = $titleCount
        $section.publishers = @($publishers)
        $section.titles = @($titles)
        $section.selectedPublisher = $selectedPublisher
        $section.selectedTitle = $selectedTitle

        # 库清单 / 选中项的档案**当场拷成快照**：后面的用例（在线更新安装、E-4 换坏档案）
        # 会把库改掉，断言再去读现场就"物是人非"（README 踩坑 #22 的老账）。
        $toolDir = Split-Path -Parent $AppExe
        $libInfo = Get-LibraryInfo -ToolDir $toolDir
        $stamp = if ($SnapshotTag) { $SnapshotTag } else { ('{0}-{1}' -f $Game, $stepLabel) }
        $manifestSnapshot = ''
        $archiveSnapshot = ''
        if ($libInfo.ManifestPath -and (Test-Path -LiteralPath $libInfo.ManifestPath))
        {
            $manifestSnapshot = Join-Path $outDir ('set-browse-manifest-{0}.txt' -f $stamp)
            Copy-Item -LiteralPath $libInfo.ManifestPath -Destination $manifestSnapshot -Force
        }
        if ($libInfo.Root)
        {
            $entryDirectory = Join-Path (Join-Path $libInfo.Root $selectedPublisher) $selectedTitle
            $entryArchive = Join-Path $entryDirectory 'rules.int'
            if ($selectedPublisher -and $selectedTitle -and (Test-Path -LiteralPath $entryArchive))
            {
                $archiveSnapshot = Join-Path $outDir ('set-browse-archive-{0}.rules.int' -f $stamp)
                Copy-Item -LiteralPath $entryArchive -Destination $archiveSnapshot -Force
            }
        }

        $record.setBrowse = [ordered]@{
            stamp               = $stamp
            libraryRoot         = $libInfo.Root
            libraryManifest     = $libInfo.ManifestPath
            libraryManifestText = $libInfo.ManifestText
            manifestRowCount    = @($libInfo.Rows).Count
            manifestRows        = @($libInfo.Rows)
            publishers          = @($publishers)
            titles              = @($titles)
            publisherCount      = $pubCount
            titleCount          = $titleCount
            selectedPublisher   = $selectedPublisher
            selectedTitle       = $selectedTitle
            publisherIndex      = $SetPublisherIndex
            titleIndex          = $SetTitleIndex
            libraryHidden       = [bool]$LibraryHide
            pageBeforeSelect    = $pageBeforeSelect
            pageText            = $pageText
            pageTextLength      = ([string]$pageText).Length
            manifestSnapshot    = $manifestSnapshot
            archiveSnapshot     = $archiveSnapshot
            archiveSnapshotSha256 = $(if ($archiveSnapshot) { Get-Sha256Hex -Path $archiveSnapshot } else { '' })
        }
        $section.ok = $true
        $record.stepResults['setbrowse'] = $section
        Add-Event ("列表取样完成：会社 {0} 个 / 作品 {1} 个（选中 '{2} / {3}'）；面板 {4} 字" -f `
                $pubCount, $titleCount, $selectedPublisher, $selectedTitle, ([string]$pageText).Length)
    }
    finally
    {
        if ($libraryHidden)
        {
            if (Test-Path -LiteralPath $libraryOriginal) { [void](Remove-TreeUnder -Path $libraryOriginal -MustBeUnder $toolDir) }
            Move-Item -LiteralPath $libraryHidden -Destination $libraryOriginal
            Add-Event ("已把扩展集库挪回来：{0}" -f $libraryOriginal)
        }
    }
}

if ($Step -contains 'setnote')
{
    $section = [ordered]@{ name = 'setnote'; ok = $false; reason = '' }
    # 「扩展集留言」是**只读**弹窗（§8.5 明确：与制作页签图2 的输入框不是一回事）
    $dialogs = Invoke-ModalAction ([int]$script:UiIds.SetmNote) '扩展集留言（只读弹窗）' -TimeoutMs 10000
    $section.dialogCount = @($dialogs).Count
    $section.dialogs = @($dialogs | ForEach-Object { [pscustomobject]@{ title = $_.Title; text = $_.Text; buttons = $_.Buttons } })
    $section.pageText = Read-ControlText ([int]$script:UiIds.Page) 200000
    $record.setNote = [ordered]@{
        dialogs  = $section.dialogs
        pageText = $section.pageText
    }
    $section.ok = $true
    $record.stepResults['setnote'] = $section
    Add-Event ("留言弹窗取样完成：{0} 个模态框" -f @($dialogs).Count)
}

if ($Step -contains 'setcheck')
{
    $section = [ordered]@{ name = 'setcheck'; ok = $false; reason = '' }
    $toolDir = Split-Path -Parent $AppExe
    $libInfo = Get-LibraryInfo -ToolDir $toolDir
    $entryArchive = ''
    if ($libInfo.Root -and @($libInfo.Rows).Count -gt 0)
    {
        $row = @($libInfo.Rows)[0]
        # 库里的**实际**落库目录是 sanitize 之后的形态（与产品同口径）
        $entryArchive = Join-Path (Join-Path (Join-Path $libInfo.Root (ConvertTo-SafePathComponent $row.publisher)) `
                (ConvertTo-SafePathComponent $row.title)) 'rules.int'
    }
    # DLL 的真实位置见 Sync-ReleaseSnapshot：<工具目录>\CxdecExtractordll\CxdecStringDumper.dll
    # （老版发布结构是 CxdecExtractorDll，两个都试一遍，别因为目录改名把用例跑成"没有 DLL"）
    $dllCandidateNames = @('CxdecExtractordll', 'CxdecExtractorDll', 'CxdecExtractorLoaderDll')
    $dllPath = ''
    foreach ($name in $dllCandidateNames)
    {
        $candidate = Join-Path $toolDir (Join-Path $name 'CxdecStringDumper.dll')
        if (Test-Path -LiteralPath $candidate) { $dllPath = $candidate; break }
    }
    if (-not $dllPath) { $dllPath = Join-Path $toolDir 'CxdecExtractordll\CxdecStringDumper.dll' }
    $record['selfCheckDllPath'] = $dllPath
    $record['selfCheckEntryArchive'] = $entryArchive

    $variants = @()
    foreach ($item in $SetSelfCheckVariants)
    {
        foreach ($part in ([string]$item -split ',')) { if ($part.Trim()) { $variants += $part.Trim() } }
    }
    if ($variants.Count -eq 0) { $variants = @('clean') }

    $results = New-Object System.Collections.ArrayList
    foreach ($variant in $variants)
    {
        $result = [ordered]@{
            variant = $variant; mutation = ''; mutationError = ''
            panelBefore = ''; panelAfter = ''; modals = @()
            libraryDigestBefore = $null; libraryDigestAfter = $null
            toolDigestBefore = $null; toolDigestAfter = $null
        }
        # 一次只改一处、改完**一定还原**（finally）：自检用例如果把夹具改坏了，
        # 后面每一条都会变成谜一样的失败。
        $revert = $null
        try
        {
            switch ($variant)
            {
                'clean' { }
                'bad-archive'
                {
                    if (-not $BadArchivePath) { throw '-SetSelfCheckVariants 里有 bad-archive，但没有给 -BadArchivePath' }
                    if (-not $entryArchive) { throw '库里没有可换的档案（库是空的？）' }
                    $backup = "$entryArchive.harness-backup"
                    Copy-Item -LiteralPath $entryArchive -Destination $backup -Force
                    Copy-Item -LiteralPath $BadArchivePath -Destination $entryArchive -Force
                    $revert = { Copy-Item -LiteralPath $backup -Destination $entryArchive -Force; Remove-Item -LiteralPath $backup -Force -ErrorAction SilentlyContinue }
                    $result.mutation = ('把库档案换成坏档案：{0}' -f $BadArchivePath)
                }
                'byte-flip'
                {
                    if (-not $entryArchive) { throw '库里没有可换的档案（库是空的？）' }
                    $bytes = [System.IO.File]::ReadAllBytes($entryArchive)
                    $offset = [int]($bytes.Length * 0.6)
                    $original = $bytes[$offset]
                    $bytes[$offset] = [byte]($original -bxor 0x5A)
                    [System.IO.File]::WriteAllBytes($entryArchive, $bytes)
                    $revert = {
                        $again = [System.IO.File]::ReadAllBytes($entryArchive)
                        $again[$offset] = $original
                        [System.IO.File]::WriteAllBytes($entryArchive, $again)
                    }
                    $result.mutation = ('把库档案第 {0} 个字节 {1:X2} → {2:X2}（CRC 必然不符）' -f $offset, $original, $bytes[$offset])
                }
                'manifest-malformed'
                {
                    if (-not $libInfo.ManifestPath) { throw '库里没有 manifest.txt' }
                    $text = [string](Read-TextAutoShared -Path $libInfo.ManifestPath -MaxBytes 2000000)
                    # **真的少一列**：表头是 10 列，这里拼 9 列（踩过一次：数组写了 10 项，
                    # 结果是"结构完全合法、只是指向一个不存在的档案"，考到的是另一条防线）。
                    $badFields = @('YuaHarness', '坏行少一列', ('A' * 64 -join ''), '', ('B' * 64 -join ''), ('C' * 64 -join ''), '3', '', '')
                    $appended = $text + ("{0}`r`n" -f ($badFields -join "`t"))
                    Write-Utf8File -Path $libInfo.ManifestPath -Text $appended -NoNewline
                    $revert = { Write-Utf8File -Path $libInfo.ManifestPath -Text $text -NoNewline }
                    $result.mutation = ('往库清单尾部追加一行只有 {0} 列的坏行' -f $badFields.Count)
                }
                'manifest-bad-digest'
                {
                    if (-not $libInfo.ManifestPath) { throw '库里没有 manifest.txt' }
                    $text = [string](Read-TextAutoShared -Path $libInfo.ManifestPath -MaxBytes 2000000)
                    $appended = $text + ("{0}`r`n" -f (@('YuaHarness', '摘要不是hex', ('Z' * 64 -join ''), '', ('B' * 64 -join ''), ('C' * 64 -join ''), '3', '', '', '') -join "`t"))
                    Write-Utf8File -Path $libInfo.ManifestPath -Text $appended -NoNewline
                    $revert = { Write-Utf8File -Path $libInfo.ManifestPath -Text $text -NoNewline }
                    $result.mutation = '往库清单尾部追加一行"档案摘要不是 64 位 hex"（这一份文本不可信）'
                }
                'library-readonly'
                {
                    if (-not $libInfo.Root) { throw '库根不存在' }
                    $acl = Get-Acl -LiteralPath $libInfo.Root
                    $deny = New-Object System.Security.AccessControl.FileSystemAccessRule(
                        (New-Object System.Security.Principal.NTAccount($env:USERNAME)), 'Write',
                        'ContainerInherit,ObjectInherit', 'None', 'Deny')
                    $acl.AddAccessRule($deny)
                    Set-Acl -LiteralPath $libInfo.Root -AclObject $acl
                    $revert = {
                        $restoreAcl = Get-Acl -LiteralPath $libInfo.Root
                        $restoreAcl.RemoveAccessRule($deny) | Out-Null
                        Set-Acl -LiteralPath $libInfo.Root -AclObject $restoreAcl
                    }
                    $result.mutation = ('给库根加一条 Deny Write ACL（{0}）' -f $env:USERNAME)
                }
                'missing-dll'
                {
                    if (-not (Test-Path -LiteralPath $dllPath)) { throw ("找不到要藏起来的 DLL：{0}" -f $dllPath) }
                    $hidden = "$dllPath.harness-hidden"
                    Move-Item -LiteralPath $dllPath -Destination $hidden
                    $revert = { if (Test-Path -LiteralPath $hidden) { Move-Item -LiteralPath $hidden -Destination $dllPath } }
                    $result.mutation = ('把 Hash 收集模块挪走：{0}' -f $dllPath)
                }
                default { throw ("未知的自检变体：{0}（只认 clean/bad-archive/byte-flip/manifest-malformed/manifest-bad-digest/library-readonly/missing-dll）" -f $variant) }
            }

            $result.panelBefore = Read-ControlText ([int]$script:UiIds.Page) 200000
            $result.libraryDigestBefore = Get-TreeDigestExcluding -Path $libInfo.Root
            $result.toolDigestBefore = Get-TreeDigestExcluding -Path $toolDir -ExcludeNames @('Log')
            # 改过库之后要**先刷新列表**：自检读的是 context.Catalog（上一次 RefreshLibrary 的结果），
            # 不刷新的话它看的是内存里的旧清单，用例会静默地什么都没测到。
            if ($variant -ne 'clean') { Click-Control ([int]$script:UiIds.SetmRefresh) '刷新列表（自检前，让内存清单跟上磁盘）' }
            $dialogs = Invoke-ModalAction ([int]$script:UiIds.SetmSelfCheck) '运行环境自检' -TimeoutMs 30000
            Start-Sleep -Milliseconds 500
            $result.modals = @($dialogs | ForEach-Object { [pscustomobject]@{ title = $_.Title; text = $_.Text; buttons = $_.Buttons } })
            $result.panelAfter = Read-ControlText ([int]$script:UiIds.Page) 200000
            $result.libraryDigestAfter = Get-TreeDigestExcluding -Path $libInfo.Root
            $result.toolDigestAfter = Get-TreeDigestExcluding -Path $toolDir -ExcludeNames @('Log')
        }
        catch
        {
            $result.mutationError = $_.Exception.Message
            Add-Event ("自检变体 {0} 执行出错：{1}" -f $variant, $result.mutationError)
        }
        finally
        {
            if ($revert) { try { & $revert } catch { Add-Event ("还原变体 {0} 失败：{1}" -f $variant, $_.Exception.Message) } }
        }
        [void]$results.Add([pscustomobject]$result)
        Add-Event ("自检变体 {0} 完成（改法：{1}；弹框 {2} 个；面板 {3} 字）" -f `
                $variant, $result.mutation, @($result.modals).Count, ([string]$result.panelAfter).Length)
    }

    $record.setCheck = [ordered]@{
        libraryRoot = $libInfo.Root
        entryArchive = $entryArchive
        dllPath = $dllPath
        variants = @($results)
    }
    $section.ok = $true
    $section.variantCount = $results.Count
    $record.stepResults['setcheck'] = $section
}

if ($Step -contains 'setupdate')
{
    $section = [ordered]@{ name = 'setupdate'; ok = $false; reason = '' }
    $toolDir = Split-Path -Parent $AppExe
    $libInfo = Get-LibraryInfo -ToolDir $toolDir

    $probeBefore = [ordered]@{
        libraryRoot   = $libInfo.Root
        libraryDigest = Get-TreeDigestExcluding -Path $libInfo.Root
        manifestText  = $libInfo.ManifestText
        manifestRows  = @($libInfo.Rows).Count
        rows          = @($libInfo.Rows)
        toolDigest    = Get-TreeDigestExcluding -Path $toolDir -ExcludeNames @('Log')
        stagingFiles  = @(Get-FileListing -Path (Join-Path $toolDir 'Log\OnlineStaging') -IncludeDirectories)
    }

    Click-Control ([int]$script:UiIds.SetmUpdate) '在线更新（模态弹窗）'
    $dialog = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline -and $dialog -eq [IntPtr]::Zero)
    {
        foreach ($window in [Harness.Win]::TopLevel($appPid))
        {
            if ($window.Class -eq [string]$script:UiIds.OnlineWindowClass) { $dialog = [IntPtr]$window.Hwnd; break }
        }
        if ($dialog -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 200 }
    }
    if ($dialog -eq [IntPtr]::Zero)
    {
        $seen = ([Harness.Win]::TopLevel($appPid) | ForEach-Object { '{0}({1})' -f $_.Class, $_.Text }) -join '; '
        $section.reason = ("等不到「在线更新」弹窗（类 {0}）。当前窗口：{1}" -f $script:UiIds.OnlineWindowClass, $seen)
        $record.stepResults['setupdate'] = $section
        Save-And-Exit $section.reason
    }
    Start-Sleep -Milliseconds 500
    Add-Event ("在线更新弹窗出现（class={0} title='{1}'）；主窗口 enabled={2}（模态应为 False）" -f `
            [Harness.Win]::ClassOf($dialog), [Harness.Win]::CaptionOf($dialog), [Harness.Win]::IsWindowEnabled($script:mainWindow))

    $rounds = New-Object System.Collections.ArrayList
    try
    {
        for ($round = 1; $round -le [Math]::Max(1, $OnlineRuns); $round++)
        {
            $entry = [ordered]@{
                round = $round
                endpointShown = (Get-DialogControlText $dialog ([int]$script:UiIds.OnlineEndpoint) 4000)
                statusAfterRefresh = ''; listItemsAfterRefresh = @(); refreshElapsedMs = 0
                installClicked = $false; statusAfterInstall = ''; listItemsAfterInstall = @()
                installElapsedMs = 0; modals = @(); manifestRowsAfter = 0; manifestTextAfter = ''; libraryDigestAfter = $null
            }
            # 拉索引
            Click-DialogControl $dialog ([int]$script:UiIds.OnlineRefresh) '拉取索引'
            $refresh = Wait-OnlineIdle -Window $dialog -TimeoutSeconds $OnlineJobTimeoutSec -What ('拉取索引（第 {0} 轮）' -f $round)
            $entry.statusAfterRefresh = $refresh.Status
            $entry.refreshElapsedMs = $refresh.ElapsedMs
            $entry.refreshTimedOut = [bool]$refresh.Busy
            $entry.listItemsAfterRefresh = @(Get-DialogListItems $dialog ([int]$script:UiIds.OnlineList))
            $entry.progressAfterRefresh = (Get-DialogControlText $dialog ([int]$script:UiIds.OnlineProgress) 200)

            if ($OnlineInstall)
            {
                $entry.installClicked = $true
                if ($entry.listItemsAfterRefresh.Count -gt 0)
                {
                    [void](Select-DialogListItem $dialog ([int]$script:UiIds.OnlineList) $OnlineListIndex '远端条目')
                }
                Click-DialogControl $dialog ([int]$script:UiIds.OnlineInstall) '下载并安装选中'
                $install = Wait-OnlineIdle -Window $dialog -TimeoutSeconds $OnlineJobTimeoutSec -What ('下载并安装（第 {0} 轮）' -f $round)
                # 覆盖确认 / 结论 / 失败都会弹模态框：确认框默认点「否」（不覆盖本地），
                # 要测"用远端覆盖"才传 -OnlinePreferYes。
                $installModals = New-Object System.Collections.ArrayList
                $deadline = (Get-Date).AddSeconds(10)
                while ((Get-Date) -lt $deadline)
                {
                    $dialogs = @(Resolve-ModalDialogs -PreferYes:$OnlinePreferYes)
                    foreach ($dialogEntry in $dialogs)
                    {
                        [void]$installModals.Add([pscustomobject]@{ title = $dialogEntry.Title; text = $dialogEntry.Text; buttons = $dialogEntry.Buttons })
                        $record.modalDialogs += $dialogEntry
                    }
                    if ($installModals.Count -gt 0) { break }
                    Start-Sleep -Milliseconds 300
                }
                Start-Sleep -Milliseconds 500
                $entry.modals = @($installModals)
                $entry.statusAfterInstall = Get-DialogControlText $dialog ([int]$script:UiIds.OnlineStatus) 200000
                $entry.installElapsedMs = $install.ElapsedMs
                $entry.installTimedOut = [bool]$install.Busy
                $entry.listItemsAfterInstall = @(Get-DialogListItems $dialog ([int]$script:UiIds.OnlineList))
            }

            # 每一轮都记一次本地状态：**失败时前后必须逐字节相同**、成功时清单必须多一条，
            # 判据都在断言那侧（这里只如实记录）。
            $libNow = Get-LibraryInfo -ToolDir $toolDir
            $entry.manifestRowsAfter = @($libNow.Rows).Count
            $entry.manifestTextAfter = $libNow.ManifestText
            $entry.libraryDigestAfter = Get-TreeDigestExcluding -Path $libNow.Root
            $entry.stagingFiles = @(Get-FileListing -Path (Join-Path $toolDir 'Log\OnlineStaging') -IncludeDirectories)
            [void]$rounds.Add([pscustomobject]$entry)
            Add-Event ("第 {0} 轮完成：索引 {1} 项 / 库清单 {2} 行" -f $round, $entry.listItemsAfterRefresh.Count, $entry.manifestRowsAfter)
        }
    }
    finally
    {
        # 关弹窗：**必须先点「关闭」**（有联网任务在跑时产品会问一句，兜底点「是」）。
        # 不关的话主窗口一直是 disabled，后面的收尾会很难看。
        try
        {
            if ([Harness.Win]::IsWindow($dialog))
            {
                Click-DialogControl $dialog ([int]$script:UiIds.OnlineClose) '关闭（在线更新弹窗）'
                Start-Sleep -Milliseconds 600
                foreach ($dialogEntry in (Resolve-ModalDialogs -PreferYes)) { $record.modalDialogs += $dialogEntry }
                if ([Harness.Win]::IsWindow($dialog))
                {
                    Add-Event '「关闭」按钮没关掉弹窗，改用 WM_CLOSE 兜底'
                    [void][Harness.Win]::PostClose($dialog)
                    Start-Sleep -Milliseconds 600
                    foreach ($dialogEntry in (Resolve-ModalDialogs -PreferYes)) { $record.modalDialogs += $dialogEntry }
                }
            }
        }
        catch { Add-Event ("关在线更新弹窗出错：{0}" -f $_.Exception.Message) }
    }

    Start-Sleep -Milliseconds 800
    $libAfter = Get-LibraryInfo -ToolDir $toolDir
    $record.online = [ordered]@{
        endpoint        = $Endpoint
        endpointShown   = $(if (@($rounds).Count -gt 0) { $rounds[0].endpointShown } else { '' })
        rounds          = @($rounds)
        before          = $probeBefore
        after           = [ordered]@{
            libraryRoot   = $libAfter.Root
            libraryDigest = Get-TreeDigestExcluding -Path $libAfter.Root
            manifestText  = $libAfter.ManifestText
            manifestRows  = @($libAfter.Rows).Count
            rows          = @($libAfter.Rows)
            toolDigest    = Get-TreeDigestExcluding -Path $toolDir -ExcludeNames @('Log')
            stagingFiles  = @(Get-FileListing -Path (Join-Path $toolDir 'Log\OnlineStaging') -IncludeDirectories)
        }
        toolIniText     = $record.toolIniText
        # 弹窗关掉之后主界面会重刷库（OnSetOnlineUpdate 里 RefreshLibrary），这条就是它的证据
        pageTextAfterClose = $(if ($dialog -ne [IntPtr]::Zero -and -not [Harness.Win]::IsWindow($dialog)) { Read-ControlText ([int]$script:UiIds.Page) 200000 } else { '' })
    }
    $section.ok = $true
    $record.stepResults['setupdate'] = $section
    Add-Event ("在线更新用例结束：{0} 轮；库清单 {1} → {2} 行" -f @($rounds).Count, $probeBefore.manifestRows, $libAfter.Rows.Count)
}

# 「打开选中扩展集」(4133)：三种路径各自取证。
#   ok      —— 正常打开（面板「最近操作」要写「已打开 <路径>」，日志同）
#   none    —— 什么都没选就点（必须给出可操作提示，而不是默默什么都不做）
#   missing —— 选好了、但库内目录被人在资源管理器里删了（必须报"目录不存在"）
#
# **只断言路径文本，不去判资源管理器窗口**：那一层是 ShellExecute 的领域，
# 判它等于把"Explorer 起没起来"混进产品结论里（Explorer 的单例/焦点行为不可控）。
$setOpenModes = @('ok', 'none', 'missing')
if ($setOpenMode -notin $setOpenModes) { throw ("-SetOpenMode 只认 {0}" -f ($setOpenModes -join '/')) }
if ($Step -contains 'setopen')
{
    $section = [ordered]@{ name = 'setopen'; ok = $false; reason = '' }
    $toolDir = Split-Path -Parent $AppExe
    $libInfo = Get-LibraryInfo -ToolDir $toolDir

    $selected = $true
    $movedDirectory = ''
    $originalDirectory = ''
    if ($setOpenMode -eq 'none')
    {
        # 什么都不选：先刷一次列表（清掉上一步可能留下的选中态是不可能的——选中态在内存里，
        # 所以 none 场景必须**单独跑一次驱动**，这条在调用方那侧保证）。
        $selected = $false
    }
    else
    {
        if (@($libInfo.Rows).Count -eq 0) { Save-And-Exit '-SetOpenMode 需要库里有至少一份扩展集（先跑 install 或在线更新）' }
        # 选中态由**界面**决定（列表顺序由产品给，不一定等于清单行序）：
        # 先按列表第 0 项选会社、再第 0 项选作品；期望目录必须用 **sanitize 之后**的名字拼
        #（产品落库时就走 MakeSafePathComponent；拿原始名拼会指向一个不存在的路径）。
        $publisherPick = Set-ListSelection ([int]$script:UiIds.LibPublishers) 0 '会社'
        $titlePick = Set-ListSelection ([int]$script:UiIds.LibTitles) 0 '作品'
        $row = [pscustomobject]@{ publisher = [string]$publisherPick.Text; title = [string]$titlePick.Text }
        $originalDirectory = Join-Path (Join-Path $libInfo.Root (ConvertTo-SafePathComponent $row.publisher)) `
            (ConvertTo-SafePathComponent $row.title)
        if (-not $row.publisher -or -not $row.title) { Save-And-Exit '会社 / 作品列表里没有可选项，-SetOpenMode 的 ok/missing 场景做不了' }
        if ($setOpenMode -eq 'missing')
        {
            if (-not (Test-Path -LiteralPath $originalDirectory)) { Save-And-Exit ("-SetOpenMode missing：库内目录本来就不存在：{0}" -f $originalDirectory) }
            $movedDirectory = "$originalDirectory.harness-moved"
            if (Test-Path -LiteralPath $movedDirectory) { [void](Remove-TreeUnder -Path $movedDirectory -MustBeUnder $runRoot) }
            Move-Item -LiteralPath $originalDirectory -Destination $movedDirectory
            Add-Event ("把库内目录暂时挪走（E-15「目录缺失」场景）：{0}" -f $originalDirectory)
        }
    }

    try
    {
        $panelBefore = Read-ControlText ([int]$script:UiIds.Page) 200000
        $dialogs = Invoke-ModalAction ([int]$script:UiIds.SetmOpenEntry) '打开选中扩展集' -TimeoutMs 8000
        Start-Sleep -Milliseconds 800
        $panelAfter = Read-ControlText ([int]$script:UiIds.Page) 200000
        $record.setOpen = [ordered]@{
            mode                 = $setOpenMode
            libraryRoot          = $libInfo.Root
            expectedDirectory    = $originalDirectory
            directoryMovedTo     = $movedDirectory
            selectedAnything     = $selected
            panelBefore          = $panelBefore
            panelAfter           = $panelAfter
            modals               = @($dialogs | ForEach-Object { [pscustomobject]@{ title = $_.Title; text = $_.Text; buttons = $_.Buttons } })
            # 日志在收尾时统一抓（toolLogs），断言那侧读它找「已打开扩展集目录：<路径>」
        }
    }
    finally
    {
        if ($movedDirectory -and (Test-Path -LiteralPath $movedDirectory))
        {
            if (Test-Path -LiteralPath $originalDirectory) { [void](Remove-TreeUnder -Path $originalDirectory -MustBeUnder $runRoot) }
            Move-Item -LiteralPath $movedDirectory -Destination $originalDirectory
            Add-Event ("库内目录已挪回：{0}" -f $originalDirectory)
        }
    }
    $section.ok = $true
    $record.stepResults['setopen'] = $section
    Add-Event ("打开选中扩展集（{0}）取样完成：弹框 {1} 个" -f $setOpenMode, @($record.setOpen.modals).Count)
}

# ==================================================================
# 步骤：首页模式1 / 传统动态模式
# ==================================================================
if ($Step -contains 'mode1' -or $Step -contains 'classic' -or $Step -contains 'step1' -or $Step -contains 'step3')
{
    Select-Tab ([int]$script:UiIds.TabHome) 0 '首页' -ExpectVisible @([int]$script:UiIds.ModeExtract)
}

if ($Step -contains 'mode1' -or $Step -contains 'classic' -or $Step -contains 'step1')
{
    $modeId = if ($Step -contains 'classic') { [int]$script:UiIds.ModeClassic } else { [int]$script:UiIds.ModeExtract }
    $modeName = if ($Step -contains 'classic') { '传统动态模式' } else { '该作扩展集提取（模式1）' }
    $modeInfo = Require-Control $modeId ('模式按钮 ' + $modeName)
    [void][Harness.Win]::PostClick($modeInfo.Hwnd)
    Start-Sleep -Milliseconds 600
    $checked = [Harness.Win]::CheckState($modeInfo.Hwnd)
    Add-Event ("点了模式按钮「{0}」（id={1}），BM_GETCHECK={2}" -f $modeName, $modeId, $checked)
    if ($checked -ne 1) { Save-And-Exit ("模式按钮「{0}」点下去后 BM_GETCHECK={1}（应为 1；按钮可能是 BS_AUTORADIOBUTTON，见 §8.4）" -f $modeName, $checked) }
    $record.mode = if ($Step -contains 'classic') { 'classic' } else { 'mode1' }

    # 模式1 需要先在左侧选会社/作品（扫扩展集库）
    if (-not ($Step -contains 'classic'))
    {
        Set-ListSelection ([int]$script:UiIds.LibPublishers) 0 '会社'
        Start-Sleep -Milliseconds 600
        Set-ListSelection ([int]$script:UiIds.LibTitles) 0 '作品'
        $record.overviewText = Read-ControlText $overviewId
    }
}

if ($Step -contains 'mode1' -or $Step -contains 'step1')
{
    $section = [ordered]@{ name = 'step1'; ok = $false; reason = '' }
    [void](Remove-TreeUnder -Path $staticHashDir -MustBeUnder $runRoot)
    Click-Control ([int]$script:UiIds.Step1) '步骤1：根据该作扩展集生成静态 Hash 表'
    $taskText = Wait-TaskIdle '模式1 步骤1（生成静态 Hash 表）'
    Start-Sleep -Milliseconds $SettleMs
    $section.taskText = $taskText
    $lst = Join-Path $staticHashDir 'StaticHash.lst'
    $info = Get-FileInfo $lst
    $section.staticHashLst = $lst
    $section.exists = $info.Exists
    $section.size = $info.Size
    $section.sha256 = $info.Sha256
    $record.staticHashLst = $lst
    $record.staticHashLstSize = $info.Size
    $record.staticHashLstSha256 = $info.Sha256
    if (-not $info.Exists)
    {
        $section.reason = ("步骤1 结束后没有 {0}（§13.2：StaticHash_Output 只写 StaticHash.lst）" -f $lst)
        $record.stepResults['step1'] = $section
        $record.overviewText = Read-ControlText $overviewId
        Save-And-Exit $section.reason
    }
    $text = Read-TextAutoShared -Path $lst -MaxBytes 4000000
    $lines = @($text -split "`r?`n" | Where-Object { $_.Trim() })
    $record.staticHashLineCount = $lines.Count
    $record.staticHashText = if ($text.Length -gt 400000) { $text.Substring(0, 400000) } else { $text }
    # 留一份**不可变快照**：后续用例（尤其 E-4 的坏档案 step1）会把 StaticHash_Output 清掉重建，
    # 断言再去读现场就会"文件不存在"，从而把 E-3 的独立枚举弄成假失败（踩过）。
    $snapshot = Join-Path $outDir ("extension-{0}-StaticHash.lst" -f $Game)
    Copy-Item -LiteralPath $lst -Destination $snapshot -Force
    $record['staticHashLstSnapshot'] = $snapshot
    $record['staticHashLstSnapshotSha256'] = (Get-Sha256Hex -Path $snapshot)
    $section.lineCount = $lines.Count
    $section.snapshot = $snapshot
    $section.ok = $true
    $record.stepResults['step1'] = $section
    Add-Event ("StaticHash.lst：{0} 字节 / {1} 行" -f $info.Size, $lines.Count)
}

if ($Step -contains 'mode1' -or $Step -contains 'classic' -or $Step -contains 'step3')
{
    $section = [ordered]@{ name = 'step3'; ok = $false; reason = '' }
    [void](Remove-TreeUnder -Path $restoredOutput -MustBeUnder $runRoot)
    $record.outputDir = $restoredOutput

    # 步骤2：模式1 是「提取动态 XP3 资源」；合成工作区里没有真游戏，用预置的 Extractor_Output 代替。
    # 传统模式同样不跑真解包，沿用同一份预置目录 —— 两个模式的输入因此完全一致。
    $record.notes += '步骤2（提取动态 XP3 资源）在本脚手架里是**预置目录**，没有真的跑解包；E-1/E-10 的对账对象是同一份解包目录，因此差异只可能来自"映射来源"（动态采集 vs 扩展集）'

    $step3Id = [int]$script:UiIds.Step3
    $runStart = Get-Date
    Click-Control $step3Id '步骤3：用（静态/动态）Hash 还原资源名'

    # 还原窗口（既有实现是独立顶层窗口；也可能是主窗口内联）
    $restoreWindow = [IntPtr]::Zero
    $restoreClass = ''
    $deadline = (Get-Date).AddSeconds(40)
    while ((Get-Date) -lt $deadline)
    {
        foreach ($dialog in (Resolve-ModalDialogs)) { $record.modalDialogs += $dialog }
        foreach ($window in [Harness.Win]::TopLevel($appPid))
        {
            if ($window.Hwnd -eq $script:mainWindow.ToInt64()) { continue }
            if ($window.Class -in @('#32770', 'IME', 'MSCTFIME UI', 'Default IME', 'tooltips_class32', 'SysShadow')) { continue }
            $restoreWindow = [IntPtr]$window.Hwnd
            $restoreClass = $window.Class
            break
        }
        if ($restoreWindow -ne [IntPtr]::Zero) { break }
        Start-Sleep -Milliseconds 250
    }
    if ($restoreWindow -eq [IntPtr]::Zero)
    {
        # 内联还原：主流程按钮会被禁用
        $enabled = [Harness.Win]::IsWindowEnabled([Harness.Win]::GetDlgItem($script:mainWindow, $step3Id))
        if ($enabled)
        {
            $section.reason = '点了步骤3 之后 40 秒内既没出现还原窗口，也没进入内联还原状态'
            $record.stepResults['step3'] = $section
            Save-And-Exit $section.reason
        }
        Add-Event '判定为内联还原（主流程按钮被禁用）'
    }
    else
    {
        Add-Event ("还原窗口出现：class={0} title='{1}'" -f $restoreClass, [Harness.Win]::CaptionOf($restoreWindow))
        # 还原窗口里点「开始」（文案归一化后再比，避免加速键踩坑）
        $startButton = $null
        foreach ($child in [Harness.Win]::Children($restoreWindow.ToInt64()))
        {
            if ($child.Class -ne 'Button') { continue }
            $normalized = (($child.Text -replace '&', '') -replace '\s*\([^)]*\)\s*', '').Trim()
            if ($normalized -in @('开始', '开始还原', '开始执行')) { $startButton = $child; break }
        }
        if ($startButton) { [void][Harness.Win]::PostClick($startButton.Hwnd); Add-Event ("点「{0}」开始还原" -f $startButton.Text) }
        else { Add-Event '还原窗口里没有「开始」按钮（可能点了步骤3 就直接跑）' }
    }

    # 等结束：标题终态 + 输出目录稳定
    $titleSamples = New-Object System.Collections.ArrayList
    $outputSignature = ''
    $outputStableSince = Get-Date
    $completionReason = ''
    $lastTitle = ''
    $timeout = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $timeout)
    {
        if ($script:appProcess.HasExited) { $completionReason = ("被测程序已退出（退出码 {0}）" -f $script:appProcess.ExitCode); break }
        foreach ($dialog in (Resolve-ModalDialogs)) { $record.modalDialogs += $dialog }

        $currentTitle = if ($restoreWindow -ne [IntPtr]::Zero -and [Harness.Win]::IsWindow($restoreWindow)) { [Harness.Win]::CaptionOf($restoreWindow) } else { [Harness.Win]::CaptionOf($script:mainWindow) }
        if ($currentTitle -ne $lastTitle)
        {
            $lastTitle = $currentTitle
            [void]$titleSamples.Add(('{0}|{1}' -f [int]((Get-Date) - $runStart).TotalMilliseconds, $currentTitle))
            Add-Event ("标题：{0}" -f $currentTitle)
        }
        $signature = if (Test-Path -LiteralPath $restoredOutput) { [Harness.Batch]::Signature($restoredOutput) } else { '' }
        if ($signature -ne $outputSignature) { $outputSignature = $signature; $outputStableSince = Get-Date }
        $settled = ((Get-Date) - $outputStableSince).TotalMilliseconds
        $count = Get-FastFileCount -Path $restoredOutput
        $terminal = ($currentTitle -match '排队\s*0' -and $currentTitle -match '运行中\s*0' -and $currentTitle -match '完成\s*[1-9]')
        if ($settled -ge $SettleMs -and $count -gt 0 -and $terminal) { $completionReason = '标题显示终态且输出目录稳定'; break }
        if ($settled -ge [Math]::Max($SettleMs * 4, 20000) -and $count -gt 0) { $completionReason = ('标题未出现终态字样，但输出目录已稳定 {0:N0}ms' -f $settled); break }
        Start-Sleep -Milliseconds 300
    }
    $record.titleTimeline = @($titleSamples)
    $record.completionReason = $completionReason
    if (-not $completionReason)
    {
        $section.reason = ("{0} 秒内没等到还原结束（输出 {1} 个文件）" -f $TimeoutSec, (Get-FastFileCount -Path $restoredOutput))
        $record.stepResults['step3'] = $section
        Save-And-Exit $section.reason
    }

    $outputFiles = @(Get-OutputListing -Path $restoredOutput)
    if ($outputFiles.Count -gt 2000)
    {
        $tsvPath = Join-Path $outDir ("extension-{0}-output.tsv" -f $Game)
        $lines = New-Object System.Collections.ArrayList
        [void]$lines.Add("rel`tsize`tsha256")
        foreach ($item in $outputFiles) { [void]$lines.Add(("{0}`t{1}`t{2}" -f $item.rel, $item.size, $item.sha256)) }
        Write-TsvFile -Path $tsvPath -Lines ([string[]]$lines.ToArray())
        $record.outputTsv = $tsvPath
    }
    else { $record.output = $outputFiles }

    $reportCandidate = @($outputFiles | Where-Object { $_.rel -notmatch '\\' -and $_.rel -match '^RestoreReport' })
    if ($reportCandidate.Count -gt 0)
    {
        $record.reportPath = Join-Path $restoredOutput $reportCandidate[0].rel
        $record.reportText = Read-TextAutoShared -Path $record.reportPath -MaxBytes 2000000
    }
    $record.overviewText = Read-ControlText $overviewId
    $section.outputCount = $outputFiles.Count
    $section.reportPath = $record.reportPath
    $section.ok = $true
    $record.stepResults['step3'] = $section
    Add-Event ("还原结束：{0}（输出 {1} 个文件）" -f $completionReason, $outputFiles.Count)
}

# ==================================================================
# 收尾：只读证据 + 落盘
# ==================================================================

$record.extractorDigestAfter = Get-DirectoryDigest -Path $extractorOutput -WithHash
if ($record.sourceDir)
{
    $record.sourceDigestAfter = Get-DirectoryDigest -Path $record.sourceDir -WithHash
    $record['sourceContentDigestAfter'] = Get-DirectoryContentDigest -Path $record.sourceDir
}
Add-Event ("解包目录摘要（后）：{0} 个文件，digest {1}" -f $record.extractorDigestAfter.Count, ([string]$record.extractorDigestAfter.Digest).Substring(0, 12))

# 工具日志（诊断用；共享读，避免被还活着的被测程序占住）
$toolLogDir = Join-Path (Split-Path -Parent $AppExe) 'Log'
$toolLogs = @()
if (Test-Path -LiteralPath $toolLogDir)
{
    foreach ($file in (Get-ChildItem -LiteralPath $toolLogDir -File -ErrorAction SilentlyContinue | Sort-Object Name))
    {
        $text = Read-TextAutoShared -Path $file.FullName -MaxBytes 2000000
        $toolLogs += [pscustomobject]@{ Name = $file.Name; Size = [long]$file.Length; Text = $(if ($text -and $text.Length -gt 200000) { $text.Substring(0, 200000) } else { $text }) }
    }
}
$record['toolLogs'] = $toolLogs

# 只属于**这一次用例**的日志尾部（按启动时记下的偏移量切）
$toolLogTails = @()
if (Test-Path -LiteralPath $toolLogDir)
{
    foreach ($file in (Get-ChildItem -LiteralPath $toolLogDir -File -ErrorAction SilentlyContinue | Sort-Object Name))
    {
        $offset = 0
        if ($script:toolLogOffsetsBefore.Contains($file.Name)) { $offset = [long]$script:toolLogOffsetsBefore[$file.Name] }
        if ($file.Length -le $offset) { continue }
        $tail = ''
        try
        {
            $stream = [System.IO.File]::Open($file.FullName, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read,
                                             [System.IO.FileShare]::ReadWrite)
            try
            {
                [void]$stream.Seek($offset, [System.IO.SeekOrigin]::Begin)
                $length = [int][Math]::Min(($file.Length - $offset), 2000000)
                $buffer = New-Object byte[] $length
                $read = $stream.Read($buffer, 0, $length)
                $tail = [System.Text.Encoding]::UTF8.GetString($buffer, 0, $read)
            }
            finally { $stream.Dispose() }
        }
        catch { $tail = '' }
        if ($tail) { $toolLogTails += [pscustomobject]@{ Name = $file.Name; Offset = $offset; Text = $tail } }
    }
}
$record['toolLogTails'] = $toolLogTails

$record.completed = $true
Save-And-Exit '' 0
