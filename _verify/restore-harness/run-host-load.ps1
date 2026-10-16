# 用例运行器（断言C / 断言E 的原料）：用空宿主进程加载 CxdecStringDumper.dll，观察它
# 创建了哪些窗口、写了哪些日志 —— 分别跑「只收集」与「对照组」两种环境。
#
#   pwsh -File run-host-load.ps1 -Mode collect-only   # CXDEC_HASH_COLLECT_ONLY=1
#   pwsh -File run-host-load.ps1 -Mode control        # 不设该变量（上游行为）
#
# 产出：Run\out\host-<mode>.json（断言脚本 assert-collect-only.ps1 读它）
#       Run\out\host-<mode>-<窗口类>.bmp/.png（窗口自绘截图，非交互桌面也能拿到内容）
#
# 关于环境变量（断言E 的关键）：**在父进程里设好，再启动宿主让它继承**，
# 刻意不用 .NET 的 ProcessStartInfo.Environment 逐项拼环境块 ——
# 因为 Loader 的机制是 SetEnvironmentVariableW + CreateProcessW(lpEnvironment = NULL)，
# 也就是"子进程继承父进程环境块"。这里是它的同构复现：
#   父进程设 CXDEC_HASH_COLLECT_ONLY / CXDEC_RUNTIME_HASH_TARGET_DIR → 启动 EmptyHost.exe
#   → EmptyHost 里 LoadLibrary(CxdecStringDumper.dll) → DLL 读到（或读不到）该开关。
#
# 只收集模式下还会顺手做几件"必须靠界面/文件系统才能确认"的事：
#   1) 点「清空目录文件」→ 确认框默认按钮是不是「否」→ 点是/否分别会发生什么；
#   2) 记录控制台控件矩形（布局自检）与截图；
#   3) 记录两条进度条的分母（来自对目标目录的扫描）；
#   4) 纯 Hash 目录（CXDEC_RUNTIME_HASH_TARGET_DIR）里放 canary，
#      前后比对文件名/内容 —— 只收集模式绝不改名。

[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('collect-only', 'control')][string]$Mode,
    [string]$DllPath = '',
    [int]$ObserveMs = 7000,
    [int]$LifetimeMs = 90000,
    # 数组一律用逗号分隔的字符串传：pwsh -File 下传真数组时，
    # 多余元素会被当成下一个位置参数（实测把第二个类名绑到了 $DllPath 上）。
    [string]$ForbiddenWindowClassesCsv = 'CxdecRuntimeHashWindow,CxdecHashRestoreWindow',
    [string]$ForbiddenTitlePattern = 'Cxdec Hash映射恢复|运行时Hash映射恢复|运行时还原|Hash还原',
    [string]$ConsoleWindowClass = 'CxdecHashCollectConsole',
    [string]$ConsoleWindowTitle = 'YuaExtract 动态 Hash 收集控制台',
    [int]$ClearButtonId = 4003,
    [string]$ClearButtonText = '清空目录文件',
    [switch]$SkipClearButtonCheck,
    [switch]$SkipEnvInheritance,
    [string]$SystemNoiseClassesCsv = 'IME,MSCTFIME UI,Default IME,tooltips_class32,SysShadow',
    [string]$OutJson = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessNative.ps1')

$ForbiddenWindowClasses = @($ForbiddenWindowClassesCsv -split ',' | Where-Object { $_ })
$SystemNoiseClasses = @($SystemNoiseClassesCsv -split ',' | Where-Object { $_ })

$hostDir = Join-Path $root 'Run\Host'
$outDir = Join-Path $root 'Run\out'
$hostExe = Join-Path $hostDir 'EmptyHost.exe'
$statusPath = Join-Path $hostDir 'host-status.txt'
$outputFixtureDir = Join-Path $hostDir 'StringHashDumper_Output'
# 纯 Hash 目录：Loader 也是把 CXDEC_RUNTIME_HASH_TARGET_DIR 指到 <游戏目录>\Extractor_Output。
$pureHashDir = Join-Path $hostDir 'Extractor_Output'
if (-not $OutJson) { $OutJson = Join-Path $outDir ('host-{0}.json' -f $Mode) }

foreach ($directory in @($hostDir, $outDir))
{
    if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
}

$events = New-Object System.Collections.ArrayList
function Add-Event([string]$Text)
{
    if ($events.Count -lt 2000) { [void]$events.Add(("[{0:HH:mm:ss.fff}] {1}" -f (Get-Date), $Text)) }
}

function New-FailureRecord
{
    param([string]$Reason)
    $record = [ordered]@{
        case               = "host-$Mode"
        mode               = $Mode
        startedUtc         = (Get-Date).ToUniversalTime().ToString('o')
        finishedUtc        = (Get-Date).ToUniversalTime().ToString('o')
        failure            = $Reason
        hostExe            = $hostExe
        dll                = ''
        loadOk             = $false
        loadStatus         = [ordered]@{}
        windows            = @()
        forbiddenWindows   = @()
        console            = $null
        screenshot         = $null
        clearButton        = $null
        envInheritance     = $null
        progress           = $null
        pureHash           = $null
        runtimeLogs        = @()
        runtimeLogProduced = $false
        fallbackLogTouched = $false
        events             = $events
    }
    Write-JsonFile -Path $OutJson -Object $record
    Write-Host "[host-$Mode] 失败：$Reason"
    exit 2
}

# ---------- 前置：宿主 EXE 与 DLL ----------

if (-not (Test-Path -LiteralPath $hostExe))
{
    Write-Host '[host] 还没构建空宿主，先构建…'
    & pwsh -NoProfile -File (Join-Path $root 'build-harness.ps1')
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $hostExe)) { New-FailureRecord 'EmptyHost.exe 构建失败' }
}

$dllCandidates = @()
if (-not $DllPath)
{
    # 与步骤3 用例共用同一份 Release 快照（Run\App\），保证两个断言测的是同一个版本。
    $snapshotDll = Join-Path $root 'Run\App\CxdecExtractordll\CxdecStringDumper.dll'
    if (-not (Test-Path -LiteralPath $snapshotDll))
    {
        try { [void](Sync-ReleaseSnapshot -RepoRoot ([System.IO.Path]::GetFullPath((Join-Path $root '..\..'))) -RunRoot (Join-Path $root 'Run')) }
        catch { Add-Event ("刷新 Release 快照失败：{0}" -f $_.Exception.Message) }
    }
    $dllCandidates = @(
        $snapshotDll,
        (Join-Path $root '..\..\Release\CxdecExtractordll\CxdecStringDumper.dll'),
        (Join-Path $root '..\..\Release\CxdecStringDumper.dll')
    )
    foreach ($candidate in $dllCandidates)
    {
        $resolved = [System.IO.Path]::GetFullPath($candidate)
        if (Test-Path -LiteralPath $resolved) { $DllPath = $resolved; break }
    }
}
elseif (Test-Path -LiteralPath $DllPath)
{
    $DllPath = [System.IO.Path]::GetFullPath($DllPath)
}

if (-not $DllPath -or -not (Test-Path -LiteralPath $DllPath))
{
    $tried = if ($dllCandidates.Count -gt 0) { ($dllCandidates | ForEach-Object { [System.IO.Path]::GetFullPath($_) }) -join ' ; ' } else { $DllPath }
    New-FailureRecord ("找不到 CxdecStringDumper.dll。找过这些位置：{0}（可能正在编译：链接阶段会先把旧文件删掉）" -f $tried)
}

$dllMachine = Get-PeMachine -Path $DllPath
$hostMachine = Get-PeMachine -Path $hostExe
if ($dllMachine -ne $hostMachine)
{
    New-FailureRecord ("位数不匹配：DLL 是 {0}，宿主是 {1}" -f (Format-PeMachine $dllMachine), (Format-PeMachine $hostMachine))
}

# 把 DLL 复制到宿主旁边：DLL 是拿"自己所在目录"算日志/输出目录的，
# 放在 Run\Host\ 下才能把它的产物全部框在脚手架里。
$localDll = Join-Path $hostDir 'CxdecStringDumper.dll'
Copy-Item -LiteralPath $DllPath -Destination $localDll -Force
$dllSha = (Get-Sha256Hex -Path $localDll)

# ---------- 清场：上一次运行的产物不能留在观测窗口里 ----------

Remove-Item -LiteralPath $statusPath -Force -ErrorAction SilentlyContinue
foreach ($produced in @('Log', 'StringHashDumper_Output', 'Extractor_Output'))
{
    $path = Join-Path $hostDir $produced
    [void](Remove-TreeUnder -Path $path -MustBeUnder (Join-Path $root 'Run'))
}
$fallbackLog = Join-Path $env:LOCALAPPDATA 'KrkrExtract\Log\RuntimeHashRestore.log'
$fallbackBefore = if (Test-Path -LiteralPath $fallbackLog) { (Get-Item -LiteralPath $fallbackLog).LastWriteTimeUtc } else { $null }

# ---------- canary 1：纯 Hash 目录（只收集模式不许改这里的文件名） ----------

$canaryDirs = @('1111111111111111', '2222222222222222')
$canaryFiles = @(
    @{ Rel = "canarypkg\$($canaryDirs[0])\AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"; Seed = 7001; Size = 1024 }
    @{ Rel = "canarypkg\$($canaryDirs[0])\BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"; Seed = 7002; Size = 2048 }
    @{ Rel = "canarypkg\$($canaryDirs[1])\CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"; Seed = 7003; Size = 333 }
)
foreach ($item in $canaryFiles)
{
    $full = Join-Path $pureHashDir $item.Rel
    New-Item -ItemType Directory -Path (Split-Path -Parent $full) -Force | Out-Null
    [System.IO.File]::WriteAllBytes($full, (New-DeterministicBytes -Seed $item.Seed -Length $item.Size))
}
Write-Utf8File -Path (Join-Path $pureHashDir 'canarypkg\readme.txt') -Text 'canary：这个文件是明文名，任何模式下都不该被改名。' -NoNewline
$pureHashBefore = Get-DirectorySnapshot -Path $pureHashDir -WithHash

# ---------- canary 2：映射产物目录（验证「清空目录文件」） ----------

New-Item -ItemType Directory -Path $outputFixtureDir -Force | Out-Null
[System.IO.File]::WriteAllBytes((Join-Path $outputFixtureDir 'canary-1.bin'), (New-DeterministicBytes -Seed 9001 -Length 4096))
[System.IO.File]::WriteAllBytes((Join-Path $outputFixtureDir 'canary-2.bin'), (New-DeterministicBytes -Seed 9002 -Length 64))
Write-Utf8File -Path (Join-Path $outputFixtureDir 'canary-3.txt') -Text 'canary：如果你看到我还在这里，说明"清空目录文件"没删干净。' -NoNewline
$canaryNames = @('canary-1.bin', 'canary-2.bin', 'canary-3.txt')

function Get-FixtureListing
{
    $items = @()
    if (Test-Path -LiteralPath $outputFixtureDir)
    {
        $items = @(Get-ChildItem -LiteralPath $outputFixtureDir -File -Force -ErrorAction SilentlyContinue |
            Sort-Object Name | ForEach-Object { $_.Name })
    }
    return ,$items
}
$canaryBefore = Get-FixtureListing

# ---------- 启动（父进程设环境变量 → 子进程继承） ----------

$startedAt = Get-Date
$envRecord = [ordered]@{
    mechanism             = '父进程 SetEnvironmentVariable 等价的 $env: 赋值 → CreateProcessW 继承（不用 ProcessStartInfo.Environment 逐项覆盖）'
    collectOnlySetInParent = $false
    targetDirSetInParent   = $false
    targetDir              = $pureHashDir
    hostEnvironmentOverrides = 0
}

Write-Host ("[host-{0}] 启动 {1}" -f $Mode, $hostExe)
Write-Host ("[host-{0}] DLL {1} ({2})" -f $Mode, $localDll, (Format-PeMachine $dllMachine))

$savedCollect = $env:CXDEC_HASH_COLLECT_ONLY
$savedTarget = $env:CXDEC_RUNTIME_HASH_TARGET_DIR
try
{
    if ($SkipEnvInheritance)
    {
        Remove-Item env:CXDEC_HASH_COLLECT_ONLY -ErrorAction SilentlyContinue
        Remove-Item env:CXDEC_RUNTIME_HASH_TARGET_DIR -ErrorAction SilentlyContinue
        Add-Event '（-SkipEnvInheritance）不设任何环境变量'
    }
    else
    {
        $env:CXDEC_RUNTIME_HASH_TARGET_DIR = $pureHashDir
        $envRecord.targetDirSetInParent = $true
        if ($Mode -eq 'collect-only')
        {
            $env:CXDEC_HASH_COLLECT_ONLY = '1'
            $envRecord.collectOnlySetInParent = $true
        }
        else
        {
            Remove-Item env:CXDEC_HASH_COLLECT_ONLY -ErrorAction SilentlyContinue
        }
        Add-Event ("父进程环境：CXDEC_HASH_COLLECT_ONLY={0} CXDEC_RUNTIME_HASH_TARGET_DIR={1}" -f `
                $(if ($Mode -eq 'collect-only') { '1' } else { '<未设置>' }), $pureHashDir)
    }

    Write-Host ("[host-{0}] 父进程环境：CXDEC_HASH_COLLECT_ONLY={1}" -f $Mode, $(if ($Mode -eq 'collect-only') { '1' } else { '<未设置>' }))

    try
    {
        # 刻意不传 -Environment：让子进程直接继承父进程环境块（等价 lpEnvironment = NULL）。
        $process = Start-HarnessProcess -FilePath $hostExe -WorkingDirectory $hostDir `
            -Arguments @('--dll', $localDll, '--status', $statusPath, '--lifetime', "$LifetimeMs")
    }
    catch
    {
        New-FailureRecord ("启动空宿主失败：{0}（文件被占用/正在编译时会出现这种错）" -f $_.Exception.Message)
    }
}
finally
{
    # 子进程创建时已经把环境块复制走了，这里立刻把父进程复原，别把开关漏给后面的用例。
    if ($null -eq $savedCollect) { Remove-Item env:CXDEC_HASH_COLLECT_ONLY -ErrorAction SilentlyContinue }
    else { $env:CXDEC_HASH_COLLECT_ONLY = $savedCollect }
    if ($null -eq $savedTarget) { Remove-Item env:CXDEC_RUNTIME_HASH_TARGET_DIR -ErrorAction SilentlyContinue }
    else { $env:CXDEC_RUNTIME_HASH_TARGET_DIR = $savedTarget }
}

$pid32 = $process.Id
Add-Event ("宿主 pid={0}" -f $pid32)

$status = $null
$seenWindows = [ordered]@{}
$console = $null
$deadline = (Get-Date).AddMilliseconds($ObserveMs)

function Update-Windows
{
    $windows = @()
    try { $windows = [Harness.Win]::TopLevel($pid32) } catch { }
    foreach ($window in $windows)
    {
        if ($SystemNoiseClasses -contains $window.Class) { continue }   # 输入法/阴影一类的系统窗口，不算 DLL 产物
        $key = '{0}|{1}' -f $window.Class, $window.HwndHex
        if (-not $seenWindows.Contains($key))
        {
            $seenWindows[$key] = [pscustomobject]@{
                HwndHex = $window.HwndHex
                Hwnd    = $window.Hwnd
                Class   = $window.Class
                Title   = $window.Text
                Visible = $window.Visible
            }
            Add-Event ("窗口出现：class='{0}' title='{1}' visible={2}" -f $window.Class, $window.Text, $window.Visible)
        }
    }
    return $windows
}

while ((Get-Date) -lt $deadline)
{
    if (-not $status -and (Test-Path -LiteralPath $statusPath))
    {
        Start-Sleep -Milliseconds 120
        $status = Read-TextShared -Path $statusPath
        Add-Event ("宿主状态文件：{0}" -f ($status -replace '\s+', ' '))
    }

    $windows = Update-Windows
    $candidate = $windows | Where-Object { $_.Class -eq $ConsoleWindowClass -or $_.Text -like ('*' + $ConsoleWindowTitle + '*') } | Select-Object -First 1
    if ($candidate) { $console = $candidate; break }

    $runtimeLogs = @(Get-ChildItem -LiteralPath $hostDir -Recurse -File -Filter 'RuntimeHashRestore.log' -ErrorAction SilentlyContinue)
    if ($runtimeLogs.Count -gt 0 -and $Mode -eq 'control') { Start-Sleep -Milliseconds 600; break }   # 对照组看到日志就可以收工
    Start-Sleep -Milliseconds 220
}

if ($console) { Add-Event ("收集控制台窗口：class='{0}' title='{1}' hwnd={2}" -f $console.Class, $console.Text, $console.HwndHex) }

# ---------- 控制台：布局 + 截图 + 进度分母 + 「清空目录文件」 ----------

$consoleInfo = $null
$screenshot = $null
$clearResult = $null
$progressInfo = $null

if ($console)
{
    $consoleHwnd = $console.Hwnd
    $children = @([Harness.Win]::Children($consoleHwnd))
    $consoleRect = [Harness.Win]::WindowRect($consoleHwnd)
    $clientRect = [Harness.Win]::ClientRectOnScreen($consoleHwnd)

    $controls = @()
    foreach ($child in $children)
    {
        $rect = [Harness.Win]::WindowRect($child.Hwnd)
        $tabCount = $null
        if ($child.Class -eq 'SysTabControl32') { $tabCount = [Harness.Win]::TabItemCount($child.Hwnd) }
        $controls += [pscustomobject]@{
            CtrlId   = $child.CtrlId
            Class    = $child.Class
            Text     = $(if ($child.Text -and $child.Text.Length -gt 60) { $child.Text.Substring(0, 60) + '…' } else { $child.Text })
            Visible  = $child.Visible
            Enabled  = $child.Enabled
            Rect     = $rect
            TabCount = $tabCount
        }
        if ($child.Class -eq 'SysTabControl32') { Add-Event ("页签控件 id={0} 条目数={1}" -f $child.CtrlId, $tabCount) }
    }

    $outOfBounds = @()
    if ($clientRect)
    {
        foreach ($control in $controls)
        {
            if (-not $control.Visible -or -not $control.Rect) { continue }
            $rect = $control.Rect
            if ($rect[0] -lt ($clientRect[0] - 1) -or $rect[1] -lt ($clientRect[1] - 1) -or
                $rect[2] -gt ($clientRect[2] + 1) -or $rect[3] -gt ($clientRect[3] + 1))
            {
                $outOfBounds += [pscustomobject]@{ CtrlId = $control.CtrlId; Class = $control.Class; Text = $control.Text; Rect = $rect }
            }
        }
        Add-Event ("控制台客户区 {0}；可见控件 {1} 个，越界 {2} 个" -f ($clientRect -join ','), @($controls | Where-Object { $_.Visible }).Count, $outOfBounds.Count)
    }

    $consoleInfo = [ordered]@{
        HwndHex     = $console.HwndHex
        Class       = $console.Class
        Title       = $console.Text
        WindowRect  = $consoleRect
        ClientRect  = $clientRect
        Controls    = $controls
        OutOfBounds = $outOfBounds
    }

    # 进度条分母：4007 DirectoryHash、4009 FileNameHash、4014 状态行。
    function Get-ControlText([int]$id)
    {
        $found = $controls | Where-Object { $_.CtrlId -eq $id } | Select-Object -First 1
        if ($found) { return [string]$found.Text }
        return ''
    }
    $expectedDirHashes = @($canaryFiles | ForEach-Object { ($_.Rel -split '\\')[1] } | Select-Object -Unique).Count
    $expectedFileHashes = @($canaryFiles | ForEach-Object { ($_.Rel -split '\\')[2] } | Select-Object -Unique).Count
    $progressInfo = [ordered]@{
        directoryHashText     = (Get-ControlText 4007)
        fileNameHashText      = (Get-ControlText 4009)
        restoreRateText       = (Get-ControlText 4010)
        statusLineText        = (Get-ControlText 4014)
        targetDir             = $pureHashDir
        expectedDirHashCount  = $expectedDirHashes
        expectedFileHashCount = $expectedFileHashes
    }
    Add-Event ("进度文字：DirectoryHash='{0}' FileNameHash='{1}' 状态行='{2}'" -f `
            $progressInfo.directoryHashText, $progressInfo.fileNameHashText, $progressInfo.statusLineText)

    $bmpPath = Join-Path $outDir ('host-{0}-{1}.bmp' -f $Mode, ($console.Class -replace '[^A-Za-z0-9]', ''))
    $captureError = [Harness.Win]::CaptureWindowToBmp($consoleHwnd, $bmpPath)
    $pngPath = [System.IO.Path]::ChangeExtension($bmpPath, '.png')
    $pngOk = $false
    if (-not $captureError) { $pngOk = Convert-BmpToPng -BmpPath $bmpPath -PngPath $pngPath }
    $screenshot = [ordered]@{
        Bmp        = $bmpPath
        Png        = $(if ($pngOk) { $pngPath } else { '' })
        Error      = $captureError
        CapturedAt = (Get-Date).ToUniversalTime().ToString('o')
    }
    Add-Event ("截图：{0}" -f $(if ($captureError) { '失败 - ' + $captureError } else { $bmpPath }))

    if (-not $SkipClearButtonCheck)
    {
        $clearButton = [Harness.Win]::GetDlgItem([IntPtr]$consoleHwnd, $ClearButtonId)
        if ($clearButton -eq [IntPtr]::Zero)
        {
            $match = $children | Where-Object { $_.Class -eq 'Button' -and $_.Text -and $_.Text.Contains($ClearButtonText) } | Select-Object -First 1
            if ($match) { $clearButton = [IntPtr]$match.Hwnd }
        }

        $clearResult = [ordered]@{
            ButtonFound      = ($clearButton -ne [IntPtr]::Zero)
            ButtonId         = $ClearButtonId
            DialogAppeared   = $false
            DialogTitle      = ''
            DialogText       = ''
            DefaultButtonId  = -1
            DefaultIsNo      = $false
            Buttons          = @()
            BeforeListing    = $canaryBefore
            AfterDecline     = @()
            DeclineKeptAll   = $false
            AfterAccept      = @()
            AcceptRemovedAny = $false
            Notes            = @()
        }

        if ($clearButton -eq [IntPtr]::Zero)
        {
            $clearResult.Notes += ("找不到「{0}」（控件 ID {1}，也按文字找过了）" -f $ClearButtonText, $ClearButtonId)
            Add-Event '找不到「清空目录文件」按钮'
        }
        else
        {
            function Wait-Dialog([int]$timeoutMs)
            {
                $until = (Get-Date).AddMilliseconds($timeoutMs)
                while ((Get-Date) -lt $until)
                {
                    $dialog = [Harness.Win]::TopLevel($pid32) | Where-Object { $_.Class -eq '#32770' } | Select-Object -First 1
                    if ($dialog) { return $dialog }
                    Start-Sleep -Milliseconds 150
                }
                return $null
            }

            [void][Harness.Win]::PostClick($clearButton.ToInt64())
            $dialog = Wait-Dialog 5000
            if (-not $dialog)
            {
                $clearResult.Notes += '点了「清空目录文件」但没等到确认框'
                Add-Event '点了「清空目录文件」但没等到确认框'
            }
            else
            {
                $clearResult.DialogAppeared = $true
                $clearResult.DialogTitle = $dialog.Text
                $clearResult.DefaultButtonId = [Harness.Win]::DialogDefaultButtonId($dialog.Hwnd)
                $clearResult.DefaultIsNo = ($clearResult.DefaultButtonId -eq 7)      # IDNO
                $dialogChildren = @([Harness.Win]::Children($dialog.Hwnd))
                $clearResult.Buttons = @($dialogChildren | Where-Object { $_.Class -eq 'Button' } | ForEach-Object { "{0}#{1}" -f $_.Text, $_.CtrlId })
                $clearResult.DialogText = (@($dialogChildren | Where-Object { $_.Class -eq 'Static' -and $_.Text } | ForEach-Object { $_.Text }) -join ' | ')
                Add-Event ("确认框：title='{0}' 按钮={1} 默认按钮ID={2}" -f $clearResult.DialogTitle, ($clearResult.Buttons -join ','), $clearResult.DefaultButtonId)

                $no = [Harness.Win]::GetDlgItem($dialog.Hwnd, 7)
                if ($no -eq [IntPtr]::Zero)
                {
                    $byText = $dialogChildren | Where-Object { $_.Class -eq 'Button' -and $_.Text -in @('否', 'No') } | Select-Object -First 1
                    if ($byText) { $no = [IntPtr]$byText.Hwnd }
                }
                if ($no -eq [IntPtr]::Zero)
                {
                    $clearResult.Notes += '确认框上找不到「否」按钮，只能关掉它'
                    [void][Harness.Win]::PostClose($dialog.Hwnd)
                }
                else
                {
                    [void][Harness.Win]::PostClick($no.ToInt64())
                }
                Start-Sleep -Milliseconds 800
                $clearResult.AfterDecline = Get-FixtureListing
                $missingAfterDecline = @($canaryBefore | Where-Object { $_ -notin $clearResult.AfterDecline })
                $clearResult.DeclineKeptAll = ($missingAfterDecline.Count -eq 0)
                if (-not $clearResult.DeclineKeptAll) { $clearResult.Notes += ('选「否」之后仍然少了：' + ($missingAfterDecline -join ', ')) }
                Add-Event ("选否之后目录里还有：{0}" -f ($clearResult.AfterDecline -join ', '))

                [void][Harness.Win]::PostClick($clearButton.ToInt64())
                $dialog2 = Wait-Dialog 5000
                if ($dialog2)
                {
                    $yes = [Harness.Win]::GetDlgItem($dialog2.Hwnd, 6)
                    if ($yes -eq [IntPtr]::Zero)
                    {
                        $byText = @([Harness.Win]::Children($dialog2.Hwnd)) | Where-Object { $_.Class -eq 'Button' -and $_.Text -in @('是', 'Yes') } | Select-Object -First 1
                        if ($byText) { $yes = [IntPtr]$byText.Hwnd }
                    }
                    if ($yes -ne [IntPtr]::Zero) { [void][Harness.Win]::PostClick($yes.ToInt64()) }
                    else { $clearResult.Notes += '确认框上找不到「是」按钮'; [void][Harness.Win]::PostClose($dialog2.Hwnd) }
                    Start-Sleep -Milliseconds 900
                }
                else
                {
                    $clearResult.Notes += '第二次点「清空目录文件」没等到确认框'
                }
                $clearResult.AfterAccept = Get-FixtureListing
                $removed = @($canaryBefore | Where-Object { $_ -notin $clearResult.AfterAccept })
                $clearResult.AcceptRemovedAny = ($removed.Count -gt 0)
                Add-Event ("选是之后目录里还剩：{0}（删掉了 {1} 个）" -f ($clearResult.AfterAccept -join ', '), $removed.Count)
            }
        }
    }

    $deadline = (Get-Date).AddMilliseconds([Math]::Max(1500, $ObserveMs / 3))
}

while ((Get-Date) -lt $deadline)
{
    if (-not $status -and (Test-Path -LiteralPath $statusPath))
    {
        Start-Sleep -Milliseconds 120
        $status = Read-TextShared -Path $statusPath
        Add-Event ("宿主状态文件：{0}" -f ($status -replace '\s+', ' '))
    }
    [void](Update-Windows)
    Start-Sleep -Milliseconds 220
}

# ---------- 收尾 ----------

$exitedCleanly = $process.HasExited
[void](Stop-HarnessProcess -Process $process -GraceMs 2000)
$exitCode = $null
try { $exitCode = $process.ExitCode } catch { }

$pureHashAfter = Get-DirectorySnapshot -Path $pureHashDir -WithHash
$pureHashDiff = Compare-DirectorySnapshot -Before $pureHashBefore -After $pureHashAfter

$runtimeLogs = @(Get-ChildItem -LiteralPath $hostDir -Recurse -File -Filter 'RuntimeHashRestore.log' -ErrorAction SilentlyContinue |
    ForEach-Object { [pscustomobject]@{ Path = $_.FullName; Size = $_.Length; ModifiedUtc = $_.LastWriteTimeUtc } })

$fallbackAfter = if (Test-Path -LiteralPath $fallbackLog) { (Get-Item -LiteralPath $fallbackLog).LastWriteTimeUtc } else { $null }
$fallbackTouched = ($fallbackAfter -ne $null) -and (($fallbackBefore -eq $null) -or ($fallbackAfter -gt $fallbackBefore))

$windowList = @()
$forbidden = @()
foreach ($key in $seenWindows.Keys)
{
    $window = $seenWindows[$key]
    $windowList += $window
    $isForbidden = ($ForbiddenWindowClasses -contains $window.Class) -or ($window.Title -match $ForbiddenTitlePattern)
    if ($isForbidden) { $forbidden += $window }
}

$statusMap = [ordered]@{}
if ($status)
{
    foreach ($line in ($status -split "`r?`n"))
    {
        if ($line -match '^([^=]+)=(.*)$') { $statusMap[$matches[1]] = $matches[2] }
    }
}

$record = [ordered]@{
    case                 = "host-$Mode"
    mode                 = $Mode
    startedUtc           = $startedAt.ToUniversalTime().ToString('o')
    finishedUtc          = (Get-Date).ToUniversalTime().ToString('o')
    hostExe              = $hostExe
    hostMachine          = Format-PeMachine $hostMachine
    dll                  = $localDll
    dllSource            = $DllPath
    dllSha256            = $dllSha
    dllMachine           = Format-PeMachine $dllMachine
    collectOnlyEnv       = ($Mode -eq 'collect-only')
    envInheritance       = $envRecord
    pid                  = $pid32
    loadStatus           = $statusMap
    loadOk               = ($statusMap['load'] -eq 'ok')
    hostExitCode         = $exitCode
    hostExitedByItself   = $exitedCleanly
    observeMs            = $ObserveMs
    windows              = $windowList
    forbiddenWindows     = $forbidden
    forbiddenClasses     = $ForbiddenWindowClasses
    forbiddenTitleRegex  = $ForbiddenTitlePattern
    consoleWindowClass   = $ConsoleWindowClass
    consoleWindowTitle   = $ConsoleWindowTitle
    console              = $consoleInfo
    progress             = $progressInfo
    screenshot           = $screenshot
    clearButton          = $clearResult
    fixtureDir           = $outputFixtureDir
    canaryBefore         = $canaryBefore
    pureHashDir          = $pureHashDir
    pureHashBeforeCount  = $pureHashBefore.Count
    pureHashAfterCount   = $pureHashAfter.Count
    pureHashDiff         = [ordered]@{ Added = $pureHashDiff.Added; Removed = $pureHashDiff.Removed; Changed = $pureHashDiff.Changed; Same = $pureHashDiff.Same }
    runtimeLogs          = $runtimeLogs
    runtimeLogProduced   = ($runtimeLogs.Count -gt 0)
    fallbackLogPath      = $fallbackLog
    fallbackLogTouched   = $fallbackTouched
    fallbackLogModified  = if ($fallbackAfter) { $fallbackAfter.ToString('o') } else { $null }
    events               = $events
    failure              = $null
}

if (-not $record.loadOk)
{
    $record.failure = ('DLL 没装进宿主进程：load={0} error={1}（126 = 依赖缺失，193 = 位数不对）' -f $statusMap['load'], $statusMap['error'])
}

Write-JsonFile -Path $OutJson -Object $record

Write-Host ("[host-{0}] 窗口 {1} 个：" -f $Mode, $windowList.Count)
foreach ($window in $windowList) { Write-Host ("    class='{0}' title='{1}' visible={2}" -f $window.Class, $window.Title, $window.Visible) }
if ($consoleInfo)
{
    Write-Host ("[host-{0}] 控制台：{1}x{2}，可见控件 {3} 个，越界 {4} 个" -f `
            $Mode,
        ($consoleInfo.WindowRect[2] - $consoleInfo.WindowRect[0]),
        ($consoleInfo.WindowRect[3] - $consoleInfo.WindowRect[1]),
        @($consoleInfo.Controls | Where-Object { $_.Visible }).Count,
        @($consoleInfo.OutOfBounds).Count)
    foreach ($item in @($consoleInfo.OutOfBounds)) { Write-Host ("    越界：id={0} class={1} text='{2}' rect={3}" -f $item.CtrlId, $item.Class, $item.Text, ($item.Rect -join ',')) }
    Write-Host ("[host-{0}] 进度分母：DirectoryHash='{1}'（目标目录里 {2} 个去重目录 Hash） FileNameHash='{3}'（{4} 个去重文件 Hash）" -f `
            $Mode, $progressInfo.directoryHashText, $progressInfo.expectedDirHashCount, $progressInfo.fileNameHashText, $progressInfo.expectedFileHashCount)
}
else
{
    Write-Host ("[host-{0}] 没有出现收集控制台窗口（class={1}）" -f $Mode, $ConsoleWindowClass)
}
if ($screenshot) { Write-Host ("[host-{0}] 截图：{1}" -f $Mode, $(if ($screenshot.Error) { '失败 - ' + $screenshot.Error } else { $screenshot.Bmp + $(if ($screenshot.Png) { '  /  ' + $screenshot.Png } else { '' }) })) }
if ($clearResult)
{
    Write-Host ("[host-{0}] 清空目录文件：按钮={1} 确认框={2} 默认按钮ID={3}（{4}）" -f `
            $Mode, $clearResult.ButtonFound, $clearResult.DialogAppeared, $clearResult.DefaultButtonId, $(if ($clearResult.DefaultIsNo) { '否' } else { '不是否' }))
    Write-Host ("    选否之后剩余：{0}" -f ($clearResult.AfterDecline -join ', '))
    Write-Host ("    选是之后剩余：{0}" -f ($clearResult.AfterAccept -join ', '))
    foreach ($note in $clearResult.Notes) { Write-Host ("    备注：{0}" -f $note) }
}
Write-Host ("[host-{0}] 纯 Hash 目录：前后 {1} -> {2} 个文件；快照一致={3}" -f $Mode, $pureHashBefore.Count, $pureHashAfter.Count, $pureHashDiff.Same)
if (-not $pureHashDiff.Same)
{
    foreach ($item in @($pureHashDiff.Removed)) { Write-Host ("    消失：{0}" -f $item) }
    foreach ($item in @($pureHashDiff.Added)) { Write-Host ("    新增：{0}" -f $item) }
    foreach ($item in @($pureHashDiff.Changed)) { Write-Host ("    改动：{0}" -f $item) }
}
Write-Host ("[host-{0}] RuntimeHashRestore.log：{1}" -f $Mode, $(if ($record.runtimeLogProduced) { ($runtimeLogs | ForEach-Object { $_.Path }) -join '; ' } else { '<没有>' }))
if ($forbidden.Count -gt 0) { Write-Host ("[host-{0}] 命中'运行时还原窗口'判定：{1}" -f $Mode, (($forbidden | ForEach-Object { $_.Class }) -join ',')) }
Write-Host ("[host-{0}] 结果写入 {1}" -f $Mode, $OutJson)
if ($record.failure) { Write-Host ("[host-{0}] 失败：{1}" -f $Mode, $record.failure); exit 2 }
exit 0
