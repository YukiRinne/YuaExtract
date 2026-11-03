param(
  [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$outRoot = Join-Path $repo '_verify\restore-harness\_build\security-core'
$bin = Join-Path $outRoot 'bin'
$obj = Join-Path $outRoot 'obj'
$logs = Join-Path $outRoot 'logs'
New-Item -ItemType Directory -Force -Path $bin, $obj, $logs | Out-Null

$vcvars = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat'
$includes = @(
  (Join-Path $repo 'Common'),
  (Join-Path $repo 'CxdecExtractorLoader'),
  (Join-Path $repo 'CxdecExtractor'),
  (Join-Path $repo 'KrkrPlugin'),
  (Join-Path $repo 'Detours'),
  (Join-Path $repo 'CxdecPeUnpacker'),
  (Join-Path $repo 'CxdecRepacker'),
  (Join-Path $repo 'CxdecRepacker\third_party\zlib'),
  (Join-Path $repo 'CxdecKeyStatic')
)
$incArgs = ($includes | ForEach-Object { "/I`"$_`"" }) -join ' '
$defs = '/D_CRT_SECURE_NO_WARNINGS /D_WIN32_WINNT=0x0601 /DWIN32 /D_UNICODE /DUNICODE'
$cflags = "/nologo /std:c++20 /utf-8 /EHsc /MT /O2 /W3 $defs $incArgs"

$zlibSources = @(
  'adler32.c','compress.c','crc32.c','deflate.c','infback.c','inffast.c','inflate.c',
  'inftrees.c','trees.c','uncompr.c','zutil.c'
) | ForEach-Object { Join-Path $repo "CxdecRepacker\third_party\zlib\$_" }

$commonPath = Join-Path $repo 'Common\path.cpp'

$tests = @(
  @{
    Name = 'security-core-path-test'
    Sources = @((Join-Path $PSScriptRoot 'security-core-path-test.cpp'),
                (Join-Path $repo 'CxdecExtractorLoader\YuaExtractWorkspace.cpp'),
                $commonPath)
    Libs = @()
  },
  @{
    Name = 'security-core-restorer-test'
    Sources = @((Join-Path $PSScriptRoot 'security-core-restorer-test.cpp'),
                (Join-Path $repo 'CxdecExtractorLoader\HashNameMiner.cpp'),
                (Join-Path $repo 'CxdecExtractorLoader\Blake2s.cpp'),
                (Join-Path $repo 'CxdecExtractorLoader\YuaExtractWorkspace.cpp'),
                $commonPath)
    Libs = @('ole32.lib')
  },
  @{
    Name = 'security-core-xp3-test'
    Sources = @((Join-Path $PSScriptRoot 'security-core-xp3-test.cpp'),
                (Join-Path $repo 'CxdecRepacker\core\xp3.cpp'),
                (Join-Path $repo 'CxdecRepacker\core\pack.cpp'),
                (Join-Path $repo 'CxdecRepacker\core\archive.cpp'),
                (Join-Path $repo 'CxdecRepacker\core\drip.cpp'),
                (Join-Path $repo 'CxdecRepacker\core\filter.cpp'),
                (Join-Path $repo 'CxdecRepacker\core\mapping.cpp'),
                (Join-Path $repo 'CxdecRepacker\core\resource_hash.cpp'),
                (Join-Path $repo 'CxdecRepacker\core\text_scramble.cpp'),
                (Join-Path $repo 'CxdecRepacker\core\tjs_variant.cpp'),
                (Join-Path $repo 'CxdecRepacker\crypto\aead.cpp'),
                (Join-Path $repo 'CxdecRepacker\crypto\blake2s.cpp'),
                (Join-Path $repo 'CxdecRepacker\crypto\chacha.cpp'),
                (Join-Path $repo 'CxdecRepacker\crypto\checksum.cpp'),
                (Join-Path $repo 'CxdecRepacker\crypto\poly1305.cpp'),
                (Join-Path $repo 'CxdecRepacker\crypto\sha3.cpp'),
                (Join-Path $repo 'CxdecRepacker\crypto\siphash.cpp'),
                (Join-Path $repo 'CxdecKeyStatic\Crypto\ZlibInflate.cpp')) + $zlibSources
    Libs = @()
  },
  @{
    Name = 'security-core-pe-test'
    Sources = @((Join-Path $PSScriptRoot 'security-core-pe-test.cpp'),
                (Join-Path $repo 'CxdecPeUnpacker\PeParser.cpp'),
                (Join-Path $repo 'CxdecPeUnpacker\PackerDetect.cpp'),
                (Join-Path $repo 'CxdecPeUnpacker\UnpackCore.cpp'),
                (Join-Path $repo 'CxdecPeUnpacker\CryptoUtil.cpp'),
                (Join-Path $repo 'CxdecKeyStatic\PeResource.cpp'))
    Libs = @('bcrypt.lib')
  },
  @{
    Name = 'security-core-stream-test'
    Sources = @((Join-Path $PSScriptRoot 'security-core-stream-test.cpp'),
                (Join-Path $repo 'CxdecExtractor\ExtractCore.cpp'),
                (Join-Path $repo 'Common\directory.cpp'),
                (Join-Path $repo 'Common\encoding.cpp'),
                (Join-Path $repo 'Common\file.cpp'),
                (Join-Path $repo 'Common\log.cpp'),
                $commonPath,
                (Join-Path $repo 'Common\pe.cpp'),
                (Join-Path $repo 'Common\stringhelper.cpp'),
                (Join-Path $repo 'Common\util.cpp'),
                (Join-Path $repo 'KrkrPlugin\tp_stub.cpp'))
    Libs = @('advapi32.lib', 'ole32.lib', 'user32.lib', 'shell32.lib')
  }
)

$overall = 0
foreach ($test in $tests) {
  $name = $test.Name
  $exe = Join-Path $bin "$name.exe"
  Write-Host "=== build $name"
  if (-not $NoBuild) {
    $sources = ($test.Sources | ForEach-Object { "`"$_`"" }) -join ' '
    $libs = ($test.Libs -join ' ')
    $objDir = Join-Path $obj $name
    New-Item -ItemType Directory -Force -Path $objDir | Out-Null
    $command = "`"$vcvars`" x86 >nul 2>nul && cl $cflags /Fo`"$objDir\\`" /Fe`"$exe`" $sources /link /SUBSYSTEM:CONSOLE $libs"
    cmd.exe /c $command 2>&1 | Tee-Object -FilePath (Join-Path $logs "$name.build.log")
    if ($LASTEXITCODE -ne 0) {
      Write-Host "BUILD FAILED $name exit=$LASTEXITCODE"
      $overall = 1
      continue
    }
  }

  $scratch = Join-Path $outRoot "scratch\$name"
  if (Test-Path $scratch) { Remove-Item -Recurse -Force $scratch }
  New-Item -ItemType Directory -Force -Path $scratch | Out-Null
  $stdout = Join-Path $logs "$name.stdout.log"
  if ($name -eq 'security-core-stream-test') {
    & $exe 2>&1 | Tee-Object -FilePath $stdout
  } else {
    & $exe $scratch 2>&1 | Tee-Object -FilePath $stdout
  }
  $code = $LASTEXITCODE
  Write-Host "$name exit=$code stdout=$stdout"
  if ($code -ne 0) { $overall = 1 }
}
exit $overall
