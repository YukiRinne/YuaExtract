# 扩展集一期夹具生成器（task-3）：造"旧作采集成果 + 新解包目录"，并给出**独立期望值清单**。
#
#   pwsh -File make-extension-fixture.ps1
#   pwsh -File make-extension-fixture.ps1 -SkipHashProbe      # 没装 VS 时不编 HashProbe（少一重对账）
#   pwsh -File make-extension-fixture.ps1 -ExtraFiles 2000    # 规模档：额外铺 N 个可还原文件（量 E-8 体积）
#
# 产出（全部在 _verify\restore-harness\Run\ 下，**不碰 Run\FakeWorkspace 与 Run\manifest.json**）：
#   Run\ExtensionFixture\OldGame\           "旧作"工作区：采集日志 + 旧解包目录 + 盐
#   Run\ExtensionFixture\RawGame\           E-2 专用：采集日志里的目录名**故意没归一化**（哈希按原样算）
#   Run\ExtensionFixture\OtherGame\         E-4 专用：**另一个盐**的旧作（盐不匹配必须被拒）
#   Run\ExtensionFixture\Mode1Game\         模式1 工作区：解包目录已预置（合成工作区没有真游戏）
#   Run\ExtensionFixture\NewUnpacked\       "新解包目录"（纯哈希名：可还原 / 不可还原两类）
#   Run\extension-manifest.json             独立期望值清单（含逐条明细）
#   Run\extension-manifest.tsv              逐条明细（列口径与既有 manifest.tsv 前 8 列一致）
#
# 哈希真值：lib\hash-extension.py 计算（BLAKE2s-256 / SipHash-2-4+bswap），
# 再用 Run\Host\HashProbe.exe（**产品自己的 resource_hash.cpp**）逐条交叉对账，不一致直接抛错。
# 这是为了不重演 README 踩坑 #14：夹具写错会让好实现看起来像没实现。
#
# 与产品的目录约定（docs\扩展集设计.md §9）：
#   工作区 = 游戏目录；StringHashDumper_Output = 采集成果；ExtensionDraft = 草案；
#   Extractor_Output = 解包（哈希名）；StaticHash_Output = 模式1 步骤1 产物；Restored_Extractor_Output = 还原结果。

[CmdletBinding()]
param(
    [string]$FixtureRoot = '',
    [switch]$SkipHashProbe,
    # 演示用：故意在 FileNameHash.log 里塞一条"重算不回去"的伪造哈希，
    # 用来证明 §3.2 生成期自校验真的会把不一致报成例外（默认关闭 → 正式口径例外 = 0）。
    [switch]$PlantFakeDup,
    [int]$ExtraFiles = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessNative.ps1')

$runRoot = Join-Path $root 'Run'
if (-not $FixtureRoot) { $FixtureRoot = Join-Path $runRoot 'ExtensionFixture' }
$manifestPath = Join-Path $runRoot 'extension-manifest.json'
$manifestTsvPath = Join-Path $runRoot 'extension-manifest.tsv'
$hashExtensionPy = Join-Path $root 'lib\hash-extension.py'
$rulesIntPy = Join-Path $root 'lib\rules-int.py'
$hashProbeExe = Join-Path $runRoot 'Host\HashProbe.exe'

foreach ($directory in @($runRoot, $FixtureRoot))
{
    if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
}
[void](Remove-TreeUnder -Path $FixtureRoot -MustBeUnder $runRoot)
New-Item -ItemType Directory -Path $FixtureRoot -Force | Out-Null

# ---------------- Python / HashProbe ----------------

function Resolve-Python
{
    $candidates = @(
        'C:\Users\Administrator\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
    )
    foreach ($candidate in $candidates) { if (Test-Path -LiteralPath $candidate) { return $candidate } }
    $found = Get-Command python -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    throw '找不到 Python 运行时（夹具侧算哈希需要它）'
}

$python = Resolve-Python
Write-Host ("[fixture] Python：{0}" -f $python)

$probeUsed = $false
if (-not $SkipHashProbe)
{
    if (-not (Test-Path -LiteralPath $hashProbeExe))
    {
        Write-Host '[fixture] 先生成夹具哈希真值探针 HashProbe.exe …'
        & pwsh -NoProfile -File (Join-Path $root 'build-hashprobe.ps1')
        if ($LASTEXITCODE -ne 0) { throw 'build-hashprobe.ps1 失败（可以 -SkipHashProbe 跳过这重对账）' }
    }
    $probeUsed = Test-Path -LiteralPath $hashProbeExe
}
if (-not $probeUsed) { Write-Host '[fixture] 警告：没有 HashProbe，夹具哈希只由 Python 自算（少一重产品侧对账）' }

# ---------------- 常量：盐 / 目录 / 名字 ----------------

$salt = 'xp3hnp'                 # 与既有补充还原夹具同口径（真实游戏 Log\Universal.log 的 Hash Seed）
$otherSalt = 'othersalt123'      # E-4：另一个游戏的盐
$negativeName = 'nosuch_999_9999'

# 目录（raw = 采集日志里可能出现的写法；norm = 产品归一化后的口径）
$dirs = [ordered]@{
    char   = [pscustomobject]@{ key = 'char';   raw = 'characters/yuki'; norm = 'characters/yuki/';  logName = 'characters/yuki' }
    ui     = [pscustomobject]@{ key = 'ui';     raw = 'ui';              norm = 'ui/';               logName = 'ui' }
    cg     = [pscustomobject]@{ key = 'cg';     raw = 'event/cg';        norm = 'event/cg/';         logName = 'event/cg' }
    nested = [pscustomobject]@{ key = 'nested'; raw = 'sub/dir/nested';  norm = 'sub/dir/nested/';   logName = 'sub/dir/nested' }
    root   = [pscustomobject]@{ key = 'root';   raw = '';                norm = '/';                 logName = '%EmptyString%' }
}

# 文件名 -> 采集来源（filenamehash / minednames / extra-lst / recovered-lst）
$fileSpecs = @(
    [pscustomobject]@{ name = 'base.png';           source = 'filenamehash' }
    [pscustomobject]@{ name = 'stand_a.png';        source = 'filenamehash' }
    [pscustomobject]@{ name = 'logo.png';           source = 'filenamehash' }
    [pscustomobject]@{ name = 'script.ks';          source = 'filenamehash' }
    [pscustomobject]@{ name = 'ev01.png';           source = 'filenamehash' }
    [pscustomobject]@{ name = 'ev02.png';           source = 'filenamehash' }
    [pscustomobject]@{ name = 'same.png';           source = 'filenamehash' }   # 采集日志里出现两次（去重口径）
    [pscustomobject]@{ name = 'miss1_payload.bin';  source = 'filenamehash' }
    [pscustomobject]@{ name = 'data_missing.bin';   source = 'filenamehash' }
    [pscustomobject]@{ name = 'anj_000_0001.ogg';   source = 'minednames' }
    [pscustomobject]@{ name = 'nj_005_0012.png';    source = 'minednames' }
    [pscustomobject]@{ name = 'bgm_010_0003.bin';   source = 'minednames' }
    [pscustomobject]@{ name = '街_道a_春.ogg';       source = 'minednames' }
    [pscustomobject]@{ name = 'manual_note.txt';    source = 'extra-lst' }
    [pscustomobject]@{ name = 'recovered_only.dat'; source = 'recovered-lst' }
)

# 新解包目录里"故意查不到映射"的两个哈希（既不在采集日志里，也不是任何名字的哈希）
$unmappedDirHash = (Get-TextSha256Hex -Text 'extension-fixture-unmapped-dir').Substring(0, 16)
$unmappedFileHash = Get-TextSha256Hex -Text 'extension-fixture-unmapped-file'
$unmappedFileHash2 = Get-TextSha256Hex -Text 'extension-fixture-unmapped-file-2'
# "同名两个哈希"里的第二个（伪造的哈希，只进采集日志，不进扩展集：档案里 FILES 只有名字）
$fakeDupHash = Get-TextSha256Hex -Text 'extension-fixture-fake-dup-hash-of-same.png'

# ---------------- 算哈希（Python + HashProbe 交叉对账） ----------------

$specPath = Join-Path $runRoot 'extension-hash-spec.tsv'
$specLines = New-Object System.Collections.ArrayList
[void]$specLines.Add("# kind`tinput`tmode（本文件由 make-extension-fixture.ps1 生成）")
foreach ($key in $dirs.Keys)
{
    $dir = $dirs[$key]
    if ($dir.key -eq 'root') { [void]$specLines.Add("ROOT`t`t") }
    else { [void]$specLines.Add(("DIR`t{0}`t" -f $dir.raw)) }
}
foreach ($spec in $fileSpecs) { [void]$specLines.Add(("FILE`t{0}`t" -f $spec.name)) }
# E-2 专用：未归一化的目录名（哈希按**原样**算，模拟"采集时没归一化"的日志）
$rawSpecs = @(
    [pscustomobject]@{ raw = 'Characters\Yuki' }
    [pscustomobject]@{ raw = 'UI' }
    [pscustomobject]@{ raw = 'Event\Cg' }
    [pscustomobject]@{ raw = 'sub\dir\nested' }
    [pscustomobject]@{ raw = 'voice\SE' }
)
foreach ($raw in $rawSpecs) { [void]$specLines.Add(("DIR`t{0}`traw" -f $raw.raw)) }
# E-4 专用：另一个游戏的盐（同名不同哈希）
[void]$specLines.Add("DIR`tcharacters/yuki`t")
[void]$specLines.Add("FILE`tbase.png`t")
Write-Utf8File -Path $specPath -Text (($specLines -join "`r`n") + "`r`n") -NoNewline

$hashOutPath = Join-Path $runRoot 'extension-hashes.tsv'
$probeSpec = Join-Path $runRoot 'extension-hash-spec-probe.tsv'
$probeOut = Join-Path $runRoot 'extension-hashes-probe.tsv'

# 注意顺序：先让 Python 算出哈希并**导出"最终要哈希的串"**，再喂给 HashProbe。
# HashProbe 只负责哈希、不负责归一化；把未归一化的目录名直接喂它，算的就是另一个串（踩过）。
& $python $hashExtensionPy --spec $specPath --out $hashOutPath --salt $salt --emit-probe-spec $probeSpec
if ($LASTEXITCODE -ne 0) { throw ("hash-extension.py 失败（退出码 {0}）" -f $LASTEXITCODE) }
if ($probeUsed)
{
    & $hashProbeExe $probeSpec $probeOut $salt
    if ($LASTEXITCODE -ne 0) { throw ("HashProbe 失败（退出码 {0}）" -f $LASTEXITCODE) }
    & $python $hashExtensionPy --spec $specPath --out $hashOutPath --salt $salt --crosscheck $probeOut
    if ($LASTEXITCODE -ne 0) { throw '夹具哈希与产品（HashProbe）对账失败：先修夹具，别去怀疑实现' }
}

# E-4 用的"另一个盐"
$specOther = Join-Path $runRoot 'extension-hash-spec-other.tsv'
Write-Utf8File -Path $specOther -Text ("DIR`tcharacters/yuki`t`r`nFILE`tbase.png`t`r`n") -NoNewline
$otherHashOut = Join-Path $runRoot 'extension-hashes-other.tsv'
$probeSpecOther = Join-Path $runRoot 'extension-hash-spec-other-probe.tsv'
$probeOther = Join-Path $runRoot 'extension-hashes-probe-other.tsv'
& $python $hashExtensionPy --spec $specOther --out $otherHashOut --salt $otherSalt --emit-probe-spec $probeSpecOther
if ($LASTEXITCODE -ne 0) { throw ("hash-extension.py（另一个盐）失败（退出码 {0}）" -f $LASTEXITCODE) }
if ($probeUsed)
{
    & $hashProbeExe $probeSpecOther $probeOther $otherSalt
    if ($LASTEXITCODE -ne 0) { throw ("HashProbe（另一个盐）失败（退出码 {0}）" -f $LASTEXITCODE) }
    & $python $hashExtensionPy --spec $specOther --out $otherHashOut --salt $otherSalt --crosscheck $probeOther
    if ($LASTEXITCODE -ne 0) { throw '另一个盐的哈希对账失败' }
}

# 解析哈希表：(KIND|输入) -> 哈希
#
# **必须用大小写敏感的字典**：PowerShell 的 @{} 默认大小写不敏感，
# 于是 `DIR|ui`（正常夹具）与 `DIR|UI`（E-2 的未归一化变体）会互相覆盖 ——
# 结果 `ui` 拿到的是 hash('UI')，采集日志里就被写了一个"重算不回去"的哈希，
# 产品生成期自校验如实报成例外（看起来像产品缺陷，其实是夹具的锅）。
function Read-HashTable([string]$Path)
{
    $table = New-Object 'System.Collections.Generic.Dictionary[string,string]' ([System.StringComparer]::Ordinal)
    foreach ($line in [System.IO.File]::ReadAllLines($Path, [System.Text.Encoding]::UTF8))
    {
        if (-not $line.Trim()) { continue }
        $parts = $line -split "`t"
        if ($parts.Count -lt 4) { continue }
        $table[('{0}|{1}' -f $parts[0], $parts[1])] = $parts[3]
    }
    return $table
}
$hashes = Read-HashTable -Path $hashOutPath
$hashesOther = Read-HashTable -Path $otherHashOut

function Get-FileHashHex([string]$Name) { return [string]$hashes[('FILE|' + $Name)] }
function Get-DirHashHex([string]$Raw) { return [string]$hashes[('DIR|' + $Raw)] }
$rootHash = [string]$hashes['ROOT|']
$otherDirHash = [string]$hashesOther['DIR|characters/yuki']
$otherFileHash = [string]$hashesOther['FILE|base.png']

foreach ($spec in $fileSpecs)
{
    if (-not (Get-FileHashHex $spec.name)) { throw ("Python/Probe 没算出 '{0}' 的哈希" -f $spec.name) }
}
if (-not $rootHash) { throw '没算出根域哈希' }

$fileHashByName = @{}
foreach ($spec in $fileSpecs) { $fileHashByName[$spec.name] = (Get-FileHashHex $spec.name) }
$dirHashByKey = @{}
foreach ($key in $dirs.Keys) { $dirHashByKey[$key] = (Get-DirHashHex ([string]$dirs[$key].raw)) }
# 根域的哈希走 ROOT 行（DIR|'' 查不到）—— 忘了这一步会让"根域目录"变成空串，
# 于是根域文件落到 `<包>\<64位哈希>`（包根裸哈希文件），口径就完全变了（踩过）。
$dirHashByKey['root'] = $rootHash
if (-not $dirHashByKey['root']) { throw '根域哈希为空：根域目录条目会退化成空串' }
$rawDirHashByRaw = @{}
foreach ($raw in $rawSpecs) { $rawDirHashByRaw[$raw.raw] = (Get-DirHashHex $raw.raw) }

Write-Host ("[fixture] 哈希就绪：目录 {0} 个 / 文件 {1} 个 / 根域 {2}" -f $dirs.Count, $fileSpecs.Count, $rootHash)

# ---------------- 写文件的小工具 ----------------

function Write-Payload([string]$Path, [int]$Size, [int]$Seed)
{
    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    if ($Size -le 1024)
    {
        $bytes = New-Object byte[] $Size
        for ($index = 0; $index -lt $bytes.Length; $index++) { $bytes[$index] = [byte](($index * 31 + $Seed) -band 0xFF) }
        [System.IO.File]::WriteAllBytes($Path, $bytes)
    }
    else
    {
        [System.IO.File]::WriteAllBytes($Path, (New-DeterministicBytes -Seed $Seed -Length $Size))
    }
}

function Write-Utf16Log([string]$Path, [string[]]$Lines)
{
    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    $encoding = New-Object System.Text.UnicodeEncoding($false, $true)
    [System.IO.File]::WriteAllText($Path, ((($Lines -join "`r`n")) + "`r`n"), $encoding)
}

# ---------------- 新解包目录（模式1 步骤3 的对象） ----------------

$newUnpacked = Join-Path $FixtureRoot 'NewUnpacked'
$filePlan = New-Object System.Collections.ArrayList

function Add-NewFile
{
    param(
        [string]$Category, [string]$Rel, [string]$Expected, [string]$KnownDir, [string]$KnownFile,
        [int]$Size, [int]$Seed, [string]$Note, [string]$Source, [string]$DirHash, [string]$FileHash
    )
    [void]$filePlan.Add([pscustomobject]@{
            Category = $Category; Rel = $Rel; Expected = $Expected; KnownDir = $KnownDir; KnownFile = $KnownFile
            Size = $Size; Seed = $Seed; Note = $Note; Source = $Source; DirHash = $DirHash; FileHash = $FileHash
        })
}

# 可还原（普通采集）
$restoredSpecs = @(
    [pscustomobject]@{ pkg = 'bgimage'; dirKey = 'char';   file = 'base.png';          size = 3072;  seed = 2001 }
    [pscustomobject]@{ pkg = 'bgimage'; dirKey = 'char';   file = 'stand_a.png';       size = 1;     seed = 2002 }
    [pscustomobject]@{ pkg = 'bgimage'; dirKey = 'ui';     file = 'logo.png';          size = 0;     seed = 2003 }
    [pscustomobject]@{ pkg = 'data';    dirKey = 'root';   file = 'script.ks';         size = 65536; seed = 2004 }
    [pscustomobject]@{ pkg = 'evimage'; dirKey = 'cg';     file = 'ev01.png';          size = 4096;  seed = 2005 }
    [pscustomobject]@{ pkg = 'evimage'; dirKey = 'cg';     file = 'ev02.png';          size = 12345; seed = 2006 }
)
foreach ($spec in $restoredSpecs)
{
    $dir = $dirs[$spec.dirKey]
    $hash = [string]$fileHashByName[$spec.file]
    $dirHash = [string]$dirHashByKey[$spec.dirKey]
    $rel = if ($spec.dirKey -eq 'root') { '{0}\{1}\{2}' -f $spec.pkg, $dirHash, $hash } else { '{0}\{1}\{2}' -f $spec.pkg, $dirHash, $hash }
    # 期望落点一律用 **Windows 分隔符**：档案里的目录名是 `characters/yuki/`（§3 的规范化形态），
    # 但还原到磁盘是 `characters\yuki\`。期望值写成正斜杠会得到"文件明明在却报缺少"的假失败。
    $dirForPath = ([string]$dir.norm).TrimEnd('/') -replace '/', '\'
    $expected = if ($spec.dirKey -eq 'root') { '{0}\{1}' -f $spec.pkg, $spec.file } else { '{0}\{1}\{2}' -f $spec.pkg, $dirForPath, $spec.file }
    Add-NewFile -Category 'restored' -Rel $rel -Expected $expected -KnownDir ([string]$dir.norm) -KnownFile $spec.file `
        -Size $spec.size -Seed $spec.seed -Note ('目录与文件名都来自采集日志（来源 {0}）' -f ($fileSpecs | Where-Object { $_.name -eq $spec.file })[0].source) `
        -Source ([string]($fileSpecs | Where-Object { $_.name -eq $spec.file })[0].source) -DirHash $dirHash -FileHash $hash
}

# 可还原（名字来自挖掘成果 MinedNames.lst / 手工 *.lst）
$minedSpecs = @(
    [pscustomobject]@{ pkg = 'scn';   dirKey = 'char';   file = 'anj_000_0001.ogg';  size = 2048; seed = 3001; category = 'mined' }
    [pscustomobject]@{ pkg = 'scn';   dirKey = 'char';   file = 'nj_005_0012.png';   size = 1536; seed = 3002; category = 'mined' }
    [pscustomobject]@{ pkg = 'scn';   dirKey = 'nested'; file = 'bgm_010_0003.bin';  size = 512;  seed = 3003; category = 'mined' }
    [pscustomobject]@{ pkg = 'scn';   dirKey = 'char';   file = '街_道a_春.ogg';      size = 1024; seed = 3004; category = 'mined-optional' }
    [pscustomobject]@{ pkg = 'extra'; dirKey = 'ui';     file = 'manual_note.txt';   size = 640;  seed = 3005; category = 'manual' }
    [pscustomobject]@{ pkg = 'extra'; dirKey = 'cg';     file = 'recovered_only.dat'; size = 700; seed = 3006; category = 'recovered' }
)
foreach ($spec in $minedSpecs)
{
    $dir = $dirs[$spec.dirKey]
    $hash = [string]$fileHashByName[$spec.file]
    $dirHash = [string]$dirHashByKey[$spec.dirKey]
    $source = [string]($fileSpecs | Where-Object { $_.name -eq $spec.file })[0].source
    Add-NewFile -Category $spec.category -Rel ('{0}\{1}\{2}' -f $spec.pkg, $dirHash, $hash) `
        -Expected ('{0}\{1}\{2}' -f $spec.pkg, (([string]$dir.norm).TrimEnd('/') -replace '/', '\'), $spec.file) `
        -KnownDir ([string]$dir.norm) -KnownFile $spec.file -Size $spec.size -Seed $spec.seed `
        -Note ('名字只出现在采集成果里（来源 {0}），目录来自采集日志' -f $source) -Source $source -DirHash $dirHash -FileHash $hash
}

# 不可还原：缺目录哈希 / 缺文件哈希 / 两缺
Add-NewFile -Category 'missing-dir' -Rel ('bgimage\{0}\{1}' -f $unmappedDirHash, $fileHashByName['base.png']) `
    -Expected $null -KnownDir $null -KnownFile 'base.png' -Size 888 -Seed 4001 `
    -Note '文件哈希在采集里、目录哈希不在' -Source 'filenamehash' -DirHash $unmappedDirHash -FileHash ([string]$fileHashByName['base.png'])
Add-NewFile -Category 'missing-file' -Rel ('data\{0}\{1}' -f $dirHashByKey['char'], $unmappedFileHash) `
    -Expected $null -KnownDir ([string]$dirs['char'].norm) -KnownFile $null -Size 777 -Seed 4002 `
    -Note '目录哈希在采集里、文件哈希不在' -Source '' -DirHash ([string]$dirHashByKey['char']) -FileHash $unmappedFileHash
Add-NewFile -Category 'missing-both' -Rel ('data\{0}\{1}' -f $unmappedDirHash, $unmappedFileHash2) `
    -Expected $null -KnownDir $null -KnownFile $null -Size 666 -Seed 4003 `
    -Note '两个哈希都查不到' -Source '' -DirHash $unmappedDirHash -FileHash $unmappedFileHash2

# 明文名（不是哈希名，两种模式都必须原样保留）
Add-NewFile -Category 'plain' -Rel 'bgimage\readme.txt' -Expected 'bgimage\readme.txt' -KnownDir $null -KnownFile 'readme.txt' `
    -Size 37 -Seed 5001 -Note '本来就是明文名' -Source '' -DirHash '' -FileHash ''
Add-NewFile -Category 'plain' -Rel 'data\data.alst' -Expected 'data\data.alst' -KnownDir $null -KnownFile 'data.alst' `
    -Size 640 -Seed 5002 -Note '封包清单（明文名）' -Source '' -DirHash '' -FileHash ''
Add-NewFile -Category 'plain' -Rel 'evimage\evimage.alst' -Expected 'evimage\evimage.alst' -KnownDir $null -KnownFile 'evimage.alst' `
    -Size 512 -Seed 5003 -Note '封包清单（明文名）' -Source '' -DirHash '' -FileHash ''

# 工具日志（必须跳过）
Add-NewFile -Category 'skipped-log' -Rel 'Extractor_Log\Extractor.log' -Expected $null -KnownDir $null -KnownFile $null `
    -Size 200 -Seed 6001 -Note '根目录工具日志，按契约跳过' -Source '' -DirHash '' -FileHash ''
Add-NewFile -Category 'skipped-log' -Rel 'bgimage\Extractor_Log\bgimage_extract.log' -Expected $null -KnownDir $null -KnownFile $null `
    -Size 150 -Seed 6002 -Note '封包内工具日志，按契约跳过' -Source '' -DirHash '' -FileHash ''

# 规模档：额外铺 N 个"名字在采集日志里 + 磁盘上有对应哈希"的文件（量 E-8 的体积与吞吐）
if ($ExtraFiles -gt 0)
{
    $scaleSpecPath = Join-Path $runRoot 'extension-hash-spec-scale.tsv'
    $scaleLines = New-Object System.Collections.ArrayList
    $scaleNames = New-Object System.Collections.ArrayList
    for ($index = 0; $index -lt $ExtraFiles; $index++)
    {
        $name = 'gen_{0:D3}_{1:D5}.ogg' -f ([int][Math]::Floor($index / 10000) + 1), ($index % 10000)
        [void]$scaleNames.Add($name)
        [void]$scaleLines.Add(("FILE`t{0}`t" -f $name))
    }
    Write-Utf8File -Path $scaleSpecPath -Text (($scaleLines -join "`r`n") + "`r`n") -NoNewline
    $scaleOut = Join-Path $runRoot 'extension-hashes-scale.tsv'
    & $python $hashExtensionPy --spec $scaleSpecPath --out $scaleOut --salt $salt
    if ($LASTEXITCODE -ne 0) { throw '规模档哈希计算失败' }
    $scaleHashes = Read-HashTable -Path $scaleOut
    $index = 0
    foreach ($name in $scaleNames)
    {
        $hash = [string]$scaleHashes[('FILE|' + $name)]
        Add-NewFile -Category 'restored-scale' -Rel ('scale\{0}\{1}' -f $dirHashByKey['nested'], $hash) `
            -Expected ('scale\{0}\{1}' -f (([string]$dirs['nested'].norm).TrimEnd('/') -replace '/', '\'), $name) `
            -KnownDir ([string]$dirs['nested'].norm) -KnownFile $name -Size 128 -Seed (7000 + $index) `
            -Note '规模档：名字在采集日志里、磁盘上有对应哈希' -Source 'scale-filenamehash' `
            -DirHash ([string]$dirHashByKey['nested']) -FileHash $hash
        $index++
    }
    Write-Host ("[fixture] 规模档：额外 {0} 个可还原文件" -f $ExtraFiles)
}

# 真正落盘
foreach ($item in $filePlan)
{
    $full = Join-Path $newUnpacked $item.Rel
    if ($item.Rel -like '*.alst')
    {
        Write-Utf8File -Path $full -Text ("# YuaExtract harness fake manifest`r`n# package=$($item.Rel.Split('\')[0])`r`nentry=extension-fixture`r`n") -NoNewline
    }
    elseif ($item.Rel -like '*.txt')
    {
        Write-Utf8File -Path $full -Text 'YuaExtract 验证脚手架（扩展集夹具）：本来就是明文名，应原样出现在输出里。' -NoNewline
    }
    else
    {
        Write-Payload -Path $full -Seed $item.Seed -Size $item.Size
    }
}
New-Item -ItemType Directory -Path (Join-Path $newUnpacked 'emptypkg') -Force | Out-Null
Write-Host ("[fixture] 新解包目录：{0} 个文件 -> {1}" -f $filePlan.Count, $newUnpacked)

# ---------------- 旧作工作区（OldGame） ----------------

$oldGame = Join-Path $FixtureRoot 'OldGame'
$oldHashDir = Join-Path $oldGame 'StringHashDumper_Output'
$oldExtract = Join-Path $oldGame 'Extractor_Output'
$oldRestored = Join-Path $oldGame 'Restored_Extractor_Output'

# 假游戏 exe：必须是真实 32 位 PE（界面会校验），并且**与 Mode1Game 逐字节相同**（exe 摘要命中是库查找主键）
$hostExe = Join-Path $runRoot 'Host\EmptyHost.exe'
if (-not (Test-Path -LiteralPath $hostExe))
{
    Write-Host '[fixture] 先生成空宿主（借用它当假游戏 exe）…'
    & pwsh -NoProfile -File (Join-Path $root 'build-harness.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'build-harness.ps1 失败' }
}
$oldGameExe = Join-Path $oldGame 'otomeki_ckr.exe'
New-Item -ItemType Directory -Path $oldGame -Force | Out-Null
Copy-Item -LiteralPath $hostExe -Destination $oldGameExe -Force

# 盐：Hash Seed 有值、两个 HasherSalt **留空**（README 踩坑 #14：写成非空会被读成"带密钥哈希"）
$stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
$saltContent = (@(
        ('{0} | I | T1 | Hash Seed:{1}' -f $stamp, $salt)
        ('{0} | I | T1 | FileNameHasherSalt:' -f $stamp)
        ('{0} | I | T1 | PathNameHasherSalt:' -f $stamp)
    ) -join "`r`n") + "`r`n"
foreach ($relative in @('Log\Universal.log', 'Universal.log'))
{
    Write-Utf8File -Path (Join-Path $oldGame $relative) -Text $saltContent -NoNewline
}

# 采集日志：DirectoryHash.log / FileNameHash.log（UTF-16LE BOM，格式 名字##YSig##哈希）
$dirLogLines = New-Object System.Collections.ArrayList
foreach ($key in @('char', 'ui', 'cg', 'nested'))
{
    [void]$dirLogLines.Add(('{0}##YSig##{1}' -f $dirs[$key].logName, $dirHashByKey[$key]))
}
[void]$dirLogLines.Add(('{0}##YSig##{1}' -f $dirs['root'].logName, $dirHashByKey['root']))

$fileLogLines = New-Object System.Collections.ArrayList
foreach ($spec in $fileSpecs)
{
    if ($spec.source -ne 'filenamehash') { continue }
    [void]$fileLogLines.Add(('{0}##YSig##{1}' -f $spec.name, $fileHashByName[$spec.name]))
}
# 同名两个哈希：档案里 FILES 只有名字（去重后 1 条），采集来源是 2 条 —— 去重口径要能被看到。
# 默认写成**同一条 (名字, 哈希) 两次**：去重照样被考，但不会产生例外（正式口径：例外 = 0）。
# `-PlantFakeDup` 会再塞一条"重算不回去"的伪造哈希（用于演示 §3.2 生成期自校验是活的），
# 那种情况下 expectedExceptionCount = 1。
[void]$fileLogLines.Add(('same.png##YSig##{0}' -f $fileHashByName['same.png']))
if ($PlantFakeDup) { [void]$fileLogLines.Add(('same.png##YSig##{0}' -f $fakeDupHash)) }
$expectedExceptionCount = if ($PlantFakeDup) { 1 } else { 0 }
$expectedExceptionLines = if ($PlantFakeDup) { @(('{0}:same.png' -f $fakeDupHash)) } else { @() }

# 挖掘成果 MinedNames.lst（HASH:名字，只有文件名）
$minedLines = New-Object System.Collections.ArrayList
foreach ($spec in $fileSpecs)
{
    if ($spec.source -ne 'minednames') { continue }
    [void]$minedLines.Add(('{0}:{1}' -f $fileHashByName[$spec.name], $spec.name))
}
# 撞库成果 HashRestore_RecoveredNames.lst（HASH:名字，目录 + 文件）
$recoveredLines = New-Object System.Collections.ArrayList
[void]$recoveredLines.Add(('{0}:{1}' -f $dirHashByKey['ui'], $dirs['ui'].logName))
[void]$recoveredLines.Add(('{0}:{1}' -f $dirHashByKey['root'], $dirs['root'].logName))
[void]$recoveredLines.Add(('{0}:{1}' -f $fileHashByName['logo.png'], 'logo.png'))
foreach ($spec in $fileSpecs)
{
    if ($spec.source -ne 'recovered-lst') { continue }
    [void]$recoveredLines.Add(('{0}:{1}' -f $fileHashByName[$spec.name], $spec.name))
}
# 同目录其它 *.lst（用户手工放入的对照文件，既有契约）
$extraLstLines = New-Object System.Collections.ArrayList
foreach ($spec in $fileSpecs)
{
    if ($spec.source -ne 'extra-lst') { continue }
    [void]$extraLstLines.Add(('{0}:{1}' -f $fileHashByName[$spec.name], $spec.name))
}

New-Item -ItemType Directory -Path $oldHashDir -Force | Out-Null
Write-Utf16Log -Path (Join-Path $oldHashDir 'DirectoryHash.log') -Lines $dirLogLines.ToArray()
Write-Utf16Log -Path (Join-Path $oldHashDir 'FileNameHash.log') -Lines $fileLogLines.ToArray()
if ($minedLines.Count -gt 0) { Write-Utf8File -Path (Join-Path $oldHashDir 'MinedNames.lst') -Text ((($minedLines -join "`r`n")) + "`r`n") -Bom }
if ($recoveredLines.Count -gt 0) { Write-Utf8File -Path (Join-Path $oldHashDir 'HashRestore_RecoveredNames.lst') -Text ((($recoveredLines -join "`r`n")) + "`r`n") -Bom }
if ($extraLstLines.Count -gt 0) { Write-Utf8File -Path (Join-Path $oldHashDir 'ExtraManual.lst') -Text ((($extraLstLines -join "`r`n")) + "`r`n") -Bom }

# 旧解包目录：与新解包目录同构（多一个"伪造哈希同名文件"，用来覆盖重名/去重口径）
foreach ($item in $filePlan)
{
    if ($item.Category -eq 'skipped-log') { continue }
    $target = Join-Path $oldExtract $item.Rel
    $parent = Split-Path -Parent $target
    if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    if ($item.Rel -like '*.alst' -or $item.Rel -like '*.txt') { Write-Utf8File -Path $target -Text 'YuaExtract 验证脚手架（旧作解包目录）' -NoNewline }
    else { Write-Payload -Path $target -Seed $item.Seed -Size $item.Size }
}
Write-Payload -Path (Join-Path $oldExtract ('bgimage\{0}\{1}' -f $dirHashByKey['char'], $fakeDupHash)) -Seed 9001 -Size 1111
New-Item -ItemType Directory -Path (Join-Path $oldExtract 'Extractor_Log') -Force | Out-Null
Write-Payload -Path (Join-Path $oldExtract 'Extractor_Log\Extractor.log') -Seed 9002 -Size 200
# 来源目录（§8.1 默认 = Restored_Extractor_Output）：
# **刻意与新解包目录逐字节同构** —— 干跑（§5）默认就测这个目录，
# 这样"干跑对象"与"模式1 步骤3 的对象"是同一份内容，E-3 的独立枚举才有唯一解。
Copy-Item -LiteralPath $newUnpacked -Destination $oldRestored -Recurse -Force
# 假脚本：里面写一个"负例名"（磁盘上没有任何对应哈希）—— E-5 用它证明"不伪造名字"
Write-Utf8File -Path (Join-Path $oldRestored 'scn_names.txt') -Text ("; YuaExtract harness fake script (extension fixture)`r`n*{0}`r`n" -f $negativeName) -NoNewline

# ---------------- RawGame（E-2：采集日志里的目录名故意没归一化） ----------------

$rawGame = Join-Path $FixtureRoot 'RawGame'
New-Item -ItemType Directory -Path $rawGame -Force | Out-Null
Copy-Item -LiteralPath $hostExe -Destination (Join-Path $rawGame 'otomeki_ckr.exe') -Force
foreach ($relative in @('Log\Universal.log', 'Universal.log'))
{
    Write-Utf8File -Path (Join-Path $rawGame $relative) -Text $saltContent -NoNewline
}
$rawHashDir = Join-Path $rawGame 'StringHashDumper_Output'
New-Item -ItemType Directory -Path $rawHashDir -Force | Out-Null
$rawDirLines = New-Object System.Collections.ArrayList
foreach ($raw in $rawSpecs)
{
    [void]$rawDirLines.Add(('{0}##YSig##{1}' -f $raw.raw, $rawDirHashByRaw[$raw.raw]))
}
# 一条**正常**的目录映射（用来证明例外是"归一口径"造成的，而不是"所有目录都报例外"）
[void]$rawDirLines.Add(('{0}##YSig##{1}' -f $dirs['cg'].logName, $dirHashByKey['cg']))
Write-Utf16Log -Path (Join-Path $rawHashDir 'DirectoryHash.log') -Lines $rawDirLines.ToArray()
Write-Utf16Log -Path (Join-Path $rawHashDir 'FileNameHash.log') -Lines @(
    ('base.png##YSig##{0}' -f $fileHashByName['base.png'])
    ('stand_a.png##YSig##{0}' -f $fileHashByName['stand_a.png'])
)
Write-Utf8File -Path (Join-Path $rawHashDir 'MinedNames.lst') -Text ((('{0}:{1}' -f $fileHashByName['anj_000_0001.ogg'], 'anj_000_0001.ogg')) + "`r`n") -Bom
# 解包目录：给生成一个"来源目录"（内容不重要，生成主要读采集日志）
New-Item -ItemType Directory -Path (Join-Path $rawGame 'Restored_Extractor_Output') -Force | Out-Null
Write-Payload -Path (Join-Path $rawGame ('Restored_Extractor_Output\bgimage\{0}\{1}' -f $dirHashByKey['char'], $fileHashByName['base.png'])) -Seed 9101 -Size 512
Copy-Item -LiteralPath (Join-Path $rawGame 'Restored_Extractor_Output') -Destination (Join-Path $rawGame 'Extractor_Output') -Recurse -Force

# ---------------- OtherGame（E-4：另一个盐） ----------------

$otherGame = Join-Path $FixtureRoot 'OtherGame'
New-Item -ItemType Directory -Path $otherGame -Force | Out-Null
# exe 必须与 OldGame 的**摘要不同**：否则"exe 摘要命中"这条最强信号会先命中，
# 盐不匹配的判定路径根本走不到（那 E-4 就测不到东西了）。
# 追加几十字节仍是一个合法 PE（PE 加载器忽略尾部数据），但 sha256 变了。
$otherGameExe = Join-Path $otherGame 'othergame.exe'
$hostBytes = [System.IO.File]::ReadAllBytes($hostExe)
$pad = [System.Text.Encoding]::ASCII.GetBytes('YuaExtract harness: different game, different exe digest')
$otherBytes = New-Object byte[] ($hostBytes.Length + $pad.Length)
[Array]::Copy($hostBytes, $otherBytes, $hostBytes.Length)
[Array]::Copy($pad, 0, $otherBytes, $hostBytes.Length, $pad.Length)
[System.IO.File]::WriteAllBytes($otherGameExe, $otherBytes)
$otherSaltContent = (@(
        ('{0} | I | T1 | Hash Seed:{1}' -f $stamp, $otherSalt)
        ('{0} | I | T1 | FileNameHasherSalt:' -f $stamp)
        ('{0} | I | T1 | PathNameHasherSalt:' -f $stamp)
    ) -join "`r`n") + "`r`n"
foreach ($relative in @('Log\Universal.log', 'Universal.log'))
{
    Write-Utf8File -Path (Join-Path $otherGame $relative) -Text $otherSaltContent -NoNewline
}
$otherHashDir = Join-Path $otherGame 'StringHashDumper_Output'
New-Item -ItemType Directory -Path $otherHashDir -Force | Out-Null
Write-Utf16Log -Path (Join-Path $otherHashDir 'DirectoryHash.log') -Lines @(('characters/yuki##YSig##{0}' -f $otherDirHash))
Write-Utf16Log -Path (Join-Path $otherHashDir 'FileNameHash.log') -Lines @(('base.png##YSig##{0}' -f $otherFileHash))
New-Item -ItemType Directory -Path (Join-Path $otherGame 'Restored_Extractor_Output') -Force | Out-Null
Write-Payload -Path (Join-Path $otherGame ('Restored_Extractor_Output\bgimage\{0}\{1}' -f $otherDirHash, $otherFileHash)) -Seed 9201 -Size 256
Copy-Item -LiteralPath (Join-Path $otherGame 'Restored_Extractor_Output') -Destination (Join-Path $otherGame 'Extractor_Output') -Recurse -Force

# ---------------- EmptyGame（E-4 补充：档案里一个名字都没有 -> 必须明确报错，不写空表） ----------------
#
# lead 同步的 task-1 事实 #5：WriteStaticHashMap 在档案没有任何名字时明确报错、不写空表。
# 这个夹具就是一个"盐合法、结构合法、但采集成果全空"的旧作。
$emptyGame = Join-Path $FixtureRoot 'EmptyGame'
New-Item -ItemType Directory -Path $emptyGame -Force | Out-Null
Copy-Item -LiteralPath $hostExe -Destination (Join-Path $emptyGame 'emptygame.exe') -Force
foreach ($relative in @('Log\Universal.log', 'Universal.log'))
{
    Write-Utf8File -Path (Join-Path $emptyGame $relative) -Text $saltContent -NoNewline
}
$emptyHashDir = Join-Path $emptyGame 'StringHashDumper_Output'
New-Item -ItemType Directory -Path $emptyHashDir -Force | Out-Null
Write-Utf16Log -Path (Join-Path $emptyHashDir 'DirectoryHash.log') -Lines @()
Write-Utf16Log -Path (Join-Path $emptyHashDir 'FileNameHash.log') -Lines @()
Write-Utf8File -Path (Join-Path $emptyHashDir 'MinedNames.lst') -Text '' -NoNewline
New-Item -ItemType Directory -Path (Join-Path $emptyGame 'Restored_Extractor_Output') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $emptyGame 'Restored_Extractor_Output') -Destination (Join-Path $emptyGame 'Extractor_Output') -Recurse -Force

# ---------------- Mode1Game（模式1 工作区；合成工作区没有真游戏，解包那步用预置代替） ----------------

$mode1Game = Join-Path $FixtureRoot 'Mode1Game'
New-Item -ItemType Directory -Path $mode1Game -Force | Out-Null
$mode1Exe = Join-Path $mode1Game 'otomeki_ckr.exe'
Copy-Item -LiteralPath $hostExe -Destination $mode1Exe -Force
Copy-Item -LiteralPath $newUnpacked -Destination (Join-Path $mode1Game 'Extractor_Output') -Recurse -Force
# 模式1 工作区**故意没有** StringHashDumper_Output、也**故意没有**盐文件：
# 免跑游戏的证据就是"没有动态采集成果、没有 Universal.log，照样还原出一样的结果"。
New-Item -ItemType Directory -Path (Join-Path $mode1Game 'StringHashDumper_Output') -Force | Out-Null

# ---------------- 独立期望值清单 ----------------

$files = New-Object System.Collections.ArrayList
$tsvLines = New-Object System.Collections.ArrayList
[void]$tsvLines.Add("category`tsourceRel`texpectedRel`tknownDir`tknownFile`tsize`tsha256`tnote`tsource`tdirHash`tfileHash")
$sha256 = [System.Security.Cryptography.SHA256]::Create()
try
{
    foreach ($item in $filePlan)
    {
        $full = Join-Path $newUnpacked $item.Rel
        $bytes = [System.IO.File]::ReadAllBytes($full)
        $hash = ([System.BitConverter]::ToString($sha256.ComputeHash($bytes)) -replace '-', '')
        [void]$files.Add([ordered]@{
                category    = $item.Category
                sourceRel   = $item.Rel
                expectedRel = $item.Expected
                knownDir    = $item.KnownDir
                knownFile   = $item.KnownFile
                size        = [long]$bytes.Length
                sha256      = $hash
                note        = $item.Note
                source      = $item.Source
                dirHash     = $item.DirHash
                fileHash    = $item.FileHash
            })
        [void]$tsvLines.Add(("{0}`t{1}`t{2}`t{3}`t{4}`t{5}`t{6}`t{7}`t{8}`t{9}`t{10}" -f `
                    $item.Category, $item.Rel, $item.Expected, $item.KnownDir, $item.KnownFile, $bytes.Length, $hash, $item.Note, $item.Source, $item.DirHash, $item.FileHash))
    }
}
finally { $sha256.Dispose() }

$restorableCategories = @('restored', 'mined', 'mined-optional', 'manual', 'recovered', 'restored-scale')
$expectedDefault = @($files | Where-Object { $_.category -in $restorableCategories -or $_.category -eq 'plain' })
$expectedKeep = @($files | Where-Object { $_.category -ne 'skipped-log' })
$expectedHashNamed = @($files | Where-Object { $_.category -notin @('plain', 'skipped-log') })

# ---------------- 夹具自检 ----------------
#
# 踩过的坑（README 第二轮 E-1）：类别与映射表实际内容不一致的夹具会**冤枉实现**。
# 这里逐条核对：这一条到底能不能还原，必须与采集日志/lst 的真实内容对得上。
$mappedDirs = @{}
foreach ($line in $dirLogLines) { $mappedDirs[(($line -split '##YSig##')[1])] = $true }
$mappedFiles = @{}
foreach ($line in $fileLogLines) { $mappedFiles[(($line -split '##YSig##')[1])] = $true }
foreach ($line in @($minedLines) + @($recoveredLines) + @($extraLstLines))
{
    $parts = $line -split ':'
    if ($parts.Count -lt 2) { continue }
    if ($parts[0].Length -eq 16) { $mappedDirs[$parts[0]] = $true }
    elseif ($parts[0].Length -eq 64) { $mappedFiles[$parts[0]] = $true }
}

$fixtureErrors = @()
foreach ($item in $files)
{
    if ($item.category -in @('plain', 'skipped-log')) { continue }
    $parts = $item.sourceRel -split '\\'
    if ($parts.Count -lt 3) { continue }
    $dirHash = $parts[1]
    $fileHash = $parts[$parts.Count - 1]
    $restorable = $item.category -in $restorableCategories
    $dirMapped = $mappedDirs.ContainsKey($dirHash)
    $fileMapped = $mappedFiles.ContainsKey($fileHash)
    # 明文名文件不走哈希；其余：可还原要求两半都在，不可还原要求至少缺一半
    if ($item.category -eq 'restored-scale') { continue }
    if ($restorable -and -not ($dirMapped -and $fileMapped))
    {
        $fixtureErrors += ('{0}：类别 {1} 说可还原，但 目录映射={2} / 文件映射={3}' -f $item.sourceRel, $item.category, $dirMapped, $fileMapped)
    }
    if (-not $restorable -and ($dirMapped -and $fileMapped))
    {
        $fixtureErrors += ('{0}：类别 {1} 说不可还原，但两半映射都在 —— 实现会正常还原它，断言反而冤枉实现' -f $item.sourceRel, $item.category)
    }
}

# 负例：名字在"脚本"里，磁盘上**不能**有任何对应哈希的文件
$negativeHash = Get-TextSha256Hex -Text 'extension-fixture-negative-name-is-not-a-hash'
$negativeHashReal = ''
foreach ($spec in $fileSpecs) { if ($spec.name -eq $negativeName) { $negativeHashReal = $fileHashByName[$spec.name] } }
if ($negativeHashReal) { $fixtureErrors += '负例名居然在采集文件清单里' }
$negativeHit = @(Get-ChildItem -LiteralPath $newUnpacked -Recurse -File -Force | Where-Object { $_.Name -like 'nosuch*' })
if ($negativeHit.Count -gt 0) { $fixtureErrors += '负例名的文件居然存在于新解包目录' }

# 未映射哈希必须真的不在任何映射里
foreach ($probe in @($unmappedDirHash, $unmappedFileHash, $unmappedFileHash2, $fakeDupHash))
{
    if ($mappedDirs.ContainsKey($probe)) { $fixtureErrors += ("本该未映射的目录哈希出现在映射里：{0}" -f $probe) }
}
if ($mappedFiles.ContainsKey($unmappedFileHash)) { $fixtureErrors += '本该未映射的文件哈希出现在映射里' }
# 伪造的同名哈希是**故意**写进采集日志的（考"同名两哈希 -> 文件名去重成 1 条"），
# 但它绝不能是任何名字的真实哈希，也绝不能出现在新解包目录里 —— 否则 E-10 会莫名多出一条差异。
if ($PlantFakeDup -and -not $mappedFiles.ContainsKey($fakeDupHash)) { $fixtureErrors += '伪造的同名哈希应当出现在采集日志里（-PlantFakeDup 时）' }
if (@($fileHashByName.Values) -contains $fakeDupHash) { $fixtureErrors += '伪造的同名哈希撞上了某个真实名字的哈希' }
if (Test-Path -LiteralPath (Join-Path $newUnpacked ('bgimage\{0}\{1}' -f $dirHashByKey['char'], $fakeDupHash))) { $fixtureErrors += '伪造的同名哈希文件不该出现在新解包目录' }
# 同名两哈希：真身必须在映射里（它是可还原的）
if (-not $mappedFiles.ContainsKey($fileHashByName['same.png'])) { $fixtureErrors += 'same.png 的真身哈希不在映射里' }

# E-2 夹具：例外必须"大面积" —— 5 条 raw 目录 + 1 条正常目录
if ($rawSpecs.Count -lt 4) { $fixtureErrors += 'E-2 夹具的未归一化目录太少，报不出"大面积例外"' }

# ---------- 采集日志自洽自检（用产品自己的哈希当真值）----------
#
# 每一条采集日志都必须满足"按文档口径归一化后重算 == 日志里那个哈希"，
# 否则产品生成期自校验会如实报成例外 —— 那是**夹具的锅**，却看起来像产品缺陷。
# 踩过一次：PowerShell 的 @{} 大小写不敏感，`DIR|ui` 被 `DIR|UI` 覆盖，
# 于是 `ui` 写成了 hash('UI')，产品报例外（它做对了）。
function Get-NormalizedDir([string]$Raw)
{
    $text = (($Raw -replace '\\', '/') -replace '//', '/').Trim('/')
    if (-not $text -or $Raw -eq '%EmptyString%') { return '/' }
    return ($text.ToLowerInvariant() + '/')
}
foreach ($line in $dirLogLines)
{
    $parts = $line -split '##YSig##'
    if ($parts.Count -lt 2) { continue }
    $name = $parts[0]
    $hash = $parts[1]
    $normalized = Get-NormalizedDir $name
    # 哈希表是按 spec 的**输入**建的键：正常夹具的日志名就是 spec 输入（'ui'），
    # 归一化后的形态（'ui/'）只是用来算哈希。所以先按日志名查，再退回归一化形态。
    $expected = if ($normalized -eq '/') { $rootHash } else { [string]$hashes[('DIR|' + $name)] }
    if (-not $expected -and $normalized -ne '/') { $expected = [string]$hashes[('DIR|' + $normalized)] }
    if (-not $expected) { $fixtureErrors += ("目录映射 {0} 的归一化形态 '{1}' 没算出哈希" -f $name, $normalized) }
    elseif ($expected -ne $hash) { $fixtureErrors += ("目录映射 {0} 自相矛盾：日志里是 {1}，按 '{2}' 重算应为 {3}" -f $name, $hash, $normalized, $expected) }
}
foreach ($line in $fileLogLines)
{
    $parts = $line -split '##YSig##'
    if ($parts.Count -lt 2) { continue }
    $name = $parts[0]
    $hash = $parts[1]
    if ($hash -eq $fakeDupHash) { continue }   # 故意留的"重算不回去"样本（考生成期自校验）
    $expected = [string]$hashes[('FILE|' + $name)]
    if (-not $expected) { $fixtureErrors += ("文件映射 {0} 没算出哈希" -f $name) }
    elseif ($expected -ne $hash) { $fixtureErrors += ("文件映射 {0} 自相矛盾：日志里是 {1}，重算应为 {2}" -f $name, $hash, $expected) }
}
# E-2 的 raw 夹具反过来：日志里的哈希必须是**按原样**算出来的（未归一化）
foreach ($line in $rawDirLines)
{
    $parts = $line -split '##YSig##'
    if ($parts.Count -lt 2) { continue }
    $expected = [string]$hashes[('DIR|' + $parts[0])]
    if ($expected -ne $parts[1]) { $fixtureErrors += ("E-2 夹具的 '{0}' 哈希不是按原样算的（{1} != {2}）" -f $parts[0], $parts[1], $expected) }
}

# E-4 夹具：另一个游戏的 exe 摘要必须与目标游戏不同（否则"exe 摘要命中"会先命中，盐不匹配测不到）
if ((Get-Sha256Hex -Path $otherGameExe) -eq (Get-Sha256Hex -Path $oldGameExe))
{
    $fixtureErrors += 'OtherGame 的 exe 摘要与 OldGame 相同 —— 盐不匹配的判定路径会被"exe 摘要命中"短路掉'
}
if ((Get-PeMachine -Path $otherGameExe) -ne 0x014C) { $fixtureErrors += 'OtherGame 的 exe 不是 32 位 PE（界面会拒绝它当目标）' }
# E-4 补充夹具：EmptyGame 的采集成果必须真的是空的（档案结构合法但名字层为空）
foreach ($path in @(
        (Join-Path $emptyHashDir 'DirectoryHash.log'),
        (Join-Path $emptyHashDir 'FileNameHash.log'),
        (Join-Path $emptyHashDir 'MinedNames.lst')
    ))
{
    if (-not (Test-Path -LiteralPath $path)) { $fixtureErrors += ('EmptyGame 缺采集文件：' + $path); continue }
    $text = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::Unicode)
    if ($text -and $text.Trim().Length -gt 0) { $fixtureErrors += ('EmptyGame 的 {0} 不是空的' -f (Split-Path $path -Leaf)) }
}

if ($fixtureErrors.Count -gt 0)
{
    throw ("扩展集夹具自检失败（这样的夹具会冤枉实现）：`r`n    " + (($fixtureErrors | Select-Object -First 12) -join "`r`n    "))
}
Write-Host ("[fixture] 夹具自检通过：可还原 {0} / 不可还原 {1} / 明文 {2} / 跳过日志 {3}" -f `
        @($files | Where-Object { $_.category -in $restorableCategories }).Count,
    @($files | Where-Object { $_.category -like 'missing-*' }).Count,
    @($files | Where-Object { $_.category -eq 'plain' }).Count,
    @($files | Where-Object { $_.category -eq 'skipped-log' }).Count)

# ---------------- manifest ----------------

$oldGameExeSha = (Get-Sha256Hex -Path $oldGameExe)
$mode1ExeSha = (Get-Sha256Hex -Path $mode1Exe)

$manifest = [ordered]@{
    generatedUtc      = (Get-Date).ToUniversalTime().ToString('o')
    fixtureRoot       = $FixtureRoot
    kind              = 'extension-phase1'
    salt              = $salt
    otherSalt         = $otherSalt
    negativeName      = $negativeName
    hashProbeUsed     = $probeUsed
    hashSource        = 'lib/hash-extension.py（BLAKE2s-256 / SipHash-2-4+bswap，与 Run\Host\HashProbe.exe 逐条对账）'
    oldGame           = [ordered]@{
        workspace     = $oldGame
        gameExe       = $oldGameExe
        gameExeSha256 = $oldGameExeSha
        hashDir       = $oldHashDir
        sourceDir     = $oldExtract
        restoredDir   = $oldRestored
        saltFiles     = @((Join-Path $oldGame 'Log\Universal.log'), (Join-Path $oldGame 'Universal.log'))
        collectionSources = [ordered]@{
            directoryHash = @($dirLogLines)
            fileNameHash  = @($fileLogLines)
            minedNames    = @($minedLines)
            recoveredNames = @($recoveredLines)
            extraLst      = @($extraLstLines)
        }
    }
    rawGame           = [ordered]@{
        workspace        = $rawGame
        gameExe          = (Join-Path $rawGame 'otomeki_ckr.exe')
        hashDir          = $rawHashDir
        sourceDir        = (Join-Path $rawGame 'Restored_Extractor_Output')
        directoryHash    = @($rawDirLines)
        # E-2 判据：未归一化的目录条目（哈希按原样算）必须被生成期自校验**大面积**报成例外
        unnormalizedDirCount = $rawSpecs.Count
        normalizedDirCount   = 1
        expectExceptionsAtLeast = [Math]::Max(3, [int][Math]::Floor($rawSpecs.Count * 0.8))
        note             = '故意构造：采集日志里的目录名没归一化、哈希按原样算 —— 用来证明生成期自校验真的会报例外（正确实现应当报例外；不报反而是缺陷）'
    }
    otherGame         = [ordered]@{
        workspace     = $otherGame
        gameExe       = $otherGameExe
        gameExeSha256 = (Get-Sha256Hex -Path $otherGameExe)
        salt          = $otherSalt
        dirHash       = $otherDirHash
        fileHash      = $otherFileHash
        note          = 'E-4 盐不匹配：这个档案属于另一个盐、exe 摘要也不同，加载时必须明确拒绝（默认 allowUnverified=false）'
    }
    emptyGame         = [ordered]@{
        workspace     = $emptyGame
        gameExe       = (Join-Path $emptyGame 'emptygame.exe')
        salt          = $salt
        hashDir       = $emptyHashDir
        note          = 'E-4 补充：结构合法但一个名字都没有的档案 —— 生成静态 Hash 表时必须明确报错，不许写空表'
    }
    mode1Game         = [ordered]@{
        workspace     = $mode1Game
        gameExe       = $mode1Exe
        gameExeSha256 = $mode1ExeSha
        extractorOutput = (Join-Path $mode1Game 'Extractor_Output')
        note          = '模式1 工作区：解包目录已预置（合成工作区没有真游戏，步骤2 用预置代替 —— 报告里必须写明这一点）；没有盐、没有动态采集成果'
    }
    newUnpacked       = [ordered]@{
        root          = $newUnpacked
        fileCount     = $filePlan.Count
    }
    extensionSet      = [ordered]@{
        # 档案里应当有的名字层（脚手架独立算出的期望）
        dirs          = @(foreach ($key in $dirs.Keys) {
                [ordered]@{
                    key = $key; raw = [string]$dirs[$key].raw; normalized = [string]$dirs[$key].norm
                    hash = [string]$dirHashByKey[$key]; source = 'DirectoryHash.log'
                }
            })
        files         = @(foreach ($spec in $fileSpecs) {
                [ordered]@{ name = $spec.name; hash = [string]$fileHashByName[$spec.name]; source = $spec.source }
            })
        expectedDirCount    = $dirs.Count
        expectedFileCount   = $fileSpecs.Count
        # 正式口径下**例外 = 0**（同名两哈希用"同一条重复两次"来考去重，不制造例外）。
        # `-PlantFakeDup` 时故意留 1 条重算不回去的样本，用来演示生成期自校验（§3.2）是活的。
        expectedExceptionCount = $expectedExceptionCount
        expectedExceptions  = @($expectedExceptionLines)
        # 采集来源条数 vs 去重后条数（同名两个哈希 -> 文件名去重 1 条）
        collectedDirEntries  = $dirLogLines.Count
        collectedFileEntries = $fileLogLines.Count
        dedupFileNames       = $fileSpecs.Count
        fakeDupHash          = $fakeDupHash
    }
    expectations      = [ordered]@{
        defaultOutputCount = $expectedDefault.Count
        keepOutputCount    = $expectedKeep.Count
        hashNamedCount     = $expectedHashNamed.Count
        restorableCount    = @($files | Where-Object { $_.category -in $restorableCategories }).Count
        restorableCategories = $restorableCategories
        plainCount         = @($files | Where-Object { $_.category -eq 'plain' }).Count
        skippedLogCount    = @($files | Where-Object { $_.category -eq 'skipped-log' }).Count
        missingDirCount    = @($files | Where-Object { $_.category -eq 'missing-dir' }).Count
        missingFileCount   = @($files | Where-Object { $_.category -eq 'missing-file' }).Count
        missingBothCount   = @($files | Where-Object { $_.category -eq 'missing-both' }).Count
    }
    unmapped          = [ordered]@{ dirHash = $unmappedDirHash; fileHash = $unmappedFileHash; fileHash2 = $unmappedFileHash2 }
    filesTsv          = $manifestTsvPath
    files             = $files
    note              = '扩展集一期夹具：旧作采集成果 -> 生成扩展集 -> 新解包目录（可还原/不可还原）；期望值由脚手架独立枚举'
}
Write-JsonFile -Path $manifestPath -Object $manifest
Write-TsvFile -Path $manifestTsvPath -Lines ([string[]]$tsvLines.ToArray())

Write-Host ''
Write-Host ("[fixture] 夹具根：{0}" -f $FixtureRoot)
Write-Host ("[fixture] 清单：{0}" -f $manifestPath)
Write-Host ("[fixture] 逐条明细：{0}（{1} 条）" -f $manifestTsvPath, $files.Count)
Write-Host ("[fixture] 期望：默认模式输出 {0} 个 / 勾选保留 {1} 个 / 哈希名 {2} 个" -f $manifest.expectations.defaultOutputCount, $manifest.expectations.keepOutputCount, $manifest.expectations.hashNamedCount)
Write-Host ("[fixture] E-2 夹具：未归一化目录 {0} 条（例外至少 {1} 条）" -f $manifest.rawGame.unnormalizedDirCount, $manifest.rawGame.expectExceptionsAtLeast)
Write-Host ("[fixture] 旧作 exe sha256 {0} / 模式1 exe sha256 {1}（两者相同 = {2}）" -f $oldGameExeSha.Substring(0, 16), $mode1ExeSha.Substring(0, 16), ($oldGameExeSha -eq $mode1ExeSha))
