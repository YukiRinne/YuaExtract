# 页签像素幽灵（task-7 / 断言 E-11）UI 驱动：黑盒抓图，把"证据快照"落盘。
#
#   pwsh -File run-tab-pixels.ps1                       # 默认：App 快照 + 全状态矩阵
#   pwsh -File run-tab-pixels.ps1 -SkipSnapshotSync    # 不重同步 Release 快照（调试用）
#   pwsh -File run-tab-pixels.ps1 -OutJson Run\out\tab-pixels.json
#
# 为什么要有这条用例（用户实测的缺陷）：
#   切到「制作扩展集」页签后，**首页控件的旧像素留在画面上**（叠字）。
#   此时控件的 `IsWindowVisible` 全是 False（可见性是对的），所以既有断言体系全绿 ——
#   只有"抓图逐像素比对"能看见它（本脚本 + assert-tab-pixels.ps1 的 E-11）。
#
# 判据的设计（标定过程见 README「E-11」一节，数据见 Run\out\tab-pixels.json 的 calibration）：
#   1) 同一个页签，**不管你是怎么到达它的**，画面必须一样 —— 所以基线取
#      「新启动 →（第一次点击）直接点制作扩展集」，其余路径（首页→制作、扩展集→制作、
#      日志展开收起→制作、以及同进程内反复来回）都必须与它逐像素一致。
#      * 这是"自洽性"判据：不需要预先准备一张"干净参考图"（那种参考图会在界面改动后失效，
#        也会把"实现长什么样"写死进断言）。
#      * 缺陷存在时实测最小信号 12,762 px（占客户区 1.369%），而同一状态连拍两张的
#        **噪声地板实测 0 px** —— 阈值有 13 倍以上的余量（阈值 0.10%）。
#   2) 抓图要能判读：图不能是空白/单色，且"首页图 vs 制作页图"必须显著不同
#      （防"页签没切过去，两边都是同一张图"这种假通过）。
#   3) 同状态连拍必须逐像素一致（噪声地板），否则本次测量不可判读。
#   4) 另起一个进程走一遍「新启动→直接点制作扩展集」，与进程1 的基线比 —— 证明
#      基线与进程无关（窗口尺寸/DPI 一致，图就应当一致）。
#
# 存证：Run\out\tab-pixels\*.bmp（逐像素权威证据）+ *.png（人眼看）+ *-diff.png（差异热区）
#       断言只读这些快照 + Run\out\tab-pixels.json，**读不到就判失败**，绝不回读现场
#       （现场会被下一条用例改掉；README 踩坑 #22）。
#
# 收尾：默认把**所有** YuaExtract 实例结束掉（残留实例会让下一条用例认到旧窗口、也会让别人
#       构建时撞 LNK1104）；`-KeepForeignInstances` 只收自己起的那个。
#       每个关键步骤都查进程活性：被测进程中途消失（常见原因：并发用例的收尾把它杀了）
#       会记成"快照不完整"并在 JSON 里写明，而不是伪装成断言数据。

[CmdletBinding()]
param(
    [string]$AppExe = '',
    [string]$OutJson = '',
    [string]$OutDir = '',
    [int]$ClientWidth = 1150,
    [int]$ClientHeight = 795,
    [int]$StartupTimeoutSec = 45,
    [int]$SettleMs = 900,
    [int]$TimeoutSec = 300,
    [string]$IdsJson = '',
    [switch]$SkipSnapshotSync,
    # 跳过"另起一个进程走 direct 路径"的跨进程复现（调试用；正式跑不要关）
    [switch]$NoCrossProcess,
    # 跳过"扩展集页签状态矩阵"那一段（E-12-a 的依据；调试用，正式跑不要关）
    [switch]$NoSetTab,
    # 结果 JSON 里只记元数据、不记控件矩形表（自检造合成快照时用不到；正常跑不要关）
    [switch]$NoControlRects,
    [switch]$KeepForeignInstances
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessNative.ps1')
. (Join-Path $root 'lib\HarnessPixels.ps1')

$runRoot = Join-Path $root 'Run'
$outRoot = Join-Path $runRoot 'out'
if (-not $OutJson) { $OutJson = Join-Path $outRoot 'tab-pixels.json' }
if (-not $OutDir) { $OutDir = Join-Path $outRoot 'tab-pixels' }
foreach ($directory in @($runRoot, $outRoot, $OutDir))
{
    if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
}

# 清掉上一轮的同名产物：只删本驱动自己的命名（tab-*/diff-*/probe-*），
# 避免"这一次没抓到的图"被后来的断言/人眼当成这次的证据。
foreach ($pattern in @('tab-*.bmp', 'tab-*.png', 'diff-*.bmp', 'diff-*.png', 'probe-*.bmp', 'probe-*.png'))
{
    Get-ChildItem -LiteralPath $OutDir -Filter $pattern -File -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue
}

# ==================================================================
# 控件 ID（docs\扩展集设计.md §8.4 冻结；-IdsJson 可覆盖，便于排查）
# ==================================================================
$script:UiIds = [ordered]@{
    MainWindowClass = 'YuaExtractMainWindow'
    TabHome         = 4001
    TabMakeSet      = 4002
    TabSet          = 4003
    TabAbout        = 4004
    LogButton       = 4072
    LogView         = 4073
    Overview        = 4070
    Page            = 4071
    SetStatus       = 4111
    SetGenerate     = 4101   # 制作页签标志控件
    SetmRefresh     = 4131   # 「扩展集」页签标志控件（§8.5 驱动约定 4）
    Step1           = 4050   # 首页标志控件
    FuncExtract     = 4030   # 首页模式按钮
    GroupExe        = 4010
}
if ($IdsJson)
{
    $override = Read-JsonFile -Path $IdsJson
    if (-not $override) { throw ("读不到 -IdsJson：{0}" -f $IdsJson) }
    foreach ($property in $override.PSObject.Properties)
    {
        if (-not $script:UiIds.Contains($property.Name)) { throw ("-IdsJson 里有未知键：{0}" -f $property.Name) }
        $script:UiIds[$property.Name] = $property.Value
    }
}

# 阈值随快照一起落盘（断言读它；标定依据写进 README，数值来自实测）
$script:Thresholds = [ordered]@{
    # 主判据：页签状态矩阵里任意两张制作页图的最大允许差异比例。
    # 实测：噪声地板 0.0000%，缺陷最小信号 1.369%（12,762 px）→ 取 0.10%（≈932 px），
    # 距信号 13.7 倍、距噪声地板（0）有 932 px 余量。
    stateDiffRatio        = 0.0010
    # 同状态连拍（噪声地板）：必须几乎完全相同。
    noiseDiffRatio        = 0.0001
    # 跨进程复现：另起进程的 direct 路径与进程1 基线。
    crossProcessDiffRatio = 0.0010
    # 首页页签的历史无关性（幽灵是双向的：制作页的像素也会留在首页上）。实测 11.57%。
    #
    # 为什么这里给 1%（比制作页签宽 10 倍）：修复后首页仍有 **1,698 px（0.186%）** 的合法差异，
    # 现场判读过，两块来源都不是幽灵：
    #   ① 概览 RichEdit（4070）**同一段文字被重新 SetWindowText 之后的重绘差异**（约 1,510 px）——
    #      用 SmartText 逐字比对：启动时与回首页后都是 464 字、正文完全相同（切页签时
    #      RenderedOverview 被制作页的文字顶掉，回到首页会重设一次，行内像素因此有几像素抖动）；
    #   ② 点过的页签按钮画出**焦点框**（约 188 px）—— BM_GETSTATE 实测启动时 0x0000、
    #      点过之后 0x0008（BST_FOCUS）。
    # 也就是说：首页判据必须容忍"重绘噪声"，但绝不容忍"旧控件像素残留"（修复前 11.57%）。
    # 1% 距合法噪声 5.4 倍、距缺陷信号 11.6 倍。
    homeHistoryDiffRatio  = 0.0100
    # 正对照：首页图与制作页图必须至少差这么多（实测 18.01% / 18.54%）。
    minHomeMakeDiffRatio  = 0.05
    # 「扩展集」页签（E-12-a）：**故意与制作页签同口径**（0.10%）。
    # 为什么同口径：两页的残留机理是同一个（大号 BS_GROUPBOX 只画边框、父窗口被 WS_CLIPCHILDREN
    # 挡住，被隐藏控件画过的像素留在表面上），没有理由给新页更宽的口子。
    # 已知风险（实测一轮后按需标定，不许悄悄放宽）：右栏 4071 与首页 4070 一样是 RichEdit，
    # 首页那边实测过"同一段文字被重设一次"有 0.186% 的合法重绘差异；扩展集页签切页签时
    # 也会重设 4071，所以**首次实跑**要盯这一项：若超阈值且热区全落在 4071 内、最大通道差 ≤1，
    # 按"合法重绘"处理并在这里写明实测值（README「E-12」一节记录）。
    setStateDiffRatio     = 0.0010
    # 正对照：扩展集页签图与首页/制作页图必须至少差这么多（防"页签没切过去"）
    minSetRefDiffRatio    = 0.05
    # 抓图有效性：不同颜色数下限、主色占比上限（单色图 = 抓图坏了）。
    minColors             = 50
    maxModalShare         = 0.95
}

$record = [ordered]@{
    case           = 'tab-pixels'
    startedUtc     = (Get-Date).ToUniversalTime().ToString('o')
    finishedUtc    = ''
    appExe         = ''
    appExeSha256   = ''
    appExeBytes    = 0
    snapshotCopied = $false
    clientWidth    = $ClientWidth
    clientHeight   = $ClientHeight
    settleMs       = $SettleMs
    thresholds     = $script:Thresholds
    failure        = $null
    notes          = @()
    events         = @()
    captures       = @()
    controlRects   = [ordered]@{}
    dialogs        = @()
    processCleanup = [ordered]@{ own = 0; foreign = 0; foreignKilled = 0; deaths = 0 }
    calibration    = [ordered]@{
        measuredUtc        = '2026-10-14（修复前 6B664840 / 修复后 492522BD 两轮实测）'
        noiseFloorRatio    = 0.0
        minGhostSignalRatio = 0.01369
        otherSignals       = @(0.0351, 0.0576, 0.0713, 0.0928, 0.1059)
        homeHistoryRatio   = 0.1157
        homeBenignRatioAfterFix = 0.00186
        homeVsMakeRatio    = 0.1854
        note               = ('制作页签：同状态连拍 0 px、最小幽灵信号 12,762 px = 1.369% → 阈值 0.10%（距信号 13.7 倍）。' +
            '首页：修复后合法重绘噪声 1,698 px = 0.186%（概览 RichEdit 重设同文本 1,510 px + 页签焦点框 188 px，' +
            '已用逐字文本比对与 BM_GETSTATE 判读），缺陷信号 11.57% → 阈值 1%。')
    }
}

$script:events = New-Object System.Collections.ArrayList
function Add-Event([string]$Text)
{
    if ($script:events.Count -lt 500) { [void]$script:events.Add(("[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $Text)) }
    Write-Host ("  · {0}" -f $Text)
}

# ==================================================================
# 被测进程
# ==================================================================
$script:appProcess = $null
$script:mainWindow = [IntPtr]::Zero
$script:deaths = 0

function Stop-AppProcess
{
    if ($script:appProcess)
    {
        try
        {
            if (-not $script:appProcess.HasExited)
            {
                if ($script:mainWindow -ne [IntPtr]::Zero) { [void][Harness.Win]::PostClose($script:mainWindow) }
                Start-Sleep -Milliseconds 500
                if (-not $script:appProcess.HasExited) { [void](Stop-HarnessProcess -Process $script:appProcess -GraceMs 2500) }
            }
        }
        catch { }
        $record.processCleanup.own++
        $script:appProcess = $null
    }

    # README 踩坑 #16：残留实例会让下一条用例认到旧窗口。
    $others = @(Get-Process YuaExtract -ErrorAction SilentlyContinue)
    if ($others.Count -gt 0)
    {
        $record.processCleanup.foreign = $others.Count
        if ($KeepForeignInstances) { Add-Event ("收尾：还有 {0} 个 YuaExtract 实例（-KeepForeignInstances，未结束）" -f $others.Count) }
        else
        {
            foreach ($process in $others)
            {
                try { $process.Kill(); $record.processCleanup.foreignKilled++ } catch { }
            }
            Add-Event ("收尾：结束 {0} 个残留 YuaExtract 实例（-KeepForeignInstances 可只收自己起的）" -f $record.processCleanup.foreignKilled)
        }
    }
}

function Save-And-Exit
{
    param([string]$Reason, [int]$Code = 2)

    Stop-AppProcess
    $record.events = @($script:events)
    if ($Reason) { $record.failure = $Reason }
    $record.finishedUtc = (Get-Date).ToUniversalTime().ToString('o')
    Write-JsonFile -Path $OutJson -Object $record
    if ($Reason) { Write-Host ("[tab-pixels] 失败：{0}" -f $Reason) }
    Write-Host ("[tab-pixels] 结果写入 {0}" -f $OutJson)
    exit $Code
}

# 被测进程还活着吗？死了就抛（带上"疑似被并发用例收尾杀掉"的提示）。
function Assert-AppAlive([string]$Where)
{
    if (-not $script:appProcess) { throw ("[{0}] 被测进程不存在" -f $Where) }
    $script:appProcess.Refresh()
    if ($script:appProcess.HasExited)
    {
        $script:deaths++
        $record.processCleanup.deaths = $script:deaths
        throw ("[{0}] 被测进程已退出（退出码 {1}）—— 快照不完整。常见原因：并发跑 UI 用例时别人的收尾 " +
            "`Get-Process YuaExtract | Stop-Process -Force` 把本进程一起杀了（README 踩坑 #16）" -f $Where, $script:appProcess.ExitCode)
    }
    return $true
}

function Start-AppProcess
{
    param([string]$What)

    $exePath = $record.appExe
    Add-Event ("启动被测进程（{0}）：{1}" -f $What, $exePath)
    $script:appProcess = Start-HarnessProcess -FilePath $exePath -WorkingDirectory (Split-Path -Parent $exePath)
    $script:mainWindow = [IntPtr]::Zero
    $deadline = (Get-Date).AddSeconds($StartupTimeoutSec)
    while ((Get-Date) -lt $deadline)
    {
        Start-Sleep -Milliseconds 300
        Assert-AppAlive '启动' | Out-Null
        foreach ($window in @([Harness.Win]::TopLevel([uint32]$script:appProcess.Id)))
        {
            if ($window.Class -eq [string]$script:UiIds.MainWindowClass) { $script:mainWindow = [IntPtr]$window.Hwnd; break }
        }
        if ($script:mainWindow -ne [IntPtr]::Zero) { break }
    }
    if ($script:mainWindow -eq [IntPtr]::Zero) { throw ("启动 {0} 秒后仍找不到主窗口（类名 {1}）" -f $StartupTimeoutSec, $script:UiIds.MainWindowClass) }

    Start-Sleep -Milliseconds 2000
    Assert-AppAlive '启动后稳定期' | Out-Null
    Add-Event ("主窗口就绪：{0}（类 {1}）" -f [Harness.Win]::CaptionOf($script:mainWindow), [Harness.Win]::ClassOf($script:mainWindow))

    # 客户区固定尺寸：跨进程/跨状态的图必须同尺寸才能逐像素比。
    $resizeError = [Harness.Win]::ResizeClientArea($script:mainWindow.ToInt64(), $ClientWidth, $ClientHeight)
    if ($resizeError) { throw ("调整客户区尺寸失败：{0}" -f $resizeError) }
    Start-Sleep -Milliseconds 1200
    Assert-AppAlive '调整客户区尺寸后' | Out-Null
    $client = [Harness.Win]::ClientRectOnScreen($script:mainWindow.ToInt64())
    $actualWidth = $client[2] - $client[0]
    $actualHeight = $client[3] - $client[1]
    if ($actualWidth -ne $ClientWidth -or $actualHeight -ne $ClientHeight)
    {
        throw ("客户区尺寸没对上：期望 {0}x{1}，实际 {2}x{3}（逐像素比对要求几何完全一致）" -f `
                $ClientWidth, $ClientHeight, $actualWidth, $actualHeight)
    }
    Add-Event ("客户区固定为 {0}x{1}" -f $actualWidth, $actualHeight)
}

# ==================================================================
# 控件感知（抓图当时的状态 + 热区归属用的矩形表）
# ==================================================================

# 主窗口可见子控件的矩形（客户区坐标）—— 热区归属靠它把像素位置翻成"哪个控件"。
function Get-VisibleControlRects
{
    $client = [Harness.Win]::ClientRectOnScreen($script:mainWindow.ToInt64())
    $originX = $client[0]
    $originY = $client[1]
    $rows = New-Object System.Collections.ArrayList
    foreach ($child in [Harness.Win]::Children($script:mainWindow.ToInt64()))
    {
        if ($child.CtrlId -le 0) { continue }
        $rect = [Harness.Win]::WindowRect($child.Hwnd)
        if (-not $rect) { continue }
        $text = [string]$child.Text
        if ($text.Length -gt 40) { $text = $text.Substring(0, 40) + '…' }
        [void]$rows.Add([ordered]@{
                id      = $child.CtrlId
                class   = $child.Class
                text    = $text
                visible = $child.Visible
                x       = $rect[0] - $originX
                y       = $rect[1] - $originY
                w       = $rect[2] - $rect[0]
                h       = $rect[3] - $rect[1]
            })
    }
    return @($rows)
}

function Get-ControlVisible([int]$Id)
{
    $handle = [Harness.Win]::GetDlgItem($script:mainWindow, $Id)
    if ($handle -eq [IntPtr]::Zero) { return $null }
    return [Harness.Win]::IsWindowVisible($handle)
}

# 意外弹框：记下来并点掉（弹框挡着窗口，抓出来的像素就不是页面本身了）。
function Resolve-ModalDialogs
{
    $found = New-Object System.Collections.ArrayList
    foreach ($window in @([Harness.Win]::TopLevel([uint32]$script:appProcess.Id)))
    {
        if ($window.Hwnd -eq $script:mainWindow.ToInt64()) { continue }
        if ($window.Class -ne '#32770') { continue }
        $buttons = @()
        $texts = @()
        foreach ($child in [Harness.Win]::Children($window.Hwnd))
        {
            if ($child.Class -eq 'Static' -and $child.Text) { $texts += $child.Text }
            if ($child.Class -eq 'Button' -and $child.Text) { $buttons += $child }
        }
        [void]$found.Add([ordered]@{ title = $window.Text; text = ($texts -join ' | '); buttons = (@($buttons | ForEach-Object { $_.Text }) -join ',') })
        if ($buttons.Count -gt 0) { [void][Harness.Win]::PostClick($buttons[0].Hwnd) }
        else { [void][Harness.Win]::PostCommand($window.Hwnd, 1, 0) }
        Add-Event ("抓图期间出现弹框（已点掉）：{0} / {1}" -f $window.Text, ($texts -join ' | '))
    }
    return @($found)
}

function Click-Control([int]$Id, [string]$What)
{
    $handle = [Harness.Win]::GetDlgItem($script:mainWindow, $Id)
    if ($handle -eq [IntPtr]::Zero) { throw ("找不到控件 {0}（{1}）" -f $Id, $What) }
    $ok = [Harness.Win]::SendClick($handle.ToInt64())
    if (-not $ok) { throw ("点控件 {0}（{1}）没有返回（界面卡住？）" -f $Id, $What) }
    Start-Sleep -Milliseconds $SettleMs
    Assert-AppAlive ("点击 {0}" -f $What) | Out-Null
    foreach ($dialog in (Resolve-ModalDialogs)) { $record.dialogs += $dialog }
    Add-Event ("点「{0}」（id={1}）" -f $What, $Id)
}

# ==================================================================
# 抓图（证据快照）
# ==================================================================

function Save-Capture
{
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$Page,
        [Parameter(Mandatory)][string]$History,
        [int]$ActiveTabId = 0
    )

    Assert-AppAlive ("抓图 {0}" -f $Name) | Out-Null
    $bmpPath = Join-Path $OutDir ('tab-{0}.bmp' -f $Name)
    $error = [Harness.Win]::CaptureClientToBmp($script:mainWindow.ToInt64(), $bmpPath)
    if ($error) { throw ("抓图失败（{0}）：{1}" -f $Name, $error) }

    $pngPath = [System.IO.Path]::ChangeExtension($bmpPath, '.png')
    $pngOk = Convert-BmpToPng -BmpPath $bmpPath -PngPath $pngPath
    $image = Read-HarnessBmp -Path $bmpPath
    if (-not $image) { throw ("抓图写出来了但读不回来：{0}" -f $bmpPath) }
    $stats = Get-HarnessBmpStats -Image $image

    $entry = [ordered]@{
        name          = $Name
        page          = $Page
        history       = $History
        activeTabId   = $ActiveTabId
        bmp           = $bmpPath
        png           = $(if ($pngOk) { $pngPath } else { '' })
        width         = $stats.Width
        height        = $stats.Height
        colors        = $stats.Colors
        modalShare    = $stats.ModalShare
        meanLuminance = $stats.MeanLuminance
        sha256        = (Get-Sha256Hex -Path $bmpPath)
        capturedUtc   = (Get-Date).ToUniversalTime().ToString('o')
        # 用哪种方法抓的（bitblt / printwindow1 / printwindow2）——兜底抓的图必须标出来，
        # 不能让人以为全是 PrintWindow 抓的（PrintWindow 实测会无限期阻塞，见 HarnessNative.ps1）
        captureMethod = [Harness.Win]::LastCaptureMethod()
        # 该页标志控件的可见性（切页签成功的自证 + 可见性和像素两条线分开看）
        makeMarkerVisible = (Get-ControlVisible ([int]$script:UiIds.SetGenerate))
        homeMarkerVisible = (Get-ControlVisible ([int]$script:UiIds.Step1))
        setMarkerVisible  = (Get-ControlVisible ([int]$script:UiIds.SetmRefresh))
    }
    $record.captures = @($record.captures) + @([pscustomobject]$entry)
    if (-not $NoControlRects) { $record.controlRects[$Name] = Get-VisibleControlRects }
    Add-Event ("抓图 {0}（{1}）：{2}x{3}，颜色 {4}，主色占比 {5:P1}，方法 {6}，PNG {7}" -f `
            $Name, $Page, $stats.Width, $stats.Height, $stats.Colors, $stats.ModalShare, $entry.captureMethod,
            $(if ($pngOk) { 'OK' } else { '失败' }))
    # 注意：这里**不要** return 这个有序字典 —— OrderedDictionary 是可枚举的，返回值会被
    # PowerShell 拆成一堆 DictionaryEntry。调用方一律从 $record.captures 取。
}

# ==================================================================
# 主流程
# ==================================================================

$record.appExe = $AppExe
if (-not $record.appExe)
{
    $releaseExe = Join-Path (Split-Path -Parent (Split-Path -Parent $root)) 'Release\YuaExtract.exe'
    if (-not $SkipSnapshotSync)
    {
        try
        {
            $snapshot = Sync-ReleaseSnapshot -RepoRoot (Split-Path -Parent (Split-Path -Parent $root)) -RunRoot $runRoot
            $record.snapshotCopied = $snapshot.Copied
            $record.appExe = $snapshot.Exe
            Add-Event ("Release 快照：{0}（重新复制={1}）" -f $snapshot.Exe, $snapshot.Copied)
        }
        catch
        {
            Save-And-Exit ("同步 Release 快照失败：{0}" -f $_.Exception.Message) 2
        }
    }
    else
    {
        $record.appExe = Join-Path $runRoot 'App\YuaExtract.exe'
    }
}
if (-not (Test-Path -LiteralPath $record.appExe)) { Save-And-Exit ("找不到被测程序：{0}（先编译 Release|x86，或不要用 -SkipSnapshotSync）" -f $record.appExe) 2 }
$record.appExeSha256 = Get-Sha256Hex -Path $record.appExe
$record.appExeBytes = (Get-Item -LiteralPath $record.appExe).Length
# 产品指纹（README 一直用"exe 前 16 位 sha256"标定构建）
$record.appFingerprint = $record.appExeSha256.Substring(0, 16)
Add-Event ("被测构建：{0} 字节，指纹 {1}" -f $record.appExeBytes, $record.appFingerprint)

if ($TimeoutSec -gt 0) { $record.noteTimeoutSec = $TimeoutSec }

$fatal = ''
try
{
    # ---------- 进程 1：基线 + 状态矩阵（同一进程）----------
    Start-AppProcess '进程1：状态矩阵'
    Save-Capture -Name 'home-startup' -Page 'home' -History 'startup' -ActiveTabId ([int]$script:UiIds.TabHome)

    # 基线：新启动 → **第一次点击**就是「制作扩展集」（任务要求的基线口径）
    Click-Control ([int]$script:UiIds.TabMakeSet) '制作扩展集（基线：新启动后第一次切换）'
    Save-Capture -Name 'make-baseline' -Page 'make' -History 'startup→make' -ActiveTabId ([int]$script:UiIds.TabMakeSet)
    # 同状态连拍 → 噪声地板
    Start-Sleep -Milliseconds 400
    Save-Capture -Name 'make-baseline-noise' -Page 'make' -History 'startup→make（连拍）' -ActiveTabId ([int]$script:UiIds.TabMakeSet)

    # 对照 1：首页 → 制作扩展集
    Click-Control ([int]$script:UiIds.TabHome) '首页'
    Save-Capture -Name 'home-visit1' -Page 'home' -History 'startup→make→home' -ActiveTabId ([int]$script:UiIds.TabHome)
    Click-Control ([int]$script:UiIds.TabMakeSet) '制作扩展集（首页→制作）'
    Save-Capture -Name 'make-after-home' -Page 'make' -History 'home→make' -ActiveTabId ([int]$script:UiIds.TabMakeSet)

    # 对照 2：扩展集 → 制作扩展集
    Click-Control ([int]$script:UiIds.TabSet) '扩展集'
    Save-Capture -Name 'set-visit1' -Page 'set' -History '…→make→set' -ActiveTabId ([int]$script:UiIds.TabSet)
    Click-Control ([int]$script:UiIds.TabMakeSet) '制作扩展集（扩展集→制作）'
    Save-Capture -Name 'make-after-set' -Page 'make' -History 'set→make' -ActiveTabId ([int]$script:UiIds.TabMakeSet)

    # 对照 3：日志展开 → 收起 → 再切
    Click-Control ([int]$script:UiIds.TabHome) '首页（准备日志用例）'
    Click-Control ([int]$script:UiIds.LogButton) '显示详细日志（展开）'
    Save-Capture -Name 'log-open' -Page 'home' -History 'home+日志展开' -ActiveTabId ([int]$script:UiIds.TabHome)
    Click-Control ([int]$script:UiIds.LogButton) '隐藏详细日志（收起）'
    Save-Capture -Name 'log-closed' -Page 'home' -History 'home+日志收起' -ActiveTabId ([int]$script:UiIds.TabHome)
    Click-Control ([int]$script:UiIds.TabMakeSet) '制作扩展集（日志展开收起→制作）'
    Save-Capture -Name 'make-after-log' -Page 'make' -History 'home+日志展开收起→make' -ActiveTabId ([int]$script:UiIds.TabMakeSet)

    # 对照 4：同进程内来回后回首页 —— 首页也必须与启动时一致（幽灵是双向的）
    Click-Control ([int]$script:UiIds.TabHome) '首页（回看历史无关性）'
    Save-Capture -Name 'home-visit2' -Page 'home' -History '…→make→home（第二次）' -ActiveTabId ([int]$script:UiIds.TabHome)

    Stop-AppProcess

    # ---------- 进程 2：跨进程复现（新启动 → 直接点制作扩展集）----------
    if (-not $NoCrossProcess)
    {
        Start-AppProcess '进程2：跨进程复现'
        Click-Control ([int]$script:UiIds.TabMakeSet) '制作扩展集（新进程重跑基线）'
        Save-Capture -Name 'make-baseline-process2' -Page 'make' -History 'startup→make（另一个进程）' -ActiveTabId ([int]$script:UiIds.TabMakeSet)
        Stop-AppProcess
    }

    # ---------- 进程 3：「扩展集」页签的状态矩阵（E-12-a）----------
    #
    # 为什么另起一个进程：基线口径必须是"**新启动后第一次点击**就是扩展集页签"。
    # 进程 1 里已经切过好几次页签了，在它里面再抓一张"基线"是自欺欺人
    # （那正是 E-11 里"基线 = startup→make"的同一个理由）。
    # 参照图（首页 / 制作页）用进程 1 抓的那两张：跨进程比对在本机实测是 0 px
    # （E-11-d：make-baseline vs make-baseline-process2），所以拿它们当正对照是稳的。
    if (-not $NoSetTab)
    {
        Start-AppProcess '进程3：扩展集页签状态矩阵'
        Click-Control ([int]$script:UiIds.TabSet) '扩展集（基线：新启动后第一次切换）'
        Save-Capture -Name 'set-baseline' -Page 'set' -History 'startup→set' -ActiveTabId ([int]$script:UiIds.TabSet)
        Start-Sleep -Milliseconds 400
        Save-Capture -Name 'set-baseline-noise' -Page 'set' -History 'startup→set（连拍）' -ActiveTabId ([int]$script:UiIds.TabSet)

        Click-Control ([int]$script:UiIds.TabHome) '首页'
        Click-Control ([int]$script:UiIds.TabSet) '扩展集（首页→扩展集）'
        Save-Capture -Name 'set-after-home' -Page 'set' -History 'home→set' -ActiveTabId ([int]$script:UiIds.TabSet)

        Click-Control ([int]$script:UiIds.TabMakeSet) '制作扩展集'
        Click-Control ([int]$script:UiIds.TabSet) '扩展集（制作→扩展集）'
        Save-Capture -Name 'set-after-make' -Page 'set' -History 'make→set' -ActiveTabId ([int]$script:UiIds.TabSet)

        Click-Control ([int]$script:UiIds.TabHome) '首页（准备日志用例）'
        Click-Control ([int]$script:UiIds.LogButton) '显示详细日志（展开）'
        Click-Control ([int]$script:UiIds.LogButton) '隐藏详细日志（收起）'
        Click-Control ([int]$script:UiIds.TabSet) '扩展集（日志展开收起→扩展集）'
        Save-Capture -Name 'set-after-log' -Page 'set' -History 'home+日志展开收起→set' -ActiveTabId ([int]$script:UiIds.TabSet)

        Stop-AppProcess
    }
}
catch
{
    $fatal = $_.Exception.Message
}

if ($fatal) { Save-And-Exit $fatal 2 }

$record.failure = $null
Save-And-Exit '' 0
