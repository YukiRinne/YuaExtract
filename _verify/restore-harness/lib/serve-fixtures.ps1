# 本地「远端扩展集仓库」夹具服务（在线更新用例专用，**绝不访问外网**）
#
#   pwsh -File lib\serve-fixtures.ps1 -Root Run\ExtensionRepo\good -PortFile Run\out\online-port.json
#
# 为什么必须自己写一个服务器，而不是随便找个静态服务器：
#   1) **产品只接受 https**（OnlineUpdate.cpp 的 StartsWithHttps / ParseHttpsUrl：`http://` 直接
#      报「只允许 https 地址」）。所以夹具不能是明文 http —— 否则 E-18/E-19 一个用例都跑不到
#      "下载"那一步，全都会在"基址不可用"上炸，看起来像产品缺陷。
#      本脚本用自签名证书 + 把该证书装进 LocalMachine\Root（WinHTTP 只用机器证书库，
#      **不认用户库**），这样产品的 WinHttp 才会信。
#   2) 用例要**故意造坏**：404 / 截断 / 拖住不返回 / 乱字节 / 非 200 —— 静态服务器给不了这些。
#      这些坏法必须在"线上到底发了哪些字节"这一层做，才知道产品是被哪一条防线拦下的。
#   3) 单线程 + 一次性处理一个连接（我们的用例是串行的），换来"每一条响应都在自己手里"。
#
# 「造坏」写在**夹具目录里的 _faults.json**，服务器只负责照做（坏法是夹具的属性，不是服务器的）：
#   {
#     "index.txt":                     { "mode": "404" },
#     "YuaHarness/other/rules.int":    { "mode": "truncate", "ratio": 0.5 },
#     "*":                             { "mode": "hang", "seconds": 40 }
#   }
#   mode: ok（默认）| 404 | status | hang | truncate | short-body | garbage | slow
#     · 404        —— 服务器上没有这个文件（走 DescribeHttpStatus 的 404 措辞）
#     · status     —— 返回指定状态码（默认 500）
#     · hang       —— 收下请求后**什么都不发**，拖住 seconds 秒再断开（打产品的接收超时 30s）
#     · truncate   —— 只发前 ratio 的字节，且 Content-Length **就按截断后的长度报**
#                     （对产品来说这是一份"完整的短文件"：只能靠 sha256 / 结构自洽抓出来）
#     · short-body —— Content-Length 按完整长度报，只发 ratio 就断开（打"读到一半连接断了"）
#     · garbage    —— 发一堆随机字节（声称是 200 + 正确长度）
#     · slow       —— 分段慢慢发（每 chunkSeconds 发一点），用来区分"慢"与"死"
#
# 端口：-Port 0（默认）= 让系统挑一个空闲端口，实际端口写进 -PortFile（JSON）并打印一行
#       `PORT=<n>`；**不要写死端口**（会和别的进程/别的用例撞）。
#
# 收尾：用例结束后调用方负责停掉本进程；本脚本还带两道保险：
#   · -IdleTimeoutSec  —— 连续 N 秒没有请求就自己退出（防止调用方崩了留下僵尸进程）
#   · -ParentPid <pid> —— 父进程没了就自己退出
#
# 只在 pwsh 7 下运行（用到了 .NET 的 TcpListener/SslStream；Windows PowerShell 5.1 未验证）。

[CmdletBinding()]
param(
    # 要服务的夹具目录（仓库根）。里面有 index.txt / <会社>/<作品>/rules.int / 可选 _faults.json
    [Parameter(Mandatory)][string]$Root,
    # 监听端口；0 = 让系统挑空闲端口（推荐）
    [int]$Port = 0,
    # 把实际端口/基址写成 JSON 到这里（调用方读它拿端口，别去解析 stdout）
    [string]$PortFile = '',
    # 每个请求一行（时间 / 方法 / 路径 / 模式 / 状态 / 字节数 / 耗时）——断言用它证明
    # "产品到底请求了哪些地址、请求了几次"，也是"没有访问外网"的证据
    [string]$LogFile = '',
    [string]$BindAddress = '127.0.0.1',
    # 自签名证书放哪（默认 <脚手架>\Run\certs）。证书是**跨轮复用**的：每次跑都新建一张
    # 会让机器证书库长出一堆垃圾。
    [string]$CertDir = '',
    [string]$CertSubject = 'CN=YuaExtract Harness Fixture',
    # 明文 http（产品**拒收** http，这里只为"用 curl 排查夹具本身"留一条路）
    [switch]$Http,
    # 只生成证书、不装进 LocalMachine\Root（装了才有意义：不装的话产品 TLS 握手必失败）
    [switch]$NoCertificateInstall,
    # 卸掉夹具证书（本机信任库里留一张自签 CA 是**有安全含义**的，收尾时可以清掉）
    [switch]$UninstallCertificate,
    # 空闲多少秒没请求就自己退出（0 = 不自动退出）
    [int]$IdleTimeoutSec = 0,
    # 父进程 pid：它没了就自己退出（调用方异常退出时的保险，避免留下僵尸进程）
    [int]$ParentPid = 0,
    # 每个请求都在 stdout 打印一行（调用方会重定向走，正常不用关）
    [switch]$Quiet
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:PipelineRoot = $PSScriptRoot
$harnessRoot = Split-Path -Parent $PSScriptRoot
if (-not $CertDir) { $CertDir = Join-Path $harnessRoot 'Run\certs' }
if (-not $LogFile) { $LogFile = Join-Path $harnessRoot 'Run\out\serve-fixtures.log' }

function Write-FixtureLog([string]$Path, [string]$Text)
{
    try
    {
        $directory = Split-Path -Parent $Path
        if ($directory -and -not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
        [System.IO.File]::AppendAllText($Path, $Text + "`r`n", (New-Object System.Text.UTF8Encoding($false)))
    }
    catch { }
}

function Write-PortFile([string]$Path, $Info)
{
    if (-not $Path) { return }
    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    [System.IO.File]::WriteAllText($Path, ($Info | ConvertTo-Json -Depth 5), (New-Object System.Text.UTF8Encoding($false)))
}

# ==================================================================
# 证书：自签名 + 装进机器信任库
# ==================================================================
#
# 为什么非要动机器证书库：WinHTTP 做服务器证书校验时用的是**机器**库（CERT_SYSTEM_STORE_LOCAL_MACHINE），
# 不读用户的 `Cert:\CurrentUser\Root`。只导进 CurrentUser 的话，产品会报
# 「TLS 握手没通过（证书链不受信任…）」，而我们会误以为是产品的问题。
# 这一步需要管理员权限（本机验证环境本来就是 Administrator）。

function Get-FixtureCertificate
{
    param([string]$Subject, [string]$Directory)

    # 先找现成的：Subject 相同、带私钥、还没过期
    foreach ($storeName in @('My', 'Root'))
    {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
        try
        {
            $store.Open('ReadOnly')
            foreach ($certificate in $store.Certificates)
            {
                if ($certificate.Subject -ne $Subject) { continue }
                if ($certificate.NotAfter -le (Get-Date).AddDays(7)) { continue }
                if (-not $certificate.HasPrivateKey) { continue }
                return $certificate
            }
        }
        catch { }
        finally { $store.Close() }
    }

    if (-not (Test-Path -LiteralPath $Directory)) { New-Item -ItemType Directory -Path $Directory -Force | Out-Null }
    # SAN 必须同时有 DNS=localhost 与 IPAddress=127.0.0.1：基址用哪个写法都要能过校验。
    $san = '2.5.29.17={text}DNS=localhost&IPAddress=127.0.0.1'
    $eku = '2.5.29.37={text}1.3.6.1.5.5.7.3.1'
    $created = New-SelfSignedCertificate -Subject $Subject -CertStoreLocation 'Cert:\LocalMachine\My' `
        -NotAfter (Get-Date).AddYears(5) -KeyUsage DigitalSignature, KeyEncipherment `
        -TextExtension @($san, $eku) -ErrorAction Stop
    return $created
}

function Install-FixtureCertificateTrust
{
    param([string]$Thumbprint, [string]$Subject, [string]$Directory)

    $source = Get-ChildItem -Path 'Cert:\LocalMachine\My' | Where-Object { $_.Thumbprint -eq $Thumbprint } | Select-Object -First 1
    if (-not $source) { throw "找不到刚创建的夹具证书（指纹 $Thumbprint）" }

    # 存一份 .cer 到脚手架目录：换机器/重装后能用它手工装信任（也方便审计这张证书是哪来的）
    $cerPath = Join-Path $Directory 'yuaextract-fixture.cer'
    [System.IO.File]::WriteAllBytes($cerPath, $source.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))

    $root = New-Object System.Security.Cryptography.X509Certificates.X509Store('Root', 'LocalMachine')
    try
    {
        $root.Open('ReadWrite')
        $already = $false
        foreach ($certificate in $root.Certificates) { if ($certificate.Thumbprint -eq $Thumbprint) { $already = $true; break } }
        if (-not $already)
        {
            $toAdd = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($cerPath)
            $root.Add($toAdd)
        }
        return [pscustomobject]@{ Installed = (-not $already); AlreadyThere = $already; CerPath = $cerPath }
    }
    finally { $root.Close() }
}

function Uninstall-FixtureCertificate
{
    param([string]$Subject)

    $removed = New-Object System.Collections.ArrayList
    foreach ($storeName in @('Root', 'My'))
    {
        $store = New-Object System.Security.Cryptography.X509Certificates.X509Store($storeName, 'LocalMachine')
        try
        {
            $store.Open('ReadWrite')
            $doomed = @($store.Certificates | Where-Object { $_.Subject -eq $Subject })
            foreach ($certificate in $doomed)
            {
                try
                {
                    $store.Remove($certificate)
                    [void]$removed.Add(('{0}\{1}' -f $storeName, $certificate.Thumbprint))
                }
                catch { }
            }
        }
        catch { }
        finally { $store.Close() }
    }
    return @($removed)
}

if ($UninstallCertificate)
{
    $removed = Uninstall-FixtureCertificate -Subject $CertSubject
    Write-Host ("[serve-fixtures] 已从本机信任库卸掉 {0} 项（{1}）" -f $removed.Count, ($removed -join ', '))
    exit 0
}

# ==================================================================
# 坏法配置（_faults.json）
# ==================================================================

function Get-FaultTable
{
    param([string]$Directory)

    $table = @{}
    $path = Join-Path $Directory '_faults.json'
    if (-not (Test-Path -LiteralPath $path)) { return $table }
    $parsed = $null
    try { $parsed = (Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json) }
    catch { throw ("_faults.json 不是合法 JSON：{0} —— {1}" -f $path, $_.Exception.Message) }
    foreach ($property in $parsed.PSObject.Properties)
    {
        $key = ([string]$property.Name).TrimStart('/')
        $value = $property.Value
        $entry = @{ Mode = 'ok'; Ratio = 0.5; Seconds = 40.0; Status = 500; Bytes = 4096; ChunkSeconds = 5.0 }
        $mode = [string]$value.mode
        if ($mode) { $entry.Mode = $mode }
        foreach ($name in @('ratio', 'seconds', 'status', 'bytes', 'chunkSeconds'))
        {
            $prop = $value.PSObject.Properties[$name]
            if ($prop -and $null -ne $prop.Value) { $entry[$name.Substring(0, 1).ToUpper() + $name.Substring(1)] = $prop.Value }
        }
        $table[$key] = $entry
    }
    return $table
}

function Get-FaultFor($Table, [string]$RelativePath)
{
    $key = $RelativePath.TrimStart('/')
    if ($Table.ContainsKey($key)) { return $Table[$key] }
    if ($Table.ContainsKey('*')) { return $Table['*'] }
    return @{ Mode = 'ok'; Ratio = 0.5; Seconds = 40.0; Status = 500; Bytes = 4096; ChunkSeconds = 5.0 }
}

# ==================================================================
# HTTP 应答
# ==================================================================

function Write-ResponseHead
{
    param($Stream, [int]$Status, [string]$Reason, [long]$Length, [string]$ContentType = 'application/octet-stream')

    $head = "HTTP/1.1 $Status $Reason`r`n" +
            "Server: yua-harness-fixture`r`n" +
            "Content-Type: $ContentType`r`n" +
            "Content-Length: $Length`r`n" +
            "Connection: close`r`n" +
            "`r`n"
    $bytes = [System.Text.Encoding]::ASCII.GetBytes($head)
    $Stream.Write($bytes, 0, $bytes.Length)
    $Stream.Flush()
}

function Send-AllBytes($Stream, [byte[]]$Bytes)
{
    if ($Bytes.Length -gt 0) { $Stream.Write($Bytes, 0, $Bytes.Length); $Stream.Flush() }
}

# 读请求行 + 头（读到 CRLFCRLF 为止；最多 8 KB）。
function Read-RequestHead
{
    param($Stream, [int]$TimeoutMs = 3000)

    $buffer = New-Object System.Collections.Generic.List[byte]
    $one = New-Object byte[] 1
    $deadline = (Get-Date).AddMilliseconds($TimeoutMs)
    while ((Get-Date) -lt $deadline -and $buffer.Count -lt 8192)
    {
        $read = $Stream.Read($one, 0, 1)
        if ($read -le 0) { break }
        $buffer.Add($one[0])
        $count = $buffer.Count
        if ($count -ge 4 -and $buffer[$count - 4] -eq 13 -and $buffer[$count - 3] -eq 10 -and
            $buffer[$count - 2] -eq 13 -and $buffer[$count - 1] -eq 10)
        {
            break
        }
    }
    if ($buffer.Count -eq 0) { return $null }
    return [System.Text.Encoding]::ASCII.GetString($buffer.ToArray())
}

function Get-RelativePathFromRequestLine([string]$Head)
{
    if (-not $Head) { return $null }
    $line = ($Head -split "`r`n")[0]
    $match = [regex]::Match($line, '^(?<method>[A-Z]+)\s+(?<target>\S+)\s+HTTP/')
    if (-not $match.Success) { return $null }
    $target = $match.Groups['target'].Value
    # 只处理绝对路径；带 ?query 的一律丢掉查询串（产品自己也会丢，见 ParseHttpsUrl）
    $cut = $target.IndexOfAny([char[]]@('?', '#'))
    if ($cut -ge 0) { $target = $target.Substring(0, $cut) }
    $decoded = [System.Uri]::UnescapeDataString($target)
    $decoded = $decoded.TrimStart('/')
    return [pscustomobject]@{ Method = $match.Groups['method'].Value; Path = $decoded }
}

# 夹具目录内的安全拼接：把 `..` 之类挡在目录外（夹具虽是自己的，但不该有路径穿越的口子）
function Resolve-FixtureFile
{
    param([string]$Directory, [string]$Relative)

    $rootFull = [System.IO.Path]::GetFullPath($Directory).TrimEnd('\')
    $candidate = [System.IO.Path]::GetFullPath((Join-Path $rootFull ($Relative -replace '/', '\')))
    if (-not $candidate.StartsWith($rootFull + '\', [System.StringComparison]::OrdinalIgnoreCase))
    {
        return $null
    }
    return $candidate
}

# ==================================================================
# 主流程
# ==================================================================

if (-not (Test-Path -LiteralPath $Root)) { throw ("夹具目录不存在：{0}" -f $Root) }
$Root = [System.IO.Path]::GetFullPath($Root)

$faults = Get-FaultTable -Directory $Root

$certificate = $null
$thumbprint = ''
if (-not $Http)
{
    $certificate = Get-FixtureCertificate -Subject $CertSubject -Directory $CertDir
    $thumbprint = $certificate.Thumbprint
    if (-not $NoCertificateInstall)
    {
        try
        {
            $trust = Install-FixtureCertificateTrust -Thumbprint $thumbprint -Subject $CertSubject -Directory $CertDir
            Write-Host ("[serve-fixtures] 夹具证书 {0}（{1}）；信任：{2}" -f `
                    $thumbprint.Substring(0, 12), $certificate.Subject, $(if ($trust.AlreadyThere) { '已在 LocalMachine\Root 里' } else { '刚装进 LocalMachine\Root' }))
        }
        catch
        {
            Write-FixtureLog -Path $LogFile -Text ("证书装信任库失败：{0}" -f $_.Exception.Message)
            Write-Host ("[serve-fixtures] **证书装信任库失败**：{0}" -f $_.Exception.Message)
            Write-Host '[serve-fixtures] 产品的 WinHttp 只看机器证书库 —— 不装进去的话它会报「TLS 握手没通过」。'
            Write-Host '[serve-fixtures] 需要用管理员权限重跑，或先手工导入 Run\certs\yuaextract-fixture.cer 到「受信任的根证书颁发机构（本地计算机）」。'
            exit 3
        }
    }
}

$listener = New-Object System.Net.Sockets.TcpListener([System.Net.IPAddress]::Parse($BindAddress), $Port)
$listener.Start()
$actualPort = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
$scheme = if ($Http) { 'http' } else { 'https' }
$base = ('{0}://{1}:{2}/' -f $scheme, $BindAddress, $actualPort)

$info = [ordered]@{
    port            = $actualPort
    base            = $base
    scheme          = $scheme
    root            = $Root
    logFile         = $LogFile
    certThumbprint  = $thumbprint
    faultCount      = $faults.Count
    startedUtc      = (Get-Date).ToUniversalTime().ToString('o')
    pid             = $PID
}
Write-PortFile -Path $PortFile -Info $info
Write-Host ("[serve-fixtures] 已监听 {0}（根目录 {1}，坏法 {2} 条）" -f $base, $Root, $faults.Count)
Write-Host ("PORT={0}" -f $actualPort)
Write-FixtureLog -Path $LogFile -Text ("# serve-fixtures 启动 base={0} root={1} pid={2}" -f $base, $Root, $PID)

$script:requestCount = 0
$script:lastRequestAt = Get-Date
$pendingAccept = $null
$running = $true

while ($running)
{
    if (-not $pendingAccept) { $pendingAccept = $listener.AcceptTcpClientAsync() }
    if (-not $pendingAccept.Wait(400))
    {
        # 没连接进来：做家务（空闲退出 / 父进程消失）
        if ($ParentPid -gt 0)
        {
            $parent = Get-Process -Id $ParentPid -ErrorAction SilentlyContinue
            if (-not $parent)
            {
                Write-Host '[serve-fixtures] 父进程已退出，夹具服务随之退出'
                break
            }
        }
        if ($IdleTimeoutSec -gt 0 -and ((Get-Date) - $script:lastRequestAt).TotalSeconds -ge $IdleTimeoutSec)
        {
            Write-Host ("[serve-fixtures] 空闲 {0} 秒，自动退出" -f $IdleTimeoutSec)
            break
        }
        continue
    }

    $client = $pendingAccept.Result
    $pendingAccept = $null
    $script:requestCount++
    $script:lastRequestAt = Get-Date
    $started = Get-Date
    $stream = $null
    $requestPath = ''
    $faultMode = ''
    $status = 0
    $sent = 0

    try
    {
        $stream = $client.GetStream()
        if ($certificate)
        {
            $ssl = New-Object System.Net.Security.SslStream($stream, $false)
            $protocols = [System.Security.Authentication.SslProtocols]::Tls12 -bor [System.Security.Authentication.SslProtocols]::Tls13
            try { $ssl.AuthenticateAsServer($certificate, $false, $protocols, $false) }
            catch
            {
                # TLS 1.3 在这台机器上不启用时退回 1.2 再试一次（每次新建 SslStream，旧的不能用）
                $ssl.Dispose()
                $ssl = New-Object System.Net.Security.SslStream($stream, $false)
                $ssl.AuthenticateAsServer($certificate, $false, [System.Security.Authentication.SslProtocols]::Tls12, $false)
            }
            $stream = $ssl
        }

        $head = Read-RequestHead -Stream $stream
        $parsed = Get-RelativePathFromRequestLine -Head $head
        if (-not $parsed) { throw '请求行读不出来（不是 HTTP 请求？）' }
        $requestPath = $parsed.Path
        $fault = Get-FaultFor $faults $requestPath
        $faultMode = [string]$fault.Mode

        $file = Resolve-FixtureFile -Directory $Root -Relative $requestPath
        $fileExists = ($file -and (Test-Path -LiteralPath $file -PathType Leaf))
        $contentType = if ($requestPath -like '*.txt') { 'text/plain; charset=utf-8' } else { 'application/octet-stream' }

        # 文件不在就给**真 404**。曾经写成 throw：结果是"连接直接被掐断"，
        # 调用方看到的是"连接被中断"而不是 404 —— 用例会把"夹具没放这个文件"误判成网络故障。
        # garbage 例外：它自己造字节，不需要文件存在。
        if (-not $fileExists -and $faultMode -notin @('garbage', '404', 'status'))
        {
            $faultMode = '404'
        }

        switch ($faultMode)
        {
            '404'
            {
                $status = 404
                $body = [System.Text.Encoding]::UTF8.GetBytes("404 fixture: no such file`r`n")
                Write-ResponseHead -Stream $stream -Status 404 -Reason 'Not Found' -Length $body.Length -ContentType 'text/plain; charset=utf-8'
                Send-AllBytes -Stream $stream -Bytes $body
                $sent = $body.Length
            }
            'status'
            {
                $status = [int]$fault.Status
                $body = [System.Text.Encoding]::UTF8.GetBytes(("fixture status {0}`r`n" -f $status))
                Write-ResponseHead -Stream $stream -Status $status -Reason 'Fixture Status' -Length $body.Length -ContentType 'text/plain; charset=utf-8'
                Send-AllBytes -Stream $stream -Bytes $body
                $sent = $body.Length
            }
            'hang'
            {
                # 收下请求之后**一个字节都不发**：产品的接收超时（30s）必须响。
                # 注意：这里不能回 100-continue 之类的东西 —— 一发字节就不是"等不到回应"了。
                $seconds = [double]$fault.Seconds
                Write-Host ("[serve-fixtures] hang {0} 秒：{1}" -f $seconds, $requestPath)
                Start-Sleep -Milliseconds ([int]($seconds * 1000))
                $status = 0
            }
            'slow'
            {
                if (-not (Test-Path -LiteralPath $file)) { throw ("slow 模式要发的文件不存在：{0}" -f $file) }
                $bytes = [System.IO.File]::ReadAllBytes($file)
                Write-ResponseHead -Stream $stream -Status 200 -Reason 'OK' -Length $bytes.Length -ContentType $contentType
                $chunkSeconds = [double]$fault.ChunkSeconds
                $chunkBytes = [Math]::Max(1, [int]([Math]::Ceiling($bytes.Length / 4.0)))
                $offset = 0
                while ($offset -lt $bytes.Length)
                {
                    $take = [Math]::Min($chunkBytes, $bytes.Length - $offset)
                    $slice = New-Object byte[] $take
                    [Array]::Copy($bytes, $offset, $slice, 0, $take)
                    Send-AllBytes -Stream $stream -Bytes $slice
                    $offset += $take
                    $sent = $offset
                    Start-Sleep -Milliseconds ([int]($chunkSeconds * 1000))
                }
                $status = 200
            }
            'truncate'
            {
                if (-not (Test-Path -LiteralPath $file)) { throw ("truncate 模式要发的文件不存在：{0}" -f $file) }
                $bytes = [System.IO.File]::ReadAllBytes($file)
                $keep = [int]([Math]::Floor($bytes.Length * [double]$fault.Ratio))
                if ($keep -le 0) { $keep = [Math]::Max(1, [int]($bytes.Length / 2)) }
                $slice = New-Object byte[] $keep
                [Array]::Copy($bytes, 0, $slice, 0, $keep)
                # Content-Length 按**截断后**的长度报：对产品来说这是一份"完整的短文件"，
                # 只能靠 sha256 / 结构自洽判出来（正是我们要考的）。
                Write-ResponseHead -Stream $stream -Status 200 -Reason 'OK' -Length $keep -ContentType $contentType
                Send-AllBytes -Stream $stream -Bytes $slice
                $sent = $keep
                $status = 200
            }
            'short-body'
            {
                if (-not (Test-Path -LiteralPath $file)) { throw ("short-body 模式要发的文件不存在：{0}" -f $file) }
                $bytes = [System.IO.File]::ReadAllBytes($file)
                $keep = [int]([Math]::Floor($bytes.Length * [double]$fault.Ratio))
                $slice = New-Object byte[] ([Math]::Max(1, $keep))
                [Array]::Copy($bytes, 0, $slice, 0, $slice.Length)
                # Content-Length 报完整长度，只发一半就断开 —— 打"读到一半连接断了"。
                Write-ResponseHead -Stream $stream -Status 200 -Reason 'OK' -Length $bytes.Length -ContentType $contentType
                Send-AllBytes -Stream $stream -Bytes $slice
                $sent = $slice.Length
                $status = 200
            }
            'garbage'
            {
                $length = [int]$fault.Bytes
                $garbage = New-Object byte[] $length
                (New-Object System.Random(20261015)).NextBytes($garbage)
                Write-ResponseHead -Stream $stream -Status 200 -Reason 'OK' -Length $length -ContentType $contentType
                Send-AllBytes -Stream $stream -Bytes $garbage
                $sent = $length
                $status = 200
            }
            default
            {
                if (-not $file -or -not (Test-Path -LiteralPath $file -PathType Leaf)) { throw ("夹具目录里没有这个文件：{0}" -f $requestPath) }
                $bytes = [System.IO.File]::ReadAllBytes($file)
                Write-ResponseHead -Stream $stream -Status 200 -Reason 'OK' -Length $bytes.Length -ContentType $contentType
                Send-AllBytes -Stream $stream -Bytes $bytes
                $sent = $bytes.Length
                $status = 200
            }
        }
    }
    catch
    {
        # 客户端中途断开（WinHttp 在证书不被信任时就是这样）会走到这里：如实记一行，不当成服务端故障。
        $status = -1
        Write-FixtureLog -Path $LogFile -Text ("# 连接异常 path={0} mode={1}：{2}" -f $requestPath, $faultMode, $_.Exception.Message)
    }
    finally
    {
        $elapsed = [int]((Get-Date) - $started).TotalMilliseconds
        try { if ($stream) { $stream.Dispose() } } catch { }
        try { $client.Close() } catch { }
        if ($status -ne -1)
        {
            Write-FixtureLog -Path $LogFile -Text ('{0} | #{1} | {2} | mode={3} | status={4} | bytes={5} | {6} ms' -f `
                    (Get-Date).ToString('HH:mm:ss.fff'), $script:requestCount, $requestPath, $faultMode, $status, $sent, $elapsed)
            if (-not $Quiet) { Write-Host ("[serve-fixtures] #{0} {1} mode={2} status={3} bytes={4} {5}ms" -f $script:requestCount, $requestPath, $faultMode, $status, $sent, $elapsed) }
        }
    }
}

try { $listener.Stop() } catch { }
Write-FixtureLog -Path $LogFile -Text ("# serve-fixtures 退出（共处理 {0} 个请求）" -f $script:requestCount)
Write-Host ("[serve-fixtures] 退出（共处理 {0} 个请求）" -f $script:requestCount)
exit 0
