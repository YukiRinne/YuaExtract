# 自检：证明 assert-extension.ps1 的每条断言（E-1..E-19）都有鉴别力。
#
#   pwsh -File self-test-extension.ps1
#
# 做法与 self-test-assertions.ps1 / self-test-supplement.ps1 一致：**不跑产品**，
# 直接喂合成的运行结果（JSON）+ 合成的产物目录（真实文件，供"独立枚举"那几条断言用）：
#   1) "全对"的一份结果 → 所有断言必须通过（证明断言不是永远失败）
#   2) 每个场景故意做错一处 → **只有**该炸的断言炸（证明断言不是永远通过，也不是一炸一大片）
#
# 合成结果里的"期望值"全部来自真实夹具清单（Run\extension-manifest.json），
# 所以这里验的是"断言读得对不对"，不是"夹具造得对不对"。
#
# E-12..E-19（扩展集页签 + 在线更新）另有一份**合成世界**（New-SyntheticSetWorld）：
# 库清单与档案是真文件（档案由 lib\rules-int.py make 按 §3 规格造），远端仓库里的索引也是真 TSV，
# 这样"断言自己独立解析一遍"这条纪律在自检里同样成立（不然自检本身就成假绿了）。

[CmdletBinding()]
param(
    [string]$ManifestPath = '',
    [string]$ChecksJson = '',
    [switch]$KeepWork
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = $PSScriptRoot
. (Join-Path $root 'lib\HarnessUtil.ps1')
. (Join-Path $root 'lib\HarnessNative.ps1')
. (Join-Path $root 'lib\HarnessPixels.ps1')

$runRoot = Join-Path $root 'Run'
$outRoot = Join-Path $runRoot 'out'
$workRoot = Join-Path $runRoot 'selftest-extension'
if (-not $ManifestPath) { $ManifestPath = Join-Path $runRoot 'extension-manifest.json' }
if (-not $ChecksJson) { $ChecksJson = Join-Path $outRoot 'checks-selftest-extension.json' }

Reset-HarnessChecks

$fixture = Read-JsonFile -Path $ManifestPath
if (-not $fixture) { throw ("缺少扩展集夹具清单：{0}；请先运行 make-extension-fixture.ps1" -f $ManifestPath) }
$files = @(Get-ManifestFiles -JsonPath $ManifestPath)
$restorableCategories = @(Get-ObjectValue -Object $fixture.expectations -Name 'restorableCategories' -Default @('restored', 'mined', 'manual', 'recovered'))
$restorable = @($files | Where-Object { $_.category -in $restorableCategories })
$missing = @($files | Where-Object { $_.category -like 'missing-*' })
$plain = @($files | Where-Object { $_.category -eq 'plain' })
$negativeName = [string]$fixture.negativeName
$extDirs = @($fixture.extensionSet.dirs)
$extFiles = @($fixture.extensionSet.files)
$newUnpacked = [string]$fixture.newUnpacked.root
$exeSha = [string]$fixture.oldGame.gameExeSha256

Write-Host ''
Write-Host '=== 扩展集断言自检（鉴别力） ==='
Write-Host ("[自检] 夹具：可还原 {0} / 不可还原 {1} / 明文 {2}；目录层 {3} 条 / 文件层 {4} 条；负例名 {5}" -f `
        $restorable.Count, $missing.Count, $plain.Count, $extDirs.Count, $extFiles.Count, $negativeName)

if (-not (Test-Path -LiteralPath $newUnpacked)) { throw ("夹具的新解包目录不存在：{0}" -f $newUnpacked) }

function New-OutItem([string]$Rel, [string]$Sha)
{
    return [ordered]@{ rel = $Rel; size = 128; sha256 = $Sha }
}

function New-Digest([string]$Digest, [int]$Count)
{
    return [ordered]@{ Count = $Count; Digest = $Digest; HashDigest = ('H-' + $Digest) }
}

# 合成一份"运行结果"：字段与 run-extension-ui.ps1 写出来的一致
function New-RunRecord
{
    param(
        [string]$Name, [string]$Workspace, $Output, [string]$Overview = '', [string]$Failure = '',
        [string]$StaticLst = '', [int]$StaticLines = 0, [string]$TaskText = '（空闲）',
        $ExtractorBefore = $null, $ExtractorAfter = $null, $SourceBefore = $null, $SourceAfter = $null,
        $ArchiveDump = $null, [long]$ArchiveSize = 0, [string]$ArchiveSha = '', $StepResults = $null,
        [string]$ManifestText = '', [string]$ManifestPath2 = '', [string]$ReportText = '', [string]$LibraryArchive = '',
        $ExtractorContentDigest = $null, [string]$ExtractorOutputPath = '', $Dialogs = @()
    )
    return [ordered]@{
        case = $Name; workspace = $Workspace; failure = $Failure; completed = $true
        output = @($Output); outputTsv = ''; overviewText = $Overview; setStatusText = $Overview
        taskText = $TaskText; staticHashLst = $StaticLst; staticHashLineCount = $StaticLines
        draftArchiveSize = $ArchiveSize; draftArchiveSha256 = $ArchiveSha
        archiveDump = $ArchiveDump; stepResults = $StepResults
        extractorDigestBefore = $ExtractorBefore; extractorDigestAfter = $ExtractorAfter
        extractorContentDigest = $ExtractorContentDigest; extractorOutputPath = $ExtractorOutputPath
        sourceDigestBefore = $SourceBefore; sourceDigestAfter = $SourceAfter
        libraryManifestText = $ManifestText; libraryManifest = $ManifestPath2; libraryArchive = $LibraryArchive
        reportText = $ReportText; modalDialogs = @($Dialogs); notes = @('步骤2（提取动态 XP3 资源）用预置目录代替')
        toolLogs = @()
    }
}

function New-ArchiveDump([int]$DirCount, [int]$FileCount, [int]$ExceptionCount, [string[]]$DirNames = @(), [string[]]$FileNames = @(), [string[]]$Problems = @(), [string[]]$ExceptionLines = @(), [string]$ChunkTag = '', [string]$MetaTag = '', [string]$Salt = 'xp3hnp', [int]$Version = 1, [bool]$CrcOk = $true, [bool]$IncludeIdentity = $true)
{
    # 注意：PowerShell 里 `1..0` 是 @(1,0) 而不是空数组 —— 直接写 1..$Count 会在"0 条"时
    # 凭空造出 2 条（E-2 的"对照组不该有例外"就是这么假失败的）。
    function New-Range([int]$Count, [scriptblock]$Factory)
    {
        if ($Count -le 0) { return @() }
        $items = New-Object System.Collections.ArrayList
        for ($index = 1; $index -le $Count; $index++) { [void]$items.Add((& $Factory $index)) }
        return $items.ToArray()
    }
    # 注意：`$x = if (...) { $arr } else { ... }` 在**只有一个元素**时会被解包成标量，
    # 紧接着的 `.Count` 在 StrictMode 下直接抛 —— 一律在外面套 `@(...)`（踩过）。
    $dirs = @(if ($DirNames.Count -gt 0) { $DirNames } else { New-Range $DirCount { param($i) 'dir{0}/' -f $i } })
    $files = @(if ($FileNames.Count -gt 0) { $FileNames } else { New-Range $FileCount { param($i) 'file{0}.bin' -f $i } })
    # ExceptionLines 优先：正常夹具里那条例外是"夹具故意埋的 same.png 伪造哈希"，
    # 字符串必须与清单里的 expectedExceptions 一模一样，否则 E-2 的对照组会假失败。
    $exceptions = @(if ($ExceptionLines.Count -gt 0) { $ExceptionLines } else { New-Range $ExceptionCount { param($i) ('{0:X16}:bad{1}.bin' -f $i, $i) } })
    $chunks = New-Object System.Collections.ArrayList
    if ($MetaTag)
    {
        [void]$chunks.Add([ordered]@{ id = 1; name = 'META'; sha256 = ('M-' + $MetaTag); count = 1; size = 128; text = ('publisher=YuaHarness' + "`n" + 'generatedAt=' + $MetaTag) })
    }
    if ($IncludeIdentity)
    {
        [void]$chunks.Add([ordered]@{ id = 2; name = 'IDENTITY'; sha256 = ('I-' + $Salt); count = 1; size = 96 })
    }
    [void]$chunks.Add([ordered]@{ id = 3; name = 'DIRS'; sha256 = ('D' + $ChunkTag); count = $dirs.Count; size = 32 })
    [void]$chunks.Add([ordered]@{ id = 4; name = 'FILES'; sha256 = ('F' + $ChunkTag); count = $files.Count; size = 160 })
    if ($exceptions.Count -gt 0)
    {
        [void]$chunks.Add([ordered]@{ id = 5; name = 'EXCEPTIONS'; sha256 = 'E'; count = $exceptions.Count; size = 80 })
    }
    $meta = [ordered]@{}
    if ($MetaTag) { $meta['publisher'] = 'YuaHarness'; $meta['generatedAt'] = $MetaTag }
    return [ordered]@{
        magic = 'YXS1'; version = $Version; headerSize = 20; flags = 0; compressed = $false
        crc32Stored = '00000000'; crc32Computed = '00000000'; crcOk = $CrcOk; chunkCount = $chunks.Count
        problems = @($Problems)
        chunks = $chunks.ToArray()
        parsed = [ordered]@{ dirs = @($dirs); files = @($files); exceptions = @($exceptions); meta = $meta; salt = $Salt; stats = [ordered]@{} }
    }
}

# ==================================================================
# 合成"扩展集页签 + 在线更新"世界（E-12..E-19 的鉴别力自检用）
# ==================================================================
#
# 为什么需要它：E-12..E-19 读的是 run-extension-online.ps1 产出的记录，而自检**不跑产品**。
# 所以这里按同样的 schema 合成一份"全对"的世界，再按场景故意做错一处。
#
# 三件事必须是真的（否则自检自己就成了假绿）：
#   ① 库清单（manifest.txt）与档案（rules.int）是**真文件**：断言会独立解析它们
#      （清单按 10 列 TSV 解析、档案走 lib\rules-int.py dump），所以档案要用它造；
#   ② 合成档案的 META/IDENTITY/STATS 与清单行**逐项一致**（构造式保证，不是抄一遍）；
#   ③ 面板文字按产品的**真实措辞**拼（"会社：…" / "条数：N 条" / "档案：<路径>（<大小> B）"…）。

function New-SyntheticManifestRow
{
    param(
        [string]$Publisher, [string]$Title, [string]$ExeDigest, [string]$SaltDigest, [string]$ArchiveDigest,
        [string]$EntryCount, [string]$GeneratedAt = '2026-10-15 00:00:00', [string]$InstalledAt = '2026-10-16 00:00:00',
        [string]$Source = 'harness-synthetic'
    )
    return (@($Publisher, $Title, $ExeDigest, '', $SaltDigest, $ArchiveDigest, $EntryCount, $GeneratedAt, $InstalledAt, $Source) -join "`t")
}

function Get-SyntheticShortDigest
{
    param([string]$Digest)
    if (-not $Digest) { return '（未记录）' }
    if ($Digest.Length -gt 16) { return $Digest.Substring(0, 16) + '…' }
    return $Digest
}

# 合成一份"扩展集详情"面板文字（字段与产品一致；标签值由调用方给真值）
function New-SyntheticSetPanel
{
    param($Row, [string]$LibraryRoot, [string]$ArchivePath, [long]$ArchiveSize, [int]$PublisherCount = 1,
        [int]$EntryCount = 1, [string]$EntryCountText = '')

    $publisher = [string]$Row.Publisher
    $title = [string]$Row.Title
    $lines = New-Object System.Collections.ArrayList
    [void]$lines.Add('扩展集详情')
    [void]$lines.Add('「扩展集」页签管的是库里的扩展集：浏览、打开、在线更新、运行环境自检、看档案留言。')
    [void]$lines.Add('')
    [void]$lines.Add('扩展集库')
    [void]$lines.Add(('库根：{0}' -f $LibraryRoot))
    [void]$lines.Add(('共 {0} 个会社 / {1} 份扩展集' -f $PublisherCount, $EntryCount))
    [void]$lines.Add('')
    [void]$lines.Add('当前选择')
    [void]$lines.Add(('会社：{0}' -f $publisher))
    [void]$lines.Add(('作品：{0}' -f $title))
    # 「库内目录」按**产品的口径**给：sanitize 之后的实际落库目录；被改写时另起一行说明（P-4）。
    # 合成世界也必须这么做，否则"正确的那一份"自己就是错的（E-14-b 会在所有场景里假红）。
    $safePublisher = ConvertTo-SafePathComponent $publisher
    $safeTitle = ConvertTo-SafePathComponent $title
    [void]$lines.Add(('库内目录：{0}' -f (Join-Path (Join-Path $LibraryRoot $safePublisher) $safeTitle)))
    if ($safePublisher -ne $publisher -or $safeTitle -ne $title)
    {
        [void]$lines.Add(('（库里的目录名是改写过的：名字里的 \ / : * ? " < > | 之类字符被去掉了 —— 上面那两级才是库里真正的目录）'))
    }
    [void]$lines.Add(('档案：{0}（{1} B）' -f $ArchivePath, $ArchiveSize))
    [void]$lines.Add(('条数：{0}' -f $(if ($EntryCountText) { $EntryCountText } else { '{0} 条' -f [string]$Row.EntryCount })))
    [void]$lines.Add(('目标 exe 摘要：{0}' -f (Get-SyntheticShortDigest ([string]$Row.ExeDigest))))
    if ($Row.SiblingDigest) { [void]$lines.Add(('兄弟 exe 摘要：{0}' -f (Get-SyntheticShortDigest ([string]$Row.SiblingDigest)))) }
    [void]$lines.Add(('盐摘要：{0}' -f (Get-SyntheticShortDigest ([string]$Row.SaltDigest))))
    [void]$lines.Add(('档案摘要：{0}' -f (Get-SyntheticShortDigest ([string]$Row.ArchiveDigest))))
    [void]$lines.Add(('生成时间：{0}' -f $(if ($Row.GeneratedAt) { $Row.GeneratedAt } else { '（未记录）' })))
    [void]$lines.Add(('安装时间：{0}' -f $(if ($Row.InstalledAt) { $Row.InstalledAt } else { '（未记录）' })))
    [void]$lines.Add(('来源：{0}' -f $(if ($Row.Source) { $Row.Source } else { '（未记录）' })))
    [void]$lines.Add('')
    [void]$lines.Add('名字对不对，本工具验不了')
    [void]$lines.Add('最近操作')
    [void]$lines.Add('')
    return ($lines -join "`r`n")
}

function New-SyntheticSetWorld
{
    param(
        [Parameter(Mandatory)][string]$Dir,
        [Parameter(Mandatory)][string]$PythonPath,
        [hashtable]$Options = @{}
    )

    if (Test-Path -LiteralPath $Dir) { Remove-Item -LiteralPath $Dir -Recurse -Force }
    New-Item -ItemType Directory -Path $Dir -Force | Out-Null

    $pub = 'YuaHarness'
    $title = 'otomeki_ckr'
    $pub2 = 'YuaHarnessOnline'
    $title2 = 'OnlineWork'
    $salt = 'xp3hnp'
    $hexA = ('A' * 64 -join '')
    $hexB = ('B' * 64 -join '')

    $libraryRoot = Join-Path $Dir 'Library'
    $entryDir = Join-Path (Join-Path $libraryRoot $pub) $title
    New-Item -ItemType Directory -Path $entryDir -Force | Out-Null
    $archivePath = Join-Path $entryDir 'rules.int'
    $comment = 'YuaExtract 验证脚手架合成档案'
    if ($Options.ContainsKey('NoteComment')) { $comment = [string]$Options.NoteComment }

    & $PythonPath (Join-Path $root 'lib\rules-int.py') make --out $archivePath --publisher $pub --title $title `
        --salt $salt --exe-sha $hexA --comment $comment --dir 'characters/yuki/' --file 'base.png' | Out-Null
    if (-not (Test-Path -LiteralPath $archivePath)) { throw '合成档案没造出来（rules-int.py make 失败）' }

    $exeDigest = $hexA
    if ($Options.ContainsKey('PanelExeDigest')) { $exeDigest = [string]$Options.PanelExeDigest }
    $row = [pscustomobject]@{
        Publisher = $pub; Title = $title; ExeDigest = $hexA; SiblingDigest = ''
        SaltDigest = (Get-TextSha256Hex -Text $salt); ArchiveDigest = (Get-Sha256Hex -Path $archivePath)
        EntryCount = '2'; GeneratedAt = '2026-10-15 00:00:00'; InstalledAt = '2026-10-16 00:00:00'
        Source = 'harness-synthetic'
    }
    $archiveSize = (Get-Item -LiteralPath $archivePath).Length
    $manifestText = "# 会社`t作品`t目标exe摘要`t兄弟exe摘要`t盐摘要`t档案摘要`t条数`t生成时间`t安装时间`t来源`r`n" +
        (New-SyntheticManifestRow -Publisher $row.Publisher -Title $row.Title -ExeDigest $row.ExeDigest `
            -SaltDigest $row.SaltDigest -ArchiveDigest $row.ArchiveDigest -EntryCount $row.EntryCount `
            -GeneratedAt $row.GeneratedAt -InstalledAt $row.InstalledAt -Source $row.Source) + "`r`n"
    $manifestPath = Join-Path $libraryRoot 'manifest.txt'
    Write-Utf8File -Path $manifestPath -Text $manifestText -NoNewline

    # 面板（可选：让某个字段撒谎）
    $panel = New-SyntheticSetPanel -Row $row -LibraryRoot $libraryRoot -ArchivePath $archivePath -ArchiveSize $archiveSize
    if ($Options.ContainsKey('PanelEntryCountLies')) { $panel = New-SyntheticSetPanel -Row $row -LibraryRoot $libraryRoot -ArchivePath $archivePath -ArchiveSize $archiveSize -EntryCountText '999 条' }
    if ($Options.ContainsKey('PanelEmpty')) { $panel = "扩展集详情`r`n扩展集库`r`n库里一份扩展集都没有。两条来路：`r`n" }

    # ---------- 三次 setbrowse 取样（空库 / 1 条 / 多会社多作品）----------
    $emptyPanel = "扩展集详情`r`n扩展集库`r`n库里一份扩展集都没有。两条来路：`r`n"
    $multiManifest = $manifestText + (New-SyntheticManifestRow -Publisher $pub2 -Title $title2 -ExeDigest $hexB `
            -SaltDigest (Get-TextSha256Hex -Text 'othersalt123') -ArchiveDigest (('C' * 64 -join '')) -EntryCount '2') + "`r`n"

    function New-BrowseRecord
    {
        param([string]$OutPath, [string]$Tag, [string]$Manifest, [int]$PublisherCount, [object[]]$Publishers,
            [int]$TitleCount, [object[]]$Titles, [string]$SelectedPublisher, [string]$SelectedTitle, [string]$Panel,
            [int]$RowCount, [string]$ArchiveSnapshot = '', [string]$PageTextOverride = '')

        $record = [ordered]@{
            case = ('synthetic-set-' + $Tag); game = 'old'; steps = @('settab', 'setbrowse'); mode = 'set'
            startedUtc = (Get-Date).ToUniversalTime().ToString('o'); failure = $null; completed = $true
            events = @(); modalDialogs = @(); toolLogs = @()
            stepResults = @{}; output = @(); outputTsv = ''
            setBrowse = [ordered]@{
                stamp = $Tag; libraryRoot = $libraryRoot; libraryManifest = $manifestPath
                libraryManifestText = $Manifest; manifestRowCount = $RowCount
                manifestRows = @(); publishers = @($Publishers); titles = @($Titles)
                publisherCount = $PublisherCount; titleCount = $TitleCount
                selectedPublisher = $SelectedPublisher; selectedTitle = $SelectedTitle
                publisherIndex = 0; titleIndex = 0; libraryHidden = ($Tag -eq 'empty')
                pageBeforeSelect = ''; pageText = $(if ($PageTextOverride) { $PageTextOverride } else { $Panel })
                pageTextLength = $(if ($PageTextOverride) { $PageTextOverride.Length } else { $Panel.Length })
                manifestSnapshot = $manifestPath; archiveSnapshot = $ArchiveSnapshot; archiveSnapshotSha256 = ''
            }
        }
        Write-JsonFile -Path $OutPath -Object $record
        return $OutPath
    }

    $setEmptyJson = Join-Path $Dir 'extension-set-empty.json'
    # "空库"场景的两个做错法：
    #   EmptyReportsOnePublisher —— 空库却报出 1 个会社（计数撒谎）
    #   ListsUnreadable         —— 计数对、但**列表文字读不到**（MSAA 退化的假通过，最容易漏）
    $emptyPublisherCount = 0
    $emptyPublishers = @()
    $emptyTitleCount = 0
    $emptyTitles = @()
    $emptySelectedPublisher = ''
    $emptySelectedTitle = ''
    if ($Options.ContainsKey('EmptyReportsOnePublisher'))
    {
        $emptyPublisherCount = 1; $emptyPublishers = @($pub); $emptyTitleCount = 1; $emptyTitles = @($title)
        $emptySelectedPublisher = $pub; $emptySelectedTitle = $title
    }
    if ($Options.ContainsKey('ListsUnreadable'))
    {
        $emptyPublisherCount = 1; $emptyPublishers = @(); $emptyTitleCount = 1; $emptyTitles = @()
    }
    [void](New-BrowseRecord -OutPath $setEmptyJson -Tag 'empty' -Manifest '' -PublisherCount $emptyPublisherCount `
            -Publishers $emptyPublishers -TitleCount $emptyTitleCount -Titles $emptyTitles `
            -SelectedPublisher $emptySelectedPublisher -SelectedTitle $emptySelectedTitle -Panel $emptyPanel -RowCount 0)

    $setOneJson = Join-Path $Dir 'extension-set-one.json'
    [void](New-BrowseRecord -OutPath $setOneJson -Tag 'one' -Manifest $manifestText -PublisherCount 1 -Publishers @($pub) `
            -TitleCount 1 -Titles @($title) -SelectedPublisher $pub -SelectedTitle $title -Panel $panel -RowCount 1 `
            -ArchiveSnapshot $archivePath)

    $setMultiJson = Join-Path $Dir 'extension-set-multi.json'
    # 多会社多作品时面板的"共 N 个会社 / M 份扩展集"要跟着变（真实产品就是这么显示的）
    $multiPanel = New-SyntheticSetPanel -Row $row -LibraryRoot $libraryRoot -ArchivePath $archivePath -ArchiveSize $archiveSize `
        -PublisherCount 2 -EntryCount 2
    [void](New-BrowseRecord -OutPath $setMultiJson -Tag 'multi' -Manifest $multiManifest -PublisherCount 2 `
            -Publishers @($pub, $pub2) -TitleCount 1 -Titles @($title) -SelectedPublisher $pub -SelectedTitle $title `
            -Panel $multiPanel -RowCount 2 -ArchiveSnapshot $archivePath)

    # 页签骨架（三路取样；可见集合按真实实测：set 页 = 页签 + 4071 + 4072 + 4080-4084 + 4130-4136）
    $setVisible = @(4001, 4002, 4003, 4004, 4071, 4072, 4080, 4081, 4082, 4083, 4084, 4130, 4131, 4132, 4133, 4134, 4135, 4136)
    $homeVisible = @(4001, 4002, 4003, 4004, 4010, 4011, 4012, 4013, 4014, 4020, 4021, 4022, 4030, 4031, 4032, 4040, 4050, 4051, 4052,
        4060, 4061, 4062, 4063, 4064, 4070, 4072, 4074, 4080, 4081, 4082, 4083, 4084, 4085, 4086, 4087, 4088)
    if ($Options.ContainsKey('SkeletonLeakHomeControl')) { $setVisible += 4050 }        # 首页主流程按钮该隐却可见
    if ($Options.ContainsKey('SkeletonLeakBack')) { $homeVisible += 4131 }              # 切回首页后本页签控件没隐
    if ($Options.ContainsKey('SkeletonMissingControl')) { $setVisible = @($setVisible | Where-Object { $_ -ne 4134 }) }
    $aboutVisible = @(4001, 4002, 4003, 4004, 4071, 4072)
    if ($Options.ContainsKey('AboutLeaksSetControl')) { $aboutVisible += 4131 }
    # 「关于」页签正文（取真实措辞的锚点：构建指纹栏 / SHA-256 / 免责声明）；三个做错法在下面
    $aboutText = ('关于 YuaExtract' + "`r`n" + 'YuaExtract 1.0.0' + "`r`n`r`n" + '功能模块' + "`r`n" +
        'CxdecExtractor.dll　—　XP3 动态解包核心' + "`r`n`r`n" + '目录约定' + "`r`n" + '构建指纹' + "`r`n" +
        '程序文件： YuaExtract.exe' + "`r`n" + '文件大小： 1173504 字节' + "`r`n" +
        'SHA-256： FAEB67605A77DF3F（前 16 位，标题栏取前 8 位）' + "`r`n`r`n" +
        '免责声明' + "`r`n" + '仅用于学习研究、兼容性分析、个人合法备份、汉化与资源修复等场景。请在你合法拥有的游戏副本上使用，不要用于传播游戏资源。')
    if ($Options.ContainsKey('AboutDropsFingerprint'))
    {
        $aboutText = $aboutText.Replace('SHA-256： FAEB67605A77DF3F（前 16 位，标题栏取前 8 位）', 'SHA-256：（读不到）')
    }
    if ($Options.ContainsKey('AboutDropsSection')) { $aboutText = $aboutText.Replace('构建指纹', '程序信息') }
    $setTabJson = Join-Path $Dir 'extension-set-tab.json'
    $setTabRecord = [ordered]@{
        case = 'synthetic-set-tab'; game = 'old'; steps = @('settab'); mode = 'set'
        startedUtc = (Get-Date).ToUniversalTime().ToString('o'); failure = $null; completed = $true
        events = @(); modalDialogs = @(); toolLogs = @(); stepResults = @{}; output = @(); outputTsv = ''
        setTab = [ordered]@{
            passes = @(
                [ordered]@{ name = 'set-1'; visible = @($setVisible); hidden = @($homeVisible); inventory = @() }
                [ordered]@{ name = 'home-back'; visible = @($homeVisible); hidden = @($setVisible); inventory = @() }
                [ordered]@{ name = 'set-2'; visible = @($setVisible); hidden = @($homeVisible); inventory = @() }
                [ordered]@{ name = 'set-3'; visible = @($setVisible); hidden = @($homeVisible); inventory = @() }
            )
            pageText = $panel; overviewText = ''; tabClass = 'Button'; tabText = '扩展集'
            about = [ordered]@{
                name = 'about'
                visible = @($aboutVisible)
                hidden = @(4130, 4131, 4132, 4133, 4134, 4135, 4136, 4080, 4081, 4082, 4083, 4084, 4070, 4050, 4051, 4052, 4030, 4031, 4032, 4085, 4086, 4087, 4088, 4100, 4101, 4102, 4103, 4104, 4105, 4106, 4107, 4108, 4109, 4110, 4111, 4010, 4020, 4021, 4022)
                inventory = @()
                pageText = $aboutText
                pageTextLength = $aboutText.Length
                takenAt = (Get-Date).ToUniversalTime().ToString('o')
                appFingerprint = 'FAEB67605A77DF3F'
                appExeSize = 1173504
            }
        }
    }
    Write-JsonFile -Path $setTabJson -Object $setTabRecord

    # ---------- 名字会被改写的条目（E-14-b / P-4）----------
    # 会社『A:B』、作品『Yuzu/Soft』 → 落库目录 sanitize 成 `AB\YuzuSoft`；
    # manifest 里记的是**原始名**，面板的「库内目录」必须给磁盘上真实存在的那一级（+ 改写说明）。
    $hostileRawPublisher = 'A:B'
    $hostileRawTitle = 'Yuzu/Soft'
    $hostileDir = Join-Path (Join-Path $libraryRoot 'AB') 'YuzuSoft'
    New-Item -ItemType Directory -Path $hostileDir -Force | Out-Null
    Copy-Item -LiteralPath $archivePath -Destination (Join-Path $hostileDir 'rules.int') -Force
    $hostileRow = New-SyntheticManifestRow -Publisher $hostileRawPublisher -Title $hostileRawTitle -ExeDigest $hexB `
        -SaltDigest (Get-TextSha256Hex -Text 'othersalt123') -ArchiveDigest (Get-Sha256Hex -Path (Join-Path $hostileDir 'rules.int')) `
        -EntryCount '2'
    $hostileManifest = $manifestText + $hostileRow + "`r`n"
    $hostilePanelBase = New-SyntheticSetPanel -Row ([pscustomobject]@{
            Publisher = $hostileRawPublisher; Title = $hostileRawTitle; ExeDigest = $hexB; SiblingDigest = ''
            SaltDigest = (Get-TextSha256Hex -Text 'othersalt123'); ArchiveDigest = (Get-Sha256Hex -Path (Join-Path $hostileDir 'rules.int'))
            EntryCount = '2'; GeneratedAt = ''; InstalledAt = ''; Source = 'harness-synthetic'
        }) -LibraryRoot $libraryRoot -ArchivePath (Join-Path $hostileDir 'rules.int') `
        -ArchiveSize (Get-Item -LiteralPath (Join-Path $hostileDir 'rules.int')).Length -PublisherCount 2 -EntryCount 2
    $hostilePanel = $hostilePanelBase
    if ($Options.ContainsKey('HostileRawJoin'))
    {
        # **做错法**：面板给的是"拿原始名拼出来"的路径（那个路径根本不存在）且没有改写说明
        $rawDir = Join-Path (Join-Path $libraryRoot $hostileRawPublisher) $hostileRawTitle
        $hostilePanel = $hostilePanelBase.Replace($hostileDir, $rawDir)
    }
    $setHostileJson = Join-Path $Dir 'extension-set-hostile.json'
    [void](New-BrowseRecord -OutPath $setHostileJson -Tag 'hostile' -Manifest $hostileManifest -PublisherCount 2 `
            -Publishers @($pub, $hostileRawPublisher) -TitleCount 1 -Titles @($hostileRawTitle) `
            -SelectedPublisher $hostileRawPublisher -SelectedTitle $hostileRawTitle -Panel $hostilePanel -RowCount 2 `
            -ArchiveSnapshot (Join-Path $hostileDir 'rules.int'))

    # ---------- 打开选中扩展集（三路）----------
    $expectedDir = Join-Path (Join-Path $libraryRoot $pub) $title
    $openJson = [ordered]@{}
    foreach ($mode in @('none', 'ok', 'missing'))
    {
        $dialogs = @()
        $panelAfter = $panel
        $modeLog = ''
        $infoText = ''
        switch ($mode)
        {
            'none'
            {
                $infoText = '先在左侧「扩展集选择」里选一个会社，再选一个作品。'
                $dialogs = @([ordered]@{ Title = 'YuaExtract'; Text = $infoText; Buttons = '确定' })
            }
            'missing'
            {
                $infoText = ('这个扩展集在库里没有目录：' + "`r`n" + $expectedDir + "`r`n`r`n会社 / 作品是从列表里选出来的，目录却不在。")
                $dialogs = @([ordered]@{ Title = 'YuaExtract'; Text = $infoText; Buttons = '确定' })
                $panelAfter = $panel + "`r`n最近操作：打开失败：库内目录不存在"
            }
            'ok'
            {
                $panelAfter = $panel + "`r`n最近操作：已打开 " + $expectedDir
                $modeLog = ('[YuaExtract] 已打开扩展集目录：' + $expectedDir)
            }
        }
        if ($Options.ContainsKey('OpenPanelDropsPath') -and $mode -eq 'ok') { $panelAfter = $panel + "`r`n最近操作：已打开 （路径没写）" }
        if ($Options.ContainsKey('OpenLogDropsPath') -and $mode -eq 'ok') { $modeLog = '[YuaExtract] 已打开扩展集目录：' }
        $path = Join-Path $Dir ('extension-set-open-{0}.json' -f $mode)
        $logLines = @()
        if ($modeLog) { $logLines = @([ordered]@{ Name = 'YuaExtract.log'; Size = $modeLog.Length; Text = $modeLog }) }
        Write-JsonFile -Path $path -Object ([ordered]@{
                case = ('synthetic-set-open-' + $mode); game = 'old'; steps = @('settab', 'setbrowse', 'setopen'); mode = 'set'
                startedUtc = (Get-Date).ToUniversalTime().ToString('o'); failure = $null; completed = $true
                events = @(); modalDialogs = @($dialogs); toolLogs = @($logLines); stepResults = @{}; output = @(); outputTsv = ''
                setOpen = [ordered]@{
                    mode = $mode; libraryRoot = $libraryRoot; expectedDirectory = $(if ($mode -eq 'none') { '' } else { $expectedDir })
                    directoryMovedTo = ''; selectedAnything = ($mode -ne 'none')
                    panelBefore = $panel; panelAfter = $panelAfter; modals = @($dialogs)
                }
            })
        $openJson[$mode] = $path
    }

    # ---------- 扩展集留言 ----------
    $notePath = Join-Path $Dir 'extension-set-note.json'
    $noteText = ('扩展集留言：' + $pub + ' / ' + $title + "`r`n`r`n留言：" + $comment + "`r`n`r`n档案摘要：" +
        (Get-SyntheticShortDigest $row.ArchiveDigest) + "`r`n档案路径：" + $archivePath +
        "`r`n`r`n说明：档案里不存哈希层，所以本工具无法校验「名字对不对」—— 名字的最终把关是事实。")
    if ($Options.ContainsKey('NoteDropsComment')) { $noteText = $noteText.Replace($comment, '（这里没有留言）') }
    if ($Options.ContainsKey('NoteClaimsVerify')) { $noteText = $noteText.Replace('无法校验「名字对不对」', '能验证里面的名字对不对') }
    Write-JsonFile -Path $notePath -Object ([ordered]@{
            case = 'synthetic-set-note'; game = 'old'; steps = @('settab', 'setbrowse', 'setnote'); mode = 'set'
            startedUtc = (Get-Date).ToUniversalTime().ToString('o'); failure = $null; completed = $true
            events = @(); modalDialogs = @([ordered]@{ Title = 'YuaExtract'; Text = $noteText; Buttons = '确定' })
            toolLogs = @(); stepResults = @{}; output = @(); outputTsv = ''
            setBrowse = [ordered]@{ archiveSnapshot = $archivePath; libraryManifestText = $manifestText; pageText = $panel }
            setNote = [ordered]@{ dialogs = @([ordered]@{ title = 'YuaExtract'; text = $noteText; buttons = '确定' }); pageText = $panel }
        })

    # ---------- 运行环境自检（clean + 6 个变体）----------
    # 自检面板的**真实形状**：`· 项名：详情` 若干项（AppendSelfCheckRuns / SelfCheckPlainText 都是这个形状）。
    # E-16 现在判"必须由**那一项自己**报出来"，所以合成世界也得长成这样，否则那条判据没东西可判。
    $selfCheckItems = [ordered]@{
        '工具目录可写'                                    = '可写 —— E:\App'
        'Hash 收集模块 CxdecStringDumper.dll'             = 'E:\App\CxdecExtractordll\CxdecStringDumper.dll' + "`r`n" + '  32 位（IMAGE_FILE_MACHINE_I386），能注进 32 位游戏进程。'
        '扩展集库目录'                                    = '存在且可写 —— E:\App\ExtensionLibrary'
        '库清单 manifest.txt'                             = '可解析，共 1 条 —— E:\App\ExtensionLibrary\manifest.txt'
        '库内档案'                                        = '共 1 份，全部可解码、无例外、带目标 exe 摘要'
        '盐来源（Log\Universal.log 的 Hash Seed:）'        = '读到 Hash Seed:'
    }
    function New-SyntheticSelfCheckPanel([string]$FaultLabel, [string]$FaultDetail, [string]$WrongOwner = '', [string]$DropLabel = '')
    {
        # WrongOwner 非空 = 把"该报的措辞"塞到**别人那一项**里（做错法：全篇找词会假绿）
        $items = [ordered]@{}
        foreach ($key in $selfCheckItems.Keys)
        {
            if ($DropLabel -and $key -eq $DropLabel) { continue }
            $items[$key] = [string]$selfCheckItems[$key]
        }
        if ($WrongOwner) { $items[$WrongOwner] = [string]$items[$WrongOwner] + "`r`n  " + $FaultDetail }
        elseif ($FaultLabel) { $items[$FaultLabel] = $FaultDetail }
        $lines = New-Object System.Collections.ArrayList
        [void]$lines.Add($(if ($FaultLabel -or $WrongOwner -and $WrongOwner) { '结论：1 项问题' } else { '结论：全部正常' }))
        foreach ($key in $items.Keys) { [void]$lines.Add(('· ' + $key + '：' + $items[$key])) }
        return ($lines -join "`r`n")
    }

    $selfCheckVariants = @()
    foreach ($spec in @(
            @{ Name = 'clean'; Fault = ''; Detail = '' }
            @{ Name = 'bad-archive'; Fault = '库内档案'; Detail = '1 / 1 份有问题：' + "`r`n" + '  YuaHarness / otomeki_ckr：解码失败：档案结构损坏' }
            @{ Name = 'byte-flip'; Fault = '库内档案'; Detail = '1 / 1 份有问题：' + "`r`n" + '  YuaHarness / otomeki_ckr：解码失败：CRC32 不符' }
            @{ Name = 'manifest-malformed'; Fault = '库清单 manifest.txt'; Detail = '清单中有不合规的行已被跳过：第 4 行：列数不对（应有 10 列，实际 9 列）' }
            @{ Name = 'manifest-bad-digest'; Fault = '库清单 manifest.txt'; Detail = '库清单格式不对：第 4 行：目标 exe 摘要既不是空，也不是 64 位十六进制' }
            @{ Name = 'library-readonly'; Fault = '扩展集库目录'; Detail = '不可写（拒绝访问，Win32 错误码 5）' }
            @{ Name = 'missing-dll'; Fault = 'Hash 收集模块 CxdecStringDumper.dll'; Detail = '找不到：E:\App\CxdecExtractordll\CxdecStringDumper.dll' }
        ))
    {
        $faultLabel = [string]$spec.Fault
        $faultDetail = [string]$spec.Detail
        $wrongOwner = ''
        if ($Options.ContainsKey('SelfCheckWrongOwner') -and $spec.Name -eq 'bad-archive')
        {
            # 做错法：把「解码失败」塞进"工具目录可写"那一项，库内档案那一项报得干干净净
            $wrongOwner = '工具目录可写'
            $faultLabel = ''
            $faultDetail = '1 / 1 份有问题：解码失败：档案结构损坏'
        }
        $panelText = New-SyntheticSelfCheckPanel -FaultLabel $faultLabel -FaultDetail $faultDetail -WrongOwner $wrongOwner
        $modalList = @()
        if ($spec.Name -ne 'clean' -and -not $Options.ContainsKey('SelfCheckDropsItem'))
        {
            $modalList = @([ordered]@{ title = 'YuaExtract'; text = $panelText; buttons = '确定' })
        }
        if ($Options.ContainsKey('SelfCheckDropsItem') -and $spec.Name -eq 'missing-dll')
        {
            # 做错法：那一项**根本没报**（全篇找词照样能命中别的项里的词，所以旧判据会假绿）
            $panelText = New-SyntheticSelfCheckPanel -FaultLabel '' -FaultDetail '' -DropLabel 'Hash 收集模块 CxdecStringDumper.dll'
        }
        if ($Options.ContainsKey('SelfCheckSilent') -and $spec.Name -eq 'bad-archive')
        {
            $panelText = '运行环境自检：全部正常。'
            $modalList = @()
        }
        if ($Options.ContainsKey('SelfCheckTouchesFiles')) { $libraryDigestAfter = ('DIGEST-CHANGED') } else { $libraryDigestAfter = '1111111111111111111111111111111111111111111111111111111111111111' }
        $afterDirCount = 3
        if ($Options.ContainsKey('SelfCheckTouchesEmptyDir') -and $spec.Name -eq 'bad-archive')
        {
            # 做错法（N-S1）：文件一个没变、**只多了一个空目录** —— 摘要字符串故意与 before 相同，
            # 只有目录数变了；判据必须靠目录维度抓住它（只比文件的旧写法永远看不见）。
            $afterDirCount = 4
            $libraryDigestAfter = '1111111111111111111111111111111111111111111111111111111111111111'
        }
        $selfCheckVariants += [ordered]@{
            variant = $spec.Name; mutation = ('合成变体 ' + $spec.Name); mutationError = ''
            panelBefore = ''; panelAfter = $panelText; modals = @($modalList)
            libraryDigestBefore = [ordered]@{ Count = 3; DirCount = 3; Digest = '1111111111111111111111111111111111111111111111111111111111111111'; Exists = $true }
            libraryDigestAfter = [ordered]@{ Count = 3; DirCount = $afterDirCount; Digest = $libraryDigestAfter; Exists = $true }
            toolDigestBefore = [ordered]@{ Count = 7; DirCount = 2; Digest = '3333333333333333333333333333333333333333333333333333333333333333'; Exists = $true }
            toolDigestAfter = [ordered]@{ Count = 7; DirCount = 2; Digest = '3333333333333333333333333333333333333333333333333333333333333333'; Exists = $true }
        }
    }
    $checkCleanVariants = @($selfCheckVariants | Where-Object { $_.variant -eq 'clean' })
    $checkBadVariants = @($selfCheckVariants | Where-Object { $_.variant -ne 'clean' })
    function New-CheckRecord([string]$OutPath, [object[]]$Variants)
    {
        Write-JsonFile -Path $OutPath -Object ([ordered]@{
                case = 'synthetic-set-check'; game = 'old'; steps = @('settab', 'setbrowse', 'setcheck'); mode = 'set'
                startedUtc = (Get-Date).ToUniversalTime().ToString('o'); failure = $null; completed = $true
                events = @(); modalDialogs = @(); toolLogs = @(); stepResults = @{}; output = @(); outputTsv = ''
                setBrowse = [ordered]@{ libraryManifestText = $manifestText; pageText = $panel }
                setCheck = [ordered]@{ libraryRoot = $libraryRoot; entryArchive = $archivePath; dllPath = ''; variants = $Variants }
            })
        return $OutPath
    }
    $checkCleanJson = New-CheckRecord (Join-Path $Dir 'extension-set-check-clean.json') $checkCleanVariants
    $checkBadJson = New-CheckRecord (Join-Path $Dir 'extension-set-check-bad.json') $checkBadVariants

    # ---------- 远端仓库（真档案 + 真索引；E-18 会独立解析它们）----------
    $repoRoot = Join-Path $Dir 'Repo'
    $goodRoot = Join-Path $repoRoot 'good'
    $remoteEntryDir = Join-Path (Join-Path $goodRoot $pub2) $title2
    New-Item -ItemType Directory -Path $remoteEntryDir -Force | Out-Null
    $remoteArchive = Join-Path $remoteEntryDir 'rules.int'
    & $PythonPath (Join-Path $root 'lib\rules-int.py') make --out $remoteArchive --publisher $pub2 --title $title2 `
        --salt 'othersalt123' --exe-sha $hexB --comment '远端合成档案' --dir 'characters/yuki/' --file 'base.png' | Out-Null
    $remoteRow = New-SyntheticManifestRow -Publisher $pub2 -Title $title2 -ExeDigest $hexB `
        -SaltDigest (Get-TextSha256Hex -Text 'othersalt123') -ArchiveDigest (Get-Sha256Hex -Path $remoteArchive) -EntryCount '2'
    $goodIndexPath = Join-Path $goodRoot 'index.txt'
    $goodIndexText = "# 会社`t作品`t目标exe摘要`t兄弟exe摘要`t盐摘要`t档案摘要`t条数`t生成时间`t安装时间`t来源`r`n" + $remoteRow + "`r`n" +
        (New-SyntheticManifestRow -Publisher $row.Publisher -Title $row.Title -ExeDigest $row.ExeDigest `
            -SaltDigest $row.SaltDigest -ArchiveDigest $row.ArchiveDigest -EntryCount $row.EntryCount) + "`r`n"
    Write-Utf8File -Path $goodIndexPath -Text $goodIndexText -NoNewline

    # 正例的两轮记录（第一轮装进去、第二轮幂等）
    $afterManifestText = $manifestText + $remoteRow + "`r`n"
    $goodRunJson = Join-Path $Dir 'extension-online-good.json'
    $listItemRemote = ('{0} / {1} / 2 条 / 新增（本地还没有）' -f $pub2, $title2)
    $listItemLocal = ('{0} / {1} / 2 条 / 已是最新（档案摘要与本地相同）' -f $pub, $title)
    $roundOne = [ordered]@{
        round = 1; endpointShown = 'https://127.0.0.1:1/'
        statusAfterRefresh = ('索引拉取成功：共 2 条（实收 664 B）。' + "`r`n本地状态：新增 1 / 可更新 0 / 已是同一份 1 / 本地条数更多 0")
        listItemsAfterRefresh = @($listItemRemote, $listItemLocal); refreshElapsedMs = 800
        installClicked = $true
        statusAfterInstall = ('已新增：' + $pub2 + ' / ' + $title2 + '（2 条）' + "`r`n`r`n校验结论：会社：" + $pub2 +
            "`r`n已核：字节与索引一致（sha256）、结构完整（magic / 版本 / CRC32 / chunk）、内嵌 exe 摘要等于索引、无自校验例外。")
        listItemsAfterInstall = @($listItemRemote, $listItemLocal); installElapsedMs = 900; modals = @()
        manifestRowsAfter = 2; manifestTextAfter = $afterManifestText; libraryDigestAfter = [ordered]@{ Count = 5; Digest = '2222222222222222222222222222222222222222222222222222222222222222' }; stagingFiles = @()
    }
    $roundTwo = [ordered]@{
        round = 2; endpointShown = 'https://127.0.0.1:1/'
        statusAfterRefresh = ('索引拉取成功：共 2 条（实收 664 B）。' + "`r`n本地状态：新增 0 / 可更新 0 / 已是同一份 2 / 本地条数更多 0")
        listItemsAfterRefresh = @($listItemRemote, $listItemLocal); refreshElapsedMs = 800
        installClicked = $true
        statusAfterInstall = ($pub2 + ' / ' + $title2 + ' 本地已经是同一份（档案摘要相同），无需下载。')
        listItemsAfterInstall = @($listItemRemote, $listItemLocal); installElapsedMs = 700; modals = @()
        manifestRowsAfter = 2; manifestTextAfter = $afterManifestText; libraryDigestAfter = [ordered]@{ Count = 5; Digest = '2222222222222222222222222222222222222222222222222222222222222222' }; stagingFiles = @()
    }
    if ($Options.ContainsKey('GoodInstallLeavesStaging'))
    {
        $roundOne['stagingFiles'] = @([pscustomobject]@{ rel = 'Online_YuaHarnessOnline_OnlineWork.rules.int'; size = 619 })
    }
    if ($Options.ContainsKey('GoodNotIdempotent'))
    {
        $roundTwo['manifestRowsAfter'] = 3
        $roundTwo['manifestTextAfter'] = $afterManifestText + $remoteRow + "`r`n"
        $roundTwo['statusAfterInstall'] = ('已新增：' + $pub2 + ' / ' + $title2 + '（2 条）')
    }
    if ($Options.ContainsKey('GoodInstallLies'))
    {
        $roundOne['manifestRowsAfter'] = 3
        $roundOne['manifestTextAfter'] = $afterManifestText + $remoteRow + "`r`n"
    }
    Write-JsonFile -Path $goodRunJson -Object ([ordered]@{
            case = 'synthetic-online-good'; game = 'old'; steps = @('settab', 'setupdate'); mode = 'set'
            startedUtc = (Get-Date).ToUniversalTime().ToString('o'); failure = $null; completed = $true
            events = @(); modalDialogs = @(); toolLogs = @(); stepResults = @{}; output = @(); outputTsv = ''
            online = [ordered]@{
                endpoint = 'https://127.0.0.1:1/'; endpointShown = 'https://127.0.0.1:1/'; rounds = @($roundOne, $roundTwo)
                before = [ordered]@{ libraryRoot = $libraryRoot; libraryDigest = [ordered]@{ Count = 3; Digest = '1111111111111111111111111111111111111111111111111111111111111111' }
                    manifestText = $manifestText; manifestRows = 1; rows = @(); toolDigest = [ordered]@{ Count = 7; DirCount = 2; Digest = '3333333333333333333333333333333333333333333333333333333333333333' }; stagingFiles = @() }
                after = [ordered]@{ libraryRoot = $libraryRoot; libraryDigest = [ordered]@{ Count = 5; Digest = '2222222222222222222222222222222222222222222222222222222222222222' }
                    manifestText = $afterManifestText; manifestRows = 2; rows = @(); toolDigest = [ordered]@{ Count = 7; DirCount = 2; Digest = '3333333333333333333333333333333333333333333333333333333333333333' }; stagingFiles = @() }
                toolIniText = ''; pageTextAfterClose = $panel
            }
        })

    # 坏用例（每条一个 run JSON；serverRoot 随便指一个存在的目录即可）
    $badCaseStatus = [ordered]@{
        'online-bad-sha256'            = '下载 / 校验失败：' + "`r`n" + '档案摘要不符：这一份不是索引里指的那一份，已丢弃。'
        'online-truncate'              = '下载 / 校验失败：' + "`r`n" + '档案摘要不符：这一份不是索引里指的那一份，已丢弃。'
        'online-short-body'            = '下载 / 校验失败：' + "`r`n" + '拉取档案失败：连接被中断（中途断了，可能是网络不稳或对方掐了连接）。'
        'online-garbage'               = '下载 / 校验失败：' + "`r`n" + '档案结构校验不通过：magic 不是 YXS1'
        'online-salt-mismatch'         = '下载 / 校验失败：' + "`r`n" + '档案内嵌的目标 exe 摘要与索引不一致：这一份不是索引里那一条描述的东西。'
        'online-index-cols'            = '索引拉取失败：' + "`r`n" + '索引里有不合规的行（索引必须整份干净，一行都不能跳过）：' + "`r`n" + '  第 2 行：列数不对（应有 10 列，实际 9 列）。'
        'online-index-empty-publisher' = '索引拉取失败：' + "`r`n" + '索引里有不合规的行（索引必须整份干净，一行都不能跳过）：' + "`r`n" + '  第 2 行：会社为空。'
        'online-index-bad-digest'      = '索引拉取失败：' + "`r`n" + '索引格式不对（https://127.0.0.1:1/index.txt）：' + "`r`n" + '第 2 行：目标 exe 摘要既不是空，也不是 64 位十六进制'
        'online-index-empty-digest'    = '索引拉取失败：' + "`r`n" + '索引里「' + $pub2 + ' / ' + $title2 + '」这一条没有档案摘要（该列是空的）。'
        'online-truncate-declared'     = '下载 / 校验失败：' + "`r`n" + '档案结构校验不通过：payload 解压失败：数据错误(Z_DATA_ERROR)'
        'online-index-name-mismatch'   = '下载 / 校验失败：' + "`r`n" + '索引里这一条的会社 / 作品与档案内嵌 META 不一致：' + "`r`n" +
            '  索引说：IndexSaysThis / AndThis' + "`r`n" + '  档案自己的 META 说：' + $pub2 + ' / ' + $title2
        'online-missing-archive'       = '下载 / 校验失败：' + "`r`n" + '拉取档案失败：服务器上没有这个文件（HTTP 404）。'
    }
    if ($Options.ContainsKey('BadShaAccepted')) { $badCaseStatus['online-bad-sha256'] = '已新增：' + $pub2 + ' / ' + $title2 + '（2 条）' }
    if ($Options.ContainsKey('NameMismatchMetaOnly'))
    {
        # **N-S2 的假绿现场**：状态文字只点名了**档案那一侧**（索引那一侧没提）。
        # 旧判据读的证据里混着"远端列表行"（`<索引会社> / <索引作品> / …`），索引名永远在场 → 假绿。
        $badCaseStatus['online-index-name-mismatch'] = '下载 / 校验失败：' + "`r`n" + '索引与档案不是同一条，已丢弃：两边的会社 / 作品对不上。' + "`r`n" +
            '  档案自己的 META 说：' + $pub2 + ' / ' + $title2
    }
    if ($Options.ContainsKey('NameMismatchWrongReason'))
    {
        # 做错法：只说"被拒了"，**没有点名两边** —— 判据要是只判"被拒"，这条就会假绿
        $badCaseStatus['online-index-name-mismatch'] = '下载 / 校验失败：' + "`r`n" + '档案摘要不符：这一份不是索引里指的那一份，已丢弃。'
    }
    if ($Options.ContainsKey('IndexMalformedAsEmpty'))
    {
        $badCaseStatus['online-index-cols'] = '索引拉取成功：共 0 条（实收 0 B）。' + "`r`n本地状态：新增 0 / 可更新 0 / 已是同一份 0 / 本地条数更多 0"
        $badCaseStatus['online-index-empty-publisher'] = '索引拉取成功：共 0 条（实收 0 B）。'
        $badCaseStatus['online-index-bad-digest'] = '索引拉取成功：共 0 条（实收 0 B）。'
    }

    # E-19 的用例
    $e19Status = [ordered]@{
        'online-no-index'    = @{ Text = '索引拉取失败：' + "`r`n" + '拉取索引失败：服务器上没有这个文件（HTTP 404）。'; Ms = 800 }
        'online-slow'        = @{ Text = '索引拉取失败：' + "`r`n" + '拉取索引失败：等不到对方的回应（超时：连接 10000 ms / 发送 15000 ms / 接收 30000 ms）。'; Ms = 33500 }
        'online-unreachable' = @{ Text = '索引拉取失败：' + "`r`n" + '拉取索引失败：连不上服务器（对方拒绝连接，或本地防火墙 / 安全软件拦了，也可能是需要走代理）。'; Ms = 2900 }
        # N-P2 第二层：前缀是 https:// 但缺主机名 → 与"不是 https"是**两句**（判据要能区分）
        'online-https-no-host' = @{ Text = '索引拉取失败：' + "`r`n" + '基址不完整：只允许 https:// 开头的地址，但这个地址里没有主机名（缺少主机名）。'; Ms = 300 }
        # 基址是明文 http：按**三态措辞**（task-1 改后的形态）合成 —— 引用了用户原文，并点明只接受 https
        'online-http-base'   = @{ Text = '索引拉取失败：' + "`r`n" + '基址不可用："http://127.0.0.1:9/"（不是 https:// 开头的）。' + "`r`n" + '本工具只允许 https 地址：http 是明文，内容中途谁都能改。'; Ms = 300 }
    }
    if ($Options.ContainsKey('NoHostWrongLayer'))
    {
        # 做错法：把"缺主机名"那条喂成**另一层**（非 https）的措辞 → 判据必须炸（不许一句通吃）
        $e19Status['online-https-no-host'] = @{ Text = '索引拉取失败：' + "`r`n" + '基址不可用：只允许 https:// 开头的地址，你填的是 "https:///" —— 它不是 https 的地址。'; Ms = 300 }
    }
    if ($Options.ContainsKey('HttpBaseGenericWord'))
    {
        # **S-1 的假绿现场**：只有泛化兜底那一句、**没有引用原址**（这正是当前构建对 http/ftp 的实际输出）
        $e19Status['online-http-base'] = @{ Text = '索引拉取失败：' + "`r`n" + '基址不可用：""。' + "`r`n" + '本工具只在 https 上拉扩展集（http 是明文，内容中途谁都能改），地址也不能为空。'; Ms = 300 }
    }
    if ($Options.ContainsKey('UnreachableSlowButPasses')) { $e19Status['online-unreachable']['Ms'] = 45000 }
    if ($Options.ContainsKey('TimeoutTooFast')) { $e19Status['online-slow']['Ms'] = 900 }
    if ($Options.ContainsKey('E19WroteLocalLibrary'))
    {
        # 有人偷偷写了本地库：前后摘要不再相同
        $e19Status['online-no-index'] = @{ Text = $e19Status['online-no-index'].Text; Ms = 800; Wrote = $true }
    }

    $onlineCases = New-Object System.Collections.ArrayList
    $onlineRunJsons = [ordered]@{}
    $summaryPath = Join-Path $Dir 'extension-online-summary.json'

    function New-OnlineCase([string]$Name, [string]$StatusText, [int]$ElapsedMs, [string]$ServerRoot, [string]$RunJsonPath,
        [bool]$Wrote = $false, [bool]$Good = $false, [string]$Endpoint = 'https://127.0.0.1:1/', [int]$Rows = 1,
        [bool]$FirstInstall = $false)
    {
        if ($Good)
        {
            [void]$onlineCases.Add([ordered]@{
                    name = $Name; description = $Name; expected = ''; exitCode = 0; failure = ''
                    endpoint = $Endpoint; serverRoot = $ServerRoot; serverLog = ''; serverPort = 1
                    certThumbprint = ''; runJson = $RunJsonPath; arguments = @(); durationSec = 10
                })
            return
        }
        $afterDigest = $(if ($Wrote) { 'DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD' } else { '1111111111111111111111111111111111111111111111111111111111111111' })
        $afterManifest = $(if ($Wrote) { $manifestText + $remoteRow + "`r`n" } else { $manifestText })
        # 首次安装：库里本来什么都没有 → before 是"库不存在"，after 是"只有远端那一行"
        $beforeRoot = $libraryRoot
        $beforeDigestBlock = [ordered]@{ Count = 3; DirCount = 3; Digest = '1111111111111111111111111111111111111111111111111111111111111111' }
        $beforeManifest = $manifestText
        $beforeRows = 1
        $beforeExists = $true
        if ($FirstInstall)
        {
            $beforeRoot = ''
            $beforeDigestBlock = [ordered]@{ Count = 0; DirCount = 0; Digest = ''; Exists = $false }
            $beforeManifest = ''
            $beforeRows = 0
            $beforeExists = $false
            if ($Options.ContainsKey('FirstInstallLies'))
            {
                # 做错法：装完还是 0 行（或者那一行对不上远端索引）
                $afterManifest = $manifestText
                $Rows = 0
            }
            else
            {
                $afterManifest = ("# 会社`t作品`t目标exe摘要`t兄弟exe摘要`t盐摘要`t档案摘要`t条数`t生成时间`t安装时间`t来源`r`n" + $remoteRow + "`r`n")
                $Rows = 1
                if (-not $StatusText) { $StatusText = '已新增：' + $pub2 + ' / ' + $title2 + '（2 条）' }
            }
        }
        $round = [ordered]@{
            round = 1; endpointShown = $Endpoint
            statusAfterRefresh = $StatusText; listItemsAfterRefresh = @(); refreshElapsedMs = $ElapsedMs
            installClicked = $true; statusAfterInstall = $StatusText; listItemsAfterInstall = @()
            installElapsedMs = $ElapsedMs; modals = @(); manifestRowsAfter = $(if ($Wrote) { 2 } else { $Rows })
            manifestTextAfter = $afterManifest; libraryDigestAfter = [ordered]@{ Count = 3; DirCount = $afterDirCount; Digest = $afterDigest }; stagingFiles = @()
        }
        Write-JsonFile -Path $RunJsonPath -Object ([ordered]@{
                case = ('synthetic-' + $Name); game = 'old'; steps = @('settab', 'setupdate'); mode = 'set'
                startedUtc = (Get-Date).ToUniversalTime().ToString('o'); failure = $null; completed = $true
                events = @(); modalDialogs = @(); toolLogs = @(); stepResults = @{}; output = @(); outputTsv = ''
                online = [ordered]@{
                    endpoint = $Endpoint; endpointShown = $Endpoint; rounds = @($round)
                    before = [ordered]@{ libraryRoot = $beforeRoot; libraryDigest = $beforeDigestBlock
                        manifestText = $beforeManifest; manifestRows = $beforeRows; rows = @(); toolDigest = [ordered]@{ Count = 7; DirCount = 2; Digest = '3333333333333333333333333333333333333333333333333333333333333333' }; stagingFiles = @() }
                    after = [ordered]@{ libraryRoot = $libraryRoot; libraryDigest = [ordered]@{ Count = 3; DirCount = $(if ($Wrote) { 4 } else { 3 }); Digest = $afterDigest }
                        manifestText = $afterManifest; manifestRows = $(if ($Wrote) { 2 } else { $Rows }); rows = @(); toolDigest = [ordered]@{ Count = 7; DirCount = 2; Digest = '3333333333333333333333333333333333333333333333333333333333333333' }; stagingFiles = @() }
                    toolIniText = ''; pageTextAfterClose = ''
                }
            })
        [void]$onlineCases.Add([ordered]@{
                name = $Name; description = $Name; expected = ''; exitCode = 0; failure = ''
                endpoint = $Endpoint; serverRoot = $ServerRoot; serverLog = ''; serverPort = 1
                certThumbprint = ''; runJson = $RunJsonPath; arguments = @(); durationSec = 10
            })
        $onlineRunJsons[$Name] = $RunJsonPath
    }

    # 正例
    # index-name-mismatch（§14.4 第 9 条 / P-1）：索引行的名字与档案 META 的名字**故意不一致**，
    # 其余三列（档案摘要 / 目标 exe 摘要 / 条数）全部与档案真实值一致 —— 只错名字那一条。
    $nameMismatchRoot = Join-Path $repoRoot 'index-name-mismatch'
    $nameMismatchDir = Join-Path (Join-Path $nameMismatchRoot 'IndexSaysThis') 'AndThis'
    New-Item -ItemType Directory -Path $nameMismatchDir -Force | Out-Null
    Copy-Item -LiteralPath $remoteArchive -Destination (Join-Path $nameMismatchDir 'rules.int') -Force
    $nameMismatchRow = New-SyntheticManifestRow -Publisher 'IndexSaysThis' -Title 'AndThis' -ExeDigest $hexB `
        -SaltDigest (Get-TextSha256Hex -Text 'othersalt123') -ArchiveDigest (Get-Sha256Hex -Path $remoteArchive) -EntryCount '2'
    Write-Utf8File -Path (Join-Path $nameMismatchRoot 'index.txt') `
        -Text ("# 会社`t作品`t目标exe摘要`t兄弟exe摘要`t盐摘要`t档案摘要`t条数`t生成时间`t安装时间`t来源`r`n" + $nameMismatchRow + "`r`n") -NoNewline

    [void](New-OnlineCase -Name 'online-good' -StatusText '' -ElapsedMs 0 -ServerRoot $goodRoot -RunJsonPath $goodRunJson -Good $true)
    # 首次安装（S-2）：库不存在 → 在线更新把它建出来
    $firstInstallJson = Join-Path $Dir 'extension-online-first-install.json'
    [void](New-OnlineCase -Name 'online-first-install' -StatusText '' -ElapsedMs 900 -ServerRoot $goodRoot `
            -RunJsonPath $firstInstallJson -FirstInstall $true)
    # 坏例
    foreach ($name in $badCaseStatus.Keys)
    {
        $runJson = Join-Path $Dir ('{0}.json' -f $name)
        # 名字不一致那条要看它**自己的**仓库根（判据会独立解析那边的索引与档案）
        $rootForCase = if ($name -eq 'online-index-name-mismatch') { $nameMismatchRoot } else { $goodRoot }
        [void](New-OnlineCase -Name $name -StatusText ([string]$badCaseStatus[$name]) -ElapsedMs 800 -ServerRoot $rootForCase -RunJsonPath $runJson)
    }
    # E-19
    foreach ($name in $e19Status.Keys)
    {
        $spec = $e19Status[$name]
        $runJson = Join-Path $Dir ('{0}.json' -f $name)
        $wrote = [bool]$spec.ContainsKey('Wrote')
        # http 那条的"端点"就是那个明文地址本身（E-19 的判据要拿它比对"错误里有没有引用原址"）
        $endpointForCase = if ($name -eq 'online-http-base') { 'http://127.0.0.1:9/' }
            elseif ($name -eq 'online-https-no-host') { 'https:///' }
            else { 'https://127.0.0.1:1/' }
        [void](New-OnlineCase -Name $name -StatusText ([string]$spec.Text) -ElapsedMs ([int]$spec.Ms) -ServerRoot $goodRoot `
                -RunJsonPath $runJson -Wrote $wrote -Endpoint $endpointForCase)
    }

    Write-JsonFile -Path $summaryPath -Object ([ordered]@{
            generator = 'self-test-extension（合成世界）'; startedUtc = (Get-Date).ToUniversalTime().ToString('o')
            finishedUtc = (Get-Date).ToUniversalTime().ToString('o'); manifestPath = $ManifestPath; repoRoot = $repoRoot
            repoSummary = ''; hangSeconds = 40; onlinePublisher = $pub2; onlineTitle = $title2
            setup = @{ performed = $true; steps = @(); libraryArchive = $archivePath; thirdArchive = $remoteArchive; thirdWorkspace = ''; repoSummary = '' }
            cases = @($onlineCases); events = @()
        })

    return [pscustomobject]@{
        SetTabJson = $setTabJson; SetEmptyJson = $setEmptyJson; SetOneJson = $setOneJson; SetMultiJson = $setMultiJson
        OpenJson = $openJson; NoteJson = $notePath; CheckCleanJson = $checkCleanJson; CheckBadJson = $checkBadJson
        SetHostileJson = $setHostileJson
        OnlineSummaryJson = $summaryPath; LibraryRoot = $libraryRoot; ArchivePath = $archivePath
        ServerRoot = $goodRoot; ManifestText = $manifestText; AfterManifestText = $afterManifestText
    }
}

# ---------- 场景搭建 ----------

$scenarios = @(
    @{ Name = 'correct'; ExpectFail = @() }
    @{ Name = 'e1-mode1-missing'; ExpectFail = @('E-1', 'E-3', 'E-10'); Options = @{ DropMode1Restorable = $true } }
    @{ Name = 'e1-leak-missing'; ExpectFail = @('E-1'); Options = @{ LeakMissingIntoBoth = $true } }
    @{ Name = 'e1-salt-present'; ExpectFail = @('E-1'); Options = @{ SaltPresent = $true } }
    @{ Name = 'e1-input-mismatch'; ExpectFail = @('E-1'); Options = @{ InputMismatch = $true } }
    @{ Name = 'e2-no-exceptions'; ExpectFail = @('E-2'); Options = @{ RawExceptions = 0; RawOverview = '生成完成：目录 6 条 / 文件 15 条 / 例外 0 条' } }
    @{ Name = 'e2-good-has-exceptions'; ExpectFail = @('E-2', 'E-8'); Options = @{ GoodExceptions = 2 } }
    @{ Name = 'e3-dryrun-lies'; ExpectFail = @('E-3'); Options = @{ DryFull = 9; DryPercent = '60.00' } }
    @{ Name = 'e4-accepts-bad'; ExpectFail = @('E-4'); Options = @{ AcceptBad = 'crc-error' } }
    @{ Name = 'e4-accepts-empty-names'; ExpectFail = @('E-4'); Options = @{ AcceptBad = 'empty-names' } }
    # S-1 回归：把"盐不匹配"用例做成旧写法那种**同盐且没有名字**的档案（前置一查就露馅）
    @{ Name = 'e4-s1-false-pass'; ExpectFail = @('E-4'); Options = @{ SaltMismatchSameSalt = $true } }
    # S-2：拒了、但理由不是本用例声称的那个 —— 断言必须炸
    @{ Name = 'e4-wrong-reason-salt'; ExpectFail = @('E-4'); Options = @{ WrongReasonSalt = $true } }
    @{ Name = 'e4-wrong-reason-crc'; ExpectFail = @('E-4'); Options = @{ WrongReasonCrc = $true } }
    @{ Name = 'e5-fabricated-name'; ExpectFail = @('E-5'); Options = @{ FabricateIntoBoth = $true } }
    @{ Name = 'e6-source-touched'; ExpectFail = @('E-6'); Options = @{ TouchSource = $true } }
    @{ Name = 'e7-not-idempotent'; ExpectFail = @('E-7'); Options = @{ SecondArchiveDifferent = $true } }
    @{ Name = 'e7-volatile-only-auto'; ExpectFail = @(); Options = @{ VolatileOnly = $true } }
    @{ Name = 'e7-volatile-only-strict'; ExpectFail = @('E-7'); Options = @{ VolatileOnly = $true }; Args = @('-StrictBytes') }
    @{ Name = 'e7-dup-manifest'; ExpectFail = @('E-7'); Options = @{ DuplicateManifest = $true } }
    @{ Name = 'e8-volume'; ExpectFail = @('E-8'); Options = @{ ArchiveSize = 9MB; DumpDirCount = 4 } }
    @{ Name = 'e9-uppercase-dir'; ExpectFail = @('E-9'); Options = @{ UppercaseDir = $true } }
    @{ Name = 'e10-classic-drops-one'; ExpectFail = @('E-1', 'E-10'); Options = @{ DropClassicRestorable = $true } }
    # --- E-11（页签像素幽灵）：每条都对应一种"断言退化"的可能 ---
    # 注意 e11-ghost 的期望里**没有** E-11e（可见性）：就是要证明"可见性全绿、像素不干净"
    # —— 这是这条断言存在的理由（旧断言体系的盲区）。
    @{ Name = 'e11-ghost'; ExpectFail = @('E-11-a'); TabPixels = 'ghost' }
    @{ Name = 'e11-home-ghost'; ExpectFail = @('E-11-b'); TabPixels = 'home-ghost' }
    @{ Name = 'e11-noise-unstable'; ExpectFail = @('E-11-C'); TabPixels = 'unstable' }
    # 全黑快照 / 阈值被调大 / 快照缺失 —— 这三条**同时**打在 E-11 与 E-12 上（同一份快照、两页各判一次），
    # 期望里两条都要有：E-12 家族如果在这种情况下还能绿，说明它没在看证据。
    @{ Name = 'e11-blank-capture'; ExpectFail = @('E-11-F', 'E-12-E', 'E-12-F'); TabPixels = 'blank' }
    @{ Name = 'e11-frozen-tab'; ExpectFail = @('E-11-F'); TabPixels = 'frozen' }
    @{ Name = 'e11-cross-process'; ExpectFail = @('E-11-d'); TabPixels = 'crossprocess' }
    @{ Name = 'e11-threshold-tampered'; ExpectFail = @('E-11-T', 'E-12-T'); TabPixels = 'tampered' }
    @{ Name = 'e11-missing-snapshot'; ExpectFail = @('E-11', 'E-12-a'); TabPixels = '<missing>' }
    # 用**修复前旧构建**的真实抓图当输入（evidence\tab-pixels-prefix\）：断言必须炸
    # 修复前旧构建的真实抓图里**没有**扩展集页签那一路（那时页签还不是真界面），
    # 所以 E-12 家族会如实报"缺 set 抓图" —— 这正是我们要的：**证据缺失不许静默通过**。
    @{ Name = 'e11-prefix-real-capture'; ExpectFail = @('E-11-a', 'E-11-b', 'E-12-E', 'E-12-F', 'E-12-C', 'E-12-a'); TabPixels = '<prefix>' }

    # --- E-12..E-19（扩展集页签 + 在线更新）：每条都对应一种"断言可能退化"的方式 ---
    # 页签骨架（E-12）：可见性矩阵的三条退化路
    @{ Name = 'e12-skeleton-leak'; ExpectFail = @('E-12'); Options = @{ SkeletonLeakHomeControl = $true } }
    @{ Name = 'e12-skeleton-not-hidden-back'; ExpectFail = @('E-12'); Options = @{ SkeletonLeakBack = $true } }
    @{ Name = 'e12-skeleton-missing-control'; ExpectFail = @('E-12'); Options = @{ SkeletonMissingControl = $true } }
    # 扩展集页签的像素（E-12-a 家族，口径与 E-11 相同）
    @{ Name = 'e12-set-ghost'; ExpectFail = @('E-12-a'); TabPixels = 'set-ghost' }
    @{ Name = 'e12-set-unstable'; ExpectFail = @('E-12-C'); TabPixels = 'set-unstable' }
    @{ Name = 'e12-set-frozen'; ExpectFail = @('E-12-F'); TabPixels = 'set-frozen' }
    @{ Name = 'e12-set-threshold-tampered'; ExpectFail = @('E-12-T'); TabPixels = 'set-tampered' }
    # 列表（E-13）：计数撒谎 / **列表文字读不到**（MSAA 退化的假通过，最容易漏的一条）
    @{ Name = 'e13-empty-count-lies'; ExpectFail = @('E-13'); Options = @{ EmptyReportsOnePublisher = $true } }
    @{ Name = 'e13-lists-unreadable'; ExpectFail = @('E-13'); Options = @{ ListsUnreadable = $true } }
    # 详情面板（E-14）：面板撒谎
    @{ Name = 'e14-panel-lies'; ExpectFail = @('E-14'); Options = @{ PanelEntryCountLies = $true } }
    # 打开选中扩展集（E-15）：路径文本缺失（面板 / 日志各一条）
    @{ Name = 'e15-open-panel-drops-path'; ExpectFail = @('E-15'); Options = @{ OpenPanelDropsPath = $true } }
    @{ Name = 'e15-open-log-drops-path'; ExpectFail = @('E-15'); Options = @{ OpenLogDropsPath = $true } }
    # 运行环境自检（E-16）：坏档案不报 / 自检偷偷改了文件
    @{ Name = 'e16-selfcheck-silent'; ExpectFail = @('E-16'); Options = @{ SelfCheckSilent = $true } }
    @{ Name = 'e16-selfcheck-touches-files'; ExpectFail = @('E-16'); Options = @{ SelfCheckTouchesFiles = $true } }
    # E-16 的判据收紧之后的两条自证：措辞出现在**别人那一项里** / 该报的那一项**根本没报**
    # （旧写法是"全篇找关键词"，这两种都会假绿 —— 与 S-1 同类）
    @{ Name = 'e16-selfcheck-wrong-owner'; ExpectFail = @('E-16'); Options = @{ SelfCheckWrongOwner = $true } }
    @{ Name = 'e16-selfcheck-drops-item'; ExpectFail = @('E-16'); Options = @{ SelfCheckDropsItem = $true } }
    # 留言（E-17）：留言没显示 / 反而声称能验名字
    @{ Name = 'e17-note-drops-comment'; ExpectFail = @('E-17'); Options = @{ NoteDropsComment = $true } }
    @{ Name = 'e17-note-claims-verify'; ExpectFail = @('E-17'); Options = @{ NoteClaimsVerify = $true } }
    # 在线更新（E-18）：索引格式错被当成"远端为空" / 坏 sha256 被放行 / 幂等失效 / 入库记录对不上 / 暂存没清
    @{ Name = 'e18-index-malformed-as-empty'; ExpectFail = @('E-18'); Options = @{ IndexMalformedAsEmpty = $true } }
    @{ Name = 'e18-bad-sha-accepted'; ExpectFail = @('E-18'); Options = @{ BadShaAccepted = $true } }
    @{ Name = 'e18-not-idempotent'; ExpectFail = @('E-18'); Options = @{ GoodNotIdempotent = $true } }
    @{ Name = 'e18-install-lies'; ExpectFail = @('E-18'); Options = @{ GoodInstallLies = $true } }
    @{ Name = 'e18-staging-left'; ExpectFail = @('E-18'); Options = @{ GoodInstallLeavesStaging = $true } }
    # E-19：有人偷偷写了本地库 / 不可达却拖了 45 秒 / 超时却 0.9 秒就报错
    @{ Name = 'e19-wrote-local-library'; ExpectFail = @('E-19'); Options = @{ E19WroteLocalLibrary = $true } }
    @{ Name = 'e19-unreachable-slow'; ExpectFail = @('E-19'); Options = @{ UnreachableSlowButPasses = $true } }
    @{ Name = 'e19-timeout-too-fast'; ExpectFail = @('E-19'); Options = @{ TimeoutTooFast = $true } }
    # **S-1 的自证**：只给泛化兜底措辞（没有引用原址）→ 判据必须炸。
    # 这正是当前构建对 http/ftp 基址的实际输出 —— 也就是说：这条场景证明"旧判据会假绿"这件事
    # 现在被抓住了（独立验证者用 ftp:// 打穿过旧判据）。
    @{ Name = 'e19-http-base-generic-word'; ExpectFail = @('E-19'); Options = @{ HttpBaseGenericWord = $true } }
    # S-3：「关于」页签（4004）的三条做错法
    @{ Name = 'e12-about-missing-fingerprint'; ExpectFail = @('E-12-b'); Options = @{ AboutDropsFingerprint = $true } }
    @{ Name = 'e12-about-missing-section'; ExpectFail = @('E-12-b'); Options = @{ AboutDropsSection = $true } }
    @{ Name = 'e12-about-leaks-set-control'; ExpectFail = @('E-12-b'); Options = @{ AboutLeaksSetControl = $true } }
    # S-2：首次安装那条用例如果"装完库里还是 0 行"（或那一行对不上索引），必须炸
    @{ Name = 'e18-first-install-lies'; ExpectFail = @('E-18'); Options = @{ FirstInstallLies = $true } }
    # P-1：索引名 ≠ 档案 META 名那条用例，如果被拒的理由**没有点名两边**（= 换成别的防线的措辞），
    # 判据必须炸 —— 否则它区分不了"因为名字不一致被拒"和"因为别的原因被拒"（S-1 的同类风险）。
    @{ Name = 'e18-name-mismatch-wrong-reason'; ExpectFail = @('E-18'); Options = @{ NameMismatchWrongReason = $true } }
    # N-S2：状态文字**只点名档案那一侧** → 必须炸（旧判据读的证据里混着远端列表行，索引名永远在场 = 恒真）
    @{ Name = 'e18-name-mismatch-meta-only'; ExpectFail = @('E-18'); Options = @{ NameMismatchMetaOnly = $true } }
    # N-S1：文件一个没变、只多了一个**空目录** → E-16 的"自检不改任何文件或目录"必须炸
    @{ Name = 'e16-touches-empty-dir'; ExpectFail = @('E-16'); Options = @{ SelfCheckTouchesEmptyDir = $true } }
    # E-14-b：面板退回"拿原始名拼路径"（那个路径不存在）→ 必须炸（这就是协调者点名的"潜在假绿"）
    @{ Name = 'e14b-raw-join'; ExpectFail = @('E-14-b'); Options = @{ HostileRawJoin = $true } }
    # N-P2：把"缺主机名"那条喂成"非 https"那一层的措辞 → E-19 必须炸（两层措辞不许互相顶替）
    @{ Name = 'e19-https-no-host-wrong-layer'; ExpectFail = @('E-19'); Options = @{ NoHostWrongLayer = $true } }
)

# 合成世界要造真档案（E-18 的断言会独立解析它们），所以需要 Python
function Get-SelftestPython
{
    foreach ($candidate in @('C:\Users\Administrator\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'))
    {
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    $found = Get-Command python -ErrorAction SilentlyContinue
    if ($found) { return $found.Source }
    throw '找不到 Python 运行时（合成档案需要它）'
}
$script:selftestPython = Get-SelftestPython

$selftestResults = New-Object System.Collections.ArrayList

# ---------- E-11（像素幽灵）快照夹具 ----------
#
# 两条来源：
#   ① 合成图（New-SyntheticTabPixelSnapshot）：可造出"幽灵 / 首页脏 / 噪声不稳 / 全黑假图 /
#      页签没切过去 / 跨进程不一致 / 阈值被调大"七种做错法；
#   ② **修复前旧构建的真实抓图**（evidence\tab-pixels-prefix\，旧构建 6B664840，PNG 存证）：
#      用它当输入，E-11a/E-11b 必须炸 —— 这是"断言在真缺陷面前真的会响"的证据。
# 非 E-11 场景统一喂一份"干净"快照：既证明 E-11 不会误伤别的场景，也顺便覆盖了"干净必须全绿"。
$tabPixelsRoot = Join-Path $workRoot '_tabpixels'
$cleanTabSnapshot = New-SyntheticTabPixelSnapshot -Dir (Join-Path $tabPixelsRoot 'clean') -Scenario 'clean'
Write-Host ("[自检] E-11 干净基线快照：{0}" -f $cleanTabSnapshot)

$prefixEvidenceDir = Join-Path $root 'evidence\tab-pixels-prefix'
$prefixTabSnapshot = New-PrefixTabPixelSnapshot -EvidenceDir $prefixEvidenceDir -Dir (Join-Path $tabPixelsRoot 'prefix')
if (-not $prefixTabSnapshot)
{
    Write-Host ('[自检] 修复前证据图缺失（{0}）—— e11-prefix-real-capture 场景跳过并记 warn' -f $prefixEvidenceDir)
    Add-Check -Id 'SELFTEST-E/e11-prefix-real-capture' -Title '修复前真实抓图场景（证据缺失，跳过）' -Ok $false -Severity 'warn' `
        -Details @(('缺少 {0}\*.png：无法证明"断言在真缺陷面前会响"；合成幽灵场景仍然覆盖了判据本身' -f $prefixEvidenceDir))
    $scenarios = @($scenarios | Where-Object { $_.Name -ne 'e11-prefix-real-capture' })
}

foreach ($scenario in $scenarios)
{
    $options = if ($scenario.ContainsKey('Options')) { $scenario.Options } else { @{} }
    $dir = Join-Path $workRoot $scenario.Name
    if (Test-Path -LiteralPath $dir) { [void](Remove-TreeUnder -Path $dir -MustBeUnder $runRoot) }
    New-Item -ItemType Directory -Path $dir -Force | Out-Null

    # E-11 快照：本场景要用的那一份（没写 TabPixels 的场景用干净基线）
    $tabPixelsKey = if ($scenario.ContainsKey('TabPixels')) { [string]$scenario.TabPixels } else { '' }
    $tabSnapshot = $cleanTabSnapshot
    if ($tabPixelsKey -eq '<missing>') { $tabSnapshot = Join-Path $dir 'no-such-snapshot.json' }
    elseif ($tabPixelsKey -eq '<prefix>') { $tabSnapshot = $prefixTabSnapshot }
    elseif ($tabPixelsKey) { $tabSnapshot = New-SyntheticTabPixelSnapshot -Dir (Join-Path $dir 'tabpx') -Scenario $tabPixelsKey }
    $tabDiffDir = Join-Path $dir 'tabpx-diff'

    # 扩展集页签 / 在线更新的合成世界（每个场景一份：全对，或按 Options 故意做错一处）
    $setWorld = New-SyntheticSetWorld -Dir (Join-Path $dir 'setworld') -PythonPath $script:selftestPython -Options $options

    # --- 合成"源目录"（真实文件，供 E-3 的独立枚举用）---
    $sourceDir = Join-Path $dir 'source'
    foreach ($item in @($restorable) + @($missing) + @($plain))
    {
        $from = Join-Path $newUnpacked ([string]$item.sourceRel)
        $to = Join-Path $sourceDir ([string]$item.sourceRel)
        $parent = Split-Path -Parent $to
        if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
        if (Test-Path -LiteralPath $from) { Copy-Item -LiteralPath $from -Destination $to -Force }
        else { [System.IO.File]::WriteAllBytes($to, [byte[]](1, 2, 3, 4)) }
    }

    # --- 合成 StaticHash.lst（档案展开物）---
    $lstDir = Join-Path $dir 'static'
    New-Item -ItemType Directory -Path $lstDir -Force | Out-Null
    $lstPath = Join-Path $lstDir 'StaticHash.lst'
    $lstLines = New-Object System.Collections.ArrayList
    $dirIndex = 0
    foreach ($entry in $extDirs)
    {
        $name = [string]$entry.normalized
        if ($options.ContainsKey('UppercaseDir') -and $dirIndex -eq 0) { $name = 'Characters/Yuki/' }
        [void]$lstLines.Add(('{0}:{1}' -f [string]$entry.hash, $name))
        $dirIndex++
    }
    foreach ($entry in $extFiles) { [void]$lstLines.Add(('{0}:{1}' -f [string]$entry.hash, [string]$entry.name)) }
    Write-Utf8File -Path $lstPath -Text ((($lstLines -join "`r`n")) + "`r`n") -NoNewline

    # --- 合成输出清单 ---
    $mode1Output = New-Object System.Collections.ArrayList
    $classicOutput = New-Object System.Collections.ArrayList
    $skippedFirstRestorable = $false
    foreach ($item in $restorable)
    {
        if ($options.ContainsKey('DropMode1Restorable') -and -not $skippedFirstRestorable) { $skippedFirstRestorable = $true; continue }
        [void]$mode1Output.Add((New-OutItem ([string]$item.expectedRel) ([string]$item.sha256)))
    }
    foreach ($item in $restorable)
    {
        if ($options.ContainsKey('DropClassicRestorable')) { continue }
        [void]$classicOutput.Add((New-OutItem ([string]$item.expectedRel) ([string]$item.sha256)))
    }
    foreach ($item in $plain)
    {
        [void]$mode1Output.Add((New-OutItem ([string]$item.expectedRel) ([string]$item.sha256)))
        [void]$classicOutput.Add((New-OutItem ([string]$item.expectedRel) ([string]$item.sha256)))
    }
    if ($options.ContainsKey('LeakMissingIntoBoth'))
    {
        $leakRel = ('bgimage\{0}\{1}' -f (Split-Path ([string]$missing[0].sourceRel) -Parent | Split-Path -Leaf), (Split-Path ([string]$missing[0].sourceRel) -Leaf))
        [void]$mode1Output.Add((New-OutItem $leakRel 'LEAKSHA'))
        [void]$classicOutput.Add((New-OutItem $leakRel 'LEAKSHA'))
    }
    if ($options.ContainsKey('FabricateIntoBoth'))
    {
        [void]$mode1Output.Add((New-OutItem ('bgimage\characters\yuki\{0}.png' -f $negativeName) 'FAKESHA'))
        [void]$classicOutput.Add((New-OutItem ('bgimage\characters\yuki\{0}.png' -f $negativeName) 'FAKESHA'))
    }

    # --- 合成工作区（模式1 工作区必须没有盐、没有动态采集成果）---
    $mode1Ws = Join-Path $dir 'mode1'
    New-Item -ItemType Directory -Path (Join-Path $mode1Ws 'StringHashDumper_Output') -Force | Out-Null
    if ($options.ContainsKey('SaltPresent'))
    {
        New-Item -ItemType Directory -Path (Join-Path $mode1Ws 'Log') -Force | Out-Null
        Write-Utf8File -Path (Join-Path $mode1Ws 'Log\Universal.log') -Text '2026-10-13 | I | T1 | Hash Seed:xp3hnp' -NoNewline
    }
    $classicWs = Join-Path $dir 'classic'
    New-Item -ItemType Directory -Path $classicWs -Force | Out-Null

    # --- 干跑文本 ---
    # 干跑文本：按**产品当前面板**的四行口径（① 可解目录（去重）② 文件名可解（去重）③ 整路径可还原 + 成功率）
    # 以及"哈希名文件 / 扫描文件总数"自证行（S-5）。
    $dryFull = if ($options.ContainsKey('DryFull')) { [int]$options.DryFull } else { $restorable.Count }
    $dryPercent = if ($options.ContainsKey('DryPercent')) { [string]$options.DryPercent } else { ('{0:N2}' -f ($restorable.Count * 100.0 / ($restorable.Count + $missing.Count))) }
    $dryFiles = if ($options.ContainsKey('DryFiles')) { [int]$options.DryFiles } else { $restorable.Count }
    $dryText = @(
        ('来源目录：{0}' -f $sourceDir)
        '测试结果（仅供参考）'
        ('可解目录（去重）：{0}' -f $extDirs.Count)
        ('文件名可解（去重）：{0}' -f $dryFiles)
        ('整路径可还原：{0}' -f $dryFull)
        ('成功率：{0}%（整路径可还原 {1} ÷ 哈希名文件 {2}，与步骤3 同口径）' -f $dryPercent, $dryFull, ($restorable.Count + $missing.Count))
        ('哈希名文件：{0} / 扫描文件总数：{1}' -f ($restorable.Count + $missing.Count), ($restorable.Count + $missing.Count + $plain.Count))
        ('扩展集归属：来源：扩展集 YuaHarness/otomeki_ckr（exe 摘要命中）')
        ('测试来源目录：{0}' -f $sourceDir)
    ) -join "`r`n"

    # --- 档案解析结果（E-8 / E-9 用）---
    # 正常夹具的例外 = 清单里 expectedExceptions（夹具故意埋的 same.png 伪造哈希）。
    # GoodExceptions 选项用来"故意多报一条"（考 E-2 的对照组鉴别力）。
    $expectedExceptionLines = @($fixture.extensionSet.expectedExceptions | ForEach-Object { [string]$_ })
    $goodExceptionLines = @($expectedExceptionLines)
    if ($options.ContainsKey('GoodExceptions'))
    {
        for ($index = 0; $index -lt [int]$options.GoodExceptions; $index++) { $goodExceptionLines += ('{0:X16}:unexpected{1}.bin' -f (9000 + $index), $index) }
    }
    $dumpDirCount = if ($options.ContainsKey('DumpDirCount')) { [int]$options.DumpDirCount } else { $extDirs.Count }
    # VolatileOnly：两份档案都带 META（publisher 相同、generatedAt 不同）——
    # 真实档案永远有 META，只有"两边都有 META"才能考"只差易变字段"这一条。
    $firstMetaTag = if ($options.ContainsKey('VolatileOnly')) { '2026-10-13 20:00:00' } else { '' }
    $goodDump = New-ArchiveDump -DirCount $dumpDirCount -FileCount $extFiles.Count -ExceptionCount 0 `
        -DirNames @($extDirs | ForEach-Object { if ($options.ContainsKey('UppercaseDir') -and $_ -eq $extDirs[0]) { 'Characters/Yuki/' } else { [string]$_.normalized } }) `
        -FileNames @($extFiles | ForEach-Object { [string]$_.name }) -ExceptionLines $goodExceptionLines -MetaTag $firstMetaTag
    $rawExceptions = if ($options.ContainsKey('RawExceptions')) { [int]$options.RawExceptions } else { [int]$fixture.rawGame.expectExceptionsAtLeast }
    $rawOverview = if ($options.ContainsKey('RawOverview')) { [string]$options.RawOverview } else { ('生成完成：目录 {0} 条 / 文件 {1} 条 / 例外 {2} 条（归一化后重算不一致，已如实报数）' -f $extDirs.Count, $extFiles.Count, $rawExceptions) }
    $rawDump = New-ArchiveDump -DirCount $extDirs.Count -FileCount $extFiles.Count -ExceptionCount $rawExceptions

    # --- 档案摘要 / 大小（E-7 / E-8）---
    $archiveSize = if ($options.ContainsKey('ArchiveSize')) { [long]$options.ArchiveSize } else { 2048 }
    $archiveSha = 'ARCHIVESHA-1'
    $secondSha = if ($options.ContainsKey('SecondArchiveDifferent') -or $options.ContainsKey('VolatileOnly')) { 'ARCHIVESHA-2' } else { $archiveSha }
    if ($options.ContainsKey('SecondArchiveDifferent'))
    {
        # 内容真的变了（DIRS/FILES 的 chunk 摘要不同）-> E-7 必须炸（auto 口径下也是）
        $secondDump = New-ArchiveDump -DirCount $extDirs.Count -FileCount $extFiles.Count -ExceptionCount 0 -ChunkTag '2'
    }
    elseif ($options.ContainsKey('VolatileOnly'))
    {
        # 只有 META 的 generatedAt 不同 -> auto 口径算通过，-StrictBytes 必须炸
        $secondDump = New-ArchiveDump -DirCount $extDirs.Count -FileCount $extFiles.Count -ExceptionCount 0 -MetaTag '2026-10-13 21:00:01'
    }
    else { $secondDump = $goodDump }

    # --- 摘要（E-6）---
    $digestBefore = New-Digest 'EXTRACTORDIGEST' ($restorable.Count + $missing.Count + $plain.Count)
    $digestAfter = if ($options.ContainsKey('TouchSource')) { New-Digest 'EXTRACTORDIGEST-CHANGED' ($restorable.Count + $missing.Count + $plain.Count) } else { $digestBefore }
    $srcBefore = New-Digest 'SOURCEDIGEST' ($restorable.Count + $missing.Count + $plain.Count)
    # 输入等价证据（E-1 ①b）：模式1 与传统模式的解包目录内容摘要（不含 mtime）
    $contentMode1 = New-Digest 'CONTENTDIGEST-SHARED' ($restorable.Count + $missing.Count + $plain.Count)
    $contentClassic = if ($options.ContainsKey('InputMismatch')) { New-Digest 'CONTENTDIGEST-DIFFERENT' ($restorable.Count + $missing.Count + $plain.Count) } else { $contentMode1 }

    # --- 库清单（E-7）---
    $manifestLines = New-Object System.Collections.ArrayList
    [void]$manifestLines.Add('# 会社	作品	exe摘要	盐摘要	档案摘要	条数	生成时间	安装时间	来源')
    [void]$manifestLines.Add(('YuaHarness	otomeki_ckr	{0}	SALT	ARCHIVESHA-1	20	2026-10-13	2026-10-13	local' -f $exeSha))
    if ($options.ContainsKey('DuplicateManifest')) { [void]$manifestLines.Add(('YuaHarness	otomeki_ckr	{0}	SALT	ARCHIVESHA-1	20	2026-10-13	2026-10-13	local' -f $exeSha)) }
    $manifestText = ($manifestLines -join "`r`n")
    $manifestFile = Join-Path $dir 'manifest.txt'
    Write-Utf8File -Path $manifestFile -Text ($manifestText + "`r`n") -NoNewline

    # --- 写各份运行结果 ---
    $draftSteps = [ordered]@{ dryrun = [ordered]@{ name = 'dryrun'; ok = $true; setStatusText = $dryText; overviewText = $dryText; taskText = '（空闲）' } }
    $draftPath = Join-Path $dir 'draft.json'
    Write-JsonFile -Path $draftPath -Object (New-RunRecord -Name 'draft' -Workspace (Join-Path $dir 'oldgame') -Output @() `
            -Overview '生成完成：映射来源 扩展集（本地采集）' -StaticLst $lstPath -StaticLines $lstLines.Count `
            -ExtractorBefore $digestBefore -ExtractorAfter $digestAfter -SourceBefore $srcBefore -SourceAfter $srcBefore `
            -ArchiveDump $goodDump -ArchiveSize $archiveSize -ArchiveSha $archiveSha -StepResults $draftSteps -ManifestText $manifestText -ManifestPath2 $manifestFile)
    $repeatPath = Join-Path $dir 'draft-repeat.json'
    Write-JsonFile -Path $repeatPath -Object (New-RunRecord -Name 'draft-repeat' -Workspace (Join-Path $dir 'oldgame') -Output @() `
            -Overview '生成完成' -StaticLst $lstPath -StaticLines $lstLines.Count `
            -ExtractorBefore $digestBefore -ExtractorAfter $digestBefore -SourceBefore $srcBefore -SourceAfter $srcBefore `
            -ArchiveDump $secondDump -ArchiveSize $archiveSize -ArchiveSha $secondSha -ManifestText $manifestText -ManifestPath2 $manifestFile)
    $installRepeatPath = Join-Path $dir 'install-repeat.json'
    Write-JsonFile -Path $installRepeatPath -Object (New-RunRecord -Name 'install-repeat' -Workspace (Join-Path $dir 'oldgame') -Output @() `
            -ManifestText $manifestText -ManifestPath2 $manifestFile -ExtractorBefore $digestBefore -ExtractorAfter $digestBefore)
    $rawPath = Join-Path $dir 'raw.json'
    Write-JsonFile -Path $rawPath -Object (New-RunRecord -Name 'raw' -Workspace (Join-Path $dir 'rawgame') -Output @() `
            -Overview $rawOverview -ArchiveDump $rawDump -ArchiveSize $archiveSize -ArchiveSha 'RAWSHA' `
            -ExtractorBefore $digestBefore -ExtractorAfter $digestBefore)
    $mode1Path = Join-Path $dir 'mode1.json'
    Write-JsonFile -Path $mode1Path -Object (New-RunRecord -Name 'mode1' -Workspace $mode1Ws -Output $mode1Output.ToArray() `
            -Overview '映射来源：扩展集 YuaHarness/otomeki_ckr（StaticHash_Output）' -StaticLst $lstPath -StaticLines $lstLines.Count `
            -ExtractorBefore $digestBefore -ExtractorAfter $digestBefore -SourceBefore $srcBefore -SourceAfter $srcBefore `
            -ExtractorContentDigest $contentMode1 -ExtractorOutputPath (Join-Path $mode1Ws 'Extractor_Output') `
            -StepResults ([ordered]@{ step1 = [ordered]@{ name = 'step1'; ok = $true }; step3 = [ordered]@{ name = 'step3'; ok = $true } }))
    $classicPath = Join-Path $dir 'classic.json'
    Write-JsonFile -Path $classicPath -Object (New-RunRecord -Name 'classic' -Workspace $classicWs -Output $classicOutput.ToArray() `
            -Overview '映射来源：动态采集（StringHashDumper_Output）' `
            -ExtractorBefore $digestBefore -ExtractorAfter $digestBefore -SourceBefore $srcBefore -SourceAfter $srcBefore `
            -ExtractorContentDigest $contentClassic -ExtractorOutputPath (Join-Path $classicWs 'Extractor_Output'))

    # --- 坏档案用例（E-4）---
    $badMessages = [ordered]@{
        'salt-mismatch'        = '拒绝加载：无法校验扩展集归属，档案属于 YuaHarness/othergame，摘要与档案记录不符'
        'salt-mismatch-known'  = '拒绝加载：该扩展集属于 YuaHarness/othergame，与当前游戏盐不同（档案 othersalt123，当前 xp3hnp）'
        'crc-error'            = '拒绝加载：这份扩展集档案结构缺失或已损坏（读不出来 / 版本不符 / 截断 / CRC 错 / 缺 META 或 IDENTITY）：CRC32 校验失败：档案内容与头部记录不一致（已损坏或被改动）。'
        'truncated'            = '拒绝加载：这份扩展集档案结构缺失或已损坏（读不出来 / 版本不符 / 截断 / CRC 错 / 缺 META 或 IDENTITY）：档案结构损坏：payload 解压失败（数据损坏(Z_DATA_ERROR)，status=-3）。'
        'version-high'         = '拒绝加载：这份扩展集档案结构缺失或已损坏（读不出来 / 版本不符 / 截断 / CRC 错 / 缺 META 或 IDENTITY）：档案格式版本过高：文件是 65535，本工具只认 1。'
        'missing-identity'     = '拒绝加载：这份扩展集档案结构缺失或已损坏（读不出来 / 版本不符 / 截断 / CRC 错 / 缺 META 或 IDENTITY）：档案结构缺失（IDENTITY）：档案缺少 IDENTITY chunk（0x0002），没有盐与目标 exe 摘要。'
        'empty-names'          = '拒绝生成静态 Hash 表：档案里没有任何名字（DIRS/FILES 都是空的），按设计明确报错而不是写一个空表出去。'
    }
    # 每个坏档案用例的"独立解析结果"（E-4 的前置条件靠它复核）：
    # 这正是 S-1 的抓手 —— 旧写法下 salt-mismatch 的档案是同盐且没有名字，前置一查就露馅。
    $sameSaltDump = New-ArchiveDump -DirCount 0 -FileCount 0 -ExceptionCount 2 -Salt ([string]$fixture.salt)
    $realSaltDump = New-ArchiveDump -DirCount 5 -FileCount 15 -ExceptionCount 0 -Salt ([string]$fixture.otherGame.salt)
    $badDumps = [ordered]@{
        'salt-mismatch'        = if ($options.ContainsKey('SaltMismatchSameSalt')) { $sameSaltDump } else { $realSaltDump }
        'salt-mismatch-known'  = if ($options.ContainsKey('SaltMismatchSameSalt')) { $sameSaltDump } else { $realSaltDump }
        'crc-error'            = New-ArchiveDump -DirCount 5 -FileCount 15 -CrcOk $false
        'truncated'            = New-ArchiveDump -DirCount 5 -FileCount 15 -Problems @('chunk 表越界：需要 208 字节，文件只有 150 字节')
        'version-high'         = New-ArchiveDump -DirCount 5 -FileCount 15 -Version 0xFFFF
        'missing-identity'     = New-ArchiveDump -DirCount 5 -FileCount 15 -IncludeIdentity $false
        'empty-names'          = New-ArchiveDump -DirCount 0 -FileCount 0 -ExceptionCount 0
    }
    # "拒了但拒错理由"场景（S-2）：把某个用例的拒绝文本换成**另一个防线**的话
    if ($options.ContainsKey('WrongReasonSalt')) { $badMessages['salt-mismatch'] = [string]$badMessages['empty-names'] }
    if ($options.ContainsKey('WrongReasonCrc')) { $badMessages['crc-error'] = [string]$badMessages['version-high'] }

    $badPaths = [ordered]@{}
    foreach ($name in $badMessages.Keys)
    {
        $accept = ($options.ContainsKey('AcceptBad') -and [string]$options.AcceptBad -eq $name)
        $path = Join-Path $dir ('bad-{0}.json' -f $name)
        $dialogs = @()
        if (-not $accept)
        {
            $dialogs = @([ordered]@{ Title = 'YuaExtract'; Text = [string]$badMessages[$name]; Buttons = '确定' })
        }
        Write-JsonFile -Path $path -Object (New-RunRecord -Name ('bad-' + $name) -Workspace $mode1Ws -Output @() `
                -Overview $(if ($accept) { '静态 Hash 表生成完成：20 条' } else { '生成静态 Hash 表失败（详见对话框）' }) `
                -Failure $(if ($accept) { '' } else { [string]$badMessages[$name] }) `
                -Dialogs $dialogs `
                -ArchiveDump $badDumps[$name] `
                -StaticLst $(if ($accept) { $lstPath } else { '' }) -StaticLines $(if ($accept) { $lstLines.Count } else { 0 }))
        $badPaths[$name] = $path
    }
    $badCsv = (($badPaths.Keys | ForEach-Object { '{0}={1}' -f $_, $badPaths[$_] }) -join ';')
    # 用例清单（E-4 从它拿 archive 路径；合成场景下档案不在磁盘上，断言会自动退回 run JSON 的 archiveDump）
    $summaryPath = Join-Path $dir 'bad-summary.json'
    $summaryCases = @()
    foreach ($name in $badMessages.Keys)
    {
        $summaryCases += [ordered]@{
            name = $name; desc = $name; archive = (Join-Path $dir ('bad-{0}.rules.int' -f $name))
            expect = ''; ok = $true; runJson = $badPaths[$name]
        }
    }
    Write-JsonFile -Path $summaryPath -Object ([ordered]@{ libraryArchive = ''; goodBackup = ''; cases = $summaryCases; badArchiveRunJsonCsv = $badCsv })

    # --- 跑断言 ---
    # 参数一次性拼成数组再整体 splat：分两次 splat（固定参数 + 可变参数）在 native 命令上
    # 会把 `-StrictBytes` 拆成字符（实测报 "参数 '-' 不属于 ValidateSet"）。
    $checksFile = Join-Path $dir 'checks.json'
    $assertArgs = @(
        '-NoProfile', '-File', (Join-Path $root 'assert-extension.ps1'),
        '-ManifestPath', $ManifestPath,
        '-DraftRunJson', $draftPath, '-RawRunJson', $rawPath,
        '-RepeatDraftRunJson', $repeatPath, '-RepeatInstallRunJson', $installRepeatPath,
        '-Mode1RunJson', $mode1Path, '-ClassicRunJson', $classicPath,
        '-BadArchiveRunJsonCsv', $badCsv, '-BadArchiveSummaryJson', $summaryPath,
        # E-11 快照：本场景该用的那一份（干净 / 合成做错 / 不存在）
        '-TabPixelsJson', $tabSnapshot,
        '-TabPixelsDiffDir', $tabDiffDir,
        # E-12..E-19：合成世界（同一份快照也给 E-12 的像素判据用）
        '-OnlineSummaryJson', $setWorld.OnlineSummaryJson,
        '-SetTabRunJson', $setWorld.SetTabJson,
        '-SetEmptyRunJson', $setWorld.SetEmptyJson,
        '-SetOneRunJson', $setWorld.SetOneJson,
        '-SetMultiRunJson', $setWorld.SetMultiJson,
        '-SetOpenRunJsonCsv', ('none={0};ok={1};missing={2}' -f $setWorld.OpenJson['none'], $setWorld.OpenJson['ok'], $setWorld.OpenJson['missing']),
        '-SetNoteRunJson', $setWorld.NoteJson,
        '-SetHostileRunJson', $setWorld.SetHostileJson,
        '-SetCheckRunJsonCsv', ('clean={0};bad={1}' -f $setWorld.CheckCleanJson, $setWorld.CheckBadJson),
        '-SetPixelsJson', $tabSnapshot,
        '-SetPixelsDiffDir', $tabDiffDir,
        '-ChecksJson', $checksFile
    )
    foreach ($extra in $(if ($scenario.ContainsKey('Args')) { @($scenario.Args) } else { @() })) { $assertArgs += [string]$extra }
    & pwsh @assertArgs | Out-Null

    $result = Read-JsonFile -Path $checksFile
    $actualFail = @()
    if ($result)
    {
        foreach ($check in $result.checks) { if (-not $check.Ok -and $check.Severity -ne 'info') { $actualFail += $check.Id } }
    }
    $expected = @($scenario.ExpectFail)
    $missingFail = @($expected | Where-Object { $_ -notin $actualFail })
    $unexpected = @($actualFail | Where-Object { $_ -notin $expected })
    $ok = ($missingFail.Count -eq 0 -and $unexpected.Count -eq 0)

    $details = @()
    if ($result) { $details += ("{0} 项断言；失败 {1} 项：{2}" -f $result.total, $actualFail.Count, $(if ($actualFail.Count -gt 0) { $actualFail -join ', ' } else { '（无）' })) }
    else { $details += '断言脚本没有写出 checks json' }
    if ($missingFail.Count -gt 0) { $details += ('本该失败却没失败：' + ($missingFail -join ', ')) }
    if ($unexpected.Count -gt 0) { $details += ('不该失败却失败了：' + ($unexpected -join ', ')) }

    [void]$selftestResults.Add([pscustomobject]@{ Scenario = $scenario.Name; Ok = $ok; Details = $details })
    Add-Check -Id ('SELFTEST-E/' + $scenario.Name) -Title '断言响应符合预期' -Ok $ok -Details $details

    if (-not $KeepWork -and $ok) { [void](Remove-TreeUnder -Path $dir -MustBeUnder $runRoot) }
}

$selftestFail = @($script:harnessChecks | Where-Object { -not $_.Ok -and $_.Severity -eq 'fail' }).Count
$selftestWarn = @($script:harnessChecks | Where-Object { -not $_.Ok -and $_.Severity -ne 'fail' }).Count
Write-Host ''
Write-Host ("结论：{0}（{1} 项；另有 {2} 条提示）" -f `
    $(if ($selftestFail -eq 0) { '全部通过' } else { '不通过 —— ' + $selftestFail + ' 项失败' }),
    $script:harnessChecks.Count, $selftestWarn)

if ($ChecksJson) { Write-JsonFile -Path $ChecksJson -Object ([ordered]@{ title = '扩展集断言自检（鉴别力）'; failed = $selftestFail; warned = $selftestWarn; total = $script:harnessChecks.Count; checks = $script:harnessChecks }) }
exit $(if ($selftestFail -eq 0) { 0 } else { 1 })
