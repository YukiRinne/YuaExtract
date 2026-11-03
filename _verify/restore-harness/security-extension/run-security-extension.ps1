param(
    [switch]$NoBuild,
    [int]$MinimumChecks = 70
)

# 构建（可选）+ 运行 headless security-extension 回归。
# 产物与日志固定落在：
#   _verify\restore-harness\_build\security-extension\  （exe / obj）
#   _verify\restore-harness\Run\security-extension\     （stdout log + checks JSON）
$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$buildDir = Join-Path $PSScriptRoot '..\_build\security-extension'
$runDir = Join-Path $PSScriptRoot '..\Run\security-extension'
$exe = Join-Path $buildDir 'security-extension-tests.exe'
$log = Join-Path $runDir 'security-extension-tests.log'
$jsonPath = Join-Path $runDir 'checks-security-extension.json'

if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot 'build-security-extension.ps1') -Configuration Release
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}
if (-not (Test-Path $exe)) {
    Write-Error "test exe not found: $exe"
    exit 2
}

New-Item -ItemType Directory -Force -Path $runDir | Out-Null
$output = & $exe 2>&1
$exitCode = $LASTEXITCODE
$text = ($output | ForEach-Object { "$_" }) -join "`r`n"
Set-Content -Path $log -Value $text -Encoding UTF8

$checks = New-Object System.Collections.Generic.List[object]
$seen = New-Object 'System.Collections.Generic.HashSet[string]'
$duplicates = New-Object System.Collections.Generic.List[string]
$passed = 0
$failed = 0
$summaryChecks = $null
$summaryFailures = $null
foreach ($line in $output) {
    $match = [regex]::Match("$line", '^SEC-EXT (PASS|FAIL) (\S+)(?:\s*\|\s*(.*))?$')
    if ($match.Success) {
        $status = $match.Groups[1].Value
        $name = $match.Groups[2].Value
        if (-not $seen.Add($name)) { $duplicates.Add($name) }
        if ($status -eq 'PASS') { $passed++ } else { $failed++ }
        $checks.Add([ordered]@{
            name   = $name
            status = $status
            detail = $match.Groups[3].Value
        })
        continue
    }
    $summary = [regex]::Match("$line", '^SEC-EXT SUMMARY checks=(\d+) failures=(\d+)$')
    if ($summary.Success) {
        $summaryChecks = [int]$summary.Groups[1].Value
        $summaryFailures = [int]$summary.Groups[2].Value
    }
}

# 不能只看 failed：checks=0 / 名字重复 / 汇总行缺失或对不上都必须算失败。
$problems = New-Object System.Collections.Generic.List[string]
if ($checks.Count -eq 0) { $problems.Add('no SEC-EXT check lines parsed') }
if ($checks.Count -lt $MinimumChecks) {
    $problems.Add("parsed checks $($checks.Count) < expected minimum $MinimumChecks")
}
if ($duplicates.Count -gt 0) {
    $problems.Add("duplicate check names: $((($duplicates | Select-Object -Unique) -join ', '))")
}
if ($null -eq $summaryChecks) {
    $problems.Add('missing SEC-EXT SUMMARY line')
} else {
    if ($summaryChecks -ne $checks.Count) {
        $problems.Add("summary checks $summaryChecks != parsed checks $($checks.Count)")
    }
    if ($summaryFailures -ne $failed) {
        $problems.Add("summary failures $summaryFailures != parsed failures $failed")
    }
}
if ($exitCode -ne 0) { $problems.Add("test exe exit code $exitCode") }
if ($failed -ne 0) { $problems.Add("failed checks $failed") }

$summary = [ordered]@{
    scope       = 'security-extension'
    configuration = 'Release'
    platform    = 'Win32'
    buildDir    = $buildDir
    log         = $log
    exitCode    = $exitCode
    minimumChecks = $MinimumChecks
    totals      = [ordered]@{
        checks  = $checks.Count
        passed  = $passed
        failed  = $failed
    }
    problems    = $problems
    checks      = $checks
}
$summary | ConvertTo-Json -Depth 6 | Set-Content -Path $jsonPath -Encoding UTF8

$output | Write-Output
Write-Output "SEC-EXT JSON: $jsonPath"
if ($problems.Count -gt 0) {
    foreach ($problem in $problems) { Write-Output "SEC-EXT PROBLEM: $problem" }
    exit 1
}
exit 0
