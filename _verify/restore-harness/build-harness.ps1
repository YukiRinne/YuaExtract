# 构建验证用的「空宿主」EXE（_verify\restore-harness\Run\Host\EmptyHost.exe）。
#
#   pwsh -File build-harness.ps1            # 已是最新就跳过
#   pwsh -File build-harness.ps1 -Force     # 强制重编
#
# 编译方式：先用一个放在无空格/无 '&' 目录下的临时批处理把 vcvars32.bat 的环境
# 导进当前 pwsh 会话，然后由 pwsh 直接调用 cl.exe。
# 之所以不在 cmd 里拼路径：本仓库路径含 '&'，cmd 会把它当成命令分隔符。

[CmdletBinding()]
param([switch]$Force)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')

$source = Join-Path $root 'src\EmptyHost.cpp'
$hostDir = Join-Path $root 'Run\Host'
$objDir = Join-Path $root 'Run\obj'
$exePath = Join-Path $hostDir 'EmptyHost.exe'
$objPath = Join-Path $objDir 'EmptyHost.obj'

if (-not (Test-Path -LiteralPath $source)) { throw "缺少源文件：$source" }
foreach ($directory in @($hostDir, $objDir))
{
    if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
}

$needBuild = $true
if (-not $Force -and (Test-Path -LiteralPath $exePath))
{
    $needBuild = (Get-Item -LiteralPath $exePath).LastWriteTimeUtc -lt (Get-Item -LiteralPath $source).LastWriteTimeUtc
}

if (-not $needBuild)
{
    Write-Host "[build] EmptyHost.exe 已是最新，跳过（-Force 可强制重编）"
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
    if (Test-Path -LiteralPath $objPath) { Remove-Item -LiteralPath $objPath -Force }

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
        ('/Fo' + $objPath)
        ('/Fe' + $exePath)
        $source
        '/link'
        '/SUBSYSTEM:WINDOWS'
        '/INCREMENTAL:NO'
        'user32.lib'
        'shell32.lib'
    )

    Write-Host "[build] cl.exe $($arguments -join ' ')"
    & $cl.Source @arguments
    if ($LASTEXITCODE -ne 0) { throw "cl.exe 失败，退出码 $LASTEXITCODE" }
}

if (-not (Test-Path -LiteralPath $exePath)) { throw "没有产出 $exePath" }

$machine = Get-PeMachine -Path $exePath
Write-Host ("[build] 产物：{0}  ({1:N0} 字节, {2})" -f $exePath, (Get-Item -LiteralPath $exePath).Length, (Format-PeMachine $machine))
if ($machine -ne 0x014C)
{
    throw ("EmptyHost.exe 不是 32 位（{0}）。注入型 DLL 是 x86，宿主必须同为 x86。" -f (Format-PeMachine $machine))
}
Write-Host '[build] OK：32 位宿主已就绪'
