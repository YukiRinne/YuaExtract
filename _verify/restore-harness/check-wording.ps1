# 文案扫描（静态、可复现）：产品源码里所有「补充还原」出现处，逐个判"是不是用户可见的字面量"。
#
# 为什么要单独做：N-1 只能扫**运行时看得见**的文本（复选框/日志/报告/名字表）。
# 工具提示、出错分支的字符串这类"要在特定条件下才出现"的文案，运行时不容易全覆盖，
# 但静态扫一遍源码就能把"用户可见的字符串字面量"全部抓住。
#
# 判定口径（与 lead 的裁决一致）：
#   * 注释里的旧词**不算缺陷**（本轮明确"注释一律不动"）
#   * 字符串字面量里的「补充还原」必须紧跟在「推理」后面
#
#   pwsh -File check-wording.ps1 [-RepoRoot <路径>]

[CmdletBinding()]
param(
    [string]$RepoRoot = '',
    [string]$ChecksJson = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')

if (-not $RepoRoot) { $RepoRoot = [System.IO.Path]::GetFullPath((Join-Path $root '..\..')) }
if (-not $ChecksJson) { $ChecksJson = Join-Path $root 'Run\out\checks-wording.json' }

Reset-HarnessChecks
Write-Host ''
Write-Host '=== 文案静态扫描（源码里的「补充还原」逐处判定）==='

$sources = @()
foreach ($dir in @('CxdecExtractorLoader', 'CxdecStringDumper'))
{
    $full = Join-Path $RepoRoot $dir
    if (-not (Test-Path -LiteralPath $full)) { continue }
    $sources += @(Get-ChildItem -LiteralPath $full -Recurse -File -Include *.cpp, *.h, *.hpp, *.rc | Where-Object { $_.FullName -notmatch '\\Release\\|\\Debug\\|\\x64\\' })
}
Write-Host ("[扫描] 源文件 {0} 个（{1}）" -f $sources.Count, $RepoRoot)

$commentHits = 0
$stringHits = 0
$badStrings = @()
$goodStrings = @()

foreach ($file in $sources)
{
    $lines = [System.IO.File]::ReadAllLines($file.FullName, [System.Text.Encoding]::UTF8)
    $inBlockComment = $false
    for ($index = 0; $index -lt $lines.Count; $index++)
    {
        $line = $lines[$index]
        $trimmed = $line.TrimStart()

        # 先把"这一行的注释从哪里开始"算出来，再判每个「补充还原」落在注释里还是字面量里。
        #   * 行注释：第一个「引号成对」的 // 之后都是注释
        #     （不能反过来用"这行有没有引号"判 —— 注释里写 "补充还原" 也算引号，
        #       第一版就是这么误判了 5 处注释）
        #   * 块注释：整行以 * 或 /* 开头，或者上一行开了块注释还没关
        $blockStart = $trimmed.StartsWith('/*')
        $blockBody = $trimmed.StartsWith('*')
        $commentAt = -1
        if (-not $inBlockComment -and -not $blockStart -and -not $blockBody)
        {
            $slashes = $line.IndexOf('//')
            if ($slashes -ge 0)
            {
                $prefixQuotes = ([regex]::Matches($line.Substring(0, $slashes), '(?<!\\)"')).Count
                if ($prefixQuotes % 2 -eq 0) { $commentAt = $slashes }
            }
        }

        $searchFrom = 0
        while ($true)
        {
            $at = $line.IndexOf('补充还原', $searchFrom)
            if ($at -lt 0) { break }
            $searchFrom = $at + 4

            $isComment = $inBlockComment -or $blockStart -or $blockBody -or ($commentAt -ge 0 -and $at -ge $commentAt)
            $prefixed = ($at -ge 1 -and $line.Substring($at - 1, 1) -eq '理')
            $entry = ('{0}:{1}  {2}' -f $file.Name, ($index + 1), $line.Trim())

            if ($isComment) { $commentHits++ }
            elseif ($prefixed) { $stringHits++; $goodStrings += $entry }
            else { $badStrings += $entry }
        }

        # 更新块注释状态（同一行 /* ... */ 闭合时不算进入）
        if ($blockStart -and $line.IndexOf('*/', $line.IndexOf('/*') + 2) -lt 0) { $inBlockComment = $true }
        elseif ($inBlockComment -and $line.Contains('*/')) { $inBlockComment = $false }
    }
}

$ok = ($badStrings.Count -eq 0)
$details = @(
    ("源码里「补充还原」出现处：注释 {0} 处（本轮明确不改注释，不算缺陷）；字符串字面量 {1} 处带「推理」前缀" -f $commentHits, $stringHits)
)
if ($ok)
{
    $details += '[OK] 没有任何"用户可见的字符串字面量"用旧词（无孤立「补充还原」）'
    $details += ($goodStrings | Select-Object -First 6 | ForEach-Object { '    [带前缀] ' + $_ })
    if ($goodStrings.Count -gt 6) { $details += ('    …另有 {0} 处' -f ($goodStrings.Count - 6)) }
}
else
{
    $details += ('[FAIL] {0} 处字符串字面量仍在用旧词：' -f $badStrings.Count)
    $details += ($badStrings | ForEach-Object { '    ' + $_ })
}
Add-Check -Id 'N-2' -Title 'N-2 文案（静态）：源码里用户可见的字面量一律「推理补充还原」' -Ok $ok -Details $details

exit (Show-HarnessChecks -Title '文案静态扫描' -ChecksJson $ChecksJson)
