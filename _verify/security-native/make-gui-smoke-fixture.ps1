# 生成 GUI smoke 用的**纯数据**夹具（不启动 GUI、不执行游戏/原生 DLL、不联网）。
#
# 默认根：<repo>\_verify\restore-harness\Run\security-gui-smoke
# 每次运行都会在该根下新建唯一子目录（run-<时间>-<随机>），绝不删除此前运行的内容。
#
# 产出：
#   <run>\game.exe                                （当前 Release\CxdecExtractordll\CxdecCli.exe 的拷贝，仅作 PE 输入）
#   <run>\Extractor_Output\pack\0123456789ABCDEF\{A,B,C}  （64 位 hex 文件；PNG magic 小字节）
#   <run>\Extractor_Output\pack\readme.txt
#   <run>\StringHashDumper_Output\gui-smoke.lst  （UTF-8：16 位目录 -> /，D/A/B -> 真名；C 无映射）
#   <run>\expected.json                          （绝对路径、fileRel、sha256，供 main GUI 验收）
#
# -Packages N 额外生成 N 个 pkgNNNN 小包（每包 2 known + 1 unknown + readme），供排队/取消。
#
# 用法：
#   pwsh -File _verify\security-native\make-gui-smoke-fixture.ps1
#   pwsh -File ...\make-gui-smoke-fixture.ps1 -Packages 200
#   pwsh -File ...\make-gui-smoke-fixture.ps1 -Root <其它临时根>

param(
    [int]$Packages = 0,
    [string]$Root = ''
)

$ErrorActionPreference = 'Stop'

$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if ([string]::IsNullOrWhiteSpace($Root)) {
    $Root = Join-Path $repo '_verify\restore-harness\Run\security-gui-smoke'
}
$Root = [IO.Path]::GetFullPath($Root)
New-Item -ItemType Directory -Force -Path $Root | Out-Null

$cliSource = Join-Path $repo 'Release\CxdecExtractordll\CxdecCli.exe'
if (-not (Test-Path $cliSource)) {
    throw "找不到 PE 输入源：$cliSource"
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$utf8Bom = New-Object System.Text.UTF8Encoding($true)

function New-RandomHex([int]$byteCount) {
    $bytes = New-Object byte[] $byteCount
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try { $rng.GetBytes($bytes) } finally { $rng.Dispose() }
    return ([BitConverter]::ToString($bytes)).Replace('-', '')
}

function New-Hash64([string[]]$exclude) {
    do {
        $h = New-RandomHex 32
    } while ($exclude -contains $h)
    return $h
}

function Get-Sha256([byte[]]$bytes) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { $hash = $sha.ComputeHash($bytes) } finally { $sha.Dispose() }
    return ([BitConverter]::ToString($hash)).Replace('-', '').ToLowerInvariant()
}

function New-PngBytes([byte]$tag, [int]$payloadLength) {
    $magic = [byte[]](0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)
    $payload = New-Object byte[] $payloadLength
    for ($i = 0; $i -lt $payload.Length; ++$i) { $payload[$i] = [byte](($tag + $i) -band 0xFF) }
    return [byte[]]($magic + $payload)
}

function Write-Bytes([string]$path, [byte[]]$bytes) {
    $dir = Split-Path -Parent $path
    if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [IO.File]::WriteAllBytes($path, $bytes)
}

$runId = 'run-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' +
         ([guid]::NewGuid().ToString('N').Substring(0, 8))
$runRoot = Join-Path $Root $runId
New-Item -ItemType Directory -Path $runRoot | Out-Null

$gamePath = Join-Path $runRoot 'game.exe'
Copy-Item -LiteralPath $cliSource -Destination $gamePath
$peHeader = New-Object byte[] 2
$fs = [IO.File]::OpenRead($gamePath)
try { [void]$fs.Read($peHeader, 0, 2) } finally { $fs.Dispose() }
if ($peHeader[0] -ne 0x4D -or $peHeader[1] -ne 0x5A) {
    throw "拷贝的 PE 输入不是 MZ 文件：$gamePath"
}

# ==== 固定目录 Hash / 随机 64 位文件 Hash ====
$dirHash = '0123456789ABCDEF'
$hashA = New-Hash64 @()
$hashB = New-Hash64 @($hashA)
$hashC = New-Hash64 @($hashA, $hashB)
$hashD = New-Hash64 @($hashA, $hashB, $hashC)   # 仅映射诱饵，没有对应源文件

$bytesA = New-PngBytes 0x11 24
$bytesB = New-PngBytes 0x22 32
$bytesC = New-PngBytes 0x33 40
$readmeText = "YuaExtract GUI smoke fixture (synthetic data only).`r`n" +
              "run=$runId`r`n" +
              "No real game files, no native execution, no network.`r`n"
$bytesReadme = $utf8NoBom.GetBytes($readmeText)

$extractDir = Join-Path $runRoot 'Extractor_Output'
$hashDir = Join-Path $runRoot 'StringHashDumper_Output'
$restoreDir = Join-Path $runRoot 'Restored_Extractor_Output'
$packDir = Join-Path $extractDir 'pack'
$domainDir = Join-Path $packDir $dirHash

Write-Bytes (Join-Path $domainDir $hashA) $bytesA
Write-Bytes (Join-Path $domainDir $hashB) $bytesB
Write-Bytes (Join-Path $domainDir $hashC) $bytesC
Write-Bytes (Join-Path $packDir 'readme.txt') $bytesReadme

New-Item -ItemType Directory -Force -Path $hashDir | Out-Null
$lstPath = Join-Path $hashDir 'gui-smoke.lst'
$lstText = @(
    '# HASH:name  | 16-hex = directory, 64-hex = file',
    ('{0}:/' -f $dirHash),
    ('{0}:bgimage' -f $hashD),
    ('{0}:known1.png' -f $hashA),
    ('{0}:中文名.txt' -f $hashB)
) -join "`r`n"
$lstText += "`r`n"
[IO.File]::WriteAllText($lstPath, $lstText, $utf8Bom)

$shaA = Get-Sha256 $bytesA
$shaB = Get-Sha256 $bytesB
$shaC = Get-Sha256 $bytesC
$shaReadme = Get-Sha256 $bytesReadme

function New-ExpectedEntry([string]$fileRel, [string]$sha) {
    return [ordered]@{ fileRel = $fileRel; sha256 = $sha; sha = $sha }
}

# 默认模式只保留"名字全是真名"的文件：
#   pack/known1.png   <- A
#   pack/中文名.txt    <- B
#   pack/readme.txt   <- 明文直通
# C（64 位 Hash）默认被丢弃；D->bgimage 是映射诱饵，没有源文件。
$expectedDefault = @(
    (New-ExpectedEntry 'pack/known1.png' $shaA),
    (New-ExpectedEntry 'pack/中文名.txt' $shaB),
    (New-ExpectedEntry 'pack/readme.txt' $shaReadme)
)
$expectedKeep = @(
    (New-ExpectedEntry 'pack/known1.png' $shaA),
    (New-ExpectedEntry 'pack/中文名.txt' $shaB),
    (New-ExpectedEntry ('pack/' + $hashC + '.png') $shaC),
    (New-ExpectedEntry 'pack/readme.txt' $shaReadme)
)
$sourceFiles = @(
    (New-ExpectedEntry ('Extractor_Output/pack/' + $dirHash + '/' + $hashA) $shaA),
    (New-ExpectedEntry ('Extractor_Output/pack/' + $dirHash + '/' + $hashB) $shaB),
    (New-ExpectedEntry ('Extractor_Output/pack/' + $dirHash + '/' + $hashC) $shaC),
    (New-ExpectedEntry 'Extractor_Output/pack/readme.txt' $shaReadme)
)

# ==== 可选的 200 个小包（排队/取消用）====
$packageNames = @()
if ($Packages -gt 0) {
    for ($i = 1; $i -le $Packages; ++$i) {
        $name = 'pkg{0:D4}' -f $i
        $packageNames += $name
        $pkgRoot = Join-Path $extractDir $name
        $pkgDomain = Join-Path $pkgRoot $dirHash
        Write-Bytes (Join-Path $pkgDomain $hashA) $bytesA
        Write-Bytes (Join-Path $pkgDomain $hashB) $bytesB
        Write-Bytes (Join-Path $pkgDomain $hashC) $bytesC
        Write-Bytes (Join-Path $pkgRoot 'readme.txt') $bytesReadme
    }
}

$perPackageDefault = @(
    (New-ExpectedEntry 'known1.png' $shaA),
    (New-ExpectedEntry '中文名.txt' $shaB),
    (New-ExpectedEntry 'readme.txt' $shaReadme)
)
$perPackageKeep = @(
    (New-ExpectedEntry 'known1.png' $shaA),
    (New-ExpectedEntry '中文名.txt' $shaB),
    (New-ExpectedEntry ($hashC + '.png') $shaC),
    (New-ExpectedEntry 'readme.txt' $shaReadme)
)

$mappings = [ordered]@{}
$mappings[$dirHash] = '/'
$mappings[$hashD] = 'bgimage'
$mappings[$hashA] = 'known1.png'
$mappings[$hashB] = '中文名.txt'

$expected = [ordered]@{
    schemaVersion = 1
    hashAlgorithm = 'SHA-256'
    runId = $runId
    synthetic = $true
    invokedNative = $false
    networkUsed = $false
    absoluteRoot = $runRoot
    gamePath = $gamePath
    dirpaths = [ordered]@{
        extractDir = $extractDir
        hashDir = $hashDir
        restoreDir = $restoreDir
        packDir = $packDir
        domainDir = $domainDir
        basePackPackage = 'pack'
        listFile = $lstPath
        restoreReport = (Join-Path $restoreDir 'RestoreReport.txt')
    }
    hashes = [ordered]@{
        directory = $dirHash
        A = $hashA
        B = $hashB
        unknownC = $hashC
        decoyD = $hashD
    }
    mappings = $mappings
    expected = [ordered]@{
        default = $expectedDefault
        keepUnknown = $expectedKeep
        defaultFileCount = 3
        keepUnknownFileCount = 4
        decoyMappingWithoutSource = 'bgimage'
    }
    source = $sourceFiles
    sourceUnchanged = $sourceFiles
    packages = [ordered]@{
        count = $Packages
        packageNames = $packageNames
        perPackage = [ordered]@{
            default = $perPackageDefault
            keepUnknown = $perPackageKeep
            outputFileRelTemplate = '<packageName>/<fileRel>'
        }
    }
}

$expectedPath = Join-Path $runRoot 'expected.json'
$json = $expected | ConvertTo-Json -Depth 16
[IO.File]::WriteAllText($expectedPath, $json, $utf8NoBom)

Write-Host "FIXTURE_ROOT=$runRoot"
Write-Host "GAME_PATH=$gamePath"
Write-Host "EXPECTED_JSON=$expectedPath"
Write-Host "PACKAGES=$Packages"
