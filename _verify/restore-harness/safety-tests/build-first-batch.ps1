param([string]$Config = 'Debug', [string]$Platform = 'Win32')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path.TrimEnd('\') + '\'
$msb = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
$base = Join-Path $root '_verify\restore-harness\_build\security-core\msbuild'
New-Item -ItemType Directory -Force -Path $base | Out-Null
$projects = @(
  'CxdecKeyStatic\CxdecKeyStatic.vcxproj',
  'CxdecStringDumper\CxdecStringDumper.vcxproj',
  'CxdecExtractor\CxdecExtractor.vcxproj',
  'CxdecPeUnpacker\CxdecPeUnpacker.vcxproj',
  'CxdecRepacker\CxdecRepacker.vcxproj'
)
$failed = 0
foreach ($p in $projects) {
  $name = [IO.Path]::GetFileNameWithoutExtension($p)
  Write-Host "=== $name"
  $projectConfig = $Config
  # CxdecRepacker 的 Debug 配置目前是 /ZI（其它 scope 的工程设置），与 /guard:cf 冲突；
  # 本批改用 Release 验证 core/xp3.cpp / core/pack.cpp。
  if ($name -eq 'CxdecRepacker') { $projectConfig = 'Release' }
  $outDir = Join-Path $base "$name\"
  $intDir = Join-Path $base "${name}Obj\"
  $arguments = @(
    (Join-Path $root $p),
    "/p:Configuration=$projectConfig",
    "/p:Platform=$Platform",
    "/p:SolutionDir=$root",
    "/p:OutDir=$outDir",
    "/p:IntDir=$intDir",
    '/m', '/v:minimal', '/nologo'
  )
  & $msb @arguments
  if ($LASTEXITCODE -ne 0) { $failed = $LASTEXITCODE; Write-Host "FAILED $name exit=$LASTEXITCODE" } else { Write-Host "OK $name" }
}
exit $failed
