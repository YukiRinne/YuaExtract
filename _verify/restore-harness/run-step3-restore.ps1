# 用例运行器（断言A/B/D 的原料）：用产品自己的界面跑一次「步骤3 资源名还原」。
#
#   pwsh -File run-step3-restore.ps1                        # 默认语义：只输出已还原的文件
#   pwsh -File run-step3-restore.ps1 -KeepUnrestored        # 勾上「保留未还原文件名的文件」
#
# 产出：Run\out\step3-<default|keep>.json
#   里面有：源目录快照（前后各一份，供断言D）、输出目录清单（含 sha256）、
#           界面模式（新还原窗口 / 旧的窗口内联还原）、还原窗口标题时间线、
#           ListView 行、控件清单、失败原因。
#
# 为什么走界面而不是自己实现一份还原逻辑：
#   要验证的是"用户点下去会发生什么"，包括窗口有没有弹、默认勾选状态、
#   输出目录是不是约定的那个。绕开界面测核心函数的话，这些全测不到。
#
# 观测手段见 lib\HarnessNative.ps1（跨进程读标题/控件文字/ListView）。

[CmdletBinding()]
param(
    [switch]$KeepUnrestored,
    # 本轮新增：勾上「推理补充还原」（从游戏资源里挖候选名 → 算哈希 → 命中才写映射表）
    [switch]$SupplementRestore,
    # T-2：等挖掘开始（最多 -CloseAfterMs）后关窗，量进程退出耗时
    [switch]$CloseDuringMining,
    [int]$CloseAfterMs = 20000,
    [int]$CloseGraceSec = 25,
    [string]$AppExe = '',
    [string]$WorkspaceRoot = '',
    [int]$TimeoutSec = 600,
    [int]$StartupTimeoutSec = 40,
    [int]$SettleMs = 4000,
    [string[]]$KeepCheckboxTexts = @('保留未还原文件名的文件', '保留未还原文件名', '保留未还原'),
    # 文案命中用关键词列表：这轮「补充还原」→「推理补充还原」，
    # 第一项是两版文案的公共片段（最稳），前后两个构建都能识别。
    [string[]]$SupplementCheckboxTexts = @(
        '（从游戏资源中挖掘候选名',
        '推理补充还原',
        '补充还原（从游戏资源中挖掘候选名'),
    # 盐文件（Log\Universal.log）的处置：
    #   asis 不动 / ensure 按正常形态补齐 / remove 全部挪走（S-6 盐不可用）
    #   keyed 写成"带密钥哈希"形态（S-6b 盐存在但不可用）
    [ValidateSet('asis', 'ensure', 'remove', 'keyed')][string]$SaltMode = 'asis',
    [string[]]$StartButtonTexts = @('开始', '开始还原', '开始执行'),
    [string]$OutJson = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessNative.ps1')

$runRoot = Join-Path $root 'Run'
$outDir = Join-Path $runRoot 'out'
$manifestPath = Join-Path $runRoot 'manifest.json'
$mode = if ($SupplementRestore -and $KeepUnrestored) { 'keep-supplement' }
elseif ($SupplementRestore) { 'supplement' }
elseif ($KeepUnrestored) { 'keep' }
else { 'default' }
if (-not $OutJson) { $OutJson = Join-Path $outDir ("step3-{0}.json" -f $mode) }

foreach ($directory in @($runRoot, $outDir))
{
    if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
}
# 上一次运行的结论不能留在原地冒充这一次的结果。
Remove-Item -LiteralPath $OutJson -Force -ErrorAction SilentlyContinue

$events = New-Object System.Collections.ArrayList
function Add-Event([string]$Text)
{
    if ($events.Count -lt 2000) { [void]$events.Add(("[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $Text)) }
}

$script:appProcess = $null
function Stop-AppProcess
{
    if ($script:appProcess)
    {
        try
        {
            foreach ($window in [Harness.Win]::TopLevel([uint32]$script:appProcess.Id)) { [void][Harness.Win]::PostClose($window.Hwnd) }
            Start-Sleep -Milliseconds 500
            [void](Stop-HarnessProcess -Process $script:appProcess -GraceMs 2500)
        }
        catch { }
        $script:appProcess = $null
    }
}

$record = [ordered]@{
    case              = "step3-$mode"
    mode              = $mode
    keepUnrestored    = [bool]$KeepUnrestored
    supplementRestore = [bool]$SupplementRestore
    startedUtc        = (Get-Date).ToUniversalTime().ToString('o')
    appExe            = $AppExe
    workspace         = $WorkspaceRoot
    failure           = $null
    uiMode            = 'none'
    completed         = $false
    completionReason  = ''
    outputDir         = ''
    outputCandidates  = @()
    sourceSnapshotBefore = $null
    sourceSnapshotAfter  = $null
    output            = @()
    outputTsv         = ''
    reportPath        = ''
    reportText        = ''
    toolLogFiles      = @()
    uiLogLines        = 0
    uiLogChars        = 0
    pollCount         = 0
    pollHungCount     = 0
    probeLatencyMaxMs = 0
    probeLatencyAvgMs = 0
    restoreElapsedMs  = 0
    listSampleCount   = 0
    sourceDigestBefore = $null
    sourceDigestAfter  = $null
    restoreWindow     = $null
    controls          = @()
    listRows          = @()
    logText           = ''
    titleTimeline     = @()
    modalDialogs      = @()
    # 本轮新增：两个复选框的实际状态 + 盐文件的处置（S-1..S-6 要用它判"到底勾上没有"）
    checkboxStates    = @{}
    saltMode          = 'asis'
    saltFiles         = @()
    saltRemoved       = @()
    # 名字表：每条用例前清场、跑完留一份快照（后续用例会覆盖产品写的那份）
    minedStateReset   = @()
    minedNamesSnapshot = ''
    # T-1：界面往返延迟探测样本（"相对毫秒|往返毫秒"，-1 = 超时）
    probeSamples      = @()
    miningStarted     = $false
    miningFinished    = $false
    miningFirstSeenMs = -1
    probeAfterMiningCount = 0
    probeAfterMiningMaxMs = 0
    probeAfterMiningHung = 0
    uiLogFull         = ''
    # T-2：挖掘进行中关窗
    closeDuringMining = $null
    events            = $events
}
function Save-And-Exit
{
    param([string]$Reason, [int]$Code = 2)

    Stop-AppProcess

    # S-6 用的：挪走的盐不管成功失败都要放回去，否则后面的用例会莫名其妙没有盐。
    foreach ($entry in @($record.saltRemoved))
    {
        if ($entry -and (Test-Path -LiteralPath $entry.hidden))
        {
            Move-Item -LiteralPath $entry.hidden -Destination $entry.original -Force
        }
    }

    # 名字表现场还原：先删掉本轮产生的，再把挪开的旧文件放回去（夹具保持原样）
    foreach ($entry in @($record.minedStateReset))
    {
        if (-not $entry) { continue }
        if (Test-Path -LiteralPath $entry.original) { Remove-Item -LiteralPath $entry.original -Force -ErrorAction SilentlyContinue }
        if (Test-Path -LiteralPath $entry.hidden) { Move-Item -LiteralPath $entry.hidden -Destination $entry.original -Force }
    }

    if ($Reason) { $record.failure = $Reason }
    $record.finishedUtc = (Get-Date).ToUniversalTime().ToString('o')
    Write-JsonFile -Path $OutJson -Object $record
    if ($Reason) { Write-Host ("[step3-$mode] 失败：{0}" -f $Reason) }
    Write-Host ("[step3-$mode] 结果写入 {0}" -f $OutJson)
    exit $Code
}

# ---------- 清单 / 目标程序 ----------

$manifest = Read-JsonFile -Path $manifestPath
if (-not $manifest)
{
    Save-And-Exit '缺少 Run\manifest.json；请先运行 make-workspace.ps1 生成合成工作区'
}
if (-not $WorkspaceRoot) { $WorkspaceRoot = $manifest.gameDir }
$sourceDir = $manifest.sourceDir
$outputPath = $manifest.defaultOutputDir
$record.workspace = $WorkspaceRoot
$record.outputDir = $outputPath

if (-not $AppExe)
{
    # 默认跑的是 Release\ 的**快照**（Run\App\），不是 Release\ 本身：
    # 免得验证跑着跑着把队友的编译链接卡住，也免得中途换版本。
    try
    {
        $snapshot = Sync-ReleaseSnapshot -RepoRoot ([System.IO.Path]::GetFullPath((Join-Path $root '..\..'))) -RunRoot $runRoot
    }
    catch
    {
        Save-And-Exit $_.Exception.Message
    }
    $AppExe = $snapshot.Exe
    Add-Event ("被测程序快照：{0}（{1}，sha256 {2}）" -f $snapshot.Exe, $snapshot.ExeMachine, $snapshot.ExeSha256.Substring(0, 12))
    Add-Event ("快照里的 CxdecStringDumper.dll：{0} sha256 {1}" -f $snapshot.DllMachine, $(if ($snapshot.DllSha256) { $snapshot.DllSha256.Substring(0, 12) } else { '<缺>' }))

    # ---------- 盐文件（Log\Universal.log）的处置 ----------
    #
    # 夹具在**游戏目录**下写了两份盐（Log\Universal.log 与 Universal.log）；
    # 工具目录（Run\App\Log）那一份必须在这里补 —— 上面的快照同步会把 Run\App 整个换掉，
    # 生成器写在里面的盐会被冲掉（这个坑踩过一次）。
    $saltPaths = New-Object System.Collections.ArrayList
    $supplementMeta = if ($manifest.PSObject.Properties['supplement']) { $manifest.supplement } else { $null }
    if ($supplementMeta)
    {
        foreach ($path in @($supplementMeta.saltFiles)) { [void]$saltPaths.Add([string]$path) }
        $toolSalt = Join-Path (Split-Path -Parent $AppExe) 'Log\Universal.log'
        [void]$saltPaths.Add($toolSalt)
    }

    $record.saltMode = $SaltMode
    $record.saltFiles = @()
    # 必须用 ArrayList：$record.saltRemoved 在 schema 里是 @()（定长数组），
    # 往定长数组上 .Add() 会抛 "Collection was of a fixed size."（用例C 就是这么挂的）
    $record.saltRemoved = New-Object System.Collections.ArrayList

    # 名字表清场：产品会把挖到的名字持久化（<workspace>\StringHashDumper_Output\MinedNames.lst
    # 与工具目录），下一条用例若不清掉，就会把**上一条**的结果当成自己挖到的
    # —— S-6b「盐带密钥却仍然命中」的假阳性就是这么来的。跑完在 Save-And-Exit 里放回去。
    $record.minedStateReset = New-Object System.Collections.ArrayList
    $minedCandidates = New-Object System.Collections.ArrayList
    if ($manifest.PSObject.Properties['hashDir'] -and $manifest.hashDir) { [void]$minedCandidates.Add((Join-Path ([string]$manifest.hashDir) 'MinedNames.lst')) }
    [void]$minedCandidates.Add((Join-Path (Split-Path -Parent $AppExe) 'MinedNames.lst'))
    $script:minedListCandidates = @($minedCandidates)
    foreach ($candidate in $minedCandidates)
    {
        if (Test-Path -LiteralPath $candidate)
        {
            $hidden = $candidate + '.harness-prev'
            Remove-Item -LiteralPath $hidden -Force -ErrorAction SilentlyContinue
            Move-Item -LiteralPath $candidate -Destination $hidden -Force
            [void]$record.minedStateReset.Add([ordered]@{ original = $candidate; hidden = $hidden })
        }
    }
    Add-Event ("名字表清场：{0} 份旧 MinedNames.lst 已挪开（保证每条用例都从零开始挖）" -f $record.minedStateReset.Count)

    if ($SaltMode -eq 'ensure' -and $supplementMeta)
    {
        # 正常形态：Hash Seed 有值，两个 HasherSalt **留空**（用户真实游戏的样子）。
        # 踩过的坑：把 HasherSalt 也写成 seed，产品会读成"带密钥哈希"从而拒绝挖掘 ——
        # 那是产品做对了，是夹具当初写错了。
        $seedValue = [string]$supplementMeta.seed
        $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
        $saltText = (@(
                ('{0} | I | T1 | Hash Seed:{1}' -f $stamp, $seedValue)
                ('{0} | I | T1 | FileNameHasherSalt:' -f $stamp)
                ('{0} | I | T1 | PathNameHasherSalt:' -f $stamp)
            ) -join "`r`n") + "`r`n"
        foreach ($path in $saltPaths)
        {
            if (-not (Test-Path -LiteralPath (Split-Path -Parent $path))) { New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null }
            Write-Utf8File -Path $path -Text $saltText -NoNewline
        }
        Add-Event ("已保证盐文件存在（seed={0}，HasherSalt 留空）：{1}" -f $seedValue, ($saltPaths -join ' | '))
    }
    elseif ($SaltMode -eq 'keyed' -and $supplementMeta)
    {
        # S-6b：盐存在但"带密钥哈希" —— 产品应当明确拒绝挖掘并说明原因，而不是硬算或崩掉
        $keyedTemplate = [string](Get-ObjectValue -Object $supplementMeta -Name 'keyedSaltTemplate' -Default '')
        if (-not $keyedTemplate -or -not (Test-Path -LiteralPath $keyedTemplate))
        {
            Save-And-Exit '夹具缺少带密钥盐的模板（Run\supplement-salt-keyed.txt）；请先跑 make-workspace.ps1 -Supplement'
        }
        $saltText = [System.IO.File]::ReadAllText($keyedTemplate, [System.Text.Encoding]::UTF8)
        foreach ($path in $saltPaths)
        {
            if (-not (Test-Path -LiteralPath (Split-Path -Parent $path))) { New-Item -ItemType Directory -Path (Split-Path -Parent $path) -Force | Out-Null }
            Write-Utf8File -Path $path -Text $saltText -NoNewline
        }
        Add-Event ("已写入带密钥形态的盐（FileNameHasherSalt 非空）：{0}" -f ($saltPaths -join ' | '))
    }
    elseif ($SaltMode -eq 'remove')
    {
        # S-6：把盐全部挪走，模拟"游戏没留 Universal.log / 里面没有 Hash Seed"
        foreach ($path in $saltPaths)
        {
            if (Test-Path -LiteralPath $path)
            {
                $hidden = $path + '.harness-hidden'
                Move-Item -LiteralPath $path -Destination $hidden -Force
                [void]$record.saltRemoved.Add([ordered]@{ original = $path; hidden = $hidden })
            }
        }
        Add-Event ("已把 {0} 份盐挪走（模拟盐不可用）" -f $record.saltRemoved.Count)
    }

    foreach ($path in $saltPaths) { $record.saltFiles += [ordered]@{ path = $path; exists = (Test-Path -LiteralPath $path) } }
    Add-Event ("盐文件现状：" + (($record.saltFiles | ForEach-Object { '{0}={1}' -f (Split-Path $_.path -Leaf), $_.exists }) -join ', '))
}
if (-not (Test-Path -LiteralPath $AppExe))
{
    Save-And-Exit ("找不到 $AppExe；请先编译解决方案（Release|x86），见 README 的构建命令")
}
$AppExe = [System.IO.Path]::GetFullPath($AppExe)
$record.appExe = $AppExe
Add-Event ("被测程序：{0}" -f $AppExe)

# ---------- 清场 ----------

if (-not (Test-Path -LiteralPath $sourceDir)) { Save-And-Exit ("合成工作区的解包目录不存在：{0}；请先运行 make-workspace.ps1" -f $sourceDir) }
[void](Remove-TreeUnder -Path $outputPath -MustBeUnder $runRoot)
Add-Event ("已清空输出目录：{0}" -f $outputPath)

# 源目录前后对比一律留"摘要"（文件数 + 排序行的 sha256），6 万文件时也只要几百毫秒；
# 明细只在文件数不大的时候另存一份，方便小用例直接看差异。
$digestBefore = Get-DirectoryDigest -Path $sourceDir -WithHash
$record.sourceDigestBefore = $digestBefore
$record.sourceSnapshotBefore = if ($digestBefore.Count -le 3000) { Get-DirectorySnapshot -Path $sourceDir -WithHash } else { $null }
Add-Event ("源目录摘要（前）：{0} 个文件，digest {1}" -f $digestBefore.Count, $digestBefore.Digest.Substring(0, 12))

# ---------- 启动被测程序 ----------
#
# 目标游戏 exe 走**命令行参数**传进去：Loader 的 wWinMain 会校验"是个真实存在的 .exe"
# 之后直接 ApplyTarget（设编辑框、算工作区路径、深度探测目录）。
# 比跨进程往编辑框里塞文字可靠得多，也更接近用户"把 exe 拖到程序图标上"的真实路径。

$gameExe = $manifest.gameExe
try
{
    $process = Start-HarnessProcess -FilePath $AppExe -WorkingDirectory (Split-Path -Parent $AppExe) -Arguments @($gameExe)
}
catch
{
    Save-And-Exit ("启动被测程序失败：{0}（正在编译、或文件被占用时会出现这种错；等编译完再跑）" -f $_.Exception.Message)
}
$script:appProcess = $process
$appPid = [uint32]$process.Id
Add-Event ("已启动 pid={0}，命令行目标：{1}" -f $appPid, $gameExe)

$mainWindow = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds($StartupTimeoutSec)
while ((Get-Date) -lt $deadline -and $mainWindow -eq [IntPtr]::Zero)
{
    if ($process.HasExited) { Save-And-Exit ("被测程序启动后立刻退出，退出码 {0}" -f $process.ExitCode) }
    foreach ($window in [Harness.Win]::TopLevel($appPid))
    {
        if ($window.Class -eq 'YuaExtractMainWindow') { $mainWindow = [IntPtr]$window.Hwnd; break }
    }
    if ($mainWindow -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 250 }
}
if ($mainWindow -eq [IntPtr]::Zero)
{
    $seen = ([Harness.Win]::TopLevel($appPid) | ForEach-Object { '{0}({1})' -f $_.Class, $_.Text }) -join '; '
    Save-And-Exit ("等不到主窗口（类名 YuaExtractMainWindow）。当前进程窗口：{0}" -f $seen)
}
Start-Sleep -Milliseconds 800   # 让 ApplyTarget 里的深度探测跑完
$mainTitle = [Harness.Win]::CaptionOf($mainWindow)
Add-Event ("主窗口就绪：{0}" -f $mainTitle)

function Get-TopLevels
{
    return @([Harness.Win]::TopLevel($appPid) | Where-Object {
        $_.Class -notin @('IME', 'MSCTFIME UI', 'Default IME', 'tooltips_class32', 'SysShadow')
    })
}

# 未预期的模态框（消息框）：读它的文字，点掉它，并记下来。
# 这些框往往是"实现拒绝执行"的直接原因，比超时更有诊断价值。
function Resolve-ModalDialogs([switch]$PreferYes)
{
    $found = @()
    foreach ($window in Get-TopLevels)
    {
        if ($window.Hwnd -eq $mainWindow.ToInt64()) { continue }
        if ($window.Class -ne '#32770') { continue }

        $texts = @()
        $buttons = @()
        foreach ($child in [Harness.Win]::Children($window.Hwnd))
        {
            if ($child.Class -eq 'Static' -and $child.Text) { $texts += $child.Text }
            if ($child.Class -eq 'Button' -and $child.Text) { $buttons += $child }
        }
        $entry = [pscustomobject]@{
            Title   = $window.Text
            Text    = ($texts -join ' | ')
            Buttons = (@($buttons | ForEach-Object { $_.Text }) -join ',')
        }
        $found += $entry
        Add-Event ("出现模态框：title='{0}' text='{1}' buttons='{2}'" -f $entry.Title, $entry.Text, $entry.Buttons)

        # 按钮文案要**归一化**再比：产品的对话框写的是「是(&Y)」「否(&N)」，
        # 直接拿 '是' 去比永远匹配不上 —— 于是框识别到了却一次都没点，
        # 看起来就是"关不掉"（T-2 曾这么假失败：25 秒识别到 224 次、实际点击 0 次）。
        $normalizeLabel = {
            param($text)
            return (($text -replace '&', '') -replace '\s*\([^)]*\)\s*', '').Trim()
        }

        # 是/否型的框 = 需要"用户决策"。
        #   默认（PreferYes 关）：点"否"/"取消" —— 保守，绝不自作主张替用户确认破坏性操作。
        #   T-2 关窗流程（PreferYes 开）：点"是" —— 挖掘进行中关窗会弹「中止还原」，
        #   只有点"是"窗口才会真的关掉；点"否"看起来就像卡死（这个假失败踩过）。
        $wanted = if ($PreferYes) { @('是', 'Yes', '确定', 'OK', '否', 'No', '取消', 'Cancel') }
        else { @('确定', 'OK', '关闭', '否', 'No', '取消', 'Cancel') }

        $choice = $null
        foreach ($label in $wanted)
        {
            $choice = $buttons | Where-Object { (& $normalizeLabel $_.Text) -eq $label } | Select-Object -First 1
            if ($choice) { break }
        }

        if ($choice)
        {
            [void][Harness.Win]::PostClick($choice.Hwnd)
            Add-Event ("已点模态框按钮「{0}」（归一化「{1}」，PreferYes={2}）" -f $choice.Text, (& $normalizeLabel $choice.Text), [bool]$PreferYes)
        }
        else
        {
            # 兜底：标准 MessageBox 的按钮 id 固定是 IDYES=6 / IDNO=7。
            # 文案匹配不上时直接给对话框发命令，不再依赖按钮文字。
            $commandId = if ($PreferYes) { 6 } else { 7 }
            [void][Harness.Win]::PostCommand($window.Hwnd, $commandId, 0)
            Add-Event ("按钮文案都没匹配上（实际：{0}）；改用 PostCommand id={1}" -f $entry.Buttons, $commandId)
        }
    }
    return $found
}

# ---------- 设定目标（游戏 exe）----------

$exeEditId = 4021   # IDC_YUA_EXE_EDIT
$step3Id = 4052     # IDC_YUA_STEP3
$editHandle = [Harness.Win]::GetDlgItem($mainWindow, $exeEditId)
if ($editHandle -eq [IntPtr]::Zero) { Save-And-Exit '主窗口里找不到「游戏的exe程序」编辑框（IDC_YUA_EXE_EDIT=4021），界面结构可能已变' }

$exeBaseName = Split-Path -Leaf $gameExe
$targetApplied = $mainTitle -like ('*' + $exeBaseName + '*')

if (-not $targetApplied)
{
    # 命令行那条路没生效（或界面改了）时的兜底：把路径写进编辑框，再伪造 EN_KILLFOCUS
    # 让界面重建工作区路径并刷新目录探测。
    Add-Event '主窗口标题里没看到目标名，改用"写编辑框 + EN_KILLFOCUS"兜底'
    [void][Harness.Win]::LocalSetText($editHandle.ToInt64(), $gameExe)
    [void][Harness.Win]::PostCommand($mainWindow.ToInt64(), $exeEditId, 0x0200)
    Start-Sleep -Milliseconds 1500
    $mainTitle = [Harness.Win]::CaptionOf($mainWindow)
    $targetApplied = $mainTitle -like ('*' + $exeBaseName + '*')
}

$echo = [Harness.Win]::SmartText($editHandle.ToInt64(), $appPid, 2000)
Add-Event ("编辑框回读：'{0}'；主窗口标题：'{1}'" -f $echo, $mainTitle)
if (-not $targetApplied) { Save-And-Exit ("目标游戏没设上：期望编辑框/标题出现 '{0}'，实际编辑框='{1}' 标题='{2}'" -f $exeBaseName, $echo, $mainTitle) }
if ($echo -ne $gameExe) { Add-Event ("注意：编辑框文字与期望不完全一致（'{0}' vs '{1}'），但标题已确认目标生效" -f $echo, $gameExe) }

$dialogs = Resolve-ModalDialogs
foreach ($dialog in $dialogs) { $record.modalDialogs += $dialog }

# ---------- 点「步骤3」 ----------

[void][Harness.Win]::PostCommand($mainWindow.ToInt64(), $step3Id, 0)
Add-Event '已点「步骤3：用动态Hash还原资源名」'

$restoreWindow = $null
$legacyInline = $false
$step3EnabledBefore = [Harness.Win]::IsWindowEnabled([Harness.Win]::GetDlgItem($mainWindow, $step3Id))
$deadline = (Get-Date).AddSeconds(30)
while ((Get-Date) -lt $deadline)
{
    $dialogs = Resolve-ModalDialogs
    foreach ($dialog in $dialogs) { $record.modalDialogs += $dialog; $record.failure = ("界面弹出了对话框：{0} / {1}" -f $dialog.Title, $dialog.Text) }

    foreach ($window in Get-TopLevels)
    {
        if ($window.Hwnd -eq $mainWindow.ToInt64()) { continue }
        $restoreWindow = $window
        break
    }
    if ($restoreWindow) { break }

    # 旧实现：还原在主窗口里内联跑，跑的时候主流程按钮会被禁用。
    $step3Enabled = [Harness.Win]::IsWindowEnabled([Harness.Win]::GetDlgItem($mainWindow, $step3Id))
    if ($step3EnabledBefore -and -not $step3Enabled) { $legacyInline = $true; break }

    Start-Sleep -Milliseconds 250
}

if ($restoreWindow)
{
    $record.uiMode = 'restore-window'
    $record.restoreWindow = [ordered]@{ HwndHex = $restoreWindow.HwndHex; Class = $restoreWindow.Class; Title = $restoreWindow.Text }
    Add-Event ("还原窗口出现：class='{0}' title='{1}'" -f $restoreWindow.Class, $restoreWindow.Text)
}
elseif ($legacyInline)
{
    $record.uiMode = 'legacy-inline'
    Add-Event '没等到新还原窗口；主流程按钮被禁用 → 判定为「旧实现：主窗口内联还原」'
}
else
{
    $windowList = (Get-TopLevels | ForEach-Object { '{0}({1})' -f $_.Class, $_.Text }) -join '; '
    $reason = "点了步骤3 之后 30 秒内既没有出现新的还原窗口，也没有进入内联还原状态。当前进程窗口：{0}" -f $windowList
    if ($record.modalDialogs.Count -gt 0) { $reason += ("；期间弹出的对话框：" + (($record.modalDialogs | ForEach-Object { $_.Title + '/' + $_.Text }) -join '; ')) }
    [void](Stop-HarnessProcess -Process $process -GraceMs 2000)
    Save-And-Exit $reason
}

# ---------- 勾选项 / 点开始 ----------

function Find-ChildByText
{
    param($Window, [string]$Contains, [string]$Class = '')

    foreach ($child in [Harness.Win]::Children($Window.Hwnd))
    {
        if ($Class -and $child.Class -ne $Class) { continue }
        if ($child.Text -and $child.Text.Contains($Contains)) { return $child }
    }
    return $null
}

function Get-ChildInventory($Window)
{
    $items = @()
    foreach ($child in [Harness.Win]::Children($Window.Hwnd))
    {
        $text = $child.Text
        if ($text -and $text.Length -gt 120) { $text = $text.Substring(0, 120) + '…' }
        $items += [pscustomobject]@{
            HwndHex = $child.HwndHex
            Class   = $child.Class
            Text    = $text
            Visible = $child.Visible
            Enabled = $child.Enabled
        }
    }
    return $items
}

# 还原窗口里最长的那段文字 = "详细日志"框。
# 取 20 万字符（还原率的行在末尾，截太短会把证据丢掉）。
function Get-UiLogText($Window, [uint32]$AppPid, [int]$MaxChars = 200000)
{
    $longest = ''
    foreach ($child in [Harness.Win]::Children($Window.Hwnd))
    {
        if ($child.Class -ne 'Edit') { continue }
        $text = [Harness.Win]::SmartText($child.Hwnd, $AppPid, $MaxChars)
        if ($text -and $text.Length -gt $longest.Length) { $longest = $text }
    }
    return $longest
}

$startHandle = $null
if ($record.uiMode -eq 'restore-window')
{
    Start-Sleep -Milliseconds 600
    $record.controls = @(Get-ChildInventory $restoreWindow)
    Add-Event ("还原窗口控件 {0} 个" -f $record.controls.Count)

    # 复选框统一处理：勾不上就退一步 BM_SETCHECK，仍不行就带着证据失败。
    # 这样「保留」与「推理补充还原」两个开关的行为一致，断言里也好对账。
    #
    # 文案命中用**关键词列表**：这轮「补充还原」要改名成「推理补充还原」，
    # 列表里同时放改名前后都能命中的片段（`（从游戏资源中挖掘候选名`），
    # 这样改名前后的构建都能被同一份脚手架识别。
    $checkboxRequests = @()
    if ($KeepUnrestored) { $checkboxRequests += [pscustomobject]@{ Name = '保留未还原'; Texts = $KeepCheckboxTexts; Key = 'keepUnrestored' } }
    if ($SupplementRestore) { $checkboxRequests += [pscustomobject]@{ Name = '推理补充还原'; Texts = $SupplementCheckboxTexts; Key = 'supplementRestore' } }

    foreach ($request in $checkboxRequests)
    {
        $checkbox = $null
        $matchedText = ''
        foreach ($keyword in $request.Texts)
        {
            $candidate = Find-ChildByText -Window $restoreWindow -Contains $keyword -Class 'Button'
            if ($candidate) { $checkbox = $candidate; $matchedText = $keyword; break }
        }

        if (-not $checkbox)
        {
            $buttons = @([Harness.Win]::Children($restoreWindow.Hwnd) | Where-Object { $_.Class -eq 'Button' })
            Add-Event ("没找到「{0}」复选框（试过的关键词：{1}）" -f $request.Name, ($request.Texts -join ' / '))
            Add-Event ("窗口里的 Button 实际文案：{0}" -f (($buttons | ForEach-Object { $_.Text }) -join ' | '))
            $record.checkboxStates[$request.Key] = 'missing'
            continue
        }

        $before = [Harness.Win]::CheckState($checkbox.Hwnd)
        if ($before -ne 1)
        {
            [void][Harness.Win]::PostClick($checkbox.Hwnd)
            Start-Sleep -Milliseconds 400
        }
        $after = [Harness.Win]::CheckState($checkbox.Hwnd)
        if ($after -ne 1)
        {
            # BM_CLICK 没生效（可能是自绘控件）时退一步直接置位，并留证据。
            [void][Harness.Win]::SetCheck($checkbox.Hwnd, 1)
            Start-Sleep -Milliseconds 200
            $after = [Harness.Win]::CheckState($checkbox.Hwnd)
            Add-Event '复选框用 BM_CLICK 没勾上，改用 BM_SETCHECK'
        }
        $record.checkboxStates[$request.Key] = [int]$after
        Add-Event ("「{0}」复选框：{1} -> {2}（控件 id={3}，命中关键词「{4}」，完整文案「{5}」）" -f `
                $request.Name, $before, $after, $checkbox.CtrlId, $matchedText, $checkbox.Text)
        if ($after -ne 1)
        {
            [void](Stop-HarnessProcess -Process $process -GraceMs 2000)
            Save-And-Exit ("勾不上「{0}」复选框（BM_GETCHECK 仍是 {1}）" -f $request.Name, $after)
        }
    }

    foreach ($candidate in $StartButtonTexts)
    {
        $startHandle = Find-ChildByText -Window $restoreWindow -Contains $candidate -Class 'Button'
        if ($startHandle) { break }
    }
    if (-not $startHandle)
    {
        $buttons = (([Harness.Win]::Children($restoreWindow.Hwnd) | Where-Object { $_.Class -eq 'Button' }) | ForEach-Object { $_.Text }) -join ','
        [void](Stop-HarnessProcess -Process $process -GraceMs 2000)
        Save-And-Exit ("还原窗口里找不到「开始」按钮（找到的按钮：{0}）" -f $buttons)
    }
    Add-Event ("点「{0}」开始还原" -f $startHandle.Text)
    [void][Harness.Win]::PostClick($startHandle.Hwnd)
}
else
{
    Add-Event '内联还原模式：不需要额外点击（点步骤3 即开始）'
}

# ---------- T-2：在"挖掘进行中"关窗，看进程多久退出、有没有残留 ----------
#
# 语义：等挖掘真的开始（详细日志出现挖掘字样）再关；等不到就等 -CloseAfterMs 兜底。
# 记录 detectedAtMs / closedAtMs / exitMs / exited / killed，断言 T-2 据此判定。
if ($CloseDuringMining)
{
    $closeWatch = [System.Diagnostics.Stopwatch]::StartNew()
    $miningSeen = $false
    $detectedAt = -1
    $deadline = [Math]::Max($CloseAfterMs, 0)
    while ($closeWatch.ElapsedMilliseconds -lt $deadline -and -not $process.HasExited)
    {
        if ($record.uiMode -eq 'restore-window')
        {
            $alive = @(Get-TopLevels | Where-Object { $_.HwndHex -eq $record.restoreWindow.HwndHex })
            if ($alive.Count -gt 0)
            {
                $text = Get-UiLogText -Window $alive[0] -AppPid $appPid
                foreach ($keyword in @('候选挖掘', '推理补充还原', '补充还原：开始', '挖掘候选名', '开始从游戏资源里挖掘'))
                {
                    if ($text -and $text -like ('*' + $keyword + '*')) { $miningSeen = $true; $detectedAt = [int]$closeWatch.ElapsedMilliseconds; break }
                }
            }
        }
        if ($miningSeen) { break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $miningSeen) { $detectedAt = [int]$closeWatch.ElapsedMilliseconds }

    $closeResult = [ordered]@{
        requested      = $true
        miningDetected = $miningSeen
        detectedAtMs   = $detectedAt
        closedAtMs     = 0
        windowGoneMs   = -1
        threadsBefore  = 0
        threadsAfter   = -1
        exited         = $false
        exitMs         = -1
        killed         = $false
        graceSec       = $CloseGraceSec
        confirmDialogSeen = $false
        confirmClicks  = 0
    }
    try { $process.Refresh(); $closeResult.threadsBefore = [int]$process.Threads.Count } catch { $closeResult.threadsBefore = -1 }

    if (-not $process.HasExited)
    {
        $closeResult.closedAtMs = [int]$closeWatch.ElapsedMilliseconds
        if ($record.uiMode -eq 'restore-window')
        {
            $alive = @(Get-TopLevels | Where-Object { $_.HwndHex -eq $record.restoreWindow.HwndHex })
            if ($alive.Count -gt 0) { [void][Harness.Win]::PostClose($alive[0].Hwnd) }
        }
        Add-Event ("已向还原窗口发关闭消息（挖掘已开始={0}，关闭发生在 {1}ms）" -f $closeResult.miningDetected, $closeResult.closedAtMs)
    }

    # 关键：挖掘进行中关窗，产品会弹「中止还原」确认框（MB_YESNO）。
    # 只有点「是」窗口才会真的关；默认策略是点「否」（保守），在这里必须反过来
    # —— 否则看起来像"关窗后不退出"，那是**脚手架造成的假失败**（踩过两次：
    #   第一次是默认点否；第二次是按钮文案带加速键「是(&Y)」，精确比较匹配不上）。
    #
    # 语义澄清：关掉还原窗口 = 中止本次还原并**回到主窗口**，进程本身是应当继续活着的
    # （产品行为正确）。所以这里分两段量：
    #   第一段：还原窗口多久消失 + 工作线程有没有回收（这才是"不卡、线程收干净"）
    #   第二段：再关主窗口，量进程退出（"无残留"）
    $windowDeadline = (Get-Date).AddSeconds($CloseGraceSec)
    while ((Get-Date) -lt $windowDeadline)
    {
        if ($process.HasExited) { break }

        $alive = @(Get-TopLevels)
        $restoreAlive = @($alive | Where-Object { $_.HwndHex -eq $record.restoreWindow.HwndHex })
        if ($restoreAlive.Count -eq 0 -and $closeResult.windowGoneMs -lt 0)
        {
            $closeResult.windowGoneMs = [int]$closeWatch.ElapsedMilliseconds
            Add-Event ("还原窗口已消失（{0}ms）" -f $closeResult.windowGoneMs)
            break
        }

        $dialogs = Resolve-ModalDialogs -PreferYes
        foreach ($dialog in $dialogs)
        {
            $closeResult.confirmDialogSeen = $true
            $closeResult.confirmClicks++
            $record.modalDialogs += [pscustomobject]@{
                Title    = $dialog.Title
                Text     = $dialog.Text
                Buttons  = $dialog.Buttons
                Answered = '是（T-2 中止确认）'
            }
        }
        Start-Sleep -Milliseconds 80
    }
    if ($closeResult.windowGoneMs -lt 0 -and -not $process.HasExited) { $closeResult.windowGoneMs = -1 }
    # 线程回收要给它一点时间，而且要 Refresh 之后再读：
    # Process 对象会缓存进程信息，不 Refresh 读到的还是启动时的数字（踩过：一直是 9）。
    if (-not $process.HasExited)
    {
        Start-Sleep -Milliseconds 1000
        try { $process.Refresh(); $closeResult.threadsAfter = [int]$process.Threads.Count } catch { $closeResult.threadsAfter = -1 }
    }

    # 第二段：关主窗口，量进程退出
    if (-not $process.HasExited)
    {
        foreach ($candidateId in @(2011, 2010, 2012, 2001, 2))
        {
            $mainClose = [Harness.Win]::GetDlgItem($mainWindow, $candidateId)
            if ($mainClose -ne [IntPtr]::Zero) { break }
        }
        [void][Harness.Win]::PostClose($mainWindow)
        $exitDeadline = (Get-Date).AddSeconds($CloseGraceSec)
        while ((Get-Date) -lt $exitDeadline)
        {
            if ($process.HasExited) { break }
            $dialogs = Resolve-ModalDialogs -PreferYes
            foreach ($dialog in $dialogs)
            {
                $closeResult.confirmDialogSeen = $true
                $closeResult.confirmClicks++
            }
            Start-Sleep -Milliseconds 100
        }
    }
    $closeResult.exitMs = [int]$closeWatch.ElapsedMilliseconds
    $closeResult.exited = [bool]$process.HasExited
    if (-not $closeResult.exited)
    {
        [void](Stop-HarnessProcess -Process $process -GraceMs 2000)
        $closeResult.killed = $true
    }
    $record.closeDuringMining = $closeResult
    $record.completed = $closeResult.exited
    $record.completionReason = if ($closeResult.exited) { '挖掘进行中关窗（确认「中止还原」）后回到主窗口，再关主窗口进程正常退出' } else { ('关窗后 {0} 秒进程仍未退出，已强杀' -f $CloseGraceSec) }
    Add-Event ("关窗结果：还原窗口消失={0}ms 线程 {1}->{2} 确认框={3}({4}次) 进程退出={5} 用时 {6}ms killed={7}" -f `
            $closeResult.windowGoneMs, $closeResult.threadsBefore, $closeResult.threadsAfter,
        $closeResult.confirmDialogSeen, $closeResult.confirmClicks, $closeResult.exited, $closeResult.exitMs, $closeResult.killed)
    Save-And-Exit '' 0
}

# ---------- 等完成 ----------

$outputSignature = ''
$outputStableSince = Get-Date
$lastTitle = ''
$titleSamples = New-Object System.Collections.ArrayList
$terminalSeen = $false
$titleHasPattern = $false
$runStart = Get-Date
$timeout = (Get-Date).AddSeconds($TimeoutSec)
$completionReason = ''
$lastProgressPrint = Get-Date

# 重负载下的界面健康度：每轮给还原窗口发一个带超时的 WM_NULL（SendMessageTimeoutW
# + SMTO_ABORTIFHUNG + SMTO_BLOCK），量它多久才被界面线程处理。
# 如果（比如）挖掘还在界面线程上同步跑，这个往返时间会立刻变成几百毫秒甚至超时。
# T-1 要的就是"挖掘正在跑"那段时间里的这些数字，所以探测必须便宜、必须频繁。
$probeLatencies = New-Object System.Collections.ArrayList
$probeSamples = New-Object System.Collections.ArrayList   # "相对毫秒|延迟" 的紧凑记录
$probeHungCount = 0
$pollCount = 0
$lastListViewSample = @()
# 挖掘开始的判定关键词：**必须是只有真的开跑才会出现的行**。
# 产品在窗口初始化时会写一段功能说明（"勾「推理补充还原」会……挖候选名……"），
# 那段话在没勾选时也照样出现 —— 第一版拿它当"挖掘开始"，于是未勾选的用例
# 一直在等一个永远不会来的结束标记，白等到超时（踩过）。
$miningKeywords = @(
    '开始从游戏资源里挖掘候选名'   # 真的开跑
    '挖掘中：扫描'                 # 挖掘进度行
    '普通还原完成，开始'           # 两遍式的分界
    '第二遍开始'
)
$miningFirstSeenMs = -1
$lastLogProbeMs = 0
# 挖掘是**独立于还原任务**的一个阶段：日志里出现"挖掘中"时，标题已经是"完成 N / 运行中 0"，
# 输出目录也稳定了。如果只看标题+输出，会在挖掘还没跑完时就宣布"完成"——
# 规模档下（3 百万候选）就是这么被自己截断的：量到 4 秒，其实挖掘还在跑。
# 所以：一旦看到挖掘开始，就必须再等到挖掘的**结束标记**（最终统计行 / 第二遍 / 后映射）。
$miningStarted = $false
$miningFinished = $false
$miningFinishPattern = '试算\s*[0-9,]+\s*/\s*命中|第二遍|补充还原后映射|挖掘结束'
# 小用例（补充还原夹具）跑得快，探测要密一点才采得到样；大用例走原节奏别拖慢被测程序。
$probeSleepMs = if ($supplementMeta) { 120 } else { 400 }
$heavyEvery = 3

while ((Get-Date) -lt $timeout)
{
    if ($process.HasExited)
    {
        $completionReason = ("被测程序已退出（退出码 {0}）" -f $process.ExitCode)
        break
    }

    $pollCount++
    $dialogs = Resolve-ModalDialogs
    foreach ($dialog in $dialogs)
    {
        $record.modalDialogs += $dialog
        if (-not $record.failure) { $record.failure = ("还原过程中界面弹出对话框：{0} / {1}" -f $dialog.Title, $dialog.Text) }
    }

    # 还原窗口标题：新产品把 排队/运行中/完成 的计数放在标题里，是最省事的进度源。
    $currentTitle = ''
    if ($record.uiMode -eq 'restore-window')
    {
        $alive = @(Get-TopLevels | Where-Object { $_.HwndHex -eq $record.restoreWindow.HwndHex })
        if ($alive.Count -eq 0)
        {
            $completionReason = '还原窗口已关闭'
            break
        }
        $currentTitle = $alive[0].Text
        $restoreWindow = $alive[0]

        # —— 廉价探测：只发一个 WM_NULL 并量往返 ——
        $latency = [Harness.Win]::ProbeLatency($restoreWindow.Hwnd, 3000)
        [void]$probeLatencies.Add($latency)
        if ($latency -lt 0) { $probeHungCount++ }
        $sinceStartMs = [int]((Get-Date) - $runStart).TotalMilliseconds
        [void]$probeSamples.Add(('{0}|{1}' -f $sinceStartMs, $latency))

        # —— 重的活儿不必每轮都做 ——
        if ($pollCount % $heavyEvery -eq 0)
        {
            # 顺带采一次任务列表：用来算"界面上一共看到过几次进度变化"。
            $sample = @()
            foreach ($child in [Harness.Win]::Children($restoreWindow.Hwnd))
            {
                if ($child.Class -ne 'SysListView32') { continue }
                $rows = [Harness.Win]::ListCount($child.Hwnd)
                for ($row = 0; $row -lt [Math]::Min($rows, 20); $row++)
                {
                    $cells = @()
                    for ($sub = 1; $sub -lt 4; $sub++)
                    {
                        $value = [Harness.Win]::RemoteListViewText($child.Hwnd, $appPid, $row, $sub)
                        if ($null -eq $value) { $value = '?' }
                        $cells += $value
                    }
                    $sample += ($cells -join '|')
                }
            }
            if ($sample.Count -gt 0 -and (($sample -join ';') -ne ($lastListViewSample -join ';')))
            {
                $lastListViewSample = $sample
                $record.listSampleCount = [int]$record.listSampleCount + 1
            }

            # 详细日志里一旦出现"挖掘"字样，就把首次出现的时间记下来 ——
            # T-1 靠它区分"挖掘窗口内的采样"与"挖掘之前的采样"。
            # 注意：挖掘开始之后**还要继续读日志**，否则永远等不到结束标记
            # （第一版就是只在"还没看到挖掘"时读，结果挖掘早就跑完了、我还在等，
            #   白等 10 分钟才发现）。
            if ($miningFirstSeenMs -lt 0 -or ($miningStarted -and -not $miningFinished))
            {
                $tail = Get-UiLogText -Window $restoreWindow -AppPid $appPid
                if ($tail)
                {
                    foreach ($keyword in $miningKeywords)
                    {
                        if ($tail -like ('*' + $keyword + '*'))
                        {
                            if ($miningFirstSeenMs -lt 0)
                            {
                                $miningFirstSeenMs = $sinceStartMs
                                Add-Event ("检测到挖掘阶段开始（{0}ms）—— 后面要等它的结束标记才算跑完" -f $sinceStartMs)
                            }
                            $miningStarted = $true
                            break
                        }
                    }
                    # 挖掘结束标记：最终统计行（带"试算"）/ 第二遍开始 / 后映射汇总
                    if ($miningStarted -and -not $miningFinished -and $tail -match $miningFinishPattern)
                    {
                        $miningFinished = $true
                        Add-Event ("检测到挖掘阶段结束（{0}ms）" -f $sinceStartMs)
                    }
                }
            }
        }
    }
    else
    {
        $currentTitle = [Harness.Win]::CaptionOf($mainWindow)
        if (-not [Harness.Win]::IsWindowEnabled([Harness.Win]::GetDlgItem($mainWindow, $step3Id)))
        {
            # 还在跑
        }
        else
        {
            $completionReason = '主窗口的主流程按钮已恢复可用（内联还原结束）'
            Start-Sleep -Milliseconds 800
            break
        }
    }

    if ($currentTitle -ne $lastTitle)
    {
        $lastTitle = $currentTitle
        [void]$titleSamples.Add($currentTitle)
        Add-Event ("标题：{0}" -f $currentTitle)
    }
    if ($currentTitle -match '排队|运行中|完成|失败') { $titleHasPattern = $true }
    if ($currentTitle -match '排队\s*0' -and $currentTitle -match '运行中\s*0' -and $currentTitle -match '完成\s*[1-9]')
    {
        $terminalSeen = $true
    }

    # 输出目录：内容稳定一段时间就算落定。
    # 每 3 轮算一次（大用例下这一步要遍历几万个文件，不能每轮都做）。
    if ($pollCount % $heavyEvery -eq 0)
    {
        $signature = ''
        if (Test-Path -LiteralPath $outputPath)
        {
            $signature = if ('Harness.Batch' -as [type]) { [Harness.Batch]::Signature($outputPath) } else { '' }
        }
        if ($signature -ne $outputSignature)
        {
            $outputSignature = $signature
            $outputStableSince = Get-Date
        }
    }

    $settledMs = ((Get-Date) - $outputStableSince).TotalMilliseconds
    $sinceStartMs = ((Get-Date) - $runStart).TotalMilliseconds
    $hasOutput = ((Get-FastFileCount -Path $outputPath) -gt 0)

    # 这一步可能要跑很久，别让调用方以为卡死了。
    if (((Get-Date) - $lastProgressPrint).TotalSeconds -ge 5)
    {
        $lastProgressPrint = Get-Date
        $fileCount = Get-FastFileCount -Path $outputPath
        $probeMax = if ($probeLatencies.Count -gt 0) { ($probeLatencies | Measure-Object -Maximum).Maximum } else { 0 }
        # 注意：除法必须加括号。写作 -f $sinceStartMs / 1000, $a, $b 时，
        # `-f` 比逗号结合得更紧，会变成 (fmt -f ($sinceStartMs/1000)), $a, $b —— 参数不够直接报
        # "Index must be ... less than the size of the argument list"。
        # 小用例只跑 4 秒、这条 5 秒才打印一次，所以一直没暴露；压力用例一跑就炸。
        Write-Host ("    [{0:N0}s] 标题='{1}' 输出 {2} 个文件 稳定 {3:N0}ms 探测 {4} 次/最大 {5}ms" -f `
                ($sinceStartMs / 1000), $currentTitle, $fileCount, $settledMs, $probeLatencies.Count, $probeMax)
    }

    # 挖掘还在跑就不许收工：标题的"完成 N"只统计还原任务，挖掘是另一条线。
    $miningPending = ($miningStarted -and -not $miningFinished)
    if ($settledMs -ge $SettleMs -and $sinceStartMs -gt 3000 -and $hasOutput -and -not $miningPending)
    {
        if ($terminalSeen) { $completionReason = '标题显示 排队0/运行中0/完成N，且输出目录已稳定'; break }
        if (-not $titleHasPattern) { $completionReason = ('标题里没有进度字样，输出目录已稳定 {0:N0} ms' -f $settledMs); break }
        if ($settledMs -ge 20000) { $completionReason = ('标题出现过进度字样但未到终态，输出目录已稳定 {0:N0} ms（按稳定判定）' -f $settledMs); break }
    }
    elseif ($miningPending -and ($sinceStartMs % 5000) -lt 250)
    {
        Write-Host ("    [等待挖掘完成] 已过 {0:N0}ms（挖掘从 {1}ms 开始）" -f $sinceStartMs, $miningFirstSeenMs)
    }

    Start-Sleep -Milliseconds $probeSleepMs
}

$record.titleTimeline = $titleSamples
$record.completionReason = $completionReason
$record.pollCount = $pollCount
$record.pollHungCount = $probeHungCount
$record.probeLatencyMaxMs = if ($probeLatencies.Count -gt 0) { [int]($probeLatencies | Measure-Object -Maximum).Maximum } else { 0 }
$record.probeLatencyAvgMs = if ($probeLatencies.Count -gt 0) { [int]($probeLatencies | Measure-Object -Average).Average } else { 0 }
$record.restoreElapsedMs = [int]((Get-Date) - $runStart).TotalMilliseconds

# ---------- T-1 证据：探测样本 + "挖掘开始之后"的切片 ----------
#
# 采样要能证明"挖掘正在跑的那段时间界面也是活的"，所以把每次探测都留下来
# （紧凑成 "相对毫秒|往返毫秒"），并单独切出挖掘开始之后的那一段。
$record.miningStarted = $miningStarted
$record.miningFinished = $miningFinished
$record.probeSamples = @($probeSamples)
$record.miningFirstSeenMs = $miningFirstSeenMs
$afterMining = @()
if ($miningFirstSeenMs -ge 0)
{
    foreach ($sample in $probeSamples)
    {
        $parts = ([string]$sample) -split '\|'
        if ($parts.Count -lt 2) { continue }
        if ([int]$parts[0] -ge $miningFirstSeenMs) { $afterMining += [int]$parts[1] }
    }
}
$record.probeAfterMiningCount = $afterMining.Count
$record.probeAfterMiningMaxMs = if ($afterMining.Count -gt 0) { [int]($afterMining | Measure-Object -Maximum).Maximum } else { 0 }
$record.probeAfterMiningHung = @($afterMining | Where-Object { $_ -lt 0 }).Count

# ---------- 收尾取样 ----------

if ($record.uiMode -eq 'restore-window')
{
    $alive = @(Get-TopLevels | Where-Object { $_.HwndHex -eq $record.restoreWindow.HwndHex })
    if ($alive.Count -gt 0)
    {
        $restoreWindow = $alive[0]
        $record.restoreWindow = [ordered]@{ HwndHex = $restoreWindow.HwndHex; Class = $restoreWindow.Class; Title = $restoreWindow.Text }
        $record.controls = @(Get-ChildInventory $restoreWindow)

        # ListView（任务概览）逐行读出来：哪个封包失败一眼可见。
        $rows = @()
        foreach ($child in [Harness.Win]::Children($restoreWindow.Hwnd))
        {
            if ($child.Class -ne 'SysListView32') { continue }
            $count = [Harness.Win]::ListCount($child.Hwnd)
            for ($row = 0; $row -lt [Math]::Min($count, 20); $row++)
            {
                $cells = @()
                for ($sub = 0; $sub -lt 4; $sub++)
                {
                    $value = [Harness.Win]::RemoteListViewText($child.Hwnd, $appPid, $row, $sub)
                    if ($null -eq $value) { $value = '<读不到>' }
                    $cells += $value
                }
                $rows += [pscustomobject]@{ List = $child.HwndHex; Row = $row; Cells = ($cells -join ' | ') }
            }
        }
        $record.listRows = $rows

        # 最长的那段文字基本就是"详细日志"框。
        $longest = Get-UiLogText -Window $restoreWindow -AppPid $appPid
        # 行数才是"进度上报次数"的证据（界面上一条任务几行，不随文件数增长）；
        # JSON 里只留尾部，避免几万行把结果文件撑爆。
        $record.uiLogLines = @($longest -split "`r?`n" | Where-Object { $_.Trim() }).Count
        $record.uiLogChars = $longest.Length
        $record.uiLogFull = if ($longest.Length -gt 200000) { $longest.Substring(0, 200000) } else { $longest }
        if ($longest.Length -gt 4000) { $longest = $longest.Substring(0, 4000) + '…' }
        $record.logText = $longest
    }
}

# 名字表快照：产品会把挖到的名字写在游戏目录/工具目录里，后面的用例会覆盖它 ——
# 先拷一份到 out\ 里留证；断言 S-4/S-5 读这一份，不受后续用例影响。
$record.minedNamesSnapshot = ''
foreach ($candidate in @($script:minedListCandidates))
{
    if ($candidate -and (Test-Path -LiteralPath $candidate))
    {
        $snapshot = Join-Path $outDir (([System.IO.Path]::GetFileNameWithoutExtension($OutJson)) + '-MinedNames.lst')
        Copy-Item -LiteralPath $candidate -Destination $snapshot -Force
        $record.minedNamesSnapshot = $snapshot
        Add-Event ("名字表快照：{0} -> {1}" -f $candidate, $snapshot)
        break
    }
}
if (-not $record.minedNamesSnapshot) { Add-Event '本轮没有生成 MinedNames.lst' }

$digestAfter = Get-DirectoryDigest -Path $sourceDir -WithHash
$record.sourceDigestAfter = $digestAfter
$record.sourceSnapshotAfter = if ($digestAfter.Count -le 3000) { Get-DirectorySnapshot -Path $sourceDir -WithHash } else { $null }
Add-Event ("源目录摘要（后）：{0} 个文件，digest {1}" -f $digestAfter.Count, $digestAfter.Digest.Substring(0, 12))

# 输出产物清单（含 sha256，供逐字节比对）。大用例走 C# 批量哈希并落 TSV，
# 避免把几万条明细塞进结果 JSON（几十 MB 的 JSON 光是序列化就要好几分钟）。
$outputFiles = @(Get-OutputListing -Path $outputPath)
$record.output = @()
$record.outputTsv = ''
if ($outputFiles.Count -gt 2000)
{
    $tsvPath = Join-Path $outDir (([System.IO.Path]::GetFileNameWithoutExtension($OutJson)) + '-output.tsv')
    $lines = New-Object System.Collections.ArrayList
    [void]$lines.Add("rel`tsize`tsha256")
    foreach ($item in $outputFiles) { [void]$lines.Add(("{0}`t{1}`t{2}" -f $item.rel, $item.size, $item.sha256)) }
    Write-TsvFile -Path $tsvPath -Lines ([string[]]$lines.ToArray())
    $record.outputTsv = $tsvPath
    Add-Event ("输出清单 {0} 条已写入 TSV：{1}" -f $outputFiles.Count, $tsvPath)
}
else
{
    $record.output = $outputFiles
}

# 工具自己的日志（节流证据）：界面详细日志几行、每封包明细文件几行、会话日志几行。
$toolLogDir = Join-Path (Split-Path -Parent $AppExe) 'Log'
$detailStats = @()
if (Test-Path -LiteralPath $toolLogDir)
{
    foreach ($file in (Get-ChildItem -LiteralPath $toolLogDir -File -ErrorAction SilentlyContinue | Sort-Object Name))
    {
        # 用共享读打开：YuaExtract.log 可能正被还活着的被测程序占着，ReadAllLines 会抛。
        $text = Read-TextAutoShared -Path $file.FullName -MaxBytes 20000000
        $lineCount = if ($null -eq $text) { -1 } else { @($text -split "`r?`n").Count }
        $detailStats += [pscustomobject]@{ Name = $file.Name; Size = [long]$file.Length; Lines = $lineCount }
    }
}
$record.toolLogFiles = $detailStats
Add-Event ("工具日志：{0} 个文件（{1}）" -f $detailStats.Count, (($detailStats | ForEach-Object { '{0}:{1}行' -f $_.Name, $_.Lines }) -join ', '))

# 还原报告：断言A4 要拿它里面的"未还原被丢弃"计数跟脚手架独立算出来的数对比，
# 所以这里把原文抠下来存进结果 JSON（报告是产物不是日志，读它要 FileShare.ReadWrite）。
$reportCandidates = @($outputFiles | Where-Object { $_.rel -notmatch '\\' -and $_.rel -match '^RestoreReport' })
$record.reportPath = ''
$record.reportText = ''
if ($reportCandidates.Count -gt 0)
{
    $reportFull = Join-Path $outputPath $reportCandidates[0].rel
    $record.reportPath = $reportFull
    $text = Read-TextAutoShared -Path $reportFull
    if ($text) { $record.reportText = $text }
    Add-Event ("已抓取还原报告：{0}（{1} 字节）" -f $reportFull, $reportCandidates[0].size)
}
else
{
    Add-Event '输出目录顶层没有 RestoreReport* 报告文件'
}

# 输出目录跑到别处去了？把工作区里所有 Restored_Extractor_Output* 都列出来。
$candidates = @()
foreach ($directory in (Get-ChildItem -LiteralPath $WorkspaceRoot -Recurse -Directory -Force -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -like 'Restored_Extractor_Output*' }))
{
    $count = @(Get-ChildItem -LiteralPath $directory.FullName -Recurse -File -Force -ErrorAction SilentlyContinue).Count
    $candidates += [pscustomobject]@{ Path = $directory.FullName; Files = $count }
}
$record.outputCandidates = $candidates
if ($candidates.Count -gt 0 -and -not ($candidates | Where-Object { $_.Path -eq $outputPath }))
{
    Add-Event ("输出目录不是约定的那个，实际出现：{0}" -f (($candidates | ForEach-Object { $_.Path }) -join '; '))
}

Add-Event ("结束时输出文件 {0} 个" -f $outputFiles.Count)

# ---------- 关掉被测程序 ----------

Stop-AppProcess
Add-Event '已关闭被测程序'

if (-not $completionReason) { $record.failure = ("{0} 秒内没有等到还原结束（既没看到输出目录稳定，也没看到终态）" -f $TimeoutSec) }
$record.completed = [bool]$completionReason
Save-And-Exit $record.failure 0
