# 构建「夹具哈希真值探针」HashProbe.exe（_verify\restore-harness\Run\Host\HashProbe.exe）。
#
#   pwsh -File build-hashprobe.ps1            # 已是最新就跳过
#   pwsh -File build-hashprobe.ps1 -Force     # 强制重编
#
# 它把**产品自己的** CxdecRepacker\core\resource_hash.cpp + crypto\blake2s.cpp + crypto\siphash.cpp
# 编进一个控制台小程序，于是"夹具算的哈希 == 产品算的哈希"由编译器保证。
#
# 硬约束（lead 明确要求）：**不跑 MSBuild 编解决方案**（会与实现者抢 Release\ 并撞 LNK1104）。
# 这里只由 pwsh 直接调 cl.exe 编 3 个 .cpp，产物写进脚手架自己的 Run\Host\，
# 不碰 Release\、不碰任何工程文件。

[CmdletBinding()]
param([switch]$Force)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $root '..\..'))
$source = Join-Path $root 'src\HashProbe.cpp'
$productHash = Join-Path $repoRoot 'CxdecRepacker\core\resource_hash.cpp'
$blake2s = Join-Path $repoRoot 'CxdecRepacker\crypto\blake2s.cpp'
$siphash = Join-Path $repoRoot 'CxdecRepacker\crypto\siphash.cpp'
$includeDir = Join-Path $repoRoot 'CxdecRepacker'

$hostDir = Join-Path $root 'Run\Host'
$objDir = Join-Path $root 'Run\obj'
$exePath = Join-Path $hostDir 'HashProbe.exe'

foreach ($path in @($source, $productHash, $blake2s, $siphash))
{
    if (-not (Test-Path -LiteralPath $path)) { throw "缺少源文件：$path" }
}
foreach ($directory in @($hostDir, $objDir))
{
    if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
}

$needBuild = $true
if (-not $Force -and (Test-Path -LiteralPath $exePath))
{
    $newestSource = (Get-Item -LiteralPath $source).LastWriteTimeUtc
    foreach ($path in @($productHash, $blake2s, $siphash))
    {
        $stamp = (Get-Item -LiteralPath $path).LastWriteTimeUtc
        if ($stamp -gt $newestSource) { $newestSource = $stamp }
    }
    $needBuild = (Get-Item -LiteralPath $exePath).LastWriteTimeUtc -lt $newestSource
}

if (-not $needBuild)
{
    Write-Host '[build] HashProbe.exe 已是最新，跳过（-Force 可强制重编）'
}
else
{
    $vcvars = Resolve-VcVarsPath
    if (-not $vcvars) { throw '找不到 vcvars32.bat；请确认已安装 VS 2022 BuildTools 的 C++ 工具链' }
    Write-Host "[build] 导入构建环境：$vcvars"
    $count = Import-VsBuildEnvironment -VcVarsPath $vcvars
    Write-Host "[build] 环境变量 $count 条"

    $cl = (Get-Command cl.exe -ErrorAction SilentlyContinue)
    if (-not $cl) { throw 'vcvars 导入后仍找不到 cl.exe' }
    Write-Host "[build] 编译器：$($cl.Source)"

    if (Test-Path -LiteralPath $exePath) { Remove-Item -LiteralPath $exePath -Force }
    Get-ChildItem -LiteralPath $objDir -Filter 'HashProbe*.obj' -ErrorAction SilentlyContinue |
        Remove-Item -Force -ErrorAction SilentlyContinue

    # 注意：/Fo 结尾必须是 '\'，否则 cl 会把所有 obj 写进同一个文件名。
    $arguments = @(
        '/nologo'
        '/std:c++20'
        '/utf-8'
        '/W3'
        '/O2'
        '/MT'
        '/EHsc'
        '/DUNICODE'
        '/D_UNICODE'
        ('/I' + $includeDir)
        ('/Fo' + $objDir + '\')
        ('/Fe' + $exePath)
        $source
        $productHash
        $blake2s
        $siphash
        '/link'
        '/SUBSYSTEM:CONSOLE'
        '/INCREMENTAL:NO'
    )

    Write-Host "[build] cl.exe $($arguments -join ' ')"
    & $cl.Source @arguments
    if ($LASTEXITCODE -ne 0) { throw "cl.exe 失败，退出码 $LASTEXITCODE" }
}

if (-not (Test-Path -LiteralPath $exePath)) { throw "没有产出 $exePath" }
Write-Host ("[build] 产物：{0}  ({1:N0} 字节, {2})" -f $exePath, (Get-Item -LiteralPath $exePath).Length, (Format-PeMachine (Get-PeMachine -Path $exePath)))
Write-Host '[build] OK：夹具哈希真值探针已就绪'
