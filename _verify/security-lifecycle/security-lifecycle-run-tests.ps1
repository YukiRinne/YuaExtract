# 构建并运行 security-lifecycle 的 headless 测试。
# 产物只写到 _verify/restore-harness/_build/security-lifecycle/（已忽略）。
#
# 每个测试都直接编入生产实现/头文件：
#   - job-lifecycle : YuaExtractMainWindow.cpp（ResetJob / TryStopJobForClose / OnJobDone / online gate）
#   - wait          : YuaExtractHost.h 的 Detail::WaitForHandleResponsive
#   - steamswap     : CxdecExtractorLoader\SteamApiSwap.h
#   - restore-stop  : YuaRestoreWindow.cpp（PopNextTask / StopRequested / Publish*，无 HWND）
#
# 检查项：源文件存在、构建成功、exe 构建前已删除且构建后重新出现（freshness）、
# 构建日志存在、输出含 RESULT PASS 与约定的 CHECK 名称；任一项失败退出码非 0。
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$build = Join-Path $root '_verify\restore-harness\_build\security-lifecycle'
$testRoot = Join-Path $root '_verify\security-lifecycle'
New-Item -ItemType Directory -Force -Path $build | Out-Null

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat'
if (-not (Test-Path $vcvars)) { throw "vcvars32.bat not found: $vcvars" }

$includes = '/I Common /I CxdecExtractorLoader /I CxdecRepacker /I CxdecRepacker\third_party\zlib'
$common = @(
  'Common\directory.cpp', 'Common\encoding.cpp', 'Common\file.cpp', 'Common\log.cpp',
  'Common\path.cpp', 'Common\pe.cpp', 'Common\stringhelper.cpp', 'Common\util.cpp'
)
$zlib = @(
  'CxdecRepacker\third_party\zlib\adler32.c', 'CxdecRepacker\third_party\zlib\compress.c',
  'CxdecRepacker\third_party\zlib\crc32.c', 'CxdecRepacker\third_party\zlib\deflate.c',
  'CxdecRepacker\third_party\zlib\infback.c', 'CxdecRepacker\third_party\zlib\inffast.c',
  'CxdecRepacker\third_party\zlib\inflate.c', 'CxdecRepacker\third_party\zlib\inftrees.c',
  'CxdecRepacker\third_party\zlib\trees.c', 'CxdecRepacker\third_party\zlib\uncompr.c',
  'CxdecRepacker\third_party\zlib\zutil.c'
)

$jobSources = @(
  '_verify\security-lifecycle\security-lifecycle-job-lifecycle-test.cpp',
  'CxdecExtractorLoader\ExtensionSet.cpp', 'CxdecExtractorLoader\OnlineUpdate.cpp',
  'CxdecExtractorLoader\HashNameRestorer.cpp', 'CxdecExtractorLoader\HashNameMiner.cpp',
  'CxdecExtractorLoader\YuaExtractWorkspace.cpp', 'CxdecExtractorLoader\Blake2s.cpp'
) + $common + @('CxdecRepacker\crypto\siphash.cpp') + $zlib

$tests = @(
  @{
    Name = 'job-lifecycle'
    Source = '_verify\security-lifecycle\security-lifecycle-job-lifecycle-test.cpp'
    Sources = $jobSources
    Libs = 'winhttp.lib comctl32.lib shell32.lib user32.lib gdi32.lib'
    Expected = @(
      'ResetJob increments job generation',
      'timed-out worker handle is preserved',
      'stale Done message is ignored',
      'cancelled download never installs even when worker ok=true',
      'ClosePending download never installs',
      'final install recheck ignores self-set JobDoneUi',
      'final install recheck aborts after ClosePending during confirmation',
      'SetM note is a job-sensitive command',
      'Open library directory is a job-sensitive command'
    )
  },
  @{
    Name = 'wait'
    Source = '_verify\security-lifecycle\security-lifecycle-wait-test.cpp'
    Sources = @('_verify\security-lifecycle\security-lifecycle-wait-test.cpp')
    Libs = 'user32.lib'
    Expected = @(
      'bounded wait times out on unsignaled handle',
      'queued WM_CLOSE is reported as cancel',
      'WM_QUIT is preserved in the thread queue',
      'sleep child is observed as exited after it finishes'
    )
  },
  @{
    Name = 'steamswap'
    Source = '_verify\security-lifecycle\security-lifecycle-steamswap-test.cpp'
    Sources = @('_verify\security-lifecycle\security-lifecycle-steamswap-test.cpp')
    Libs = 'user32.lib'
    Expected = @(
      'backup uses unique yua-bak name',
      'explicit Rollback reports success',
      'failed Begin leaves original file untouched',
      'fixed sentinel bytes never modified'
    )
  },
  @{
    Name = 'restore-stop'
    Source = '_verify\security-lifecycle\security-lifecycle-restore-stop-test.cpp'
    Sources = @('_verify\security-lifecycle\security-lifecycle-restore-stop-test.cpp')
    Libs = 'comctl32.lib shell32.lib user32.lib gdi32.lib'
    Expected = @(
      'PopNextTask returns FIFO head',
      'PublishProgress updates task without a window handle',
      'PopNextTask refuses work after stop',
      'stop clears the remaining queued packages'
    )
  }
)

$anyFailure = $false
foreach ($test in $tests)
{
  $name = $test.Name
  foreach ($source in $test.Sources)
  {
    if (-not (Test-Path -LiteralPath (Join-Path $root $source)))
    {
      throw "missing source for ${name}: $source"
    }
  }

  $objDir = Join-Path $build ($name + '\obj')
  $binDir = Join-Path $build ($name + '\bin')
  New-Item -ItemType Directory -Force -Path $objDir | Out-Null
  New-Item -ItemType Directory -Force -Path $binDir | Out-Null
  $exe = Join-Path $binDir ('security-lifecycle-' + $name + '-test.exe')
  $log = Join-Path $build ($name + '-build.log')
  if (Test-Path -LiteralPath $exe) { Remove-Item -LiteralPath $exe -Force }
  # freshness 前提：构建前 exe 必须已不存在；构建成功后再出现即证明是本次产物。
  $exeWasRemovedBeforeBuild = -not (Test-Path -LiteralPath $exe)

  $cl = ('cl /nologo /std:c++20 /EHsc /utf-8 /MTd /D_CRT_SECURE_NO_WARNINGS /DWIN32 /D_WINDOWS ' +
         '/DUNICODE /D_UNICODE ' + $includes + ' ' + ($test.Sources -join ' ') +
         ' /Fo:"' + $objDir + '\\" /Fe:"' + $exe + '" ' +
         '/link /SUBSYSTEM:CONSOLE ' + $test.Libs)
  $cmdLine = 'call "' + $vcvars + '" >nul && cd /d "' + $root + '" && ' + $cl
  Write-Host "[build] $name"
  $prevEap = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  $buildOutput = & cmd.exe /d /s /c $cmdLine 2>&1
  $buildExit = $LASTEXITCODE
  $ErrorActionPreference = $prevEap
  $buildOutput | Set-Content -LiteralPath $log -Encoding utf8
  if ($buildExit -ne 0)
  {
    Write-Host ($buildOutput | Out-String)
    Write-Host "FAIL $name build (exit $buildExit), log: $log"
    $anyFailure = $true
    continue
  }
  if (-not (Test-Path -LiteralPath $exe))
  {
    Write-Host "FAIL $name build produced no exe, log: $log"
    $anyFailure = $true
    continue
  }

  # freshness：exe 在构建前被删除、构建后重新出现（本次真实产物），且构建日志非空。
  $exeExistsAfterBuild = Test-Path -LiteralPath $exe
  $logNonEmpty = Test-Path -LiteralPath $log
  if (-not $exeWasRemovedBeforeBuild -or -not $exeExistsAfterBuild -or -not $logNonEmpty)
  {
    Write-Host ("FAIL $name freshness: removed={0} exists={1} log={2} exe=[{3}] logPath=[{4}]" -f $exeWasRemovedBeforeBuild, $exeExistsAfterBuild, $logNonEmpty, $exe, $log)
    $anyFailure = $true
    continue
  }

  Write-Host "[run] $exe"
  $output = & $exe 2>&1
  $code = $LASTEXITCODE
  $outputText = ($output | Out-String)
  Write-Host $outputText
  $output | Set-Content -LiteralPath (Join-Path $build ($name + '-run.log')) -Encoding utf8

  if ($code -ne 0 -or $outputText -notmatch 'RESULT PASS \(0 failures\)')
  {
    Write-Host "FAIL $name run (exit $code)"
    $anyFailure = $true
    continue
  }
  $checkFailed = $false
  foreach ($expected in $test.Expected)
  {
    if ($outputText -notmatch [regex]::Escape($expected))
    {
      Write-Host "FAIL $name missing expected check: $expected"
      $checkFailed = $true
    }
  }
  if ($checkFailed)
  {
    $anyFailure = $true
  }
  else
  {
    Write-Host "PASS $name"
  }
}

if ($anyFailure) { exit 1 }
Write-Host '[security-lifecycle] ALL PASS'
exit 0
