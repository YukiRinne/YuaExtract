# 原生执行授权 / 运行时数据完整性 headless 回归。
#
# 只调用生产 DLL 的真实导出函数，不启动游戏、不注入真实进程、不访问外网。
# 产物与日志固定落在 _verify\restore-harness\_build\security-native\（已 ignore）。
#
# 用法：
#   pwsh -File _verify\security-native\run-tests.ps1
#   pwsh -File ...\run-tests.ps1 -KeyStaticDir <dir> -RepackerDir <dir> -CliDir <dir>
#
# 未传目录时按 _build\security-native\ 下的独立构建产物找。

param(
    [string]$KeyStaticDir = '',
    [string]$RepackerDir = '',
    [string]$CliDir = '',
    [string]$WorkRoot = ''
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$buildBase = Join-Path $repo '_verify\restore-harness\_build\security-native'
if ([string]::IsNullOrWhiteSpace($WorkRoot)) { $WorkRoot = Join-Path $buildBase 'tests' }
if ([string]::IsNullOrWhiteSpace($KeyStaticDir)) {
    $official = Join-Path $buildBase 'CxdecKeyStatic-official'
    $KeyStaticDir = if (Test-Path (Join-Path $official 'CxdecKeyStatic.dll')) { $official } else { Join-Path $buildBase 'CxdecKeyStatic' }
}
if ([string]::IsNullOrWhiteSpace($RepackerDir)) { $RepackerDir = Join-Path $buildBase 'CxdecRepacker' }
if ([string]::IsNullOrWhiteSpace($CliDir)) { $CliDir = Join-Path $buildBase 'CxdecCli' }

$bin = Join-Path $WorkRoot 'bin'
$obj = Join-Path $WorkRoot 'obj'
$logs = Join-Path $WorkRoot 'logs'
$tmp = Join-Path $WorkRoot 'tmp'
$mods = Join-Path $WorkRoot 'mods'
foreach ($d in @($bin, $obj, $logs, $tmp, $mods)) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}
foreach ($d in @('export', 'logdir', 'runtime', 'pin', 'tempfile', 'cleanup')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $obj $d) | Out-Null
}

$failures = 0
$results = New-Object System.Collections.Generic.List[string]
function Record([string]$name, [bool]$ok, [string]$extra = '') {
    $script:results.Add(("{0} {1}{2}" -f ($(if ($ok) { 'PASS' } else { 'FAIL' })), $name, $(if ($extra) { " | $extra" } else { '' })))
    if (-not $ok) { $script:failures++ }
}

# ---- 环境：vcvarsall x86 ----
$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
$vcvarsErr = Join-Path $logs 'vcvars.stderr.log'
$envLines = & cmd.exe /c "call `"$vcvars`" x86 >nul && set" 2> $vcvarsErr
foreach ($line in $envLines) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path ("Env:" + $matches[1]) -Value $matches[2] }
}
# vcvars 偶尔会往 stderr 打无关的"系统找不到指定的路径"；工具链是否真的可用
# 以 cl.exe/dumpbin.exe 能否解析为准，不能把这几行噪声当成构建成功或失败。
$clPath = (Get-Command cl.exe -ErrorAction SilentlyContinue).Source
$dumpbin = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue).Source
Record 'toolchain cl.exe present' ($null -ne $clPath)
Record 'toolchain dumpbin.exe present' ($null -ne $dumpbin)
if ($null -eq $clPath) { throw "cl.exe not found after vcvarsall; see $vcvarsErr" }

$commonDefs = @('/nologo', '/std:c++17', '/EHsc', '/utf-8', '/W3', '/MT', '/O2',
                '/D_CRT_SECURE_NO_WARNINGS', '/D_WIN32_WINNT=0x601', '/DWIN32', '/DNDEBUG',
                '/DUNICODE', '/D_UNICODE')

function Invoke-Cl([string]$logName, [string[]]$arguments) {
    $log = Join-Path $logs ($logName + '.build.log')
    $out = & cl.exe @arguments 2>&1
    $code = $LASTEXITCODE
    $out | Out-File -FilePath $log -Encoding utf8
    return $code
}

function Record-Fresh([string]$name, [string]$artifact, [string[]]$sources) {
    if (-not (Test-Path $artifact)) {
        Record "$name artifact present" $false "missing $artifact"
        Record "$name artifact fresh" $false "missing $artifact"
        return
    }
    Record "$name artifact present" $true "artifact=$artifact"
    $newest = [datetime]::MinValue
    foreach ($src in $sources) {
        if (Test-Path $src) {
            $t = (Get-Item $src).LastWriteTimeUtc
            if ($t -gt $newest) { $newest = $t }
        }
    }
    $artifactTime = (Get-Item $artifact).LastWriteTimeUtc
    Record "$name artifact fresh" ($artifactTime -ge $newest) "artifact=$artifact"
}

# ---- 编译测试 ----
$exportExe = Join-Path $bin 'export-contract-test.exe'
$code = Invoke-Cl 'export-contract' ($commonDefs + @(
    "/I$repo", "/I$repo\Common", "/I$repo\CxdecRepacker", "/I$repo\CxdecRepacker\hxv4p",
    "/Fo$obj\export\", "/Fe:$exportExe",
    (Join-Path $PSScriptRoot 'export-contract-test.cpp'),
    "$repo\CxdecRepacker\hxv4p\Hxv4p.cpp",
    "$repo\CxdecRepacker\hxv4p\ZlibInflate.cpp"))
Record 'build export-contract-test' ($code -eq 0) "exit=$code"

$tempfileExe = Join-Path $bin 'native-tempfile-test.exe'
$code = Invoke-Cl 'native-tempfile' ($commonDefs + @(
    "/I$repo\CxdecKeyStatic", "/I$repo\Common", "/Fo$obj\tempfile\", "/Fe:$tempfileExe",
    (Join-Path $PSScriptRoot 'native-tempfile-test.cpp')))
Record 'build native-tempfile-test' ($code -eq 0) "exit=$code"

$cleanupExe = Join-Path $bin 'native-cleanup-test.exe'
$code = Invoke-Cl 'native-cleanup' ($commonDefs + @(
    "/I$repo", "/I$repo\Common", "/I$repo\CxdecRepacker", "/I$repo\CxdecRepacker\hxv4p",
    "/Fo$obj\cleanup\", "/Fe:$cleanupExe",
    (Join-Path $PSScriptRoot 'native-cleanup-test.cpp'),
    "$repo\CxdecRepacker\crypto\blake2s.cpp",
    "$repo\CxdecRepacker\hxv4p\Hxv4p.cpp",
    "$repo\CxdecRepacker\hxv4p\ZlibInflate.cpp"))
Record 'build native-cleanup-test' ($code -eq 0) "exit=$code"

$logdirExe = Join-Path $bin 'logdir-test.exe'
$code = Invoke-Cl 'logdir' ($commonDefs + @(
    "/I$repo\Common", "/Fo$obj\logdir\", "/Fe:$logdirExe",
    (Join-Path $PSScriptRoot 'logdir-test.cpp'), 'advapi32.lib'))
Record 'build logdir-test' ($code -eq 0) "exit=$code"

$runtimeExe = Join-Path $bin 'runtime-dataintegrity-test.exe'
$code = Invoke-Cl 'runtime-dataintegrity' ($commonDefs + @(
    "/I$repo\Common", "/I$repo\Detours", "/I$repo\CxdecAntiMalform",
    "/Fo$obj\runtime\", "/Fe:$runtimeExe",
    (Join-Path $PSScriptRoot 'runtime-dataintegrity-test.cpp'),
    "$repo\CxdecAntiMalform\tjs_patcher.cpp",
    "$repo\CxdecAntiMalform\tjs2_parser.cpp",
    "$repo\CxdecAntiMalform\kmp_search.cpp",
    "$repo\Detours\detours.cpp", "$repo\Detours\disasm.cpp",
    "$repo\Detours\image.cpp", "$repo\Detours\modules.cpp",
    'shlwapi.lib', 'user32.lib'))
Record 'build runtime-dataintegrity-test' ($code -eq 0) "exit=$code"

$pinHost = Join-Path $bin 'pin-host.exe'
$pinDll = Join-Path $bin 'pin_fixture.dll'
$unpinnedDll = Join-Path $bin 'unpinned_fixture.dll'
$code1 = Invoke-Cl 'pin-host' ($commonDefs + @("/Fo$obj\pin\", "/Fe:$pinHost", (Join-Path $PSScriptRoot 'pin-host.cpp')))
$code2 = Invoke-Cl 'pin-fixture' ($commonDefs + @('/LD', "/Fo$obj\pin\", "/Fe:$pinDll", (Join-Path $PSScriptRoot 'pin_fixture.cpp')))
$code3 = Invoke-Cl 'unpinned-fixture' ($commonDefs + @('/LD', "/Fo$obj\pin\", "/Fe:$unpinnedDll", (Join-Path $PSScriptRoot 'unpinned_fixture.cpp')))
Record 'build pin harness' (($code1 -eq 0) -and ($code2 -eq 0) -and ($code3 -eq 0)) "exit=$code1/$code2/$code3"

# ---- 暂存被测 DLL ----
$keyStaticDll = Join-Path $KeyStaticDir 'CxdecKeyStatic.dll'
$repackerDll = Join-Path $RepackerDir 'CxdecRepacker.dll'
$cliExe = Join-Path $CliDir 'CxdecCli.exe'
foreach ($p in @($keyStaticDll, $repackerDll)) {
    if (-not (Test-Path $p)) { throw "missing production artifact: $p" }
    Copy-Item -Force $p $mods
}
$stagedKeyStatic = Join-Path $mods 'CxdecKeyStatic.dll'
$stagedRepacker = Join-Path $mods 'CxdecRepacker.dll'

Record-Fresh 'KeyStatic' $keyStaticDll @(
    "$repo\CxdecKeyStatic\CxdecRecover.cpp", "$repo\CxdecKeyStatic\CxdecRecover.h",
    "$repo\CxdecKeyStatic\FilterManager.cpp", "$repo\CxdecKeyStatic\FilterManager.h",
    "$repo\CxdecKeyStatic\ExportApi.cpp", "$repo\CxdecKeyStatic\ExportApi.h",
    "$repo\Common\NativeExecution.h")
Record-Fresh 'Repacker' $repackerDll @(
    "$repo\CxdecRepacker\ExportApi.cpp", "$repo\CxdecRepacker\ExportApi.h",
    "$repo\CxdecRepacker\pack_static\keystore.cpp", "$repo\CxdecRepacker\pack_static\keystore.h",
    "$repo\CxdecRepacker\pack_static\pack_static.cpp", "$repo\CxdecRepacker\pack_static\pack_static.h",
    "$repo\Common\NativeExecution.h")
if (Test-Path $cliExe) {
    Record-Fresh 'CLI' $cliExe @(
        "$repo\CxdecCli\main.cpp", "$repo\CxdecCli\ModuleApi.cpp", "$repo\CxdecCli\ModuleApi.h")
} else {
    Record 'CLI artifact present' $false "missing $cliExe"
    Record 'CLI artifact fresh' $false "missing $cliExe"
}

# ---- 导出表 ----
if ($exportExe -and (Test-Path $exportExe)) {
    $envlog = Join-Path $logs 'export-contract-test.log'
    & $exportExe $stagedKeyStatic $stagedRepacker (Join-Path $tmp 'export') 2>&1 | Tee-Object -FilePath $envlog
    Record 'export-contract-test' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"
}
if (Test-Path $tempfileExe) {
    $log = Join-Path $logs 'native-tempfile-test.log'
    & $tempfileExe (Join-Path $tmp 'tempfile') 2>&1 | Tee-Object -FilePath $log
    Record 'native-tempfile-test' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"
} else {
    Record 'native-tempfile-test' $false "missing $tempfileExe"
}
if (Test-Path $cleanupExe) {
    $log = Join-Path $logs 'native-cleanup-test.log'
    & $cleanupExe (Join-Path $tmp 'cleanup') 2>&1 | Tee-Object -FilePath $log
    Record 'native-cleanup-test' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"
} else {
    Record 'native-cleanup-test' $false "missing $cleanupExe"
}
if ($logdirExe -and (Test-Path $logdirExe)) {
    $log = Join-Path $logs 'logdir-test.log'
    & $logdirExe (Join-Path $tmp 'logdir') 2>&1 | Tee-Object -FilePath $log
    Record 'logdir-test' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"
}
if ($runtimeExe -and (Test-Path $runtimeExe)) {
    $log = Join-Path $logs 'runtime-dataintegrity-test.log'
    Push-Location $bin
    try { & $runtimeExe 2>&1 | Tee-Object -FilePath $log } finally { Pop-Location }
    Record 'runtime-dataintegrity-test' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"
}
if (Test-Path $pinHost) {
    $log = Join-Path $logs 'pin-host.log'
    & $pinHost $pinDll $unpinnedDll 2>&1 | Tee-Object -FilePath $log
    Record 'pin-host' ($LASTEXITCODE -eq 0) "exit=$LASTEXITCODE"
}

# ---- CLI 行为 ----
if (Test-Path $cliExe) {
    $cliWork = Join-Path $WorkRoot 'cli'
    New-Item -ItemType Directory -Force -Path (Join-Path $cliWork 'in') | Out-Null
    Copy-Item -Force $keyStaticDll, $repackerDll $cliWork
    Copy-Item -Force $cliExe $cliWork
    $cli = Join-Path $cliWork 'CxdecCli.exe'
    Set-Content -Path (Join-Path $cliWork 'in\a.txt') -Value 'hello' -NoNewline
    Set-Content -Path (Join-Path $cliWork 'fake.exe') -Value 'MZfake' -NoNewline

    $helpLog = Join-Path $logs 'cli-help.log'
    & $cli help *> $helpLog
    $helpText = Get-Content -Raw $helpLog
    Record 'cli help' (($LASTEXITCODE -eq 0) -and $helpText.Contains('--allow-native-code') -and $helpText.Contains('sandbox'))

    & $cli sniff (Join-Path $cliWork 'in') --allow-native-code *> (Join-Path $logs 'cli-sniff-allow.log')
    Record 'cli sniff rejects --allow-native-code' ($LASTEXITCODE -eq 2)

    & $cli importkey (Join-Path $cliWork 'x.hxv4p') --allow-native-code *> (Join-Path $logs 'cli-import-allow.log')
    Record 'cli importkey rejects --allow-native-code' ($LASTEXITCODE -eq 2)

    $repoKeys = Join-Path $cliWork 'keys'
    $outXp3 = Join-Path $cliWork 'out.xp3'
    & $cli repack (Join-Path $cliWork 'in') $outXp3 --exe (Join-Path $cliWork 'fake.exe') --keys $repoKeys *> (Join-Path $logs 'cli-repack-default.log')
    Record 'cli repack default denies and writes nothing' (($LASTEXITCODE -eq 1) -and (-not (Test-Path $outXp3)))

    & $cli keys (Join-Path $cliWork 'fake.exe') --keys $repoKeys *> (Join-Path $logs 'cli-keys-default.log')
    Record 'cli keys default denies' ($LASTEXITCODE -eq 1)

    $ksOut = Join-Path $cliWork 'ks-default'
    & $cli keystatic (Join-Path $cliWork 'fake.exe') --out $ksOut *> (Join-Path $logs 'cli-keystatic-default.log')
    Record 'cli keystatic default denies and writes nothing' (($LASTEXITCODE -eq 1) -and (-not (Test-Path $ksOut)))

    $ksAllowed = Join-Path $cliWork 'ks-allowed'
    & $cli keystatic (Join-Path $cliWork 'fake.exe') --out $ksAllowed --allow-native-code *> (Join-Path $logs 'cli-keystatic-allowed.log')
    $allowedText = Get-Content -Raw (Join-Path $logs 'cli-keystatic-allowed.log')
    Record 'cli keystatic --allow-native-code enters real path' (($LASTEXITCODE -eq 1) -and $allowedText.Contains('STARTUP.TJS resource not found'))
}

# ---- 导出名字 ----
if (-not $dumpbin) {
    Record 'export table inspection available' $false "dumpbin.exe not found"
} else {
    $expLog = Join-Path $logs 'dll-exports.txt'
    "== CxdecKeyStatic ==" | Out-File $expLog -Encoding utf8
    & $dumpbin /nologo /exports $stagedKeyStatic | Out-File -Append $expLog -Encoding utf8
    "== CxdecRepacker ==" | Out-File -Append $expLog -Encoding utf8
    & $dumpbin /nologo /exports $stagedRepacker | Out-File -Append $expLog -Encoding utf8
    $expText = Get-Content -Raw $expLog
    $names = @('ExtractKey', 'ExtractKeyEx', 'Repack', 'RepackEx', 'DeriveKeys', 'DeriveKeysEx', '_ExtractKey@16', '_ExtractKeyEx@20', '_Repack@44', '_RepackEx@48', '_DeriveKeys@24', '_DeriveKeysEx@28')
    $allNames = $true
    foreach ($n in $names) { if (-not $expText.Contains($n)) { $allNames = $false } }
    Record 'export table names + exact stdcall @N present' $allNames
}

# ---- 预期检查项：防止某段被静默跳过 ----
$expectedChecks = @(
    'toolchain cl.exe present', 'toolchain dumpbin.exe present',
    'build export-contract-test', 'build logdir-test', 'build native-tempfile-test', 'build native-cleanup-test', 'build runtime-dataintegrity-test',
    'build pin harness',
    'KeyStatic artifact present', 'KeyStatic artifact fresh',
    'Repacker artifact present', 'Repacker artifact fresh',
    'CLI artifact present', 'CLI artifact fresh',
    'export-contract-test', 'logdir-test', 'native-tempfile-test', 'native-cleanup-test', 'runtime-dataintegrity-test', 'pin-host',
    'cli help', 'cli sniff rejects --allow-native-code',
    'cli importkey rejects --allow-native-code',
    'cli repack default denies and writes nothing', 'cli keys default denies',
    'cli keystatic default denies and writes nothing',
    'cli keystatic --allow-native-code enters real path',
    'export table names + exact stdcall @N present'
)
$executed = New-Object System.Collections.Generic.List[string]
foreach ($line in $results) {
    if ($line -match '^(?:PASS|FAIL) (.+?)(?: \| .*)?$') { $executed.Add($matches[1]) }
}
$missing = @($expectedChecks | Where-Object { -not $executed.Contains($_) })
Record 'expected checks executed' ($missing.Count -eq 0) ($missing -join ', ')

# ---- 汇总 ----
Write-Host ''
$results | ForEach-Object { Write-Host $_ }
Write-Host ''
Write-Host ("TOTAL failures={0}" -f $failures)
$results | Out-File (Join-Path $logs 'summary.txt') -Encoding utf8
if ($failures -ne 0) { exit 1 }
exit 0
