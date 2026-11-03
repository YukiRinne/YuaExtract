param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release'
)

# 只构建本测试自己的独立工程，输出固定到
#   _verify\restore-harness\_build\security-extension
# 不触碰解决方案默认 Release / 其它代理的构建产物。
$ErrorActionPreference = 'Stop'

$project = Join-Path $PSScriptRoot 'security-extension-tests.vcxproj'
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
if (-not (Test-Path $msbuild)) {
    Write-Error "MSBuild v143 not found: $msbuild"
    exit 2
}

& $msbuild $project /t:Rebuild /p:Configuration=$Configuration /p:Platform=Win32 /v:minimal /nologo
$code = $LASTEXITCODE
if ($code -ne 0) {
    Write-Error "security-extension test build failed with exit code $code"
}
exit $code
