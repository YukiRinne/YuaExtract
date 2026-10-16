# 一键跑完整个验证脚手架：构建空宿主 → 造合成工作区 → 跑步骤3 两种模式 →
# 跑步骤2 两种环境 → 汇总所有断言。
#
#   pwsh -File run-all.ps1
#   pwsh -File run-all.ps1 -Scope restore                          # 只跑步骤3 相关
#   pwsh -File run-all.ps1 -Scope collect                          # 只跑断言C
#   pwsh -File run-all.ps1 -SkipWorkspaceRebuild                   # 复用现有合成工作区
#
# 退出码：0 = 所有断言通过；1 = 有断言失败；2 = 前置条件没满足（没编译等）。

[CmdletBinding()]
param(
    [ValidateSet('all', 'restore', 'collect', 'pressure', 'supplement', 'extension')][string]$Scope = 'all',
    [int]$ObserveMs = 7000,
    [int]$TimeoutSec = 600,
    [switch]$SkipBuild,
    [switch]$SkipWorkspaceRebuild,
    # 补充还原夹具的「挖掘噪声」候选数：调大能把挖掘拉到分钟级（T-1 的规模档）
    [int]$NoiseCandidates = 20000,
    [int]$PressurePackages = 12,
    [int]$PressureFilesPerPackage = 5000,
    [switch]$AllowLegacyInline,
    [string]$AppExe = '',
    # 假绿护栏：本轮实际执行到的断言项数下限（0 = 按 -Scope 取默认值）。
    # 低于下限（例如某一步在写结果之前就失败了，checks 全是上一轮的残留）→ 非 0 退出。
    [int]$MinExecutedChecks = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessSummary.ps1')

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $root '..\..'))
$releaseExe = if ($AppExe) { $AppExe } else { Join-Path $repoRoot 'Release\YuaExtract.exe' }
$releaseDll = Join-Path $repoRoot 'Release\CxdecExtractordll\CxdecStringDumper.dll'
$outRoot = Join-Path $root 'Run\out'
$runRoot = Join-Path $root 'Run'
# 本次运行的起点：汇总时用它区分「本次结果」与「上一次留下的」
$runStarted = Get-Date

Write-Host '=================================================================='
Write-Host ' YuaExtract 验证脚手架 —— 步骤2 只收集 / 步骤3 还原'
Write-Host '=================================================================='

# ---------- 前置检查 ----------

$preflightFailed = $false

function Show-Artifact([string]$Label, [string]$Path, [int]$ExpectedMachine)
{
    if (-not (Test-Path -LiteralPath $Path))
    {
        Write-Host ("  [缺] {0}：{1}" -f $Label, $Path)
        return $false
    }
    $item = Get-Item -LiteralPath $Path
    $machine = Get-PeMachine -Path $Path
    $flag = if ($machine -eq $ExpectedMachine) { 'OK' } else { '位数不对！' }
    Write-Host ("  [有] {0}：{1:yyyy-MM-dd HH:mm:ss}  {2:N0} 字节  {3}  {4}" -f `
            $Label, $item.LastWriteTime, $item.Length, (Format-PeMachine $machine), $flag)
    return ($machine -eq $ExpectedMachine)
}

Write-Host ''
Write-Host '[前置] 被测产物：'
$okExe = Show-Artifact 'YuaExtract.exe' $releaseExe 0x014C
$okDll = Show-Artifact 'CxdecStringDumper.dll' $releaseDll 0x014C
if (-not ($okExe -and $okDll))
{
    $preflightFailed = $true
    Write-Host '  → 请先编译解决方案（Release|x86），命令见 README。'
}

# 产物比源码旧 = 很可能忘了重新编译，这种假失败最难查，直接提示。
if ($okExe)
{
    $newestLoaderSource = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'CxdecExtractorLoader') -Recurse -File -Include *.cpp, *.h -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($newestLoaderSource -and $newestLoaderSource.LastWriteTime -gt (Get-Item -LiteralPath $releaseExe).LastWriteTime)
    {
        Write-Host ("  [注意] Loader 源码 {0}（{1:yyyy-MM-dd HH:mm:ss}）比 YuaExtract.exe 新 —— 可能没重新编译" -f $newestLoaderSource.Name, $newestLoaderSource.LastWriteTime)
    }
}
if ($okDll)
{
    $newestDumperSource = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'CxdecStringDumper') -Recurse -File -Include *.cpp, *.h -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($newestDumperSource -and $newestDumperSource.LastWriteTime -gt (Get-Item -LiteralPath $releaseDll).LastWriteTime)
    {
        Write-Host ("  [注意] StringDumper 源码 {0}（{1:yyyy-MM-dd HH:mm:ss}）比 DLL 新 —— 可能没重新编译" -f $newestDumperSource.Name, $newestDumperSource.LastWriteTime)
    }
}

if (-not (Test-Path -LiteralPath $outRoot)) { New-Item -ItemType Directory -Path $outRoot -Force | Out-Null }

# 把 Release\ 复制成脚手架自己的快照再跑：不锁住队友正在链接的 Release\YuaExtract.exe，
# 且一次验证全程用同一个版本。
if (-not $preflightFailed)
{
    Write-Host ''
    Write-Host '[快照] 复制 Release\ → Run\App\（被测程序从快照启动）'
    try
    {
        $snapshot = Sync-ReleaseSnapshot -RepoRoot $repoRoot -RunRoot (Join-Path $root 'Run') -Force
        Write-Host ("  YuaExtract.exe            {0:yyyy-MM-dd HH:mm:ss}  {1}  sha256 {2}" -f $snapshot.ExeTime, $snapshot.ExeMachine, $snapshot.ExeSha256.Substring(0, 16))
        Write-Host ("  CxdecStringDumper.dll     {0:yyyy-MM-dd HH:mm:ss}  {1}  sha256 {2}" -f $snapshot.DllTime, $snapshot.DllMachine, $snapshot.DllSha256.Substring(0, 16))
    }
    catch
    {
        Write-Host ("  快照失败：{0}" -f $_.Exception.Message)
        exit 2
    }
}

$steps = New-Object System.Collections.ArrayList

# 收尾：只杀**本脚手架快照**（Run\App）起的被测进程。
# 别的 teammate 可能也在手工开这个 exe，不要误伤；但自己起的必须收干净，
# 否则下一次用例会认到旧窗口，别人构建时还会撞 LNK1104。
function Stop-SnapshotProcesses
{
    $snapshotPrefix = Join-Path $root 'Run\App\*'
    $killed = 0
    foreach ($process in @(Get-Process YuaExtract -ErrorAction SilentlyContinue))
    {
        $path = ''
        try { $path = [string]$process.Path } catch { $path = '' }
        if ($path -and $path -like $snapshotPrefix)
        {
            try { $process.Kill(); $killed++ } catch { }
        }
    }
    if ($killed -gt 0) { Write-Host ("  · 收尾：结束了 {0} 个快照实例" -f $killed) }
}

function Invoke-Script
{
    param([string]$Label, [string]$Script, [string[]]$ScriptArguments, [int]$TimeoutSec = 0)

    Write-Host ''
    Write-Host ("------------------------------------------------------------------")
    Write-Host (" >> {0}" -f $Label)
    Write-Host ("------------------------------------------------------------------")

    $scriptPath = Join-Path $root $Script
    $code = 0

    if ($TimeoutSec -le 0)
    {
        & pwsh -NoProfile -File $scriptPath @ScriptArguments
        $code = $LASTEXITCODE
    }
    else
    {
        # 带硬超时的执行：UI 自动化最怕"卡在某个窗口结构上不动"，这里给每步一个上限，
        # 超时就强制结束并记 124（既不假装通过，也不会把整个 scope 拖成几十分钟）。
        $stem = [System.IO.Path]::GetFileNameWithoutExtension($Script)
        $stdoutPath = Join-Path $outRoot ('steplog-{0}.txt' -f $stem)
        $stderrPath = Join-Path $outRoot ('steplog-{0}.err.txt' -f $stem)
        # 注意：Start-Process 的 -ArgumentList 传数组时是**用空格拼**的，不自动加引号。
        # 本仓库路径里有空格和 `&`（`Harness&Agent Project`），所以必须自己给每个参数加引号，
        # 否则 pwsh 会把路径截成 "E:\Harness&Agent"（踩过）。
        function Quote-Argument([string]$Value)
        {
            if ($Value -match '[\s"&]') { return ('"' + ($Value -replace '"', '\"') + '"') }
            return $Value
        }
        $argumentList = @()
        foreach ($item in (@('-NoProfile', '-File', $scriptPath) + $ScriptArguments)) { $argumentList += (Quote-Argument ([string]$item)) }
        $process = Start-Process -FilePath 'pwsh' -ArgumentList ($argumentList -join ' ') -PassThru -NoNewWindow `
            -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
        if (-not $process.WaitForExit($TimeoutSec * 1000))
        {
            Write-Host ("  → 超过 {0} 秒还没结束：强制终止这一步（结果文件可能没写全）" -f $TimeoutSec)
            try { $process.Kill($true) } catch { }
            $code = 124
        }
        else
        {
            $code = $process.ExitCode
        }
        if (Test-Path -LiteralPath $stdoutPath) { Get-Content -LiteralPath $stdoutPath | ForEach-Object { Write-Host ('  | ' + $_) } }
        if (Test-Path -LiteralPath $stderrPath)
        {
            $errors = @(Get-Content -LiteralPath $stderrPath)
            if ($errors.Count -gt 0) { Write-Host '  [stderr]'; $errors | ForEach-Object { Write-Host ('  | ' + $_) } }
        }
    }

    Stop-SnapshotProcesses
    [void]$steps.Add([pscustomobject]@{ Label = $Label; Script = $Script; ExitCode = $code })
    return $code
}

if (-not $SkipBuild)
{
    Write-Host ''
    Write-Host '[构建] 验证用空宿主 EXE'
    & pwsh -NoProfile -File (Join-Path $root 'build-harness.ps1')
    if ($LASTEXITCODE -ne 0) { Write-Host '  → 空宿主构建失败，后续无法验证断言C' }
}

if ($preflightFailed)
{
    Write-Host ''
    Write-Host '前置条件不满足，先编译解决方案再跑。'
    exit 2
}

if ($Scope -in @('all', 'restore'))
{
    if (-not $SkipWorkspaceRebuild)
    {
        [void](Invoke-Script -Label '生成合成工作区（幂等重建）' -Script 'make-workspace.ps1' -ScriptArguments @())
    }

    [void](Invoke-Script -Label '用例：步骤3 默认语义（只输出已还原的文件）' -Script 'run-step3-restore.ps1' -ScriptArguments @('-TimeoutSec', "$TimeoutSec"))
    [void](Invoke-Script -Label '断言 A + D' -Script 'assert-restore.ps1' -ScriptArguments (@('-Mode', 'default', '-ChecksJson', (Join-Path $outRoot 'checks-default.json')) + $(if ($AllowLegacyInline) { @('-AllowLegacyInline') } else { @() })))

    [void](Invoke-Script -Label '用例：步骤3 勾选「保留未还原文件名的文件」' -Script 'run-step3-restore.ps1' -ScriptArguments @('-KeepUnrestored', '-TimeoutSec', "$TimeoutSec"))
    [void](Invoke-Script -Label '断言 B + D' -Script 'assert-restore.ps1' -ScriptArguments (@('-Mode', 'keep', '-ChecksJson', (Join-Path $outRoot 'checks-keep.json')) + $(if ($AllowLegacyInline) { @('-AllowLegacyInline') } else { @() })))
}

if ($Scope -in @('all', 'collect'))
{
    [void](Invoke-Script -Label '断言 C+E：步骤2 只收集（含对照组与环境变量端到端）' -Script 'assert-collect-only.ps1' -ScriptArguments @('-Run', '-ObserveMs', "$ObserveMs", '-ChecksJson', (Join-Path $outRoot 'checks-collect-only.json')))
}

if ($Scope -eq 'supplement')
{
    # ① 文件头补后缀 + ② 补充还原 + 本轮改造（后台线程 / 改名 / 还原率）的专用用例。
    # 每轮都是小夹具、几秒钟；每步都带硬超时，卡住就记 124 而不是把整个 scope 拖着。
    #   A 保留（不开补充还原）            -> S-1 后缀、S-3 对照组
    #   B 保留 + 补充还原                 -> S-2 真名、S-4 负例/对账、S-5 名字表、N-1 文案
    #   D 默认 + 补充还原                 -> S-2b「挖到的名字算已还原」、R-1/R-2 还原率、T-1 界面响应
    #   C 默认 + 补充还原 + 盐挪走        -> S-6 盐不可用降级
    #   K 默认 + 补充还原 + 带密钥盐      -> S-6b 盐存在但不可用
    #   X 保留 + 补充还原 + 挖掘中关窗    -> T-2 关窗能退出、无残留
    # C/K/X 会改盐或关窗口，放最后跑（各自都会自行还原现场）。
    $supplementStepTimeout = 150
    [void](Invoke-Script -Label '生成补充还原夹具' -Script 'make-workspace.ps1' -ScriptArguments @('-Supplement', '-NoiseCandidates', "$NoiseCandidates") -TimeoutSec 900)
    [void](Invoke-Script -Label 'S 用例A：保留（不开补充还原）' -Script 'run-step3-restore.ps1' -TimeoutSec $supplementStepTimeout `
            -ScriptArguments @('-KeepUnrestored', '-SaltMode', 'ensure', '-TimeoutSec', '90', '-OutJson', (Join-Path $outRoot 'step3-keep.json')))
    [void](Invoke-Script -Label 'S 用例B：保留 + 补充还原' -Script 'run-step3-restore.ps1' -TimeoutSec $supplementStepTimeout `
            -ScriptArguments @('-KeepUnrestored', '-SupplementRestore', '-SaltMode', 'ensure', '-TimeoutSec', '90', '-OutJson', (Join-Path $outRoot 'step3-keep-supplement.json')))
    [void](Invoke-Script -Label 'S 用例D：默认 + 补充还原（还原率 / 界面响应）' -Script 'run-step3-restore.ps1' -TimeoutSec $supplementStepTimeout `
            -ScriptArguments @('-SupplementRestore', '-SaltMode', 'ensure', '-TimeoutSec', '90', '-OutJson', (Join-Path $outRoot 'step3-supplement.json')))
    [void](Invoke-Script -Label 'S 用例C：盐挪走 + 补充还原' -Script 'run-step3-restore.ps1' -TimeoutSec $supplementStepTimeout `
            -ScriptArguments @('-SupplementRestore', '-SaltMode', 'remove', '-TimeoutSec', '90', '-OutJson', (Join-Path $outRoot 'step3-supplement-nosalt.json')))
    [void](Invoke-Script -Label 'S 用例K：带密钥盐 + 补充还原' -Script 'run-step3-restore.ps1' -TimeoutSec $supplementStepTimeout `
            -ScriptArguments @('-SupplementRestore', '-SaltMode', 'keyed', '-TimeoutSec', '90', '-OutJson', (Join-Path $outRoot 'step3-supplement-keysalt.json')))
    [void](Invoke-Script -Label 'S 用例X：挖掘进行中关窗' -Script 'run-step3-restore.ps1' -TimeoutSec $supplementStepTimeout `
            -ScriptArguments @('-KeepUnrestored', '-SupplementRestore', '-SaltMode', 'ensure', '-CloseDuringMining', '-CloseAfterMs', '20000', '-CloseGraceSec', '25', '-TimeoutSec', '90', '-OutJson', (Join-Path $outRoot 'step3-close-during-mining.json')))
    [void](Invoke-Script -Label '断言 S-1..S-6b + R/N/T' -Script 'assert-supplement.ps1' -TimeoutSec 300 `
            -ScriptArguments @(
                '-ManifestPath', (Join-Path $runRoot 'manifest.json'),
                '-KeepRunJson', (Join-Path $outRoot 'step3-keep.json'),
                '-MineRunJson', (Join-Path $outRoot 'step3-keep-supplement.json'),
                '-DefaultMineRunJson', (Join-Path $outRoot 'step3-supplement.json'),
                '-SaltlessRunJson', (Join-Path $outRoot 'step3-supplement-nosalt.json'),
                '-KeyedSaltRunJson', (Join-Path $outRoot 'step3-supplement-keysalt.json'),
                '-CloseRunJson', (Join-Path $outRoot 'step3-close-during-mining.json'),
                '-ChecksJson', (Join-Path $outRoot 'checks-supplement.json')))
    [void](Invoke-Script -Label '脚手架自检：S/R/N/T 断言的鉴别力' -Script 'self-test-supplement.ps1' `
            -ScriptArguments @('-ChecksJson', (Join-Path $outRoot 'checks-selftest-supplement.json')))
}

if ($Scope -in @('extension', 'all'))
{
    # 扩展集一期（task-3 脚手架）：夹具 → 生成/干跑/安装 → 模式1 三步 → 传统模式对照 →
    # E-2 未归一化夹具 → E-4 坏档案逐个驱动 → 断言 E-1..E-10 → 断言自检。
    # 每一步都带硬超时：卡住记 124，不把整个 scope 拖成几十分钟。
    # `-Scope all` 也跑这一组（lead 要求总计里含新增 E 组）：它与 FakeWorkspace 那几组
    # 用不同的夹具目录，互不重建。
    $extOld = Join-Path $outRoot 'extension-old.json'
    [void](Invoke-Script -Label '生成扩展集夹具（旧作 + 新解包目录 + 期望清单）' -Script 'make-extension-fixture.ps1' -TimeoutSec 900)
    [void](Invoke-Script -Label 'E 用例：生成当前游戏扩展集 + 干跑 + 安装到库' -Script 'run-extension-ui.ps1' `
            -ScriptArguments @('-Step', 'meta,draft,dryrun,install', '-Game', 'old', '-OutJson', $extOld) -TimeoutSec 900)
    [void](Invoke-Script -Label 'E-7 用例：再生成一次（幂等）' -Script 'run-extension-ui.ps1' `
            -ScriptArguments @('-Step', 'draft', '-Game', 'old', '-OutJson', (Join-Path $outRoot 'extension-old-repeat.json')) -TimeoutSec 900)
    [void](Invoke-Script -Label 'E-7 用例：再安装一次（upsert）' -Script 'run-extension-ui.ps1' `
            -ScriptArguments @('-Step', 'install', '-Game', 'old', '-OutJson', (Join-Path $outRoot 'extension-old-install2.json')) -TimeoutSec 900)
    # 模式1 跑在**专用的免跑游戏工作区**（Mode1Game：没有盐、没有动态采集成果）——
    # 这是 E-1「免跑游戏」的现场条件；传统模式仍用旧作工作区（它本来就有动态采集成果）。
    [void](Invoke-Script -Label 'E-1/E-3/E-10 用例：模式1 三步（步骤2 用预置目录代替）' -Script 'run-extension-ui.ps1' `
            -ScriptArguments @('-Step', 'mode1', '-Game', 'mode1', '-OutJson', (Join-Path $outRoot 'extension-mode1.json')) -TimeoutSec 1800)
    [void](Invoke-Script -Label 'E-1/E-10 用例：传统动态模式（对照组）' -Script 'run-extension-ui.ps1' `
            -ScriptArguments @('-Step', 'classic', '-Game', 'old', '-OutJson', (Join-Path $outRoot 'extension-classic.json')) -TimeoutSec 1800)
    [void](Invoke-Script -Label 'E-2 用例：采集日志故意没归一化' -Script 'run-extension-ui.ps1' `
            -ScriptArguments @('-Step', 'meta,draft', '-Game', 'raw', '-OutJson', (Join-Path $outRoot 'extension-raw.json')) -TimeoutSec 900)
    [void](Invoke-Script -Label 'E-4 用例：另一个盐的档案（生成草案）' -Script 'run-extension-ui.ps1' `
            -ScriptArguments @('-Step', 'meta,draft', '-Game', 'other', '-OutJson', (Join-Path $outRoot 'extension-other.json')) -TimeoutSec 900)
    [void](Invoke-Script -Label 'E-4 用例：坏档案逐个驱动（CRC/截断/版本/缺 IDENTITY/盐不匹配/空名字）' -Script 'run-extension-bad-archives.ps1' `
            -ScriptArguments @('-InstallRunJson', $extOld) -TimeoutSec 1800)
    $badCsv = ''
    $badSummaryPath = Join-Path $outRoot 'extension-bad-archives-summary.json'
    $badSummary = Read-JsonFile -Path $badSummaryPath
    if ($badSummary) { $badCsv = [string](Get-ObjectValue -Object $badSummary -Name 'badArchiveRunJsonCsv' -Default '') }
    # E-11/E-12（页签像素幽灵）：先抓图（黑盒，起一次真实界面走四路状态 + 扩展集页签那一路），
    # 再让断言读快照。抓图阶段带硬超时：PrintWindow 会无限期阻塞，驱动自己也有 2.5s 超时 + BitBlt 兜底。
    # **顺序**：扩展集页签/在线更新那组用例要先把库复位成"只有旧作那一份"，所以它必须在
    # E-7 的安装类用例之后跑；而抓图放在它之后（两种状态都不影响"画面与到达路径无关"这条判据）。
    [void](Invoke-Script -Label 'E-12..E-19 用例：扩展集页签 + 在线更新（本地夹具，含坏输入与超时）' `
            -Script 'run-extension-online.ps1' -TimeoutSec 2400)
    [void](Invoke-Script -Label 'E-11/E-12 用例：页签像素抓图（制作页签三路 + 扩展集页签矩阵 + 连拍 + 跨进程）' `
            -Script 'run-tab-pixels.ps1' -TimeoutSec 900)
    $onlineSummaryPath = Join-Path $outRoot 'extension-online-summary.json'
    [void](Invoke-Script -Label '断言 E-1..E-19' -Script 'assert-extension.ps1' -TimeoutSec 600 `
            -ScriptArguments @('-BadArchiveRunJsonCsv', $badCsv, '-BadArchiveSummaryJson', $badSummaryPath,
                '-TabPixelsJson', (Join-Path $outRoot 'tab-pixels.json'),
                '-TabPixelsDiffDir', (Join-Path $outRoot 'tab-pixels'),
                '-OnlineSummaryJson', $onlineSummaryPath,
                '-ChecksJson', (Join-Path $outRoot 'checks-extension.json')))
    [void](Invoke-Script -Label '脚手架自检：E 断言的鉴别力' -Script 'self-test-extension.ps1' -TimeoutSec 600 `
            -ScriptArguments @('-ChecksJson', (Join-Path $outRoot 'checks-selftest-extension.json')))
}

if ($Scope -eq 'pressure')
{
    # 压力用例：Packages × FilesPerPackage 个哈希名文件（默认 12 × 5000 = 6 万，贴近用户真实规模）。
    # 只跑默认语义（勾选保留会把输出量再翻一倍，压不出新东西）。
    [void](Invoke-Script -Label ("生成压力工作区（{0} × {1}）" -f $PressurePackages, $PressureFilesPerPackage) -Script 'make-workspace.ps1' `
            -ScriptArguments @('-Packages', "$PressurePackages", '-FilesPerPackage', "$PressureFilesPerPackage"))
    [void](Invoke-Script -Label '压力用例：步骤3 默认语义' -Script 'run-step3-restore.ps1' `
            -ScriptArguments @('-TimeoutSec', '3600', '-OutJson', (Join-Path $outRoot 'step3-pressure.json')))
    [void](Invoke-Script -Label '压力断言：A + P + D' -Script 'assert-restore.ps1' `
            -ScriptArguments @('-Mode', 'default', '-RunJson', (Join-Path $outRoot 'step3-pressure.json'), '-ChecksJson', (Join-Path $outRoot 'checks-pressure.json')))
}

if ($Scope -eq 'all')
{
    # 最后再自检一遍断言脚本本身：确认"该炸的会炸"（防止断言退化成永远通过）。
    # 只在 all 里跑：自检要求工作区是那份 19 文件的小用例，压力工作区不适用。
    [void](Invoke-Script -Label '脚手架自检：断言脚本的鉴别力' -Script 'self-test-assertions.ps1' -ScriptArguments @('-ChecksJson', (Join-Path $outRoot 'checks-selftest.json')))
}

# ---------- 汇总 ----------

Write-Host ''
Write-Host '=================================================================='
Write-Host ' 汇总'
Write-Host '=================================================================='

$checkFiles = @(
    (Join-Path $outRoot 'checks-default.json'),
    (Join-Path $outRoot 'checks-keep.json'),
    (Join-Path $outRoot 'checks-collect-only.json'),
    (Join-Path $outRoot 'checks-selftest.json'),
    (Join-Path $outRoot 'checks-supplement.json'),
    (Join-Path $outRoot 'checks-selftest-supplement.json'),
    (Join-Path $outRoot 'checks-pressure.json'),
    (Join-Path $outRoot 'checks-extension.json'),
    (Join-Path $outRoot 'checks-selftest-extension.json')
)

# 汇总 + 假绿护栏都在 lib\HarnessSummary.ps1 里（抽出来的唯一目的是能被独立自检：
# self-test-runall-guard.ps1 会造"全是残留 / 项数不够 / 有失败"三种局面，要求它该炸就炸）。
$summary = Invoke-HarnessCheckSummary -CheckFiles $checkFiles -RunStarted $runStarted -Scope $Scope -MinExecutedChecks $MinExecutedChecks
exit $summary.ExitCode