# 在线更新夹具：把**产品真档案**摆成"远端仓库"，装上坏法，给 serve-fixtures.ps1 用。
#
#   pwsh -File make-online-repo.ps1                       # 自动从夹具工作区找档案
#   pwsh -File make-online-repo.ps1 -OldArchive <rules.int> -OtherArchive <rules.int>
#
# 三条口径（决定了这个脚本为什么这么写）：
#   1) **不造档案**：档案一律是产品生成的（`run-extension-ui.ps1 -Step draft` 的产物），
#      这里只做"摆位置 + 写索引 + 写坏法"。理由：E-18 验的就是"产品能不能吃下远端那份真档案"，
#      自己合成一份格式相近的东西等于自问自答。
#   2) **索引列全部由本脚本独立算出来**（不抄产品的 manifest.txt）：
#        会社/作品    ← 档案 META（lib\rules-int.py dump 读出来）
#        目标exe摘要  ← 档案 IDENTITY 的 `target.sha256`（信任根，必须非空）
#        兄弟exe摘要  ← 档案 IDENTITY 的 `sibling.sha256`（可为空）
#        盐摘要       ← sha256(UTF-8(salt))，与 ExtensionSet::Sha256HexText 同口径
#        档案摘要     ← 该档案文件的 sha256
#        条数         ← STATS 的 directories + files
#      断言那侧会**自己再算一遍**（重新 dump + 重新 sha256），两边不共用一份结果。
#   3) **每个用例一个仓库根**（各自一个基址 URL）：坏法按目录隔离，一个用例的坏档案
#      不会污染另一个用例；"索引不存在"这种用例连 _faults.json 都不用写 —— 少放一个文件就是 404。
#
# 产物：<RepoRoot>\<case>\...（每个 case 一个可直接当基址的目录）+ 一句汇总 JSON：
#   { repoRoot, cases: { good: { root, basePath, indexRows, expected... }, ... } }
# 注意：base URL（含端口）由 serve-fixtures.ps1 决定，所以这里只写**相对路径**。

[CmdletBinding()]
param(
    [string]$ManifestPath = '',
    [string]$RepoRoot = '',
    [string]$OutJson = '',
    # 档案来源（不写就从夹具工作区的 ExtensionDraft 里找；找不到就报错，不静默造）
    [string]$OldArchive = '',
    [string]$OtherArchive = '',
    # 「与本地同一份」那一行用哪份档案（默认 = OldArchive：它正是被安装进库的那一份）
    [switch]$SkipShortBody,
    [switch]$SkipGarbage,
    # 慢响应用例要拖多久（打产品的接收超时 30s；默认 40 给它留余量）
    [double]$HangSeconds = 40.0
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')

$runRoot = Join-Path $root 'Run'
$outRoot = Join-Path $runRoot 'out'
if (-not $ManifestPath) { $ManifestPath = Join-Path $runRoot 'extension-manifest.json' }
if (-not $RepoRoot) { $RepoRoot = Join-Path $runRoot 'ExtensionRepo' }
if (-not $OutJson) { $OutJson = Join-Path $outRoot 'extension-online-repo.json' }
foreach ($directory in @($runRoot, $outRoot)) { if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null } }

$fixture = Read-JsonFile -Path $ManifestPath
if (-not $fixture) { throw ("缺少扩展集夹具清单：{0}；请先运行 make-extension-fixture.ps1" -f $ManifestPath) }

# ---------------------------------------------------------------
# Python（独立解析 rules.int）
# ---------------------------------------------------------------

function Get-HarnessPythonForRepo
{
    foreach ($candidate in @('C:\Users\Administrator\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'))
    {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $found = Get-Command python -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    throw '找不到 Python 运行时（独立解析 rules.int 需要它）'
}

$script:repoPython = Get-HarnessPythonForRepo
$script:dumpCounter = 0

function Get-ArchiveIdentity([string]$Path)
{
    if (-not (Test-Path -LiteralPath $Path)) { throw ("档案不存在：{0}" -f $Path) }
    # 用 scratch 子目录，别把中间产物撒在 out 根上
    $scratch = Join-Path $outRoot 'repo-dump'
    if (-not (Test-Path -LiteralPath $scratch)) { New-Item -ItemType Directory -Path $scratch -Force | Out-Null }
    $script:dumpCounter++
    $jsonPath = Join-Path $scratch ('dump-{0}.json' -f $script:dumpCounter)
    & $script:repoPython (Join-Path $root 'lib\rules-int.py') dump --in $Path --json $jsonPath | Out-Null
    $dump = Read-JsonFile -Path $jsonPath
    if (-not $dump) { throw ("独立解析档案失败（读不到 dump JSON）：{0}" -f $Path) }
    $problems = @($dump.problems)
    if ($problems.Count -gt 0)
    {
        throw ("档案结构本身就有问题，不能当远端夹具（先查生成那一步）：{0}`r`n  - {1}" -f $Path, ($problems -join "`r`n  - "))
    }

    $meta = Get-ObjectValue -Object $dump.parsed -Name 'meta' -Default $null
    $identity = Get-ObjectValue -Object $dump.parsed -Name 'identity' -Default $null
    $stats = Get-ObjectValue -Object $dump.parsed -Name 'stats' -Default $null
    $dirs = @(Get-ObjectValue -Object $dump.parsed -Name 'dirs' -Default @())
    $files = @(Get-ObjectValue -Object $dump.parsed -Name 'files' -Default @())
    $exceptions = @(Get-ObjectValue -Object $dump.parsed -Name 'exceptions' -Default @())

    $publisher = [string](Get-ObjectValue -Object $meta -Name 'publisher' -Default '')
    $title = [string](Get-ObjectValue -Object $meta -Name 'title' -Default '')
    if (-not $publisher -or -not $title) { throw ("档案 META 里的会社 / 作品为空，拼不出远端路径：{0}" -f $Path) }

    $salt = [string](Get-ObjectValue -Object $identity -Name 'salt' -Default '')
    $targetSha = [string](Get-ObjectValue -Object $identity -Name 'target.sha256' -Default '')
    $siblingSha = [string](Get-ObjectValue -Object $identity -Name 'sibling.sha256' -Default '')

    # 条数口径与 ExtensionSet 的 manifest 列一致：STATS.directories + STATS.files
    # （STATS 缺失/为 0 时退回"DIRS/FILES 的实际条数"，并在自证里标出来）
    $statDirs = [int](Get-ObjectValue -Object $stats -Name 'directories' -Default 0)
    $statFiles = [int](Get-ObjectValue -Object $stats -Name 'files' -Default 0)
    $statsUsable = (($statDirs + $statFiles) -gt 0) -and ($statDirs -eq $dirs.Count) -and ($statFiles -eq $files.Count)
    $entryCount = if ($statsUsable) { $statDirs + $statFiles } else { $dirs.Count + $files.Count }

    return [pscustomobject]@{
        Path          = [System.IO.Path]::GetFullPath($Path)
        FileSize      = [long]$dump.fileSize
        ArchiveSha256 = (Get-Sha256Hex -Path $Path)
        Publisher     = $publisher
        Title         = $title
        ExeDigest     = $targetSha
        SiblingDigest = $siblingSha
        SaltDigest    = (Get-TextSha256Hex -Text $salt)
        Salt          = $salt
        DirCount      = $dirs.Count
        FileCount     = $files.Count
        ExceptionCount = $exceptions.Count
        EntryCount    = $entryCount
        StatsUsable   = $statsUsable
    }
}

# ---------------------------------------------------------------
# 档案来源
# ---------------------------------------------------------------

function Find-DraftArchive([string]$Workspace)
{
    if (-not $Workspace) { return '' }
    $path = Join-Path $Workspace 'ExtensionDraft\rules.int'
    if (Test-Path -LiteralPath $path) { return $path }
    return ''
}

if (-not $OldArchive)
{
    $OldArchive = Find-DraftArchive ([string]$fixture.oldGame.workspace)
}
if (-not $OtherArchive)
{
    $OtherArchive = Find-DraftArchive ([string]$fixture.otherGame.workspace)
}
foreach ($pair in @(@('主档案（old）', $OldArchive), @('对照档案（other）', $OtherArchive)))
{
    if (-not $pair[1] -or -not (Test-Path -LiteralPath $pair[1]))
    {
        throw ("{0} 找不到：'{1}'。先跑 `pwsh -File run-extension-ui.ps1 -Step meta,draft -Game old`（和 -Game other）生成，" +
            "或用 -OldArchive / -OtherArchive 指定。" -f $pair[0], $pair[1])
    }
}

$old = Get-ArchiveIdentity $OldArchive
$other = Get-ArchiveIdentity $OtherArchive
if ($old.ExeDigest -eq $other.ExeDigest)
{
    throw ('两份档案的「目标 exe 摘要」相同 —— 这会让"盐不一致"用例退化成"同一份档案"，夹具不对。' +
        '请确认 other 那份是别的游戏（OtherGame 的 exe 摘要本来就与 OldGame 不同）。')
}

Write-Host ("[online-repo] 主档案  ：{0} / {1}（{2} 条，档案摘要 {3}…）" -f $old.Publisher, $old.Title, $old.EntryCount, $old.ArchiveSha256.Substring(0, 16))
Write-Host ("[online-repo] 对照档案：{0} / {1}（{2} 条，档案摘要 {3}…）" -f $other.Publisher, $other.Title, $other.EntryCount, $other.ArchiveSha256.Substring(0, 16))

# ---------------------------------------------------------------
# 写一行 index.txt（10 列 TSV，表头与 ExtensionSet::WriteManifest 一字不差）
# ---------------------------------------------------------------

$script:indexHeader = "# 会社`t作品`t目标exe摘要`t兄弟exe摘要`t盐摘要`t档案摘要`t条数`t生成时间`t安装时间`t来源"

function New-IndexRow
{
    param($Archive, [string]$ArchiveDigestOverride = '', [string]$ExeDigestOverride = '',
        [string]$PublisherOverride = '', [string]$TitleOverride = '')

    $digest = if ($ArchiveDigestOverride) { $ArchiveDigestOverride } else { $Archive.ArchiveSha256 }
    $exe = if ($ExeDigestOverride) { $ExeDigestOverride } else { $Archive.ExeDigest }
    return (@(
            $(if ($PublisherOverride) { $PublisherOverride } else { $Archive.Publisher })
            $(if ($TitleOverride) { $TitleOverride } else { $Archive.Title })
            $exe
            $Archive.SiblingDigest
            $Archive.SaltDigest
            $digest
            "$($Archive.EntryCount)"
            '2026-10-15 00:00:00'
            ''
            '远端仓库（验证夹具）'
        ) -join "`t")
}

function Write-IndexFile
{
    param([string]$Path, [AllowEmptyCollection()][string[]]$Rows)

    $text = (@($script:indexHeader) + @($Rows)) -join "`r`n"
    Write-Utf8File -Path $Path -Text $text -NoNewline
}

function Copy-ArchiveInto
{
    param([string]$RepoDirectory, $Archive, [string]$Publisher = '', [string]$Title = '')

    $publisher = if ($Publisher) { $Publisher } else { $Archive.Publisher }
    $title = if ($Title) { $Title } else { $Archive.Title }
    $directory = Join-Path (Join-Path $RepoDirectory $publisher) $title
    if (-not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    $target = Join-Path $directory 'rules.int'
    Copy-Item -LiteralPath $Archive.Path -Destination $target -Force
    return $target
}

function Write-Faults
{
    param([string]$RepoDirectory, $Faults)

    $text = ($Faults | ConvertTo-Json -Depth 5)
    Write-Utf8File -Path (Join-Path $RepoDirectory '_faults.json') -Text $text -NoNewline
}

# ---------------------------------------------------------------
# 逐个 case 搭仓库
# ---------------------------------------------------------------

if (Test-Path -LiteralPath $RepoRoot) { [void](Remove-TreeUnder -Path $RepoRoot -MustBeUnder $runRoot) }
New-Item -ItemType Directory -Path $RepoRoot -Force | Out-Null

$cases = [ordered]@{}
function Add-Case([string]$Name, [string]$RootPath, [string]$Description, $IndexRows, [string]$Expected)
{
    $cases[$Name] = [ordered]@{
        name        = $Name
        root        = $RootPath
        indexFile   = (Join-Path $RootPath 'index.txt')
        indexRows   = @($IndexRows)
        description = $Description
        expected    = $Expected
    }
    Write-Host ("  · {0,-18} {1}" -f $Name, $Description)
}

Write-Host '[online-repo] 搭建仓库：'

# ---------- good：正常仓库（E-18 正例）----------
# 两行：
#   ① 对照档案（other 的作品）——本地库里没有 → New（下载安装后库清单多一条）
#   ② 主档案（old 的作品）     ——本地库里已经有同一份 → Same（用来考"重复更新幂等"）
$goodRoot = Join-Path $RepoRoot 'good'
New-Item -ItemType Directory -Path $goodRoot -Force | Out-Null
[void](Copy-ArchiveInto -RepoDirectory $goodRoot -Archive $other)
[void](Copy-ArchiveInto -RepoDirectory $goodRoot -Archive $old)
$goodRows = @((New-IndexRow $other), (New-IndexRow $old))
Write-IndexFile -Path (Join-Path $goodRoot 'index.txt') -Rows $goodRows
Add-Case 'good' $goodRoot '正常仓库：1 条新增（对照档案）+ 1 条与本地同一份（主档案）' $goodRows 'index-ok'

# ---------- bad-sha256：索引里的档案摘要错一位 ----------
$badShaRoot = Join-Path $RepoRoot 'bad-sha256'
New-Item -ItemType Directory -Path $badShaRoot -Force | Out-Null
[void](Copy-ArchiveInto -RepoDirectory $badShaRoot -Archive $other)
# 错一位要落在**开头**：人眼看证据时前 16 位就直接对不上（写末尾的话两条摘要看起来一模一样）
$flipped = $(if ($other.ArchiveSha256.StartsWith('0')) { '1' } else { '0' }) + $other.ArchiveSha256.Substring(1)
$badShaRows = @((New-IndexRow $other -ArchiveDigestOverride $flipped))
Write-IndexFile -Path (Join-Path $badShaRoot 'index.txt') -Rows $badShaRows
Add-Case 'bad-sha256' $badShaRoot ('索引里的档案摘要故意改了一位数（正确 {0}…，索引写 {1}…）→ 下载回来的字节必须被拒' -f $other.ArchiveSha256.Substring(0, 16), $flipped.Substring(0, 16)) $badShaRows 'archive-digest-mismatch'

# ---------- truncate：只发前一半（Content-Length 也按截断后的长度）----------
$truncateRoot = Join-Path $RepoRoot 'truncate'
New-Item -ItemType Directory -Path $truncateRoot -Force | Out-Null
[void](Copy-ArchiveInto -RepoDirectory $truncateRoot -Archive $other)
$truncateRows = @((New-IndexRow $other))
Write-IndexFile -Path (Join-Path $truncateRoot 'index.txt') -Rows $truncateRows
Write-Faults -RepoDirectory $truncateRoot -Faults ([ordered]@{
        ('{0}/{1}/rules.int' -f $other.Publisher, $other.Title) = [ordered]@{ mode = 'truncate'; ratio = 0.5 }
    })
Add-Case 'truncate' $truncateRoot '档案只发前一半（对产品来说是一份"完整的短文件"）→ sha256 / 结构自洽必须抓出来' $truncateRows 'archive-truncated'

# ---------- truncate-declared：截断**且索引摘要就是截断后那份的摘要** ----------
# 这条比 truncate 更狠：sha256 那一条防线**过得去**（索引与字节确实一致），
# 所以必须靠更深的两条拦下 —— 解码（magic/版本/CRC/chunk）与自洽性（EXCEPTIONS=0 + 展开条数对 STATS）。
# 现实里对应的场景是"仓库侧自己发布了一份被截断的档案，并且索引也按它算了摘要"。
$declaredRoot = Join-Path $RepoRoot 'truncate-declared'
New-Item -ItemType Directory -Path $declaredRoot -Force | Out-Null
[void](Copy-ArchiveInto -RepoDirectory $declaredRoot -Archive $other)
$otherBytes = [System.IO.File]::ReadAllBytes($other.Path)
$keepBytes = [int]([Math]::Floor($otherBytes.Length * 0.5))
$halfBytes = New-Object byte[] $keepBytes
[Array]::Copy($otherBytes, 0, $halfBytes, 0, $keepBytes)
$sha256 = [System.Security.Cryptography.SHA256]::Create()
try { $halfDigest = ([System.BitConverter]::ToString($sha256.ComputeHash($halfBytes)) -replace '-', '') }
finally { $sha256.Dispose() }
$declaredRows = @((New-IndexRow $other -ArchiveDigestOverride $halfDigest))
Write-IndexFile -Path (Join-Path $declaredRoot 'index.txt') -Rows $declaredRows
Write-Faults -RepoDirectory $declaredRoot -Faults ([ordered]@{
        ('{0}/{1}/rules.int' -f $other.Publisher, $other.Title) = [ordered]@{ mode = 'truncate'; ratio = 0.5 }
    })
Add-Case 'truncate-declared' $declaredRoot ('截断且索引摘要与截断后的字节一致（{0}…）→ sha256 过得去，必须靠结构 / 自洽性拦下' -f $halfDigest.Substring(0, 16)) $declaredRows 'archive-structure'

if (-not $SkipShortBody)
{
    # ---------- short-body：Content-Length 报全长、只发一半就断 ----------
    $shortRoot = Join-Path $RepoRoot 'short-body'
    New-Item -ItemType Directory -Path $shortRoot -Force | Out-Null
    [void](Copy-ArchiveInto -RepoDirectory $shortRoot -Archive $other)
    $shortRows = @((New-IndexRow $other))
    Write-IndexFile -Path (Join-Path $shortRoot 'index.txt') -Rows $shortRows
    Write-Faults -RepoDirectory $shortRoot -Faults ([ordered]@{
            ('{0}/{1}/rules.int' -f $other.Publisher, $other.Title) = [ordered]@{ mode = 'short-body'; ratio = 0.5 }
        })
    Add-Case 'short-body' $shortRoot 'Content-Length 报全长但只发一半就断开 → 必须报"连接被中断 / 读不满"' $shortRows 'archive-short-read'
}

if (-not $SkipGarbage)
{
    # ---------- garbage：声称 200，内容是一堆随机字节 ----------
    $garbageRoot = Join-Path $RepoRoot 'garbage'
    New-Item -ItemType Directory -Path $garbageRoot -Force | Out-Null
    [void](Copy-ArchiveInto -RepoDirectory $garbageRoot -Archive $other)
    $garbageRows = @((New-IndexRow $other))
    Write-IndexFile -Path (Join-Path $garbageRoot 'index.txt') -Rows $garbageRows
    Write-Faults -RepoDirectory $garbageRoot -Faults ([ordered]@{
            ('{0}/{1}/rules.int' -f $other.Publisher, $other.Title) = [ordered]@{ mode = 'garbage'; bytes = 4096 }
        })
    Add-Case 'garbage' $garbageRoot '档案位置返回随机字节（200 + 正确长度）→ 结构校验必须拦下' $garbageRows 'archive-garbage'
}

# ---------- salt-mismatch：位置上摆的是**对照档案**（别的游戏、别的盐），
#            索引那一行声称的目标 exe 摘要来自**主档案**。
#            信任链第 4 条（内嵌 exe 摘要 == 索引里的目标 exe 摘要）必须把它拦下。
#
#            **P-1 回归（2026-10-16 修）**：索引行的 会社/作品 必须与位置上那份档案的 META
#            **一致**，否则 `VerifyArchive` 新加的交叉校验（§14.4 第 9 条：索引名 ≠ META 名 → 拒）
#            会**先**把它拒掉，这条用例想考的"内嵌 exe 摘要对索引"那条链根本走不到 ——
#            也就是"因为错误的原因被拒"（上一轮 S-1 那类问题）。
#            所以：位置与索引名都用**对照档案自己的**名字，唯一错的地方是 exe 摘要列。
#            另外档案摘要**故意写对照档案的**真摘要 —— 这样 sha256 那条防线是过的
#            （不然会被 sha256 顺带拦住，等于没测到）。
$saltRoot = Join-Path $RepoRoot 'salt-mismatch'
New-Item -ItemType Directory -Path $saltRoot -Force | Out-Null
[void](Copy-ArchiveInto -RepoDirectory $saltRoot -Archive $other)
$saltRows = @((New-IndexRow $other -ExeDigestOverride $old.ExeDigest))
Write-IndexFile -Path (Join-Path $saltRoot 'index.txt') -Rows $saltRows
Add-Case 'salt-mismatch' $saltRoot ('位置上的档案是别的盐的（{0}），索引那行声称的目标 exe 摘要来自主档案 → 内嵌摘要对索引必须不过（索引名与档案 META 一致，别被名字校验抢答）' -f $other.Salt) $saltRows 'embedded-exe-digest-mismatch'

# ---------- index-name-mismatch：**只错名字**（§14.4 第 9 条，P-1 的修复）----------
# 索引行的 会社/作品 写 A/B，位置上那份档案的内嵌 META 是 C/D；其余三列（档案摘要 / 目标 exe 摘要 /
# 条数）全部与档案**真实值一致** —— 所以 sha256 过得去、内嵌 exe 摘要也过得去，唯一的问题就是名字对不上。
# 这样才考得到"索引名与档案 META 交叉校验"那一条（要是把摘要也弄错，会命中别的防线 = 因为错误的原因被拒）。
$nameRoot = Join-Path $RepoRoot 'index-name-mismatch'
New-Item -ItemType Directory -Path $nameRoot -Force | Out-Null
$indexPublisher = 'IndexSaysThis'
$indexTitle = 'AndThis'
# 档案本体摆到**索引名**对应的位置（URL 是按索引名拼的），但它的 META 仍是对照档案自己的名字
[void](Copy-ArchiveInto -RepoDirectory $nameRoot -Archive $other -Publisher $indexPublisher -Title $indexTitle)
$nameRows = @((New-IndexRow $other -PublisherOverride $indexPublisher -TitleOverride $indexTitle))
Write-IndexFile -Path (Join-Path $nameRoot 'index.txt') -Rows $nameRows
Add-Case 'index-name-mismatch' $nameRoot ('索引名（{0} / {1}）与档案内嵌 META 名（{2} / {3}）故意不一致，其余列全部正确 → 必须按"名字对不上"这条拒，且点名两边' -f `
        $indexPublisher, $indexTitle, $other.Publisher, $other.Title) $nameRows 'index-name-mismatch'

# ---------- index-cols：列数不对（9 列）----------
$colsRoot = Join-Path $RepoRoot 'index-cols'
New-Item -ItemType Directory -Path $colsRoot -Force | Out-Null
[void](Copy-ArchiveInto -RepoDirectory $colsRoot -Archive $other)
$colsRows = @(($goodRows[0] -split "`t")[0..8] -join "`t")
Write-IndexFile -Path (Join-Path $colsRoot 'index.txt') -Rows $colsRows
Add-Case 'index-cols' $colsRoot '索引行只有 9 列（少一列）→ 必须报错并点名行号，不许静默跳过' $colsRows 'index-malformed-cols'

# ---------- index-empty-publisher：会社列为空 ----------
$emptyPubRoot = Join-Path $RepoRoot 'index-empty-publisher'
New-Item -ItemType Directory -Path $emptyPubRoot -Force | Out-Null
[void](Copy-ArchiveInto -RepoDirectory $emptyPubRoot -Archive $other)
$emptyPubFields = @($goodRows[0] -split "`t")
$emptyPubFields[0] = ''
$emptyPubRows = @($emptyPubFields -join "`t")
Write-IndexFile -Path (Join-Path $emptyPubRoot 'index.txt') -Rows $emptyPubRows
Add-Case 'index-empty-publisher' $emptyPubRoot '索引行的会社为空 → 路径拼不出来，必须整份拒（不许当成"远端为空"）' $emptyPubRows 'index-malformed-publisher'

# ---------- index-bad-digest：摘要不是 64 位 hex ----------
$badDigestRoot = Join-Path $RepoRoot 'index-bad-digest'
New-Item -ItemType Directory -Path $badDigestRoot -Force | Out-Null
[void](Copy-ArchiveInto -RepoDirectory $badDigestRoot -Archive $other)
$badDigestFields = @($goodRows[0] -split "`t")
$badDigestFields[5] = 'NOT-A-HEX-DIGEST-ZZZZ'
$badDigestRows = @($badDigestFields -join "`t")
Write-IndexFile -Path (Join-Path $badDigestRoot 'index.txt') -Rows $badDigestRows
Add-Case 'index-bad-digest' $badDigestRoot '索引行的档案摘要不是 64 位 hex → 这一份文本不可信，整份拒' $badDigestRows 'index-malformed-digest'

# ---------- index-empty-digest：档案摘要列为空 ----------
$emptyDigestRoot = Join-Path $RepoRoot 'index-empty-digest'
New-Item -ItemType Directory -Path $emptyDigestRoot -Force | Out-Null
[void](Copy-ArchiveInto -RepoDirectory $emptyDigestRoot -Archive $other)
$emptyDigestFields = @($goodRows[0] -split "`t")
$emptyDigestFields[5] = ''
$emptyDigestRows = @($emptyDigestFields -join "`t")
Write-IndexFile -Path (Join-Path $emptyDigestRoot 'index.txt') -Rows $emptyDigestRows
Add-Case 'index-empty-digest' $emptyDigestRoot '索引行的档案摘要为空 → 没有信任根，必须拒（§14.3）' $emptyDigestRows 'index-missing-digest'

# ---------- missing-archive：索引正常，但档案路径上根本没有文件（真 404）----------
$missingRoot = Join-Path $RepoRoot 'missing-archive'
New-Item -ItemType Directory -Path $missingRoot -Force | Out-Null
$missingRows = @((New-IndexRow $other))
Write-IndexFile -Path (Join-Path $missingRoot 'index.txt') -Rows $missingRows
Add-Case 'missing-archive' $missingRoot '索引指向 <会社>/<作品>/rules.int，但仓库里**没有**这个文件（真 404）→ 下载那一步必须报 404' $missingRows 'archive-404'

# ---------- no-index：连索引都没有（真 404）----------
$noIndexRoot = Join-Path $RepoRoot 'no-index'
New-Item -ItemType Directory -Path $noIndexRoot -Force | Out-Null
Add-Case 'no-index' $noIndexRoot '仓库根下没有 index.txt（真 404）→ 拉索引必须报 404' @() 'index-404'

# ---------- slow：index.txt 存在但服务器收下请求后**什么都不发**（打接收超时 30s）----------
$slowRoot = Join-Path $RepoRoot 'slow'
New-Item -ItemType Directory -Path $slowRoot -Force | Out-Null
[void](Copy-ArchiveInto -RepoDirectory $slowRoot -Archive $other)
$slowRows = @((New-IndexRow $other))
Write-IndexFile -Path (Join-Path $slowRoot 'index.txt') -Rows $slowRows
Write-Faults -RepoDirectory $slowRoot -Faults ([ordered]@{
        'index.txt' = [ordered]@{ mode = 'hang'; seconds = $HangSeconds }
    })
Add-Case 'slow' $slowRoot ('index.txt 拖住 {0} 秒不回应 → 必须报"等不到对方的回应（超时…）"，且本地一个字节都不改' -f $HangSeconds) $slowRows 'index-timeout'

# ---------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------

$summary = [ordered]@{
    generator      = 'make-online-repo.ps1'
    generatedUtc   = (Get-Date).ToUniversalTime().ToString('o')
    repoRoot       = $RepoRoot
    manifestPath   = $ManifestPath
    hangSeconds    = $HangSeconds
    # 档案侧的事实（断言会自己重算一遍，这里只是给人看的对照）
    old    = $old
    other  = $other
    indexHeader = $script:indexHeader
    cases  = $cases
}
Write-JsonFile -Path $OutJson -Object $summary
Write-Host ("[online-repo] 共 {0} 个仓库根 → {1}" -f $cases.Count, $RepoRoot)
Write-Host ("[online-repo] 汇总写入 {0}" -f $OutJson)
exit 0
