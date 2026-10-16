# 合成工作区生成器：造一个假的"游戏目录"，用来验证步骤3 的资源名还原。
#
#   pwsh -File make-workspace.ps1                      # 小用例：19 个文件，覆盖各类边界
#   pwsh -File make-workspace.ps1 -Packages 12 -FilesPerPackage 5000
#                                                      # 压力用例：6 万个文件（贴近用户的真实规模）
#
# 产出：
#   Run\FakeWorkspace\                        假游戏目录（= 工作区）
#     otomeki_ckr.exe                         假游戏主程序（真的是个 32 位 PE，供界面校验）
#     Extractor_Output\<封包>\<16位目录哈希>\<64位文件哈希>   解包产物（源目录，只读语义）
#     Extractor_Output\<封包>\<明文名>                        本来就是明文名的文件
#     Extractor_Output\Extractor_Log\...                     工具自己的日志（应被跳过）
#     StringHashDumper_Output\DirectoryHash.log              UTF-16，名字##YSig##哈希
#     StringHashDumper_Output\FileNameHash.log               UTF-16，名字##YSig##哈希
#     StringHashDumper_Output\HashRestore_RecoveredNames.lst UTF-8 BOM，哈希:名字
#   Run\manifest.json                         期望值清单（小用例含逐条明细；大用例只含计数）
#   Run\manifest.tsv                          逐条明细（大用例的唯一来源，断言脚本优先读它）
#
# 覆盖的 5 类文件（对断言A/B 至关重要）：
#   restored    目录哈希 + 文件哈希都能查到 -> 应还原成明文名
#   missing-dir 只查得到文件哈希（目录哈希不在映射里）
#   missing-file 只查得到目录哈希（文件哈希不在映射里）
#   missing-both 两个都查不到
#   plain       本来就是明文名（.alst / readme），应原样保留
# 小用例另外故意放：0 字节文件、5 MB 大文件、多级目录名、根域目录哈希、同名冲突、空封包。

[CmdletBinding()]
param(
    [string]$WorkspaceRoot = '',
    [int]$Packages = 0,
    [int]$FilesPerPackage = 0,
    [int]$DirsPerPackage = 8,
    [int]$PlainPerPackage = 2,
    # 本轮新增：『后缀识别 + 补充还原』专用夹具
    # （6 种文件头的未还原样本 + 假脚本包 + 负例名 + Log\Universal.log 盐）
    [switch]$Supplement,
    # 补充还原夹具里的"挖掘噪声"候选名个数：让挖掘真的跑起来，T-1 才采得到样
    [int]$NoiseCandidates = 20000,
    # 噪声铺成几个文件（产品对单文件的扫描有上限；真实游戏是几万个文件）
    [int]$NoiseFiles = 1,
    # 命中规模档：额外铺 N 个「脚本里有名字 + 磁盘上有对应哈希」的文件
    # （真实数据上命中约 5.1 万，合成夹具默认只有 4 个，验不到量级）
    [int]$MinedCount = 0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessNative.ps1')

if (-not $WorkspaceRoot) { $WorkspaceRoot = Join-Path $root 'Run\FakeWorkspace' }
$runRoot = Join-Path $root 'Run'
$sourceDir = Join-Path $WorkspaceRoot 'Extractor_Output'
$hashDir = Join-Path $WorkspaceRoot 'StringHashDumper_Output'
$manifestPath = Join-Path $runRoot 'manifest.json'
$manifestTsvPath = Join-Path $runRoot 'manifest.tsv'

$scaleMode = ($Packages -gt 0 -and $FilesPerPackage -gt 0)
$supplementMode = [bool]$Supplement

foreach ($directory in @($runRoot, $WorkspaceRoot))
{
    if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
}
[void](Remove-TreeUnder -Path $WorkspaceRoot -MustBeUnder $runRoot)
New-Item -ItemType Directory -Path $WorkspaceRoot -Force | Out-Null

# ---------- 假游戏主程序 ----------

$hostExe = Join-Path $root 'Run\Host\EmptyHost.exe'
if (-not (Test-Path -LiteralPath $hostExe))
{
    Write-Host '[workspace] 先生成空宿主（顺便借用它当假游戏 exe）…'
    & pwsh -NoProfile -File (Join-Path $root 'build-harness.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'build-harness.ps1 失败' }
}
$gameExe = Join-Path $WorkspaceRoot 'otomeki_ckr.exe'
Copy-Item -LiteralPath $hostExe -Destination $gameExe -Force

# ---------- 计划：写哪些文件 ----------

$plan = New-Object System.Collections.ArrayList
# 反查表：让清单自己带上"这一条的哪一半查得到映射"
$dirNameByHash = @{}
$nameByHash = @{}
$directoryLogLines = New-Object System.Collections.ArrayList
$fileNameLogLines = New-Object System.Collections.ArrayList
$lstLines = New-Object System.Collections.ArrayList
$packageNames = @()
# 补充还原模式的元数据（其它模式保持为空，StrictMode 下不能是"未定义"）
$headerSamples = New-Object System.Collections.ArrayList
$minedNames = New-Object System.Collections.ArrayList
$negativeName = ''
$negativeHash = ''
$saltSeed = 'xp3hnp'
$saltFiles = New-Object System.Collections.ArrayList
$saltKeyedTemplate = ''
$noiseCandidateCount = 0

function New-Hex16([string]$Seed) { return (Get-TextSha256Hex -Text $Seed).Substring(0, 16) }
function New-Hex64([string]$Seed) { return (Get-TextSha256Hex -Text $Seed) }

if ($scaleMode)
{
    # 压力模式：12 个封包 × 5000 个文件这种量级。
    # 哈希与文件内容都用确定性规则生成（同一个种子重跑得到同一份工作区）。
    $baseNames = @('bgimage', 'data', 'evimage', 'fgimage', 'video', 'voice', 'sound', 'script',
                   'system', 'movie', 'patch', 'extra')
    $extensions = @('.png', '.ogg', '.ks', '.bin', '.txt')
    $sha = [System.Security.Cryptography.SHA256]::Create()
    function New-HashText([string]$text)
    {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($text)
        $digest = $script:scaleSha.ComputeHash($bytes)
        return ([System.BitConverter]::ToString($digest) -replace '-', '')
    }
    $script:scaleSha = $sha

    for ($packageIndex = 0; $packageIndex -lt $Packages; $packageIndex++)
    {
        $packageName = if ($packageIndex -lt $baseNames.Count) { $baseNames[$packageIndex] } else { 'pkg{0:D2}' -f $packageIndex }
        $packageNames += $packageName

        $dirNames = @()
        for ($dirIndex = 0; $dirIndex -lt $DirsPerPackage; $dirIndex++)
        {
            $dirName = 'd{0:D2}\sub{1:D2}' -f $dirIndex, ($dirIndex % 3)
            $dirHash = (New-HashText ("yua-scale-dir|{0}|{1}" -f $packageName, $dirIndex)).Substring(0, 16)
            $dirNames += [pscustomobject]@{ Hash = $dirHash; Name = $dirName }
            $dirNameByHash[$dirHash] = $dirName
            [void]$directoryLogLines.Add(("{0}##YSig##{1}" -f ($dirName -replace '\\', '/'), $dirHash))
        }

        for ($fileIndex = 0; $fileIndex -lt $FilesPerPackage; $fileIndex++)
        {
            $slot = $fileIndex % 20
            $extension = $extensions[$fileIndex % $extensions.Count]
            $mappedName = 'f{0:D5}{1}' -f $fileIndex, $extension
            $mappedFileHash = New-HashText ("yua-scale-file|{0}|{1}" -f $packageName, $fileIndex)
            $mappedDir = $dirNames[$fileIndex % $DirsPerPackage]

            # 70% 可还原 / 15% 缺目录 / 10% 缺文件 / 5% 两缺。
            # 关键：缺的那一半必须真的**不在映射表里**，否则夹具就是错的
            # （上一版踩过：缺目录的那批共用了已映射的目录哈希，于是它们其实完全可还原，
            #   实现把它们正常还原出来，反倒把断言"冤枉"了）。
            $unmappedDirHash = (New-HashText ("yua-scale-unmapped-dir|{0}|{1}" -f $packageName, $fileIndex)).Substring(0, 16)
            $unmappedFileHash = New-HashText ("yua-scale-unmapped-file|{0}|{1}" -f $packageName, $fileIndex)

            if ($slot -lt 14)
            {
                $category = 'restored'
                $dirHash = $mappedDir.Hash
                $fileHash = $mappedFileHash
                $expected = ('{0}\{1}\{2}' -f $packageName, $mappedDir.Name, $mappedName)
                $nameByHash[$fileHash] = $mappedName
                [void]$fileNameLogLines.Add(("{0}##YSig##{1}" -f $mappedName, $fileHash))
            }
            elseif ($slot -lt 17)
            {
                # 文件名认得、目录名不认得：目录哈希不在 DirectoryHash.log 里
                $category = 'missing-dir'
                $dirHash = $unmappedDirHash
                $fileHash = $mappedFileHash
                $expected = $null
                $nameByHash[$fileHash] = $mappedName
                [void]$fileNameLogLines.Add(("{0}##YSig##{1}" -f $mappedName, $fileHash))
            }
            elseif ($slot -lt 19)
            {
                # 目录名认得、文件名不认得：文件名哈希不在 FileNameHash.log 里
                $category = 'missing-file'
                $dirHash = $mappedDir.Hash
                $fileHash = $unmappedFileHash
                $expected = $null
            }
            else
            {
                $category = 'missing-both'
                $dirHash = $unmappedDirHash
                $fileHash = $unmappedFileHash
                $expected = $null
            }

            [void]$plan.Add([pscustomobject]@{
                    Category = $category
                    Rel      = ('{0}\{1}\{2}' -f $packageName, $dirHash, $fileHash)
                    Expected = $expected
                    Size     = 24 + (($fileIndex * 7) % 200)
                    Seed     = 100000 + $packageIndex * 100000 + $fileIndex
                    Note     = '压力用例'
                })
        }

        for ($plainIndex = 0; $plainIndex -lt $PlainPerPackage; $plainIndex++)
        {
            $plainName = if ($plainIndex -eq 0) { '{0}.alst' -f $packageName } else { 'readme{0}.txt' -f $plainIndex }
            [void]$plan.Add([pscustomobject]@{
                    Category = 'plain'
                    Rel      = ('{0}\{1}' -f $packageName, $plainName)
                    Expected = ('{0}\{1}' -f $packageName, $plainName)
                    Size     = 256
                    Seed     = 900000 + $packageIndex * 10 + $plainIndex
                    Note     = '明文名文件'
                })
        }

        # 封包内部的工具日志目录：必须跳过；而且不能被"根任务递归数 + 封包任务自己数"重复计一次
        [void]$plan.Add([pscustomobject]@{
                Category = 'skipped-log'
                Rel      = ('{0}\Extractor_Log\{0}_extract.log' -f $packageName)
                Expected = $null
                Size     = 128
                Seed     = 800000 + $packageIndex
                Note     = '封包内部的 Extractor_Log（防重复计数）'
            })
    }

    # 根目录下的工具日志：由报告的"根层"那一份计数
    [void]$plan.Add([pscustomobject]@{
            Category = 'skipped-log'
            Rel      = 'Extractor_Log\Extractor.log'
            Expected = $null
            Size     = 128
            Seed     = 810000
            Note     = '根目录下的 Extractor_Log'
        })
    $sha.Dispose()
}
elseif ($supplementMode)
{
    # ---------- 补充还原 / 后缀识别专用夹具 ----------
    #
    # 布局（都在 Extractor_Output 下）：
    #   bgimage\<D_BG>\<F1,F2>     映射齐全 -> 普通还原的对照组（S-6 要证明"盐没了也照常还原"）
    #   hdr\<D_HDR>\<6 个哈希名>   映射表里查不到、也没有任何名字能对上 -> 只能靠文件头补后缀
    #   scn\scn_names.txt          假"脚本"：里面是资源名裸名 + 一个必须失败的负例名
    #   scn\<D_SCN>\<3+1 个哈希名> 哈希 = blake2s(UTF16LE(名字 + xp3hnp))，名字来自脚本；
    #                              **不写进任何映射表**，只能靠"补充还原"挖出来
    $packageNames = @('bgimage', 'hdr', 'scn')

    $dirBg = New-Hex16 'supplement-dir bgimage/characters'
    $dirHdr = New-Hex16 'supplement-dir hdr/unknown'
    $dirScn = New-Hex16 'supplement-dir scripts/data'
    $dirNameByHash = @{
        $dirBg  = 'characters'
        $dirHdr = 'hdr\unknown'
        $dirScn = 'scripts\data'
    }

    $fBase1 = New-Hex64 'supplement-file base.png'
    $fBase2 = New-Hex64 'supplement-file stand.png'
    $nameByHash = @{ $fBase1 = 'base.png'; $fBase2 = 'stand.png' }

    # --- 6 种文件头的未还原样本（哈希名、映射表里查不到） ---
    $headerSamples = New-Object System.Collections.ArrayList
    $headerSpecs = @(
        @{ Key = 'png'; Desc = 'PNG'; Suffixes = @('.png'); Bytes = [byte[]](0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A); Filler = 2048 }
        @{ Key = 'ogg'; Desc = 'OGG'; Suffixes = @('.ogg'); Bytes = [byte[]](0x4F, 0x67, 0x67, 0x53); Filler = 1500 }
        @{ Key = 'jpg'; Desc = 'JPEG'; Suffixes = @('.jpg', '.jpeg'); Bytes = [byte[]](0xFF, 0xD8, 0xFF); Filler = 3000 }
        @{ Key = 'psb'; Desc = 'PSB'; Suffixes = @('.psb'); Bytes = [byte[]](0x50, 0x53, 0x42, 0x00); Filler = 1200 }
        @{ Key = 'txt'; Desc = '纯文本'; Suffixes = @('.txt'); Bytes = [System.Text.Encoding]::ASCII.GetBytes('text'); Filler = 900 }
        @{ Key = 'bin'; Desc = '未知（随机二进制）'; Suffixes = @('.bin'); Bytes = [byte[]](0x9E, 0x37, 0xA1, 0x5C); Filler = 600 }
    )
    foreach ($spec in $headerSpecs)
    {
        $hash = New-Hex64 ('supplement-unresolved-' + $spec.Key)
        if ($spec.Key -eq 'txt')
        {
            # 纯文本样本整份都用可打印 ASCII，别混进随机字节
            $content = [System.Text.Encoding]::ASCII.GetBytes((("YuaExtract harness text sample line. The quick brown fox jumps over the lazy dog 0123456789.`r`n") * 14))
        }
        else
        {
            $content = New-Object byte[] ($spec.Bytes.Length + $spec.Filler)
            [Array]::Copy($spec.Bytes, $content, $spec.Bytes.Length)
            $tail = New-DeterministicBytes -Seed (5200 + $spec.Filler) -Length $spec.Filler
            [Array]::Copy($tail, 0, $content, $spec.Bytes.Length, $spec.Filler)
        }

        $rel = 'hdr\{0}\{1}' -f $dirHdr, $hash
        [void]$plan.Add([pscustomobject]@{
                Category = 'unresolved-header'
                Rel      = $rel
                Expected = $null
                Size     = $content.Length
                Seed     = 0
                Note     = ('未还原样本：文件头 {0}，勾选保留后应带后缀 {1}' -f $spec.Desc, ($spec.Suffixes -join '/'))
                Content  = $content
            })
        [void]$headerSamples.Add([ordered]@{
                key              = $spec.Key
                desc             = $spec.Desc
                hashName         = $hash
                sourceRel        = $rel
                acceptedSuffixes = $spec.Suffixes
                # 勾选保留、但不开「补充还原」时的落点：哈希名 + 文件头补上的后缀
                expectedKeepRel  = ('hdr\{0}\{1}' -f $dirNameByHash[$dirHdr], ($hash + $spec.Suffixes[0]))
            })
    }

    # --- 假脚本：脚本里放裸名（含一个必须失败的负例） ---
    #
    # 候选名的形态按参考实现 recover_from_scripts.py 的口径：小写 ASCII + `_NNN_NNNN`。
    # 「街_道a_春」这类非 ASCII 名字参考实现的词法抓不到，所以标成 required=$false：
    # 抓到算加分、抓不到不算缺陷（真实数据里有这种名字，实测结果会写进验证报告）。
    $minedSpecs = @(
        @{ Name = 'anj_000_0001.ogg'; Required = $true; Header = 'ogg'; Suffixes = @('.ogg') }
        @{ Name = 'nj_005_0012.png'; Required = $true; Header = 'png'; Suffixes = @('.png') }
        @{ Name = 'bgm_010_0003.bin'; Required = $true; Header = 'bin'; Suffixes = @('.bin') }
        @{ Name = '街_道a_春.ogg'; Required = $false; Header = 'ogg'; Suffixes = @('.ogg') }
    )
    $negativeName = 'nosuch_999_9999'

    # 名字 -> 哈希：**必须**用 Python 的 hashlib.blake2s 算（blake2s(UTF16LE(名字 + seed))，key 为空）
    $pythonPath = 'C:\Users\Administrator\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
    if (-not (Test-Path -LiteralPath $pythonPath))
    {
        $foundPython = Get-Command python -ErrorAction SilentlyContinue
        if ($foundPython) { $pythonPath = $foundPython.Source }
    }
    if (-not $pythonPath) { throw '找不到 Python 运行时（夹具侧算 blake2s 需要它）' }

    $seed = 'xp3hnp'
    $namesIn = Join-Path $runRoot 'supplement-names.txt'
    $hashesOut = Join-Path $runRoot 'supplement-hashes.tsv'
    $nameLines = @($minedSpecs | ForEach-Object { $_.Name }) + @($negativeName)
    Write-Utf8File -Path $namesIn -Text (($nameLines -join "`n") + "`n") -NoNewline
    & $pythonPath (Join-Path $root 'lib\hash-names.py') --in $namesIn --out $hashesOut --seed $seed | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "hash-names.py 失败（退出码 $LASTEXITCODE）" }

    $hashByName = @{}
    foreach ($line in [System.IO.File]::ReadAllLines($hashesOut, [System.Text.Encoding]::UTF8))
    {
        if (-not $line.Trim()) { continue }
        $parts = $line -split "`t"
        if ($parts.Count -lt 2) { continue }
        $hashByName[$parts[0]] = $parts[1]
    }
    foreach ($name in $nameLines)
    {
        if (-not $hashByName.ContainsKey($name)) { throw "Python 没算出 '$name' 的哈希" }
    }

    # 脚本正文：候选裸名 + 负例名都写进去
    $scriptLines = @(
        '; YuaExtract harness fake script (supplement mining fixture)'
        '; 下面这些是"像资源名"的串：产品应拿它们算哈希、再与磁盘上的哈希名文件求交集。'
        ''
    )
    foreach ($spec in $minedSpecs)
    {
        $base = [System.IO.Path]::GetFileNameWithoutExtension($spec.Name)
        $scriptLines += ('*' + $base + '|' + $spec.Name)
    }
    $scriptLines += ('*' + $negativeName)
    $scriptLines += ''
    $scriptRel = 'scn\scn_names.txt'
    [void]$plan.Add([pscustomobject]@{
            Category = 'plain'
            Rel      = $scriptRel
            Expected = $scriptRel
            Size     = 0
            Seed     = 0
            Note     = '假脚本：里面有候选裸名 + 一个必须失败的负例名'
            Content  = [System.Text.Encoding]::UTF8.GetBytes(($scriptLines -join "`r`n"))
        })

    # 与候选名对应哈希的 hash 名文件（不写进任何映射表）
    $minedNames = New-Object System.Collections.ArrayList
    $headerByKey = @{}
    foreach ($spec in $headerSpecs) { $headerByKey[$spec.Key] = $spec }

    foreach ($spec in $minedSpecs)
    {
        $hash = $hashByName[$spec.Name]
        $header = $headerByKey[$spec.Header]
        $content = New-Object byte[] ($header.Bytes.Length + 1024)
        [Array]::Copy($header.Bytes, $content, $header.Bytes.Length)
        [Array]::Copy((New-DeterministicBytes -Seed 7777 -Length 1024), 0, $content, $header.Bytes.Length, 1024)

        $rel = 'scn\{0}\{1}' -f $dirScn, $hash
        [void]$plan.Add([pscustomobject]@{
                Category = 'mined'
                Rel      = $rel
                Expected = $null
                Size     = $content.Length
                Seed     = 0
                Note     = ('补充还原目标：脚本里有名字 {0}（文件头 {1}）' -f $spec.Name, $header.Desc)
                Content  = $content
            })
        [void]$minedNames.Add([ordered]@{
                name             = $spec.Name
                hash             = $hash
                sourceRel        = $rel
                required         = [bool]$spec.Required
                acceptedSuffixes = $spec.Suffixes
                # 挖到名字之后应该落到的位置
                expectedRel      = ('scn\{0}\{1}' -f $dirNameByHash[$dirScn], $spec.Name)
                # 没开补充还原、但开了保留时的落点（哈希名 + 后缀）
                expectedKeepRel  = ('scn\{0}\{1}' -f $dirNameByHash[$dirScn], ($hash + $spec.Suffixes[0]))
            })
    }

    # 普通还原的对照组
    [void]$plan.Add([pscustomobject]@{ Category = 'restored'; Rel = ('bgimage\{0}\{1}' -f $dirBg, $fBase1); Expected = 'bgimage\characters\base.png'; Size = 4096; Seed = 6001; Note = '普通还原对照：与盐/补充还原无关，必须照常还原' })
    [void]$plan.Add([pscustomobject]@{ Category = 'restored'; Rel = ('bgimage\{0}\{1}' -f $dirBg, $fBase2); Expected = 'bgimage\characters\stand.png'; Size = 2048; Seed = 6002; Note = '普通还原对照' })

    $directoryLogLines = @(
        ('characters##YSig##' + $dirBg)
        ('hdr/unknown##YSig##' + $dirHdr)
        ('scripts/data##YSig##' + $dirScn)
    )
    $fileNameLogLines = @(
        ('base.png##YSig##' + $fBase1)
        ('stand.png##YSig##' + $fBase2)
    )
    $lstLines = @()

    # ---------- 命中规模档（可选）----------
    #
    # 真实数据上「推理补充还原」命中约 5.1 万个名字。合成夹具默认只有 4 个，
    # 验不了"命中上万"时的量级（名字表大小、第二遍补漏的量、还原率算术）。
    # -MinedCount N 会再铺 N 个"脚本里有名字 + 磁盘上有对应哈希"的文件：
    #   名字形态仍是产品词法认得的 xxx_NNN_NNNN（带 .ogg），哈希照旧交给 Python 算。
    $minedScaleCount = [Math]::Max(0, $MinedCount)
    if ($minedScaleCount -gt 0)
    {
        $scaleNames = New-Object System.Collections.ArrayList
        for ($index = 0; $index -lt $minedScaleCount; $index++)
        {
            $block = [int][Math]::Floor($index / 10000) + 1
            $rest = $index % 10000
            [void]$scaleNames.Add(('hit_{0:D3}_{1:D4}.ogg' -f $block, $rest))
        }

        $scaleIn = Join-Path $runRoot 'supplement-scale-names.txt'
        $scaleOut = Join-Path $runRoot 'supplement-scale-hashes.tsv'
        Write-Utf8File -Path $scaleIn -Text ((($scaleNames) -join "`n") + "`n") -NoNewline
        & $pythonPath (Join-Path $root 'lib\hash-names.py') --in $scaleIn --out $scaleOut --seed $seed | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "命中规模档：hash-names.py 失败（退出码 $LASTEXITCODE）" }
        $scaleHashMap = @{}
        foreach ($line in [System.IO.File]::ReadAllLines($scaleOut, [System.Text.Encoding]::UTF8))
        {
            if (-not $line.Trim()) { continue }
            $parts = $line -split "`t"
            if ($parts.Count -ge 2) { $scaleHashMap[$parts[0]] = $parts[1] }
        }

        $oggBytes = [byte[]](0x4F, 0x67, 0x67, 0x53)
        $scaleScript = New-Object System.Text.StringBuilder
        [void]$scaleScript.AppendLine('; YuaExtract harness fake script (命中规模档：名字都在磁盘上有对应哈希)')
        foreach ($name in $scaleNames)
        {
            $hash = [string]$scaleHashMap[$name]
            $content = New-Object byte[] ($oggBytes.Length + 256)
            [Array]::Copy($oggBytes, $content, $oggBytes.Length)
            [Array]::Copy((New-DeterministicBytes -Seed 31337 -Length 256), 0, $content, $oggBytes.Length, 256)
            [void]$plan.Add([pscustomobject]@{
                    Category = 'mined'
                    Rel      = ('scn\{0}\{1}' -f $dirScn, $hash)
                    Expected = $null
                    Size     = $content.Length
                    Seed     = 0
                    Note     = ('命中规模档：脚本里有名字 ' + $name)
                    Content  = $content
                })
            [void]$minedNames.Add([ordered]@{
                    name             = $name
                    hash             = $hash
                    sourceRel        = ('scn\{0}\{1}' -f $dirScn, $hash)
                    required         = $true
                    acceptedSuffixes = @('.ogg')
                    expectedRel      = ('scn\{0}\{1}' -f $dirNameByHash[$dirScn], $name)
                    expectedKeepRel  = ('scn\{0}\{1}' -f $dirNameByHash[$dirScn], ($hash + '.ogg'))
                })
            [void]$scaleScript.AppendLine(('*' + [System.IO.Path]::GetFileNameWithoutExtension($name)))
        }
        $scaleScriptRel = 'scn\scn_hits.txt'
        [void]$plan.Add([pscustomobject]@{
                Category = 'plain'
                Rel      = $scaleScriptRel
                Expected = $scaleScriptRel
                Size     = 0
                Seed     = 0
                Note     = ('命中规模档脚本：{0} 个名字' -f $minedScaleCount)
                Content  = [System.Text.Encoding]::UTF8.GetBytes($scaleScript.ToString())
            })
    }

    # 负例名的哈希（负例本身不会有文件，这里只是留个记录）
    $negativeHash = $hashByName[$negativeName]

    # 只挖 4 个候选名的话，挖掘几十毫秒就结束 —— T-1 要在"挖掘进行中"采样界面响应，
    # 窗口太短根本采不到样。所以再放一份"噪声脚本"：大量形态合法但不存在的候选名，
    # 让挖掘真的跑起来（试算量 ≈ 候选数 × 扩展名数）。它同时也是一个真实的干扰项：
    # 这些名字全都挖不到，绝不该出现在名字表或输出里。
    #
    # -NoiseFiles 用来把噪声拆成多个文件：实测产品对**单个文件**的扫描是有上限的
    # （300 万个候选塞进一个 50MB 文件，只报出 232 个候选），但真实游戏是几万个文件，
    # 所以要贴近真实规模就必须按文件铺开。
    $noiseCount = [Math]::Max(0, $NoiseCandidates)
    $noiseCandidateCount = $noiseCount
    $noiseFileCount = [Math]::Max(1, $NoiseFiles)
    if ($noiseCount -gt 0)
    {
        $perFile = [int][Math]::Ceiling($noiseCount / [double]$noiseFileCount)
        $written = 0
        for ($fileIndex = 0; $fileIndex -lt $noiseFileCount -and $written -lt $noiseCount; $fileIndex++)
        {
            $take = [Math]::Min($perFile, $noiseCount - $written)
            $noise = New-Object System.Text.StringBuilder
            [void]$noise.AppendLine(('; YuaExtract harness fake script (mining noise part {0}/{1}: 形态合法但磁盘上没有对应哈希)' -f ($fileIndex + 1), $noiseFileCount))
            for ($index = 0; $index -lt $take; $index++)
            {
                [void]$noise.AppendLine(('*gen_{0:D3}_{1:D5}' -f ((($written + $index) % 900) + 100), ($written + $index)))
            }
            $noiseRel = if ($noiseFileCount -eq 1) { 'scn\scn_noise.txt' } else { 'scn\scn_noise_{0:D3}.txt' -f $fileIndex }
            [void]$plan.Add([pscustomobject]@{
                    Category = 'plain'
                    Rel      = $noiseRel
                    Expected = $noiseRel
                    Size     = 0
                    Seed     = 0
                    Note     = ('挖掘噪声：{0} 个合法形态的候选名，全部挖不到' -f $take)
                Content  = [System.Text.Encoding]::UTF8.GetBytes($noise.ToString())
            })
            $written += $take
        }
    }
}
else
{
    # ---------- 小用例：手工编排的边界场景 ----------
    $packageNames = @('bgimage', 'data', 'evimage')

    $dirChar = New-Hex16 'yua-harness-dir characters/yuki'
    $dirUi = New-Hex16 'yua-harness-dir ui'
    $dirCg = New-Hex16 'yua-harness-dir event/cg'
    $dirRoot = New-Hex16 'yua-harness-dir root(%EmptyString%)'
    $dirMissOne = New-Hex16 'yua-harness-dir missing-one (没有映射)'
    $dirMissTwo = New-Hex16 'yua-harness-dir missing-two (没有映射)'

    $f = @{}
    foreach ($name in @('base.png', 'stand_a.png', 'logo.png', 'miss1_payload.bin', 'unknown.bin', 'script.ks',
                        'data_missing.bin', 'unknown2.bin', 'ev01.png', 'ev02.png', 'ev_missing.png',
                        'same.png', 'same_b.png'))
    {
        $f[$name] = New-Hex64 ('yua-harness-file ' + $name)
    }

    $plan = @(
        [pscustomobject]@{ Category = 'restored'; Rel = "bgimage\$dirChar\$($f['base.png'])"; Expected = 'bgimage\characters\yuki\base.png'; Size = 307200; Seed = 1001; Note = '目录名（多级）与文件名都来自 DirectoryHash.log / FileNameHash.log' }
        [pscustomobject]@{ Category = 'restored'; Rel = "bgimage\$dirChar\$($f['stand_a.png'])"; Expected = 'bgimage\characters\yuki\stand_a.png'; Size = 1; Seed = 1002; Note = '同一目录哈希下的第二个文件；1 字节' }
        [pscustomobject]@{ Category = 'restored'; Rel = "bgimage\$dirUi\$($f['logo.png'])"; Expected = 'bgimage\ui\logo.png'; Size = 0; Seed = 1003; Note = '两条映射都来自 .lst；0 字节文件（空文件不应被跳过）' }
        [pscustomobject]@{ Category = 'missing-dir'; Rel = "bgimage\$dirMissOne\$($f['miss1_payload.bin'])"; Expected = $null; Size = 2048; Seed = 1004; Note = '文件哈希查得到、目录哈希查不到' }
        [pscustomobject]@{ Category = 'missing-both'; Rel = "bgimage\$dirMissTwo\$($f['unknown.bin'])"; Expected = $null; Size = 999; Seed = 1005; Note = '目录哈希与文件哈希都查不到' }
        [pscustomobject]@{ Category = 'plain'; Rel = 'bgimage\data.alst'; Expected = 'bgimage\data.alst'; Size = 512; Seed = 1006; Note = '明文名的清单文件，应原样复制' }
        [pscustomobject]@{ Category = 'plain'; Rel = 'bgimage\readme.txt'; Expected = 'bgimage\readme.txt'; Size = 37; Seed = 1007; Note = '明文名的普通文件' }
        [pscustomobject]@{ Category = 'plain'; Rel = 'data\data.alst'; Expected = 'data\data.alst'; Size = 640; Seed = 1016; Note = '<封包>.alst 风格的清单：默认模式下必须留在输出里（丢了就是"丢得太多"）' }
        [pscustomobject]@{ Category = 'plain'; Rel = 'data\sub\note.txt'; Expected = 'data\sub\note.txt'; Size = 25; Seed = 1017; Note = '带子目录的明文名文件，验证 passthrough 不只是顶层文件' }

        [pscustomobject]@{ Category = 'restored'; Rel = "data\$dirRoot\$($f['script.ks'])"; Expected = 'data\script.ks'; Size = 65536; Seed = 1008; Note = '目录哈希映射到根域 %EmptyString%，文件应直接落到封包根下' }
        [pscustomobject]@{ Category = 'missing-dir'; Rel = "data\$dirMissOne\$($f['data_missing.bin'])"; Expected = $null; Size = 4096; Seed = 1009; Note = '与 bgimage 那条共用同一个缺映射目录哈希' }
        [pscustomobject]@{ Category = 'missing-file'; Rel = "data\$dirRoot\$($f['unknown2.bin'])"; Expected = $null; Size = 777; Seed = 1010; Note = '目录哈希查得到（根域）、文件哈希查不到' }

        [pscustomobject]@{ Category = 'restored'; Rel = "evimage\$dirCg\$($f['ev01.png'])"; Expected = 'evimage\event\cg\ev01.png'; Size = 5242880; Seed = 1011; Note = '5 MB 大文件，验证缓冲拷贝逐字节一致' }
        [pscustomobject]@{ Category = 'restored'; Rel = "evimage\$dirCg\$($f['ev02.png'])"; Expected = 'evimage\event\cg\ev02.png'; Size = 12345; Seed = 1012; Note = '同一目录下的第二个文件' }
        [pscustomobject]@{ Category = 'missing-file'; Rel = "evimage\$dirCg\$($f['ev_missing.png'])"; Expected = $null; Size = 321; Seed = 1013; Note = '目录哈希查得到、文件哈希查不到' }
        [pscustomobject]@{ Category = 'plain'; Rel = 'evimage\evimage.alst'; Expected = 'evimage\evimage.alst'; Size = 1024; Seed = 1014; Note = '封包自带的明文清单' }

        # 重名冲突：两个不同的文件哈希映射到同一个真名，必须落 __dupN，不能互相覆盖
        [pscustomobject]@{ Category = 'restored-dup'; Rel = "bgimage\$dirChar\$($f['same.png'])"; Expected = 'bgimage\characters\yuki\same.png'; Size = 1111; Seed = 1018; Note = '两个哈希映射到同一个真名之一：应落 same.png / same__dupN.png，两边的数据都不能丢' }
        [pscustomobject]@{ Category = 'restored-dup'; Rel = "bgimage\$dirChar\$($f['same_b.png'])"; Expected = 'bgimage\characters\yuki\same.png'; Size = 2222; Seed = 1019; Note = '同上，第二个同名文件' }

        [pscustomobject]@{ Category = 'skipped-log'; Rel = 'Extractor_Log\Extractor.log'; Expected = $null; Size = 200; Seed = 1015; Note = '根目录下的工具日志，按契约应被跳过' }
        # 封包内部的工具日志目录：同样要跳过，且不能被"根任务递归数 + 封包任务自己数"重复计一次
        [pscustomobject]@{ Category = 'skipped-log'; Rel = 'bgimage\Extractor_Log\bgimage_extract.log'; Expected = $null; Size = 150; Seed = 1020; Note = '封包内部的 Extractor_Log（防重复计数）' }
    )

    $dirNameByHash = @{
        $dirChar = 'characters/yuki'
        $dirUi   = 'ui'
        $dirCg   = 'event/cg'
        $dirRoot = '%EmptyString%'
    }
    # 只把**真的写进映射表**的文件名反查进来：故意不放进映射的那几个
    # （unknown.bin / unknown2.bin / ev_missing.png）不在这里。
    foreach ($mappedName in @('base.png', 'stand_a.png', 'logo.png', 'miss1_payload.bin', 'script.ks',
                              'data_missing.bin', 'ev01.png', 'ev02.png', 'same.png', 'same_b.png'))
    {
        $nameByHash[$f[$mappedName]] = $mappedName
    }

    $directoryLogLines = @(
        "characters/yuki##YSig##$dirChar"
        "event/cg##YSig##$dirCg"
    )
    $fileNameLogLines = @(
        "base.png##YSig##$($f['base.png'])"
        "stand_a.png##YSig##$($f['stand_a.png'])"
        "miss1_payload.bin##YSig##$($f['miss1_payload.bin'])"
        "script.ks##YSig##$($f['script.ks'])"
        "data_missing.bin##YSig##$($f['data_missing.bin'])"
        "ev01.png##YSig##$($f['ev01.png'])"
        "ev02.png##YSig##$($f['ev02.png'])"
        "same.png##YSig##$($f['same.png'])"
        "same.png##YSig##$($f['same_b.png'])"
    )
    $lstLines = @(
        "$dirUi`:ui"
        "$dirRoot`:%EmptyString%"
        "$($f['logo.png'])`:logo.png"
    )
}

# ---------- 写源文件 ----------

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
foreach ($entry in $plan)
{
    $fullPath = Join-Path $sourceDir $entry.Rel
    $parent = Split-Path -Parent $fullPath
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }

    if ($entry.PSObject.Properties['Content'] -and $entry.Content)
    {
        # 夹具需要精确控制字节的场景（文件头样本、脚本正文）
        [System.IO.File]::WriteAllBytes($fullPath, [byte[]]$entry.Content)
    }
    elseif ($entry.Category -eq 'plain' -and $entry.Rel -like '*.alst')
    {
        $text = "# YuaExtract harness fake manifest`r`n# package=$($entry.Rel.Split('\')[0])`r`nentry=harness`r`n"
        Write-Utf8File -Path $fullPath -Text $text -NoNewline
    }
    elseif ($entry.Category -eq 'plain' -and $entry.Rel -like '*.txt')
    {
        Write-Utf8File -Path $fullPath -Text 'YuaExtract 验证脚手架：这个文件本来就是明文名，应原样出现在输出目录。' -NoNewline
    }
    elseif ($entry.Size -le 1024)
    {
        # 大批量小文件：不逐个 New-Object Random（6 万次太慢），
        # 用"下标 + 种子"的确定性字节填充，保证每个文件内容唯一且可复现。
        $bytes = New-Object byte[] $entry.Size
        for ($index = 0; $index -lt $bytes.Length; $index++)
        {
            $bytes[$index] = [byte](($index * 31 + $entry.Seed) -band 0xFF)
        }
        [System.IO.File]::WriteAllBytes($fullPath, $bytes)
    }
    else
    {
        [System.IO.File]::WriteAllBytes($fullPath, (New-DeterministicBytes -Seed $entry.Seed -Length $entry.Size))
    }
}

# 空封包：目录在、一个文件都没有 —— 验证"空目录会不会让任务永远停在处理中"
New-Item -ItemType Directory -Path (Join-Path $sourceDir 'emptypkg') -Force | Out-Null
$writeMs = $stopwatch.ElapsedMilliseconds

# ---------- 写映射表 ----------

New-Item -ItemType Directory -Path $hashDir -Force | Out-Null

# DirectoryHash.log / FileNameHash.log：UTF-16 (LE + BOM)，格式 名字##YSig##哈希
$utf16 = New-Object System.Text.UnicodeEncoding($false, $true)
[System.IO.File]::WriteAllText((Join-Path $hashDir 'DirectoryHash.log'), ((($directoryLogLines -join "`r`n")) + "`r`n"), $utf16)
[System.IO.File]::WriteAllText((Join-Path $hashDir 'FileNameHash.log'), ((($fileNameLogLines -join "`r`n")) + "`r`n"), $utf16)
# 补充对照表：UTF-8 (BOM)，格式 哈希:名字
if ($lstLines.Count -gt 0)
{
    Write-Utf8File -Path (Join-Path $hashDir 'HashRestore_RecoveredNames.lst') -Text ((($lstLines -join "`r`n")) + "`r`n") -Bom
}

# ---------- 补充还原：盐文件（产品的 ReadSalt 要读它） ----------

if ($supplementMode)
{
    # 盐文件两种形态（都保留，各有用途）：
    #   normal：Hash Seed:xp3hnp，两个 HasherSalt **留空** —— 用户真实游戏的样子，
    #           blake2s(UTF16LE(名字 + seed)) 可复现，补充还原应当能跑。
    #   keyed ：FileNameHasherSalt/PathNameHasherSalt **非空** —— 带密钥哈希，产品应当明确
    #           拒绝挖掘并说明原因（S-6b 专测"盐存在但不可用"的降级路径）。
    # 踩过的坑：最初把三个字段都写成 xp3hnp，产品读成"带密钥哈希"于是规规矩矩地拒绝挖掘，
    # 看起来却像"产品没实现"。盐的每个字段含义不同，不能想当然。
    $stamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
    $saltContent = (@(
            ('{0} | I | T1 | Hash Seed:{1}' -f $stamp, $seed)
            ('{0} | I | T1 | FileNameHasherSalt:' -f $stamp)
            ('{0} | I | T1 | PathNameHasherSalt:' -f $stamp)
        ) -join "`r`n") + "`r`n"
    $keyedSaltContent = (@(
            ('{0} | I | T1 | Hash Seed:{1}' -f $stamp, $seed)
            ('{0} | I | T1 | FileNameHasherSalt:SECRETKEY' -f $stamp)
            ('{0} | I | T1 | PathNameHasherSalt:SECRETKEY' -f $stamp)
        ) -join "`r`n") + "`r`n"

    # 游戏目录下两个可能的位置都写一份（产品具体读哪个还没定，多写一份不吃亏）。
    # 工具目录（Run\App\Log）的那一份由 run-step3-restore.ps1 在快照同步之后写 ——
    # 否则同步快照时会把 Run\App 整个换掉，把我们写的盐冲掉。
    foreach ($relative in @('Log\Universal.log', 'Universal.log'))
    {
        $path = Join-Path $WorkspaceRoot $relative
        Write-Utf8File -Path $path -Text $saltContent -NoNewline
        [void]$saltFiles.Add($path)
    }

    # 带密钥盐的模板（S-6b 用：盐存在但不可用 → 产品应当明确拒绝挖掘并说明原因）
    $keyedSaltPath = Join-Path $runRoot 'supplement-salt-keyed.txt'
    Write-Utf8File -Path $keyedSaltPath -Text $keyedSaltContent -NoNewline
    $saltKeyedTemplate = $keyedSaltPath
}

# ---------- 期望值清单 ----------

$files = New-Object System.Collections.ArrayList
$tsvLines = New-Object System.Collections.ArrayList
[void]$tsvLines.Add("category`tsourceRel`texpectedRel`tknownDir`tknownFile`tsize`tsha256`tnote")

$sha256 = [System.Security.Cryptography.SHA256]::Create()
try
{
    foreach ($entry in $plan)
    {
        $fullPath = Join-Path $sourceDir $entry.Rel
        $parts = $entry.Rel -split '\\'
        $dirSegment = if ($parts.Count -ge 3) { $parts[1] } else { $null }
        $fileSegment = $parts[$parts.Count - 1]
        $knownDir = if ($dirSegment -and $dirNameByHash.ContainsKey($dirSegment)) { $dirNameByHash[$dirSegment] } else { $null }
        $knownFile = if ($nameByHash.ContainsKey($fileSegment)) { $nameByHash[$fileSegment] } else { $null }

        # 内容摘要直接算（文件是刚写下去的，不必再读一遍）；大小取磁盘上的真实值
        $bytes = [System.IO.File]::ReadAllBytes($fullPath)
        $hash = ([System.BitConverter]::ToString($sha256.ComputeHash($bytes)) -replace '-', '')

        [void]$files.Add([ordered]@{
                category    = $entry.Category
                sourceRel   = $entry.Rel
                expectedRel = $entry.Expected
                knownDir    = $knownDir
                knownFile   = $knownFile
                size        = [long]$bytes.Length
                sha256      = $hash
                note        = $entry.Note
            })
        [void]$tsvLines.Add(("{0}`t{1}`t{2}`t{3}`t{4}`t{5}`t{6}`t{7}" -f `
                    $entry.Category, $entry.Rel, $entry.Expected, $knownDir, $knownFile, $bytes.Length, $hash, $entry.Note))
    }
}
finally { $sha256.Dispose() }

$defaultExpected = @($files | Where-Object { $_.category -in @('restored', 'plain', 'restored-dup') })
$keepExpected = @($files | Where-Object { $_.category -ne 'skipped-log' })

# ---------- 夹具自检：类别必须与映射表实际内容一致 ----------
#
# 这一步是给自己兜底的。踩过一次：压力模式下"缺目录"的那些文件共用了**已映射**的
# 目录哈希，于是它们其实完全可还原 —— 实现老老实实还原出来，断言反而判它不合格。
# 现在把"哪一半查得到映射"直接跟映射表对一遍，不一致就在这里炸，别让错误的夹具
# 去冤枉实现。
$mappedDirHashes = @{}
$mappedFileHashes = @{}
foreach ($line in $directoryLogLines) { $mappedDirHashes[(($line -split '##YSig##')[1])] = $true }
foreach ($line in $fileNameLogLines) { $mappedFileHashes[(($line -split '##YSig##')[1])] = $true }
foreach ($line in $lstLines)
{
    $parts = $line -split ':'
    if ($parts.Count -lt 2) { continue }
    if ($parts[0].Length -eq 16) { $mappedDirHashes[$parts[0]] = $true }
    elseif ($parts[0].Length -eq 64) { $mappedFileHashes[$parts[0]] = $true }
}

$fixtureErrors = @()
foreach ($item in $files)
{
    $parts = $item.sourceRel -split '\\'
    if ($parts.Count -lt 3) { continue }   # 明文名文件与根层日志没有哈希目录
    $dirHash = $parts[1]
    $fileHash = $parts[$parts.Count - 1]
    $wantDirMapped = $item.category -in @('restored', 'missing-file', 'restored-dup', 'unresolved-header', 'mined')
    $wantFileMapped = $item.category -in @('restored', 'missing-dir', 'restored-dup')
    $dirMapped = $mappedDirHashes.ContainsKey($dirHash)
    $fileMapped = $mappedFileHashes.ContainsKey($fileHash)
    if ($dirMapped -ne $wantDirMapped -or $fileMapped -ne $wantFileMapped)
    {
        $fixtureErrors += ("{0}: 类别 {1} 期望 目录映射={2}/文件映射={3}，实际 目录={4}/文件={5}" -f `
                $item.sourceRel, $item.category, $wantDirMapped, $wantFileMapped, $dirMapped, $fileMapped)
    }
}
if ($fixtureErrors.Count -gt 0)
{
    throw ("合成工作区自检失败：映射表与文件类别对不上，这样的夹具会冤枉实现。`r`n    " + ($fixtureErrors | Select-Object -First 10 | ForEach-Object { $_ }) -join "`r`n    ")
}

# ---------- 补充还原夹具自检 ----------
#
# 这一块专门盯"补充还原"的口径：候选名的哈希必须是 Python 算出来的那个、
# 磁盘上必须真有对应的哈希名文件、负例名必须**一个文件都没有**。
if ($supplementMode)
{
    $supplementErrors = @()

    foreach ($sample in $headerSamples)
    {
        $path = Join-Path $sourceDir $sample.sourceRel
        if (-not (Test-Path -LiteralPath $path)) { $supplementErrors += ('缺文件头样本：' + $sample.sourceRel); continue }
        $bytes = [System.IO.File]::ReadAllBytes($path)
        if ($sample.key -eq 'txt')
        {
            foreach ($b in $bytes) { if ($b -gt 0x7E -or ($b -lt 0x20 -and $b -notin @(0x0D, 0x0A, 0x09))) { $supplementErrors += ('纯文本样本里混进了非可打印字节：' + $sample.sourceRel); break } }
        }
        elseif ($sample.key -eq 'bin')
        {
            foreach ($spec in $headerSpecs)
            {
                if ($spec.Key -eq 'bin') { continue }
                $prefix = $spec.Bytes
                if ($bytes.Length -ge $prefix.Length -and (@(0..($prefix.Length - 1)) | Where-Object { $bytes[$_] -ne $prefix[$_] }).Count -eq 0)
                {
                    $supplementErrors += ('"未知"样本的开头撞上了 {0} 的魔数：{1}' -f $spec.Desc, $sample.sourceRel)
                }
            }
        }
    }

    foreach ($mined in $minedNames)
    {
        $path = Join-Path $sourceDir $mined.sourceRel
        if (-not (Test-Path -LiteralPath $path)) { $supplementErrors += ('缺补充还原目标文件：' + $mined.sourceRel); continue }
        $actualName = Split-Path $path -Leaf
        if ($actualName -ne $mined.hash) { $supplementErrors += ('{0} 的文件名不是算出来的哈希' -f $mined.sourceRel) }
        if ($mappedFileHashes.ContainsKey($mined.hash)) { $supplementErrors += ('补充还原目标不该出现在映射表里：' + $mined.sourceRel) }
    }

    # 负例：名字对应的哈希在磁盘上必须不存在（否则它就不是负例了）
    if (-not $negativeHash) { $supplementErrors += '负例名的哈希没算出来' }
    else
    {
        $hit = @(Get-ChildItem -LiteralPath $sourceDir -Recurse -File -Force -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -eq $negativeHash })
        if ($hit.Count -gt 0) { $supplementErrors += ('负例名 nosuch_999_9999 的哈希在磁盘上居然存在：' + $hit[0].FullName) }
        if ($mappedFileHashes.ContainsKey($negativeHash)) { $supplementErrors += '负例名的哈希不该出现在映射表里' }
    }

    foreach ($saltPath in $saltFiles)
    {
        if (-not (Test-Path -LiteralPath $saltPath)) { $supplementErrors += ('缺盐文件：' + $saltPath); continue }
        $saltText = [System.IO.File]::ReadAllText($saltPath, [System.Text.Encoding]::UTF8)
        if ($saltText -notmatch ('Hash Seed:' + [regex]::Escape($saltSeed))) { $supplementErrors += ('盐文件里没有 Hash Seed:' + $saltSeed + ' → ' + $saltPath) }
    }

    if ($supplementErrors.Count -gt 0)
    {
        throw ("补充还原夹具自检失败：`r`n    " + ($supplementErrors | Select-Object -First 10 | ForEach-Object { $_ }) -join "`r`n    ")
    }
    Write-Host ("[workspace] 补充还原夹具自检通过：{0} 个文件头样本 / {1} 个补充还原目标 / 负例 {2} / 盐 {3} 份" -f `
            $headerSamples.Count, $minedNames.Count, $negativeName, $saltFiles.Count)
}

$manifest = [ordered]@{
    generatedUtc         = (Get-Date).ToUniversalTime().ToString('o')
    scaleMode            = $scaleMode
    packages             = $packageNames
    filesPerPackage      = $FilesPerPackage
    emptyPackage         = 'emptypkg'
    gameDir              = $WorkspaceRoot
    gameExe              = $gameExe
    sourceDir            = $sourceDir
    hashDir              = $hashDir
    defaultOutputDir     = (Join-Path $WorkspaceRoot 'Restored_Extractor_Output')
    mappingFiles         = @('DirectoryHash.log', 'FileNameHash.log', 'HashRestore_RecoveredNames.lst')
    mappingEntryCounts   = [ordered]@{
        directoryHash = $directoryLogLines.Count
        fileNameHash  = $fileNameLogLines.Count
        listFile      = $lstLines.Count
    }
    filesTsv             = $manifestTsvPath
    expectedDefaultCount = $defaultExpected.Count
    expectedKeepCount    = $keepExpected.Count
    expectedHashNamedCount = @($files | Where-Object { $_.category -notin @('plain', 'skipped-log') }).Count
    totalSourceCount     = $files.Count
    supplementMode       = $supplementMode
    note                 = $(if ($scaleMode) { '压力用例：逐条明细见 manifest.tsv' }
        elseif ($supplementMode) { '补充还原/后缀识别专用夹具' }
        else { '小用例：逐条明细见 files 数组与 manifest.tsv' })
    supplement           = $(if ($supplementMode)
        {
            [ordered]@{
                seed             = $saltSeed
                saltFiles        = @($saltFiles)
                keyedSaltTemplate = $saltKeyedTemplate
                noiseCandidates  = $noiseCandidateCount
                scriptRel        = $scriptRel
                negativeName     = $negativeName
                negativeHash     = $negativeHash
                headerSamples    = @($headerSamples)
                minedNames       = @($minedNames)
                requiredMined    = @($minedNames | Where-Object { $_.required })
                optionalMined    = @($minedNames | Where-Object { -not $_.required })
                # 判据里要用到的期望值（都由脚手架独立枚举得出）
                expectedDefaultNoMining = $defaultExpected.Count
                expectedDefaultMining   = $defaultExpected.Count + @($minedNames | Where-Object { $_.required }).Count
                expectedKeepNoMining    = $keepExpected.Count
            }
        }
        else { $null })
}
# 小用例把明细也塞进 JSON（自检与人工排查都方便）；大用例只留 TSV，避免几十 MB 的 JSON。
if ($files.Count -le 5000) { $manifest['files'] = $files }
Write-JsonFile -Path $manifestPath -Object $manifest
Write-TsvFile -Path $manifestTsvPath -Lines ([string[]]$tsvLines.ToArray())

Write-Host ("[workspace] 已生成：{0}" -f $WorkspaceRoot)
Write-Host ("[workspace] 模式：{0}；封包 {1} 个；源文件 {2} 个（哈希名 {3} / 明文名 {4} / 日志目录 {5}）；写盘 {6:N1} 秒" -f `
        $(if ($scaleMode) { "压力（$Packages × $FilesPerPackage）" } else { '小用例' }),
    $packageNames.Count,
    $files.Count,
    $manifest.expectedHashNamedCount,
    @($files | Where-Object { $_.category -eq 'plain' }).Count,
    @($files | Where-Object { $_.category -eq 'skipped-log' }).Count,
    ($writeMs / 1000.0))
Write-Host ("[workspace] 默认模式期望输出 {0} 个；勾选保留后期望输出 {1} 个" -f $manifest.expectedDefaultCount, $manifest.expectedKeepCount)
Write-Host ("[workspace] 清单：{0}（逐条 TSV：{1}）" -f $manifestPath, $manifestTsvPath)
