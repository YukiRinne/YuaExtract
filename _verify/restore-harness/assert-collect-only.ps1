# 断言 C（步骤2 只收集）：带 CXDEC_HASH_COLLECT_ONLY=1 加载 CxdecStringDumper.dll 时，
# 不出现"运行时还原窗口"，也不产生 RuntimeHashRestore.log；不带该变量的对照组
# 仍然按上游行为弹窗 + 写日志。
#
#   pwsh -File assert-collect-only.ps1          # 用已有的 Run\out\host-*.json 判定
#   pwsh -File assert-collect-only.ps1 -Run     # 先跑两个用例再判定
#
# 观测口径：
#   宿主进程（EmptyHost.exe）自己的窗口类是 YuaEmptyHostWindow，其余顶层窗口就是
#   DLL 建出来的。"运行时还原窗口"= class ∈ {CxdecRuntimeHashWindow, CxdecHashRestoreWindow}
#   或标题匹配 Cxdec Hash映射恢复 / 运行时还原 等；本轮新增的收集控制台是
#   class=CxdecHashCollectConsole（标题 YuaExtract 动态 Hash 收集控制台），
#   它是**期望出现**的，不算还原窗口。
#
# 另外两条只能靠界面确认的：
#   C-5 只收集模式下应当出现收集控制台窗口
#   C-6 「清空目录文件」的确认框默认按钮必须是「否」，且选「否」时一个文件都不该少
#   C-7 控制台控件不得越出客户区（布局自检，软检查）

[CmdletBinding()]
param(
    [switch]$Run,
    [int]$ObserveMs = 7000,
    # 数组用逗号分隔字符串传：pwsh -File 下传真数组时多余元素会被当成下一个位置参数。
    [string]$ForbiddenWindowClassesCsv = 'CxdecRuntimeHashWindow,CxdecHashRestoreWindow',
    [string]$ForbiddenTitlePattern = 'Cxdec Hash映射恢复|运行时Hash映射恢复|运行时还原|Hash还原',
    [string]$ConsoleWindowClass = 'CxdecHashCollectConsole',
    [string]$ConsoleWindowTitle = 'YuaExtract 动态 Hash 收集控制台',
    [string]$ChecksJson = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')

if (-not $ChecksJson) { $ChecksJson = Join-Path $root 'Run\out\checks-collect-only.json' }

$ForbiddenWindowClasses = @($ForbiddenWindowClassesCsv -split ',' | Where-Object { $_ })

if ($Run)
{
    foreach ($mode in @('collect-only', 'control'))
    {
        Write-Host ("[assert-C] 运行用例 host-{0}（观测 {1} ms）…" -f $mode, $ObserveMs)
        & pwsh -NoProfile -File (Join-Path $root 'run-host-load.ps1') -Mode $mode -ObserveMs $ObserveMs `
            -ForbiddenWindowClassesCsv $ForbiddenWindowClassesCsv -ForbiddenTitlePattern $ForbiddenTitlePattern `
            -ConsoleWindowClass $ConsoleWindowClass -ConsoleWindowTitle $ConsoleWindowTitle
    }
}

Reset-HarnessChecks

$collectJson = Join-Path $root 'Run\out\host-collect-only.json'
$controlJson = Join-Path $root 'Run\out\host-control.json'
$collect = Read-JsonFile -Path $collectJson
$control = Read-JsonFile -Path $controlJson

if (-not $collect -or -not $control)
{
    $missing = @()
    if (-not $collect) { $missing += $collectJson }
    if (-not $control) { $missing += $controlJson }
    Add-Check -Id 'C-0' -Title '两个宿主用例的结果都在' -Ok $false `
        -Details (@('缺少结果文件：') + ($missing | ForEach-Object { '    ' + $_ }) + @('请运行 run-host-load.ps1（或本脚本加 -Run）'))
    exit (Show-HarnessChecks -Title '断言 C（步骤2 只收集）' -ChecksJson $ChecksJson)
}

function Format-Windows($case)
{
    $windows = @(Get-ObjectValue -Object $case -Name 'windows' -Default @())
    if ($windows.Count -eq 0) { return @('    （没有出现任何顶层窗口）') }
    return @($windows | ForEach-Object { ("    class='{0}' title='{1}' visible={2}" -f $_.Class, $_.Title, $_.Visible) })
}

# 用例"能不能拿来判定"：DLL 装进去了、而且没有提前失败。
# 否则 C-1（没有还原窗口）会因为"压根没跑起来"而假通过。
$collectReason = Get-ObjectValue -Object $collect -Name 'failure'
$controlReason = Get-ObjectValue -Object $control -Name 'failure'
$collectUsable = (-not $collectReason) -and [bool](Get-ObjectValue -Object $collect -Name 'loadOk' -Default $false)
$controlUsable = (-not $controlReason) -and [bool](Get-ObjectValue -Object $control -Name 'loadOk' -Default $false)
$collectBlocked = if ($collectReason) { "只收集用例没跑起来：$collectReason" } else { '只收集用例没有把 DLL 装进宿主进程，无法判定' }
$controlBlocked = if ($controlReason) { "对照组用例没跑起来：$controlReason" } else { '对照组用例没有把 DLL 装进宿主进程，无法判定' }

foreach ($pair in @(@{ Name = 'collect-only'; Data = $collect }, @{ Name = 'control'; Data = $control }))
{
    $case = $pair.Data
    $label = $pair.Name
    $loadOk = [bool](Get-ObjectValue -Object $case -Name 'loadOk' -Default $false)
    $details = @()
    if (Get-ObjectValue -Object $case -Name 'failure') { $details += ('用例运行阶段就失败了：' + (Get-ObjectValue -Object $case -Name 'failure')) }
    if ($loadOk) { $details += ("DLL 已加载：{0}" -f $case.dll) }
    else
    {
        $status = Get-ObjectValue -Object $case -Name 'loadStatus' -Default $null
        $details += ("加载失败：load={0} error={1}" -f (Get-ObjectValue -Object $status -Name 'load' -Default '<未记录>'),
                                             (Get-ObjectValue -Object $status -Name 'error' -Default '<未记录>'))
    }
    $details += '宿主进程窗口：'
    $details += Format-Windows $case
    Add-Check -Id ("C-0/{0}" -f $label) -Title 'DLL 成功加载进 32 位空宿主' -Ok $loadOk -Details $details
}

# ---------- C-1：只收集时不应出现运行时还原窗口 ----------

$forbidden = @(Get-ObjectValue -Object $collect -Name 'forbiddenWindows' -Default @())
$c1Details = @()
if ($forbidden.Count -eq 0)
{
    $c1Details += '没有出现被判定为「运行时还原窗口」的窗口'
}
else
{
    $c1Details += ("出现 {0} 个运行时还原窗口：" -f $forbidden.Count)
    $c1Details += @($forbidden | ForEach-Object { ("    class='{0}' title='{1}'" -f $_.Class, $_.Title) })
}
$c1Details += ("判定规则：class ∈ [{0}]，标题匹配 /{1}/" -f ($ForbiddenWindowClasses -join ', '), $ForbiddenTitlePattern)
$c1Details += '（宿主进程实际出现的窗口：）'
$c1Details += Format-Windows $collect
if (-not $collectUsable) { $c1Details += $collectBlocked }
Add-Check -Id 'C-1' -Title 'CXDEC_HASH_COLLECT_ONLY=1 时不出现运行时还原窗口' -Ok ($collectUsable -and ($forbidden.Count -eq 0)) -Details $c1Details

# ---------- C-2：只收集时不应产生 RuntimeHashRestore.log ----------

$logProduced = [bool](Get-ObjectValue -Object $collect -Name 'runtimeLogProduced' -Default $false)
$fallbackTouched = [bool](Get-ObjectValue -Object $collect -Name 'fallbackLogTouched' -Default $false)
$c2Details = @()
if (-not $logProduced -and -not $fallbackTouched) { $c2Details += '收集模式下没有产生 RuntimeHashRestore.log（工具目录与 %LOCALAPPDATA% 兜底位置都没有）' }
if ($logProduced) { $c2Details += @('产生了运行时还原日志：') + @($collect.runtimeLogs | ForEach-Object { '    ' + $_.Path }) }
if ($fallbackTouched) { $c2Details += ('兜底日志位置被写入：' + $collect.fallbackLogPath) }
if (-not $collectUsable) { $c2Details += $collectBlocked }
Add-Check -Id 'C-2' -Title 'CXDEC_HASH_COLLECT_ONLY=1 时不产生 RuntimeHashRestore.log' -Ok ($collectUsable -and (-not $logProduced) -and (-not $fallbackTouched)) -Details $c2Details

# ---------- C-3：对照组仍与上游一致（弹窗 + 写日志） ----------

$dllWindows = @(@(Get-ObjectValue -Object $control -Name 'windows' -Default @()) | Where-Object { $_.Class -ne 'YuaEmptyHostWindow' })
$controlLog = [bool](Get-ObjectValue -Object $control -Name 'runtimeLogProduced' -Default $false)
$c3Details = @(("对照组 DLL 顶层窗口 {0} 个；RuntimeHashRestore.log：{1}" -f $dllWindows.Count, $(if ($controlLog) { '有' } else { '没有' })))
$c3Details += Format-Windows $control
if (-not $controlLog) { $c3Details += '对照组没有产生 RuntimeHashRestore.log —— 与上游行为不一致（上游加载即写）' }
if ($dllWindows.Count -eq 0) { $c3Details += '对照组 DLL 没有创建任何窗口 —— 与上游行为不一致（上游加载即建窗）' }
if (-not $controlUsable) { $c3Details += $controlBlocked }
Add-Check -Id 'C-3' -Title '不设该变量时行为与上游一致（弹窗 + 写 RuntimeHashRestore.log）' -Ok ($controlUsable -and $dllWindows.Count -gt 0 -and $controlLog) -Details $c3Details

# ---------- C-4（提示）：只收集模式到底弹了什么窗口 ----------

Add-Check -Id 'C-4' -Title '收集模式下 DLL 实际创建的窗口（供人工确认）' -Ok $true -Severity 'info' -Details (Format-Windows $collect)

# ---------- C-5：只收集模式应出现收集控制台 ----------

$consoleWindow = @(@(Get-ObjectValue -Object $collect -Name 'windows' -Default @()) | Where-Object { $_.Class -eq $ConsoleWindowClass }) | Select-Object -First 1
$c5Details = @()
$consoleInfo = Get-ObjectValue -Object $collect -Name 'console' -Default $null
if ($consoleWindow)
{
    $c5Details += ("出现收集控制台：class='{0}' title='{1}'" -f $consoleWindow.Class, $consoleWindow.Title)
    if ($consoleInfo)
    {
        $c5Details += ("窗口 {0}x{1}；子控件 {2} 个（可见 {3} 个）" -f `
            ($consoleInfo.WindowRect[2] - $consoleInfo.WindowRect[0]),
            ($consoleInfo.WindowRect[3] - $consoleInfo.WindowRect[1]),
            @($consoleInfo.Controls).Count,
            @($consoleInfo.Controls | Where-Object { $_.Visible }).Count)
    }
    if ($consoleWindow.Title -notlike ('*' + $ConsoleWindowTitle + '*'))
    {
        $c5Details += ("标题与约定的不一致：期望包含 '{0}'，实际 '{1}'" -f $ConsoleWindowTitle, $consoleWindow.Title)
    }
}
else
{
    $c5Details += ("没有出现 class='{0}' 的窗口（标题应包含 '{1}'）" -f $ConsoleWindowClass, $ConsoleWindowTitle)
    if (-not $collectUsable) { $c5Details += $collectBlocked }
    $c5Details += Format-Windows $collect
}
Add-Check -Id 'C-5' -Title 'CXDEC_HASH_COLLECT_ONLY=1 时出现收集控制台窗口' -Ok ($collectUsable -and ($null -ne $consoleWindow)) -Details $c5Details

# ---------- C-6：「清空目录文件」的确认框默认在「否」，选否不许删东西 ----------

$clear = Get-ObjectValue -Object $collect -Name 'clearButton' -Default $null
$c6Details = @()
$c6Ok = $false
if (-not $collectUsable) { $c6Details += $collectBlocked; $c6Details += Format-Windows $collect }
if (-not $clear)
{
    $c6Details += '用例没有做「清空目录文件」观测（-SkipClearButtonCheck？）'
}
elseif (-not $clear.ButtonFound)
{
    $c6Details += ("找不到「清空目录文件」按钮（控件 ID {0}）" -f $clear.ButtonId)
    $c6Details += @($clear.Notes | ForEach-Object { '    ' + $_ })
}
elseif (-not $clear.DialogAppeared)
{
    $c6Details += '点了「清空目录文件」但没有弹确认框'
    $c6Details += @($clear.Notes | ForEach-Object { '    ' + $_ })
}
else
{
    $c6Details += ("确认框：title='{0}'" -f $clear.DialogTitle)
    $c6Details += ("    正文：{0}" -f $clear.DialogText)
    $c6Details += ("    按钮：{0}" -f ($clear.Buttons -join ', '))
    $c6Details += ("    默认按钮 ID = {0}（7=否，6=是，1=确定）" -f $clear.DefaultButtonId)
    $c6Details += ("    确认框弹出前目录里有：{0}" -f ($clear.BeforeListing -join ', '))
    $c6Details += ("    选「否」之后目录里有：{0}" -f ($clear.AfterDecline -join ', '))
    $c6Details += ("    选「是」之后目录里有：{0}" -f ($clear.AfterAccept -join ', '))
    $c6Ok = $collectUsable -and [bool]$clear.DefaultIsNo -and [bool]$clear.DeclineKeptAll
    if (-not $clear.DefaultIsNo) { $c6Details += '默认按钮不是「否」—— 用户直接回车就会删东西，与需求不符' }
    if (-not $clear.DeclineKeptAll) { $c6Details += '选「否」之后文件变少了 —— 取消路径没有真正取消' }
    foreach ($note in @($clear.Notes)) { $c6Details += ('    备注：' + $note) }
}
Add-Check -Id 'C-6' -Title '「清空目录文件」确认框默认在「否」，且选否不删任何文件' -Ok $c6Ok -Details $c6Details

# ---------- C-6b（提示）：选「是」时删了什么（已知限制：被占用的映射日志删不掉） ----------

if ($clear -and $clear.DialogAppeared)
{
    $removed = @($clear.BeforeListing | Where-Object { $_ -notin $clear.AfterAccept })
    $remaining = @($clear.AfterAccept)
    $c6bDetails = @(("选「是」之后被删除：{0}" -f $(if ($removed.Count -gt 0) { $removed -join ', ' } else { '<一个都没删>' })),
                    ("仍然留着：{0}" -f $(if ($remaining.Count -gt 0) { $remaining -join ', ' } else { '<空>' })),
                    '已知限制：DirectoryHash.log / FileNameHash.log 在游戏运行期间被 HashCore 以写独占方式打开，删不掉属预期。')
    Add-Check -Id 'C-6b' -Title '选「是」时的清理结果（仅提示）' -Ok $true -Severity 'info' -Details $c6bDetails
}

# ---------- C-7（软）：控制台控件不越界 ----------

$consoleInfo = Get-ObjectValue -Object $collect -Name 'console' -Default $null
if ($consoleInfo)
{
    $outOfBounds = @(Get-ObjectValue -Object $consoleInfo -Name 'OutOfBounds' -Default @())
    $c7Details = @()
    $visibleCount = @($consoleInfo.Controls | Where-Object { $_.Visible }).Count
    $c7Details += ("客户区（屏幕坐标）：{0}" -f ($consoleInfo.ClientRect -join ','))
    $c7Details += ("可见子控件 {0} 个；越界 {1} 个" -f $visibleCount, $outOfBounds.Count)
    if ($outOfBounds.Count -gt 0)
    {
        $c7Details += ($outOfBounds | ForEach-Object { ("    id={0} class={1} text='{2}' rect={3}" -f $_.CtrlId, $_.Class, $_.Text, ($_.Rect -join ',')) })
    }
    else
    {
        $c7Details += '所有可见子控件都落在父窗口客户区之内（矩形判定，不是像素判定）'
    }
    $shot = Get-ObjectValue -Object $collect -Name 'screenshot' -Default $null
    if ($shot)
    {
        $c7Details += ("截图：{0}" -f $(if ($shot.Error) { '失败 - ' + $shot.Error } else { $shot.Bmp + $(if ($shot.Png) { ' / ' + $shot.Png } else { '' }) }))
    }

    # 页签：参考图里"实时日志"上是两个页签（FileNameHash / DirectoryHash 实时日志）。
    # 条目数为 0 说明页签没插进去，只有个空架子（Edit 倒是都在：一个可见一个隐藏）。
    $tabControls = @($consoleInfo.Controls | Where-Object { $_.Class -eq 'SysTabControl32' })
    $emptyTabControls = @($tabControls | Where-Object { $null -ne $_.TabCount -and [int]$_.TabCount -lt 1 })
    foreach ($tab in $tabControls)
    {
        $c7Details += ("页签控件 id={0} 条目数={1} rect={2}" -f $tab.CtrlId, $tab.TabCount, ($tab.Rect -join ','))
    }
    if ($emptyTabControls.Count -gt 0)
    {
        $c7Details += '页签控件条目数为 0 —— 布局框在了，但页签标题没插进去；参考图里这里有两个页签。'
    }
    $edits = @($consoleInfo.Controls | Where-Object { $_.Class -eq 'Edit' })
    $c7Details += ("日志框（Edit）{0} 个：可见 {1} / 隐藏 {2}（页签切换的典型形态）" -f $edits.Count, @($edits | Where-Object { $_.Visible }).Count, @($edits | Where-Object { -not $_.Visible }).Count)

    if (-not $collectUsable) { $c7Details += $collectBlocked }
    Add-Check -Id 'C-7' -Title '控制台布局：可见控件未越出客户区' -Ok ($collectUsable -and ($outOfBounds.Count -eq 0) -and ($emptyTabControls.Count -eq 0)) -Severity 'warn' -Details $c7Details
}
else
{
    Add-Check -Id 'C-7' -Title '控制台布局：可见控件未越出客户区' -Ok $false -Severity 'warn' -Details @('没有控制台窗口，无法做布局自检')
}

# ---------- C-9：进度条分母来自对目标目录的扫描 ----------

$progress = Get-ObjectValue -Object $collect -Name 'progress' -Default $null
$c9Details = @()
$c9Ok = $false
if (-not $progress)
{
    $c9Details += '用例没有记录进度文字（控制台没出现，或 -SkipEnvInheritance 下没设目标目录）'
}
else
{
    $c9Details += ("目标目录（CXDEC_RUNTIME_HASH_TARGET_DIR）：{0}" -f $progress.targetDir)
    $c9Details += ("DirectoryHash 进度文字：'{0}'；我方在目标目录里放了 {1} 个去重目录 Hash" -f $progress.directoryHashText, $progress.expectedDirHashCount)
    $c9Details += ("FileNameHash 进度文字：'{0}'；我方在目标目录里放了 {1} 个去重文件 Hash" -f $progress.fileNameHashText, $progress.expectedFileHashCount)
    $c9Details += ("状态行：'{0}'" -f $progress.statusLineText)
    $dirDenominator = $null
    $fileDenominator = $null
    if ($progress.directoryHashText -match '/\s*(\d+)') { $dirDenominator = [int]$matches[1] }
    if ($progress.fileNameHashText -match '/\s*(\d+)') { $fileDenominator = [int]$matches[1] }
    if ($null -eq $dirDenominator -or $null -eq $fileDenominator)
    {
        $c9Details += '进度文字里没找到 "当前 / 总数" 形态的分母（格式可能变了）'
    }
    else
    {
        $c9Ok = ($dirDenominator -eq $progress.expectedDirHashCount) -and ($fileDenominator -eq $progress.expectedFileHashCount)
        $c9Details += ("解析出的分母：DirectoryHash={0}（期望 {1}） FileNameHash={2}（期望 {3}）" -f `
                $dirDenominator, $progress.expectedDirHashCount, $fileDenominator, $progress.expectedFileHashCount)
        if (-not $c9Ok) { $c9Details += '分母与目标目录里实际存在的哈希数不一致 —— 进度分母不是来自目标目录扫描' }
    }
}
if (-not $collectUsable) { $c9Details += $collectBlocked }
Add-Check -Id 'C-9' -Title '两条进度条分母来自对目标目录的扫描' -Ok ($collectUsable -and $c9Ok) -Details $c9Details

# ---------- 断言 E：环境变量"设置 → 继承 → DLL 读到"端到端 ----------
#
# E-1 实验组（父进程设 CXDEC_HASH_COLLECT_ONLY=1 再起宿主）：
#     出现控制台、不出现还原窗口、不写 RuntimeHashRestore.log。
# E-2 对照组（同一个宿主、不设该变量）：出现还原窗口、控制台不出现、写 RuntimeHashRestore.log。
# E-3 两种模式下纯 Hash 目录里的文件名/内容都不变（只收集绝不改名）。
# 这一对才真正证明"步骤2 不参与还原"，而不只是二进制里含一个字符串。

$collectConsole = @(@(Get-ObjectValue -Object $collect -Name 'windows' -Default @()) | Where-Object { $_.Class -eq $ConsoleWindowClass })
$controlConsole = @(@(Get-ObjectValue -Object $control -Name 'windows' -Default @()) | Where-Object { $_.Class -eq $ConsoleWindowClass })
$collectForbidden = @(Get-ObjectValue -Object $collect -Name 'forbiddenWindows' -Default @())
$controlForbidden = @(Get-ObjectValue -Object $control -Name 'forbiddenWindows' -Default @())
$collectEnv = Get-ObjectValue -Object $collect -Name 'envInheritance' -Default $null

$e1Ok = $collectUsable -and $collectConsole.Count -gt 0 -and $collectForbidden.Count -eq 0 -and
        (-not [bool](Get-ObjectValue -Object $collect -Name 'runtimeLogProduced' -Default $false)) -and
        (-not [bool](Get-ObjectValue -Object $collect -Name 'fallbackLogTouched' -Default $false))
$e1Details = @(
    ("父进程是否真的设了开关：{0}" -f (Get-ObjectValue -Object $collectEnv -Name 'collectOnlySetInParent' -Default '<未记录>')),
    ("传递机制：{0}" -f (Get-ObjectValue -Object $collectEnv -Name 'mechanism' -Default '<未记录>')),
    ("控制台窗口 {0} 个（期望 ≥1）；还原窗口 {1} 个（期望 0）；RuntimeHashRestore.log {2}" -f `
            $collectConsole.Count, $collectForbidden.Count,
        $(if (Get-ObjectValue -Object $collect -Name 'runtimeLogProduced' -Default $false) { '被写入' } else { '没写入' }))
)
$e1Details += Format-Windows $collect
if (-not $collectUsable) { $e1Details += $collectBlocked }
Add-Check -Id 'E-1' -Title '实验组：父进程设 CXDEC_HASH_COLLECT_ONLY=1 → 子进程 DllMain 读到（只收集）' -Ok $e1Ok -Details $e1Details

$e2Ok = $controlUsable -and $controlForbidden.Count -gt 0 -and $controlConsole.Count -eq 0 -and
        [bool](Get-ObjectValue -Object $control -Name 'runtimeLogProduced' -Default $false)
$e2Details = @(
    ("还原窗口 {0} 个（期望 ≥1）；控制台窗口 {1} 个（期望 0）；RuntimeHashRestore.log {2}" -f `
            $controlForbidden.Count, $controlConsole.Count,
        $(if (Get-ObjectValue -Object $control -Name 'runtimeLogProduced' -Default $false) { '被写入' } else { '没写入' }))
)
$e2Details += Format-Windows $control
if (-not $controlUsable) { $e2Details += $controlBlocked }
Add-Check -Id 'E-2' -Title '对照组：不设该变量 → 子进程走上游路径（还原窗口 + 日志）' -Ok $e2Ok -Details $e2Details

$pureCollect = Get-ObjectValue -Object $collect -Name 'pureHashDiff' -Default $null
$pureControl = Get-ObjectValue -Object $control -Name 'pureHashDiff' -Default $null
$e3Ok = $collectUsable -and $controlUsable -and
        [bool](Get-ObjectValue -Object $pureCollect -Name 'Same' -Default $false) -and
        [bool](Get-ObjectValue -Object $pureControl -Name 'Same' -Default $false)
$e3Details = @(
    ("只收集模式：纯 Hash 目录 {0} 个文件 → {1} 个文件；快照一致={2}" -f `
        (Get-ObjectValue -Object $collect -Name 'pureHashBeforeCount' -Default '?'),
        (Get-ObjectValue -Object $collect -Name 'pureHashAfterCount' -Default '?'),
        (Get-ObjectValue -Object $pureCollect -Name 'Same' -Default '<未记录>')),
    ("对照组：纯 Hash 目录 {0} 个文件 → {1} 个文件；快照一致={2}" -f `
        (Get-ObjectValue -Object $control -Name 'pureHashBeforeCount' -Default '?'),
        (Get-ObjectValue -Object $control -Name 'pureHashAfterCount' -Default '?'),
        (Get-ObjectValue -Object $pureControl -Name 'Same' -Default '<未记录>')),
    '比对维度：文件数、每个文件的相对路径（=文件名）、大小、修改时间、内容 sha256'
)
foreach ($pair in @(@{ N = '只收集'; D = $pureCollect }, @{ N = '对照组'; D = $pureControl }))
{
    foreach ($key in @('Removed', 'Added', 'Changed'))
    {
        foreach ($item in @(Get-ObjectValue -Object $pair.D -Name $key -Default @()))
        {
            $e3Details += ("    {0} {1}：{2}" -f $pair.N, $key, $item)
        }
    }
}
Add-Check -Id 'E-3' -Title '两种模式下纯 Hash 目录里的文件名/内容都未被改动' -Ok $e3Ok -Details $e3Details

exit (Show-HarnessChecks -Title '断言 C + E（步骤2 只收集 / 环境变量端到端）' -ChecksJson $ChecksJson)
