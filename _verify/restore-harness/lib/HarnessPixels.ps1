# YuaExtract 验证脚手架：像素级回归工具（task-7 / 断言 E-11）
#
# 为什么需要它：
#   产品的「页签幽灵」是**像素残留**缺陷 —— 首页控件在切到「制作扩展集」后
#   已经是 `IsWindowVisible == False`（可见性断言全绿），但它们**留在窗口表面上的像素**
#   没有被擦掉，用户看到的是叠字。
#   只读窗口状态（visible / enabled / text）永远抓不住它，只有"逐像素比对"才行。
#
# 这一层做的事（不依赖 System.Drawing，pwsh 7 / Windows PowerShell 5.1 都能跑）：
#   * 读/写 24 位 BMP（GDI 抓图产物，见 HarnessNative.ps1 的 CaptureClientToBmp）
#   * 逐像素比对 → 差异像素数 / 差异比例 / **差异热区**（连通格子聚成矩形）
#   * 差异可视化 BMP（差异处涂红）—— 给人眼看，也给"热区落在哪个控件旧位置"做证据
#   * 合成图（自检用）：给干净图叠一块旧内容 → 造一个"一定会炸"的幽灵样本
#
# 约定：
#   * 只处理 **24 位未压缩 BMP**（抓图固定输出这种）；遇到别的格式明确报错，不猜。
#   * 坐标系一律"左上角为原点"（BMP 文件里是自下而上存的，读进来自动翻正），
#     这样热区矩形可以直接和控件矩形（客户区坐标）比较。
#   * 比对**不做**容差美化：默认逐字节严格相等才算"同像素"。容差交给调用方显式指定
#     （-Tolerance），并且必须把标定过程写进文档 —— 容差是最容易变成"永远通过"的旋钮。

if (-not ('Harness.Pixels' -as [type]))
{
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace Harness
{
    // 24 位 BGR、自上而下、每行紧密排列（stride = Width*3）的图像。
    public class PixelImage
    {
        public int Width;
        public int Height;
        public byte[] Data;
        public string Source = "";

        public PixelImage(int width, int height)
        {
            Width = width;
            Height = height;
            Data = new byte[width * height * 3];
        }
    }

    public class PixelDiff
    {
        public bool Ok = true;
        public string Error = "";
        public int Width;
        public int Height;
        public long TotalPixels;
        public long DiffPixels;
        public double DiffRatio;
        public int CellSize;
        public int DiffCells;
        // 每项："x,y,w,h,diffPixels,share"
        public List<string> Hotspots = new List<string>();
        public string DiffVisPath = "";
    }

    public static class Pixels
    {
        // ---------- BMP 读写 ----------

        public static PixelImage Load(string path)
        {
            byte[] raw = File.ReadAllBytes(path);
            if (raw.Length < 54) { throw new InvalidDataException("BMP 文件太小：" + path); }
            if (raw[0] != (byte)'B' || raw[1] != (byte)'M') { throw new InvalidDataException("不是 BMP 文件：" + path); }

            int dataOffset = BitConverter.ToInt32(raw, 10);
            int headerSize = BitConverter.ToInt32(raw, 14);
            int width = BitConverter.ToInt32(raw, 18);
            int height = BitConverter.ToInt32(raw, 22);
            short planes = BitConverter.ToInt16(raw, 26);
            short bits = BitConverter.ToInt16(raw, 28);
            int compression = BitConverter.ToInt32(raw, 30);

            if (headerSize < 40) { throw new InvalidDataException("不支持的 BMP 头（BITMAPCOREHEADER）：" + path); }
            if (planes != 1) { throw new InvalidDataException("不支持的 BMP 平面数：" + planes); }
            if (compression != 0) { throw new InvalidDataException("只支持未压缩 BMP（biCompression=" + compression + "）：" + path); }
            if (bits != 24 && bits != 32) { throw new InvalidDataException("只支持 24/32 位 BMP（biBitCount=" + bits + "）：" + path); }
            if (width <= 0 || height == 0) { throw new InvalidDataException("BMP 尺寸无效：" + width + "x" + height); }

            bool bottomUp = height > 0;
            int h = Math.Abs(height);
            int bytesPerPixel = bits / 8;
            int rowSize = ((width * bytesPerPixel + 3) / 4) * 4;

            PixelImage image = new PixelImage(width, h);
            image.Source = path;
            for (int y = 0; y < h; y++)
            {
                int sourceRow = bottomUp ? (h - 1 - y) : y;
                int rowStart = dataOffset + sourceRow * rowSize;
                if (rowStart + width * bytesPerPixel > raw.Length)
                {
                    throw new InvalidDataException("BMP 数据被截断（行 " + y + "）：" + path);
                }
                for (int x = 0; x < width; x++)
                {
                    int target = (y * width + x) * 3;
                    int source = rowStart + x * bytesPerPixel;
                    image.Data[target] = raw[source];
                    image.Data[target + 1] = raw[source + 1];
                    image.Data[target + 2] = raw[source + 2];
                }
            }
            return image;
        }

        // 写 24 位 BMP：高度写正数（自下而上）—— 这台机器上写负数会让驱动按自下而上回填，
        // 图上下颠倒（README 踩坑 #8）。这里统一按自下而上写，读回来永远一致。
        public static void Save(PixelImage image, string path)
        {
            int rowSize = ((image.Width * 3 + 3) / 4) * 4;
            int padding = rowSize - image.Width * 3;
            byte[] file = new byte[54 + rowSize * image.Height];
            file[0] = (byte)'B';
            file[1] = (byte)'M';
            BitConverter.GetBytes(file.Length).CopyTo(file, 2);
            BitConverter.GetBytes(54).CopyTo(file, 10);
            BitConverter.GetBytes(40).CopyTo(file, 14);
            BitConverter.GetBytes(image.Width).CopyTo(file, 18);
            BitConverter.GetBytes(image.Height).CopyTo(file, 22);
            BitConverter.GetBytes((short)1).CopyTo(file, 26);
            BitConverter.GetBytes((short)24).CopyTo(file, 28);
            BitConverter.GetBytes(rowSize * image.Height).CopyTo(file, 34);
            for (int y = 0; y < image.Height; y++)
            {
                int targetRow = image.Height - 1 - y;      // 自下而上
                Array.Copy(image.Data, y * image.Width * 3, file, 54 + targetRow * rowSize, image.Width * 3);
            }
            string directory = Path.GetDirectoryName(path);
            if (!string.IsNullOrEmpty(directory) && !Directory.Exists(directory)) { Directory.CreateDirectory(directory); }
            File.WriteAllBytes(path, file);
        }

        // ---------- 比对 ----------

        // 逐像素比对 + 差异热区。tolerance = 每通道允许的最大差（0 = 严格相等）。
        // diffVisPath 非空时写一张可视化图（差异涂红、相同处变淡灰）。
        public static PixelDiff Compare(PixelImage a, PixelImage b, int cellSize, int tolerance, int maxHotspots, int minHotspotPixels, string diffVisPath)
        {
            PixelDiff diff = new PixelDiff();
            if (a == null || b == null) { diff.Ok = false; diff.Error = "有一侧图像没读出来"; return diff; }
            diff.Width = a.Width;
            diff.Height = a.Height;
            diff.CellSize = cellSize <= 0 ? 8 : cellSize;
            diff.TotalPixels = (long)a.Width * a.Height;
            if (a.Width != b.Width || a.Height != b.Height)
            {
                diff.Ok = false;
                diff.Error = string.Format("两张图尺寸不同：{0}x{1} vs {2}x{3}", a.Width, a.Height, b.Width, b.Height);
                return diff;
            }

            int w = a.Width, h = a.Height;
            bool[] dirty = new bool[w * h];
            long diffCount = 0;
            PixelImage vis = string.IsNullOrEmpty(diffVisPath) ? null : new PixelImage(w, h);
            for (int i = 0; i < w * h; i++)
            {
                int p = i * 3;
                int d0 = Math.Abs(a.Data[p] - b.Data[p]);
                int d1 = Math.Abs(a.Data[p + 1] - b.Data[p + 1]);
                int d2 = Math.Abs(a.Data[p + 2] - b.Data[p + 2]);
                bool changed = d0 > tolerance || d1 > tolerance || d2 > tolerance;
                if (changed)
                {
                    diffCount++;
                    dirty[i] = true;
                }
                if (vis != null)
                {
                    if (changed) { vis.Data[p] = 0; vis.Data[p + 1] = 0; vis.Data[p + 2] = 230; }      // 蓝(BGR) = 差异
                    else
                    {
                        int lum = (a.Data[p] + a.Data[p + 1] + a.Data[p + 2]) / 3;
                        byte faded = (byte)(200 + lum / 4);
                        vis.Data[p] = faded; vis.Data[p + 1] = faded; vis.Data[p + 2] = faded;
                    }
                }
            }
            diff.DiffPixels = diffCount;
            diff.DiffRatio = (double)diffCount / diff.TotalPixels;

            // 差异格子 → 连通域 → 热区矩形
            int cell = diff.CellSize;
            int cols = (w + cell - 1) / cell;
            int rows = (h + cell - 1) / cell;
            int[] cellDiff = new int[cols * rows];
            for (int y = 0; y < h; y++)
            {
                int rowBase = y * w;
                int cellRow = (y / cell) * cols;
                for (int x = 0; x < w; x++)
                {
                    if (dirty[rowBase + x]) { cellDiff[cellRow + (x / cell)]++; }
                }
            }

            bool[] visited = new bool[cols * rows];
            List<int[]> regions = new List<int[]>();
            int[] stackX = new int[cols * rows];
            int[] stackY = new int[cols * rows];
            for (int cy = 0; cy < rows; cy++)
            {
                for (int cx = 0; cx < cols; cx++)
                {
                    int start = cy * cols + cx;
                    if (visited[start] || cellDiff[start] == 0) { continue; }
                    int top = 0;
                    stackX[top] = cx; stackY[top] = cy; top++;
                    visited[start] = true;
                    int minX = cx, maxX = cx, minY = cy, maxY = cy;
                    long pixels = 0;
                    while (top > 0)
                    {
                        top--;
                        int px = stackX[top], py = stackY[top];
                        pixels += cellDiff[py * cols + px];
                        if (px < minX) { minX = px; }
                        if (px > maxX) { maxX = px; }
                        if (py < minY) { minY = py; }
                        if (py > maxY) { maxY = py; }
                        for (int dy = -1; dy <= 1; dy++)
                        {
                            for (int dx = -1; dx <= 1; dx++)
                            {
                                if (dx == 0 && dy == 0) { continue; }
                                int nx = px + dx, ny = py + dy;
                                if (nx < 0 || ny < 0 || nx >= cols || ny >= rows) { continue; }
                                int index = ny * cols + nx;
                                if (visited[index] || cellDiff[index] == 0) { continue; }
                                visited[index] = true;
                                stackX[top] = nx; stackY[top] = ny; top++;
                            }
                        }
                    }
                    if (pixels < minHotspotPixels) { continue; }
                    int x = minX * cell;
                    int y = minY * cell;
                    int right = Math.Min(w, (maxX + 1) * cell);
                    int bottom = Math.Min(h, (maxY + 1) * cell);
                    regions.Add(new int[] { x, y, right - x, bottom - y, (int)pixels });
                }
            }
            diff.DiffCells = 0;
            for (int i = 0; i < cellDiff.Length; i++) { if (cellDiff[i] > 0) { diff.DiffCells++; } }

            regions.Sort(delegate(int[] left, int[] right) { return right[4].CompareTo(left[4]); });
            int keep = maxHotspots <= 0 ? regions.Count : Math.Min(maxHotspots, regions.Count);
            for (int i = 0; i < keep; i++)
            {
                int[] r = regions[i];
                double share = diffCount == 0 ? 0.0 : (double)r[4] / diffCount;
                diff.Hotspots.Add(string.Format("{0},{1},{2},{3},{4},{5:F3}", r[0], r[1], r[2], r[3], r[4], share));
            }
            if (regions.Count > keep)
            {
                diff.Hotspots.Add(string.Format("…另有 {0} 个热区（已限流，只列前 {1} 个）", regions.Count - keep, keep));
            }

            if (vis != null) { Save(vis, diffVisPath); diff.DiffVisPath = diffVisPath; }
            return diff;
        }

        // ---------- 图像统计（防"两张全黑图比出差异 0"这种假通过）----------

        // 输出："width=..;height=..;colors=<不同颜色数>;modal=<最多颜色的占比>;meanLum=..;minLum=..;maxLum=.."
        public static string Stats(PixelImage image, int maxColors)
        {
            Dictionary<int, int> counts = new Dictionary<int, int>();
            long sum = 0;
            int minLum = 255, maxLum = 0;
            long total = (long)image.Width * image.Height;
            for (int i = 0; i < image.Width * image.Height; i++)
            {
                int p = i * 3;
                int b = image.Data[p], g = image.Data[p + 1], r = image.Data[p + 2];
                int key = (r << 16) | (g << 8) | b;
                int current;
                if (counts.TryGetValue(key, out current)) { counts[key] = current + 1; } else { counts[key] = 1; }
                int lum = (r + g + b) / 3;
                sum += lum;
                if (lum < minLum) { minLum = lum; }
                if (lum > maxLum) { maxLum = lum; }
            }
            int modal = 0;
            foreach (KeyValuePair<int, int> pair in counts) { if (pair.Value > modal) { modal = pair.Value; } }
            return string.Format("width={0};height={1};colors={2};distinctCapped={3};modal={4:F4};meanLum={5:F2};minLum={6};maxLum={7}",
                image.Width, image.Height, counts.Count, counts.Count >= maxColors, (double)modal / total,
                (double)sum / total, minLum, maxLum);
        }

        // ---------- 合成图（自检用）----------

        // 单色图：用来验证"抓图全黑/单色 = 假通过"会被抓住（两张全黑图差异是 0）。
        public static PixelImage Solid(int width, int height, byte b, byte g, byte r)
        {
            PixelImage image = new PixelImage(width, height);
            for (int i = 0; i < width * height; i++)
            {
                image.Data[i * 3] = b;
                image.Data[i * 3 + 1] = g;
                image.Data[i * 3 + 2] = r;
            }
            return image;
        }

        // 画一张"像界面"的合成图：底色 + 网格线 + 若干色块 + 伪文字条带。
        // 目的只是"两处不同必须真的不同"，不追求好看。
        //
        // 底色抖动**必须随 variant 变**：否则两个变体在没有色块的区域是逐像素相同的，
        // 把 A 的一块背景贴到 B 上会"什么也没变"，幽灵合成图就白做了（踩过）。
        // 抖动取 64 级灰：颜色数够多（>50）、主色占比够低（<2%），E-11F 的"非空白"判据才成立。
        public static PixelImage Pattern(int width, int height, int variant)
        {
            PixelImage image = new PixelImage(width, height);
            for (int y = 0; y < height; y++)
            {
                for (int x = 0; x < width; x++)
                {
                    int p = (y * width + x) * 3;
                    int baseGray = 160 + ((x * 7 + y * 13 + variant * 11) % 64);
                    image.Data[p] = (byte)baseGray;
                    image.Data[p + 1] = (byte)baseGray;
                    image.Data[p + 2] = (byte)(baseGray > 200 ? 255 : baseGray + 10);
                    if (x % 64 == 0 || y % 64 == 0)
                    {
                        image.Data[p] = (byte)(120 + variant * 5);
                        image.Data[p + 1] = (byte)(120 + variant * 5);
                        image.Data[p + 2] = (byte)(130 + variant * 5);
                    }
                }
            }
            // 三个"控件"块：位置随 variant 变化，颜色也随 variant 变化
            int[] xs = new int[] { 20 + variant * 30, 240 + variant * 10, 520 - variant * 20 };
            int[] ys = new int[] { 120 + variant * 24, 300 + variant * 12, 460 + variant * 30 };
            for (int i = 0; i < 3; i++)
            {
                FillRect(image, xs[i], ys[i], 200, 44, (byte)(60 + variant * 40 + i * 30), (byte)(90 + i * 50), (byte)(160 - i * 30));
                FillRect(image, xs[i] + 6, ys[i] + 8, 150, 12, 255, 255, 255);   // 伪文字条
            }
            FillRect(image, 20, 20, 160, 28, (byte)(40 + variant * 20), 40, 40);  // "页签条"
            return image;
        }

        static void FillRect(PixelImage image, int x0, int y0, int width, int height, byte b, byte g, byte r)
        {
            for (int y = y0; y < y0 + height && y < image.Height; y++)
            {
                if (y < 0) { continue; }
                for (int x = x0; x < x0 + width && x < image.Width; x++)
                {
                    if (x < 0) { continue; }
                    int p = (y * image.Width + x) * 3;
                    image.Data[p] = b; image.Data[p + 1] = g; image.Data[p + 2] = r;
                }
            }
        }

        // 把 src 的 (x,y,w,h) 区域贴到 dst 的同位置 —— 模拟"旧控件的像素留在新页面上"。
        public static void PasteRegion(PixelImage dst, PixelImage src, int x, int y, int width, int height)
        {
            for (int row = 0; row < height; row++)
            {
                int dy = y + row;
                if (dy < 0 || dy >= dst.Height) { continue; }
                for (int column = 0; column < width; column++)
                {
                    int dx = x + column;
                    if (dx < 0 || dx >= dst.Width) { continue; }
                    int sx = Math.Min(Math.Max(dx, 0), src.Width - 1);
                    int sy = Math.Min(Math.Max(dy, 0), src.Height - 1);
                    int pd = (dy * dst.Width + dx) * 3;
                    int ps = (sy * src.Width + sx) * 3;
                    dst.Data[pd] = src.Data[ps];
                    dst.Data[pd + 1] = src.Data[ps + 1];
                    dst.Data[pd + 2] = src.Data[ps + 2];
                }
            }
        }
    }
}
'@
}

# ==================================================================
# PowerShell 层包装（返回 PSCustomObject；与脚手架其它部分风格一致）
# ==================================================================

# 读 BMP。失败时抛（调用方按"证据读不出来 = 断言失败"处理，绝不静默跳过）。
function Read-HarnessBmp
{
    param([Parameter(Mandatory)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    return [Harness.Pixels]::Load($Path)
}

# 写 BMP。
function Write-HarnessBmp
{
    param([Parameter(Mandatory)]$Image, [Parameter(Mandatory)][string]$Path)
    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    [Harness.Pixels]::Save($Image, $Path)
}

# 图像统计 → 有序表。
function Get-HarnessBmpStats
{
    param([Parameter(Mandatory)]$Image, [int]$MaxColors = 200000)
    $raw = [Harness.Pixels]::Stats($Image, $MaxColors)
    $map = [ordered]@{}
    foreach ($part in ($raw -split ';'))
    {
        $kv = $part -split '=', 2
        $map[$kv[0]] = $kv[1]
    }
    return [pscustomobject]@{
        Width         = [int]$map['width']
        Height        = [int]$map['height']
        Colors        = [int]$map['colors']
        ModalShare    = [double]$map['modal']
        MeanLuminance = [double]$map['meanLum']
        MinLuminance  = [int]$map['minLum']
        MaxLuminance  = [int]$map['maxLum']
        Raw           = $raw
    }
}

# 逐像素比对两个 BMP → 差异比例 + 热区。
function Compare-HarnessBmp
{
    param(
        [Parameter(Mandatory)][string]$BaselinePath,
        [Parameter(Mandatory)][string]$TargetPath,
        [string]$DiffVisPath = '',
        [int]$CellSize = 8,
        [int]$Tolerance = 0,
        [int]$MaxHotspots = 12,
        [int]$MinHotspotPixels = 16
    )

    $a = Read-HarnessBmp -Path $BaselinePath
    $b = Read-HarnessBmp -Path $TargetPath
    if (-not $a) { return [pscustomobject]@{ Ok = $false; Error = ("基线图读不出来：{0}" -f $BaselinePath) } }
    if (-not $b) { return [pscustomobject]@{ Ok = $false; Error = ("对照图读不出来：{0}" -f $TargetPath) } }

    $diff = [Harness.Pixels]::Compare($a, $b, $CellSize, $Tolerance, $MaxHotspots, $MinHotspotPixels, $DiffVisPath)
    $hotspots = New-Object System.Collections.ArrayList
    foreach ($line in $diff.Hotspots)
    {
        $parts = ([string]$line) -split ','
        if ($parts.Count -lt 5) { [void]$hotspots.Add([pscustomobject]@{ Text = [string]$line; X = -1; Y = -1; W = 0; H = 0; Pixels = 0; Share = 0 }); continue }
        [void]$hotspots.Add([pscustomobject]@{
                X = [int]$parts[0]; Y = [int]$parts[1]; W = [int]$parts[2]; H = [int]$parts[3]
                Pixels = [int]$parts[4]; Share = $(if ($parts.Count -ge 6) { [double]$parts[5] } else { 0 })
                Text = ('({0},{1}) {2}x{3} 差异像素 {4}' -f $parts[0], $parts[1], $parts[2], $parts[3], $parts[4])
            })
    }
    return [pscustomobject]@{
        Ok           = $diff.Ok
        Error        = $diff.Error
        Width        = $diff.Width
        Height       = $diff.Height
        TotalPixels  = $diff.TotalPixels
        DiffPixels   = $diff.DiffPixels
        DiffRatio    = $diff.DiffRatio
        DiffCells    = $diff.DiffCells
        CellSize     = $diff.CellSize
        Hotspots     = @($hotspots)
        DiffVisPath  = $diff.DiffVisPath
        BaselinePath = $BaselinePath
        TargetPath   = $TargetPath
    }
}

# ==================================================================
# 热区归属：把"差异矩形"翻成"哪个控件的旧位置"
# ==================================================================
#
# 抓图当时由驱动记录每个状态下的**可见子控件矩形**（客户区坐标）。
# 幽灵的特征是"位置上属于**别的页面**的控件" —— 所以归属时先看别的状态里的矩形，
# 再看当前状态自己的；同时报告两者，便于人判读（断言只负责把证据摆出来）。

function Get-RectOverlapArea
{
    param([int]$X1, [int]$Y1, [int]$W1, [int]$H1, [int]$X2, [int]$Y2, [int]$W2, [int]$H2)

    $left = [Math]::Max($X1, $X2)
    $top = [Math]::Max($Y1, $Y2)
    $right = [Math]::Min($X1 + $W1, $X2 + $W2)
    $bottom = [Math]::Min($Y1 + $H1, $Y2 + $H2)
    if ($right -le $left -or $bottom -le $top) { return 0 }
    return (($right - $left) * ($bottom - $top))
}

# 兼容两种形态的属性表：JSON 反序列化出来是 PSCustomObject（没有 .Keys / .Contains），
# 合成快照时用的是 hashtable（有）。统一走这个函数，别在下面直接点属性。
function Get-PropertyPairs
{
    param($Object)

    $pairs = New-Object System.Collections.ArrayList
    if ($null -eq $Object) { return @() }
    if ($Object -is [System.Collections.IDictionary])
    {
        foreach ($key in @($Object.Keys)) { [void]$pairs.Add([pscustomobject]@{ Name = [string]$key; Value = $Object[$key] }) }
        return @($pairs)
    }
    foreach ($property in @($Object.PSObject.Properties)) { [void]$pairs.Add([pscustomobject]@{ Name = [string]$property.Name; Value = $property.Value }) }
    return @($pairs)
}

# 按名字取值（兼容 hashtable 与 JSON 反序列化出来的 PSCustomObject）。
function Get-PropertyValue
{
    param($Object, [Parameter(Mandatory)][string]$Name, $Default = $null)

    if ($null -eq $Object) { return $Default }
    if ($Object -is [System.Collections.IDictionary])
    {
        if ($Object.Contains($Name)) { return $Object[$Name] }
        return $Default
    }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $Default }
    return $property.Value
}

# 返回 @("旧位置:set-visit1 4104«目录»（重叠 1,234 px）", ...)，最多 $Max 条。
function Resolve-HotspotOwners
{
    param(
        [Parameter(Mandatory)]$Hotspot,
        [Parameter(Mandatory)]$RectTables,
        [Parameter(Mandatory)][string]$StateName,
        [int]$Max = 3
    )

    if ($Hotspot.X -lt 0) { return @() }
    $candidates = New-Object System.Collections.ArrayList
    foreach ($pair in @(Get-PropertyPairs -Object $RectTables))
    {
        foreach ($row in @($pair.Value))
        {
            if ($null -eq $row) { continue }
            $width = [int]$row.w
            $height = [int]$row.h
            if ($width -le 0 -or $height -le 0) { continue }
            $area = Get-RectOverlapArea -X1 $Hotspot.X -Y1 $Hotspot.Y -W1 $Hotspot.W -H1 $Hotspot.H `
                -X2 ([int]$row.x) -Y2 ([int]$row.y) -W2 $width -H2 $height
            if ($area -le 0) { continue }
            $where = if ([string]$pair.Name -eq $StateName) { '本状态' } else { '旧位置:' + [string]$pair.Name }
            $text = [string]$row.text
            if ($text.Length -gt 24) { $text = $text.Substring(0, 24) + '…' }
            [void]$candidates.Add([pscustomobject]@{
                    Area  = $area
                    Where = $where
                    Label = ('{0} {1}«{2}»' -f $where, $row.id, $(if ($text) { $text } else { $row.class }))
                })
        }
    }
    $ordered = @($candidates | Sort-Object -Property Area -Descending)
    $result = New-Object System.Collections.ArrayList
    foreach ($item in $ordered)
    {
        if ($result.Count -ge $Max) { break }
        [void]$result.Add(('{0}（重叠 {1:N0} px）' -f $item.Label, $item.Area))
    }
    return @($result)
}

# ==================================================================
# E-11 断言本体（driver 抓图 → 本函数判分；也供 assert-extension.ps1 内联调用）
# ==================================================================
#
# 只读快照（images + JSON），**不读现场**；读不到就判失败。
# 需要 Add-Check / Read-JsonFile / Get-ObjectValue（来自 lib\HarnessUtil.ps1）。

function Invoke-TabPixelChecks
{
    param(
        [Parameter(Mandatory)][string]$SnapshotJson,
        [string]$DiffDir = '',
        # 快照缺失时：$true = 硬失败（正式跑）；$false = warn（单独跑断言、没抓图时）
        [bool]$RequireSnapshot = $true
    )

    $prefix = 'E-11'

    if (-not (Test-Path -LiteralPath $SnapshotJson))
    {
        # "没验证"与"验证通过"是两回事：Ok 一律 false（自检会把这条算成"该炸"），
        # 只有 Severity 决定它是否让整次运行失败：
        #   $RequireSnapshot = $true  → fail（正式跑：缺快照就是没验证，必须炸）
        #   $RequireSnapshot = $false → warn（单独跑断言、没抓图时；仍然显式标出来，不静默跳过）
        Add-Check -Id $prefix -Title '页签像素幽灵回归（快照缺失：本项未执行）' -Ok $false `
            -Severity $(if ($RequireSnapshot) { 'fail' } else { 'warn' }) `
            -Details @(("缺少抓图快照 {0} —— 请先跑 run-tab-pixels.ps1（-Scope extension 下由 run-all.ps1 自动先跑）" -f $SnapshotJson),
                '注意：这一项在缺失时**没有**验证任何东西，不能当成"通过"（可见性断言在幽灵缺陷存在时照样全绿）。')
        return $null
    }

    $snapshot = Read-JsonFile -Path $SnapshotJson
    if (-not $snapshot)
    {
        Add-Check -Id $prefix -Title '页签像素幽灵回归（快照读不出来）' -Ok $false `
            -Details @(("快照 JSON 解析失败：{0}" -f $SnapshotJson))
        return $null
    }

    $clientWidth = [int](Get-ObjectValue -Object $snapshot -Name 'clientWidth' -Default 0)
    $clientHeight = [int](Get-ObjectValue -Object $snapshot -Name 'clientHeight' -Default 0)
    $thresholds = Get-ObjectValue -Object $snapshot -Name 'thresholds' -Default $null
    $captures = @(Get-ObjectValue -Object $snapshot -Name 'captures' -Default @())
    $rectTables = Get-ObjectValue -Object $snapshot -Name 'controlRects' -Default $null

    $stateRatio = [double](Get-ObjectValue -Object $thresholds -Name 'stateDiffRatio' -Default 0.001)
    $noiseRatio = [double](Get-ObjectValue -Object $thresholds -Name 'noiseDiffRatio' -Default 0.0001)
    $crossRatio = [double](Get-ObjectValue -Object $thresholds -Name 'crossProcessDiffRatio' -Default 0.001)
    $homeRatio = [double](Get-ObjectValue -Object $thresholds -Name 'homeHistoryDiffRatio' -Default 0.01)
    $minHomeMake = [double](Get-ObjectValue -Object $thresholds -Name 'minHomeMakeDiffRatio' -Default 0.05)
    $minColors = [int](Get-ObjectValue -Object $thresholds -Name 'minColors' -Default 50)
    $maxModal = [double](Get-ObjectValue -Object $thresholds -Name 'maxModalShare' -Default 0.95)

    # 阈值可信区间：防止"把阈值调大到 50% 让断言永远通过"这种静默退化成假通过。
    $thresholdProblem = @()
    if ($stateRatio -le 0 -or $stateRatio -gt 0.01) { $thresholdProblem += ('stateDiffRatio={0} 不在可信区间 (0, 0.01]' -f $stateRatio) }
    if ($noiseRatio -le 0 -or $noiseRatio -gt 0.001) { $thresholdProblem += ('noiseDiffRatio={0} 不在可信区间 (0, 0.001]' -f $noiseRatio) }
    if ($crossRatio -le 0 -or $crossRatio -gt 0.01) { $thresholdProblem += ('crossProcessDiffRatio={0} 不在可信区间 (0, 0.01]' -f $crossRatio) }
    if ($homeRatio -le 0 -or $homeRatio -gt 0.02) { $thresholdProblem += ('homeHistoryDiffRatio={0} 不在可信区间 (0, 0.02]' -f $homeRatio) }
    if ($minHomeMake -lt 0.02) { $thresholdProblem += ('minHomeMakeDiffRatio={0} 太小（正对照失效）' -f $minHomeMake) }
    if ($thresholdProblem.Count -gt 0)
    {
        Add-Check -Id ($prefix + '-T') -Title '阈值可信性（防止阈值被调到永远通过）' -Ok $false -Details $thresholdProblem
    }
    else
    {
        Add-Check -Id ($prefix + '-T') -Title '阈值可信性（防"永远通过"）' -Ok $true -Details @(
            ('制作页签状态矩阵 ≤ {0:P2}；噪声地板 ≤ {1:P3}；跨进程 ≤ {2:P2}；首页历史 ≤ {3:P2}；首页≠制作 ≥ {4:P1}' -f `
                    $stateRatio, $noiseRatio, $crossRatio, $homeRatio, $minHomeMake))
    }

    # ---------------- 快照自证：每条记录齐全、尺寸一致、页签真的切过去了 ----------------
    # 取字段一律走 Get-ObjectValue：合成快照（自检）可能少字段，StrictMode 下直接点会抛，
    # 而那种异常会盖掉真正的失败原因。
    function Get-CaptureFile($Capture) { return [string](Get-ObjectValue -Object $Capture -Name 'bmp' -Default '') }
    function Get-CaptureName($Capture) { return [string](Get-ObjectValue -Object $Capture -Name 'name' -Default '?') }

    $missingFiles = New-Object System.Collections.ArrayList
    $sizeMismatch = New-Object System.Collections.ArrayList
    $tabNotSwitched = New-Object System.Collections.ArrayList
    $blank = New-Object System.Collections.ArrayList
    foreach ($capture in $captures)
    {
        $name = Get-CaptureName $capture
        $file = Get-CaptureFile $capture
        if (-not $file -or -not (Test-Path -LiteralPath $file)) { [void]$missingFiles.Add($name); continue }
        $width = [int](Get-ObjectValue -Object $capture -Name 'width' -Default 0)
        $height = [int](Get-ObjectValue -Object $capture -Name 'height' -Default 0)
        if ($clientWidth -gt 0 -and ($width -ne $clientWidth -or $height -ne $clientHeight))
        {
            [void]$sizeMismatch.Add(('{0}：{1}x{2}（期望 {3}x{4}）' -f $name, $width, $height, $clientWidth, $clientHeight))
        }
        $colors = [int](Get-ObjectValue -Object $capture -Name 'colors' -Default 0)
        $modal = [double](Get-ObjectValue -Object $capture -Name 'modalShare' -Default 1)
        if ($colors -lt $minColors -or $modal -gt $maxModal)
        {
            [void]$blank.Add(('{0}：颜色 {1} 种（<{2}）/ 主色占比 {3:P1}（>{4:P0}）' -f $name, $colors, $minColors, $modal, $maxModal))
        }
        $page = [string](Get-ObjectValue -Object $capture -Name 'page' -Default '')
        if ($page -eq 'make')
        {
            $makeMarker = Get-ObjectValue -Object $capture -Name 'makeMarkerVisible' -Default $null
            $homeMarker = Get-ObjectValue -Object $capture -Name 'homeMarkerVisible' -Default $null
            if ($makeMarker -ne $true -or $homeMarker -ne $false)
            {
                [void]$tabNotSwitched.Add(('{0}：制作页标志控件可见={1}（期望 True）/ 首页标志控件可见={2}（期望 False）' -f $name, $makeMarker, $homeMarker))
            }
        }
    }

    $evidenceProblem = @()
    if ($missingFiles.Count -gt 0) { $evidenceProblem += ('抓图文件不存在：' + (($missingFiles | Select-Object -First 6) -join ', ')) }
    if ($sizeMismatch.Count -gt 0) { $evidenceProblem += ('尺寸与快照声明不一致：' + (($sizeMismatch | Select-Object -First 6) -join '; ')) }
    $driverFailure = Get-ObjectValue -Object $snapshot -Name 'failure' -Default ''
    if ($driverFailure) { $evidenceProblem += ('驱动本次失败：{0}' -f $driverFailure) }
    Add-Check -Id ($prefix + '-E') -Title '快照完整性（抓图文件 / 尺寸 / 驱动失败）' -Ok ($evidenceProblem.Count -eq 0) `
        -Details $(if ($evidenceProblem.Count -eq 0) {
            @(('{0} 张抓图；客户区 {1}x{2}；构建指纹 {3}' -f $captures.Count, $clientWidth, $clientHeight,
                    [string](Get-ObjectValue -Object $snapshot -Name 'appFingerprint' -Default '?')))
        } else { $evidenceProblem })

    # ---------------- 正对照：抓的是真画面，且页签真的切过去了 ----------------
    # 防两类假通过：(1) 抓图全黑/单色（两张图都比"相同"）；(2) 页签没切过去（"制作页图"其实是首页图）。
    $byName = @{}
    foreach ($capture in $captures) { $byName[(Get-CaptureName $capture)] = $capture }

    $controlProblem = @()
    if ($blank.Count -gt 0) { $controlProblem += ('抓图像空白/单色：' + (($blank | Select-Object -First 6) -join '; ')) }
    if ($tabNotSwitched.Count -gt 0) { $controlProblem += ('页签没切过去：' + (($tabNotSwitched | Select-Object -First 6) -join '; ')) }
    $homeMakeRatio = $null
    if ($byName.ContainsKey('home-startup') -and $byName.ContainsKey('make-baseline'))
    {
        $diff = Compare-HarnessBmp -BaselinePath ([string](Get-CaptureFile $byName['home-startup'])) -TargetPath ([string](Get-CaptureFile $byName['make-baseline'])) -MaxHotspots 1
        if (-not $diff.Ok) { $controlProblem += ('首页/制作页比对失败：{0}' -f $diff.Error) }
        else
        {
            $homeMakeRatio = $diff.DiffRatio
            if ($diff.DiffRatio -lt $minHomeMake)
            {
                $controlProblem += ('首页图与制作页图只差 {0:P2}（要求 ≥ {1:P1}）—— 页签可能没切过去，或抓图是同一张' -f $diff.DiffRatio, $minHomeMake)
            }
        }
    }
    else { $controlProblem += '快照里缺 home-startup / make-baseline，正对照做不了' }

    Add-Check -Id ($prefix + '-F') -Title '抓图有效性（非空白 + 首页≠制作页）' -Ok ($controlProblem.Count -eq 0) `
        -Details $(if ($controlProblem.Count -eq 0) {
            @(('首页图 vs 制作页图差异 {0:P2}（要求 ≥ {1:P1}）；所有抓图颜色数 ≥ {2} 且主色占比 ≤ {3:P0}' -f $homeMakeRatio, $minHomeMake, $minColors, $maxModal))
        } else { $controlProblem })

    # ---------------- 噪声地板：同一状态连拍两张必须逐像素一致 ----------------
    $noiseDetail = @()
    $noiseOk = $true
    if ($byName.ContainsKey('make-baseline') -and $byName.ContainsKey('make-baseline-noise'))
    {
        $noise = Compare-HarnessBmp -BaselinePath ([string](Get-CaptureFile $byName['make-baseline'])) -TargetPath ([string](Get-CaptureFile $byName['make-baseline-noise']))
        if (-not $noise.Ok) { $noiseOk = $false; $noiseDetail += ('比对失败：{0}' -f $noise.Error) }
        else
        {
            $noiseOk = ($noise.DiffRatio -le $noiseRatio)
            $noiseDetail += ('同状态连拍（make-baseline vs make-baseline-noise）：差异 {0:N0} px / {1:P4}（阈值 {2:P3}）' -f $noise.DiffPixels, $noise.DiffRatio, $noiseRatio)
            if (-not $noiseOk) { $noiseDetail += '抓图不稳定 —— 本次测量不可判读（不是产品结论，是脚手架/环境问题：先看是否有闪烁光标、进度条、时间戳文字）' }
        }
    }
    else { $noiseOk = $false; $noiseDetail += '快照里缺 make-baseline / make-baseline-noise' }
    Add-Check -Id ($prefix + '-C') -Title '抓图稳定性（噪声地板）' -Ok $noiseOk -Details $noiseDetail

    # ---------------- 主判据：制作页签的状态矩阵（历史无关性）----------------
    #
    # 判据：**不管你怎么到达制作页签**，画面必须逐像素一致。做成"两两比对"（而不是只跟基线比）
    # 是因为缺陷是累积的：实测修复前 make-after-set 与 make-after-home2 之间也有 3.51% 差异。
    $makeCaptures = @($captures | Where-Object {
            (Get-ObjectValue -Object $_ -Name 'page' -Default '') -eq 'make' -and
            (Get-CaptureName $_) -notlike '*noise*' -and (Get-CaptureName $_) -notlike '*process2*'
        })
    $present = @($makeCaptures | ForEach-Object { Get-CaptureName $_ })
    $matrixDetail = New-Object System.Collections.ArrayList
    $matrixOk = $true
    $worstRatio = 0.0
    $worstPair = ''
    $hotspotDetail = New-Object System.Collections.ArrayList
    $visDiffCount = 0

    if ($present.Count -lt 4 -or -not ($present -contains 'make-baseline'))
    {
        $matrixOk = $false
        [void]$matrixDetail.Add(('制作页签抓图只有 {0} 路（{1}）—— 要求 ≥4 路且含 make-baseline（基线 = 新启动后第一次切到制作页签）' -f `
                    $present.Count, ($present -join ', ')))
    }
    else
    {
        for ($left = 0; $left -lt $makeCaptures.Count; $left++)
        {
            for ($right = $left + 1; $right -lt $makeCaptures.Count; $right++)
            {
                $leftName = Get-CaptureName $makeCaptures[$left]
                $rightName = Get-CaptureName $makeCaptures[$right]
                $diff = Compare-HarnessBmp -BaselinePath ([string](Get-CaptureFile $makeCaptures[$left])) `
                    -TargetPath ([string](Get-CaptureFile $makeCaptures[$right])) -MaxHotspots 6
                if (-not $diff.Ok)
                {
                    $matrixOk = $false
                    [void]$matrixDetail.Add(('{0} vs {1}：比对失败 {2}' -f $leftName, $rightName, $diff.Error))
                    continue
                }
                $pass = ($diff.DiffRatio -le $stateRatio)
                if (-not $pass)
                {
                    $matrixOk = $false
                    # 失败要给出**热区 + 控件归属**，便于定位（"是哪个控件的旧位置"）
                    foreach ($hotspot in $diff.Hotspots)
                    {
                        if ($hotspot.X -lt 0) { continue }        # "…另有 N 个热区"那行没有坐标
                        $owners = @()
                        if ($rectTables) { $owners = @(Resolve-HotspotOwners -Hotspot $hotspot -RectTables $rectTables -StateName $rightName -Max 3) }
                        [void]$hotspotDetail.Add(('  {0} vs {1} ← 差异热区 ({2},{3}) {4}x{5}，{6:N0} px（占全部差异 {7:P0}）' -f `
                                    $leftName, $rightName, $hotspot.X, $hotspot.Y, $hotspot.W, $hotspot.H, $hotspot.Pixels, $hotspot.Share))
                        foreach ($owner in $owners) { [void]$hotspotDetail.Add(('      · 疑似旧位置：{0}' -f $owner)) }
                    }
                }
                if ($diff.DiffRatio -gt $worstRatio) { $worstRatio = $diff.DiffRatio; $worstPair = ('{0} vs {1}' -f $leftName, $rightName) }
                [void]$matrixDetail.Add(('{0} vs {1}：差异 {2:N0} px / {3:P3}（阈值 {4:P2}）{5}' -f `
                            $leftName, $rightName, $diff.DiffPixels, $diff.DiffRatio, $stateRatio, $(if ($pass) { ' OK' } else { ' **超阈值**' })))
                # 差异可视化**只在真出问题时写**（每一对都写会留下十几 MB 的"全灰图"，
                # 反而把真正要紧的证据淹掉）。
                if (-not $pass -and $DiffDir -and $visDiffCount -lt 20)
                {
                    $visBmp = Join-Path $DiffDir ('diff-{0}--{1}.bmp' -f $leftName, $rightName)
                    [void](Compare-HarnessBmp -BaselinePath ([string](Get-CaptureFile $makeCaptures[$left])) `
                            -TargetPath ([string](Get-CaptureFile $makeCaptures[$right])) -DiffVisPath $visBmp -MaxHotspots 6)
                    $visPng = [System.IO.Path]::ChangeExtension($visBmp, '.png')
                    [void](Convert-BmpToPng -BmpPath $visBmp -PngPath $visPng)
                    $visDiffCount++
                }
            }
        }
        if ($worstPair -eq '') { $worstPair = '（所有组合都是 0 差异像素）' }
        [void]$matrixDetail.Add(('最差组合：{0} = {1:P3}；阈值 {2:P2}（{3} 路制作页抓图，{4} 次两两比对）' -f `
                    $worstPair, $worstRatio, $stateRatio, $present.Count, ($present.Count * ($present.Count - 1) / 2)))
        [void]$matrixDetail.Add('标定依据：修复前构建实测"最小幽灵信号"= 12,762 px = 1.369%；同一状态连拍两张实测 0 px（噪声地板 0.0000%）→ 阈值 0.10%。')
    }
    Add-Check -Id ($prefix + '-a') -Title '制作页签：不同到达路径的画面必须一致（像素幽灵主判据）' -Ok $matrixOk `
        -Details (@($matrixDetail) + @($hotspotDetail))

    # ---------------- 首页的历史无关性（幽灵是双向的）----------------
    # 只比"日志关闭状态"下抓的首页图：log-open/log-closed 那两张画面里多了日志面板，
    # 与它们比会把"日志面板"算成差异（那是设计如此，不是幽灵）。
    $homeCaptures = @($captures | Where-Object { (Get-CaptureName $_) -in @('home-startup', 'home-visit1', 'home-visit2') })
    $homeDetail = New-Object System.Collections.ArrayList
    $homeOk = $true
    if ($homeCaptures.Count -lt 2)
    {
        $homeOk = $false
        [void]$homeDetail.Add('快照里缺首页抓图（home-startup / home-visit1 / home-visit2）—— 首页历史无关性做不了')
    }
    else
    {
        $homeWorst = 0.0
        $homeWorstPair = ''
        for ($left = 0; $left -lt $homeCaptures.Count; $left++)
        {
            for ($right = $left + 1; $right -lt $homeCaptures.Count; $right++)
            {
                $leftName = Get-CaptureName $homeCaptures[$left]
                $rightName = Get-CaptureName $homeCaptures[$right]
                $diff = Compare-HarnessBmp -BaselinePath ([string](Get-CaptureFile $homeCaptures[$left])) `
                    -TargetPath ([string](Get-CaptureFile $homeCaptures[$right])) -MaxHotspots 4
                if (-not $diff.Ok) { $homeOk = $false; [void]$homeDetail.Add(('{0} vs {1}：比对失败 {2}' -f $leftName, $rightName, $diff.Error)); continue }
                $pass = ($diff.DiffRatio -le $homeRatio)
                if (-not $pass) { $homeOk = $false }
                if ($diff.DiffRatio -gt $homeWorst) { $homeWorst = $diff.DiffRatio; $homeWorstPair = ('{0} vs {1}' -f $leftName, $rightName) }
                [void]$homeDetail.Add(('{0} vs {1}：差异 {2:N0} px / {3:P3}（阈值 {4:P2}）{5}' -f `
                            $leftName, $rightName, $diff.DiffPixels, $diff.DiffRatio, $homeRatio, $(if ($pass) { ' OK' } else { ' **超阈值**' })))
                if (-not $pass)
                {
                    foreach ($hotspot in $diff.Hotspots)
                    {
                        if ($hotspot.X -lt 0) { continue }
                        [void]$homeDetail.Add(('  差异热区 ({0},{1}) {2}x{3}，{4:N0} px' -f $hotspot.X, $hotspot.Y, $hotspot.W, $hotspot.H, $hotspot.Pixels))
                        [void]$hotspotDetail.Add(('  首页 {0} vs {1} ← 差异热区 ({2},{3}) {4}x{5}，{6:N0} px' -f `
                                    $leftName, $rightName, $hotspot.X, $hotspot.Y, $hotspot.W, $hotspot.H, $hotspot.Pixels))
                        if ($rectTables)
                        {
                            foreach ($owner in @(Resolve-HotspotOwners -Hotspot $hotspot -RectTables $rectTables -StateName $rightName -Max 2))
                            {
                                [void]$homeDetail.Add(('      · 疑似旧位置：{0}' -f $owner))
                            }
                        }
                    }
                }
            }
        }
        if ($homeWorstPair -eq '') { $homeWorstPair = '（所有组合都是 0 差异像素）' }
        [void]$homeDetail.Add(('最差组合：{0} = {1:P3}（修复前实测 11.57%：制作页的像素也会留在首页上）' -f $homeWorstPair, $homeWorst))
        [void]$homeDetail.Add('首页判据的口径（阈值比制作页签宽，理由已实测）：合法重绘噪声有两块 ——')
        [void]$homeDetail.Add('  ① 概览 RichEdit 被重新 SetWindowText 同一段文字后的重绘差异（修复后实测约 1,510 px；逐字比对正文完全相同，464 字）；')
        [void]$homeDetail.Add('  ② 点过的页签按钮上的焦点框（修复后实测约 188 px；BM_GETSTATE 启动 0x0000 → 点击后 0x0008=BST_FOCUS）。')
        [void]$homeDetail.Add('而幽灵（旧控件像素残留）修复前是 11.57% —— 两者相差 60 倍，阈值 1% 正好夹在中间（噪声 5.4 倍 / 缺陷 11.6 倍）。')
        if (-not $homeOk)
        {
            [void]$homeDetail.Add('⚠ 超阈值说明首页出现了**超出"重绘噪声"量级**的差异：请按上面的热区坐标找旧控件，别当成重绘噪声放过。')
        }
    }
    Add-Check -Id ($prefix + '-b') -Title '首页页签：去过制作页之后回首页也必须干净' -Ok $homeOk -Details @($homeDetail)
    # ---------------- 跨进程复现 ----------------
    $crossDetail = New-Object System.Collections.ArrayList
    $crossOk = $true
    if ($byName.ContainsKey('make-baseline-process2'))
    {
        $diff = Compare-HarnessBmp -BaselinePath ([string](Get-CaptureFile $byName['make-baseline'])) -TargetPath ([string](Get-CaptureFile $byName['make-baseline-process2'])) -MaxHotspots 4
        if (-not $diff.Ok) { $crossOk = $false; [void]$crossDetail.Add(('比对失败：{0}' -f $diff.Error)) }
        else
        {
            $crossOk = ($diff.DiffRatio -le $crossRatio)
            [void]$crossDetail.Add(('进程1 基线 vs 进程2 基线（同为"新启动→直接点制作扩展集"）：差异 {0:N0} px / {1:P3}（阈值 {2:P2}）' -f `
                        $diff.DiffPixels, $diff.DiffRatio, $crossRatio))
            if (-not $crossOk)
            {
                foreach ($hotspot in $diff.Hotspots) { [void]$crossDetail.Add(('  差异热区 ({0},{1}) {2}x{3}，{4:N0} px' -f $hotspot.X, $hotspot.Y, $hotspot.W, $hotspot.H, $hotspot.Pixels)) }
                [void]$crossDetail.Add('两个进程的同一路径不一致：若不是产品有随机性，就是窗口几何/DPI 不一致（逐像素比对要求几何完全一致）')
            }
        }
    }
    else { $crossOk = $false; [void]$crossDetail.Add('快照里缺 make-baseline-process2（-NoCrossProcess 跑出来的快照不完整）') }
    Add-Check -Id ($prefix + '-d') -Title '跨进程可复现（基线与进程无关）' -Ok $crossOk -Details @($crossDetail)

    # ---------------- 可见性对照（**不是**幽灵判据，但必须一并记录）----------------
    # 这一条在缺陷存在时**照样全绿**（实测：幽灵 12,762 px 时首页控件 IsWindowVisible 全 False）
    # —— 写在这里是为了把"可见性"与"像素干净"两条线分开，别再用可见性冒充幽灵判据。
    $visDetail = New-Object System.Collections.ArrayList
    $visOk = $true
    foreach ($name in @($present))
    {
        $capture = $byName[$name]
        $makeMarker = Get-ObjectValue -Object $capture -Name 'makeMarkerVisible' -Default $null
        $homeMarker = Get-ObjectValue -Object $capture -Name 'homeMarkerVisible' -Default $null
        $ok = ($makeMarker -eq $true -and $homeMarker -eq $false)
        if (-not $ok) { $visOk = $false }
        [void]$visDetail.Add(('{0}：制作页标志控件(4101) 可见={1}；首页标志控件(4050) 可见={2}' -f $name, $makeMarker, $homeMarker))
    }
    $visibilityGhostBlind = $false
    if ($rectTables)
    {
        # 制作页状态下，"首页控件"里还有几个可见？（期望 0）—— 这是可见性口径的完整检查
        $lastMake = if ($present.Count -gt 0) { [string]$present[$present.Count - 1] } else { '' }
        if ($lastMake -and @(Get-PropertyPairs -Object $rectTables | Where-Object { $_.Name -eq $lastMake }).Count -gt 0)
        {
            $homeIds = @(4011, 4012, 4013, 4014, 4030, 4031, 4032, 4040, 4050, 4051, 4052, 4060, 4061, 4062, 4063, 4064, 4074, 4080, 4081, 4082, 4083, 4084)
            $lastRects = @(Get-PropertyValue -Object $rectTables -Name $lastMake)
            $visibleHome = @($lastRects | Where-Object { $_.id -in $homeIds -and $_.visible })
            [void]$visDetail.Add(('{0}：首页专属控件里仍可见 {1} 个（期望 0）' -f $lastMake, $visibleHome.Count))
            if ($visibleHome.Count -gt 0) { $visOk = $false }
        }
    }
    [void]$visDetail.Add('口径说明：本条只看"控件可见性"。幽灵缺陷存在时它**照样全绿**（实测：像素差异 12,762 px 时首页控件全是 IsWindowVisible=False）—— 判幽灵必须看 E-11a 的像素比对。')
    Add-Check -Id ($prefix + '-e') -Title '可见性对照（保留，但**不能**当幽灵判据）' -Ok $visOk -Details @($visDetail)

    # ---------------- 热区定位汇总 ----------------
    if ($matrixOk -and $homeOk)
    {
        Add-Check -Id ($prefix + '-g') -Title '差异热区定位' -Ok $true -Details @(
            ('制作页签四路抓图之间最大差异 {0:P3} —— 在阈值内，无差异热区（差异可视化只在超阈值时输出）' -f $worstRatio))
    }
    else
    {
        Add-Check -Id ($prefix + '-g') -Title '差异热区定位（失败定位用）' -Ok $false -Severity 'info' -Details @(
            ('超阈值的差异热区共 {0} 条，见 E-11a / E-11b 的详情；差异可视化 PNG 在 {1}' -f $hotspotDetail.Count, $(if ($DiffDir) { $DiffDir } else { '(未指定 -DiffDir)' })),
            '热区坐标 = 客户区坐标（左上角原点），可直接与控件矩形对照。')
    }

    return [pscustomobject]@{
        Snapshot        = $snapshot
        Captures        = $captures.Count
        WorstMakeRatio  = $worstRatio
        WorstPair       = $worstPair
        StateThreshold  = $stateRatio
        NoiseThreshold  = $noiseRatio
        HomeMakeRatio   = $homeMakeRatio
        MatrixOk        = $matrixOk
        HomeOk          = $homeOk
        CrossOk         = $crossOk
        NoiseOk         = $noiseOk
        VisibilityOk    = $visOk
    }
}

# 合成"界面图"（自检用）。
# ==================================================================
# E-12-a：「扩展集」页签的像素幽灵（与 E-11 同一套口径、同一套阈值）
# ==================================================================
#
# 为什么单开一条而不是把 set 抓图塞进 E-11 的矩阵：E-11 的判据是"制作页签的画面与到达路径无关"，
# 那条结论已经钉死了；扩展集页签是**另一个页**（左列一组按钮 + 右栏 4071），受另一种残留影响
# （从制作页签切过来时大号 GROUPBOX 的旧像素），所以它有自己的矩阵与自己的阈值项。
# 口径**故意与 E-11 完全一致**（逐像素比对、同状态连拍当地板、正对照要求"显著不同"、
# 阈值必须在可信区间、失败给热区与控件归属），这样两页的结论可以直接横向对照。
#
# 快照来自 run-tab-pixels.ps1（与 E-11 同一个 JSON），set-* 抓图由它单独走一条进程抓。
function Invoke-SetTabPixelChecks
{
    param(
        [Parameter(Mandatory)][string]$SnapshotJson,
        [string]$DiffDir = '',
        [bool]$RequireSnapshot = $true
    )

    $prefix = 'E-12'
    if (-not (Test-Path -LiteralPath $SnapshotJson))
    {
        Add-Check -Id ($prefix + '-a') -Title '扩展集页签像素幽灵（快照缺失：本项未执行）' -Ok $false `
            -Severity $(if ($RequireSnapshot) { 'fail' } else { 'warn' }) `
            -Details @(("缺少抓图快照 {0} —— 请先跑 run-tab-pixels.ps1（-Scope extension 下由 run-all.ps1 自动先跑）" -f $SnapshotJson),
                '这一项在缺失时**没有**验证任何东西，不能当成"通过"。')
        return $null
    }
    $snapshot = Read-JsonFile -Path $SnapshotJson
    if (-not $snapshot)
    {
        Add-Check -Id ($prefix + '-a') -Title '扩展集页签像素幽灵（快照读不出来）' -Ok $false `
            -Details @(("快照 JSON 解析失败：{0}" -f $SnapshotJson))
        return $null
    }

    $clientWidth = [int](Get-ObjectValue -Object $snapshot -Name 'clientWidth' -Default 0)
    $clientHeight = [int](Get-ObjectValue -Object $snapshot -Name 'clientHeight' -Default 0)
    $thresholds = Get-ObjectValue -Object $snapshot -Name 'thresholds' -Default $null
    $captures = @(Get-ObjectValue -Object $snapshot -Name 'captures' -Default @())
    $rectTables = Get-ObjectValue -Object $snapshot -Name 'controlRects' -Default $null

    $stateRatio = [double](Get-ObjectValue -Object $thresholds -Name 'stateDiffRatio' -Default 0.0010)
    $setStateRatio = [double](Get-ObjectValue -Object $thresholds -Name 'setStateDiffRatio' -Default $stateRatio)
    $noiseRatio = [double](Get-ObjectValue -Object $thresholds -Name 'noiseDiffRatio' -Default 0.0001)
    $minSetRefDiff = [double](Get-ObjectValue -Object $thresholds -Name 'minSetRefDiffRatio' -Default 0.05)
    $minColors = [int](Get-ObjectValue -Object $thresholds -Name 'minColors' -Default 50)
    $maxModal = [double](Get-ObjectValue -Object $thresholds -Name 'maxModalShare' -Default 0.95)

    # ---------- 阈值可信性 ----------
    $thresholdProblem = @()
    if ($setStateRatio -le 0 -or $setStateRatio -gt 0.01) { $thresholdProblem += ('setStateDiffRatio={0} 不在可信区间 (0, 0.01]' -f $setStateRatio) }
    if ($noiseRatio -le 0 -or $noiseRatio -gt 0.001) { $thresholdProblem += ('noiseDiffRatio={0} 不在可信区间 (0, 0.001]' -f $noiseRatio) }
    if ($minSetRefDiff -lt 0.02) { $thresholdProblem += ('minSetRefDiffRatio={0} 太小（正对照失效）' -f $minSetRefDiff) }
    if ($thresholdProblem.Count -gt 0)
    {
        Add-Check -Id ($prefix + '-T') -Title '阈值可信性（防止阈值被调到永远通过）' -Ok $false -Details $thresholdProblem
    }
    else
    {
        Add-Check -Id ($prefix + '-T') -Title '阈值可信性（防"永远通过"）' -Ok $true -Details @(
            ('扩展集页签状态矩阵 ≤ {0:P2}（制作页签同口径 {1:P2}）；噪声地板 ≤ {2:P3}；set 图与首页/制作图 ≥ {3:P1}' -f `
                    $setStateRatio, $stateRatio, $noiseRatio, $minSetRefDiff))
    }

    $byName = @{}
    foreach ($capture in $captures) { $byName[[string](Get-ObjectValue -Object $capture -Name 'name' -Default '?')] = $capture }

    # ---------- 快照完整性（含"页签真的切过去了"）----------
    $problems = New-Object System.Collections.ArrayList
    # set-visit1 是**进程 1 里的过渡状态**（E-11 抓的"扩展集→制作"那一路的中间点），
    # 它不属于扩展集页签的矩阵：那条矩阵要的是"新启动→set"这种干净基线（进程 3 单独抓）。
    # 混进来会拿"经过制作页签之后"的图去比基线，测到的是另一件事（自检里 set-visit1 用的
    # 甚至就是首页图 —— 那会让 clean 场景必炸）。
    $setCaptures = @($captures | Where-Object {
            [string](Get-ObjectValue -Object $_ -Name 'name' -Default '') -like 'set-*' -and
            [string](Get-ObjectValue -Object $_ -Name 'name' -Default '') -ne 'set-visit1'
        })
    if ($setCaptures.Count -eq 0)
    {
        [void]$problems.Add('快照里一张 set-* 抓图都没有（run-tab-pixels.ps1 里扩展集页签那一段没跑到）')
    }
    foreach ($capture in $setCaptures)
    {
        $name = [string](Get-ObjectValue -Object $capture -Name 'name' -Default '?')
        $file = [string](Get-ObjectValue -Object $capture -Name 'bmp' -Default '')
        if (-not $file -or -not (Test-Path -LiteralPath $file)) { [void]$problems.Add(('{0}：抓图文件不存在（{1}）' -f $name, $file)); continue }
        $width = [int](Get-ObjectValue -Object $capture -Name 'width' -Default 0)
        $height = [int](Get-ObjectValue -Object $capture -Name 'height' -Default 0)
        if ($clientWidth -gt 0 -and ($width -ne $clientWidth -or $height -ne $clientHeight))
        {
            [void]$problems.Add(('{0}：{1}x{2}（期望 {3}x{4}）' -f $name, $width, $height, $clientWidth, $clientHeight))
        }
        $colors = [int](Get-ObjectValue -Object $capture -Name 'colors' -Default 0)
        $modal = [double](Get-ObjectValue -Object $capture -Name 'modalShare' -Default 1)
        if ($colors -lt $minColors -or $modal -gt $maxModal) { [void]$problems.Add(('{0}：颜色 {1} 种 / 主色占比 {2:P1}（像空白图）' -f $name, $colors, $modal)) }
        # 切页签成功的硬证据：4131（扩展集页签标志控件）可见、首页/制作标志控件不可见
        $setMarker = Get-ObjectValue -Object $capture -Name 'setMarkerVisible' -Default $null
        $makeMarker = Get-ObjectValue -Object $capture -Name 'makeMarkerVisible' -Default $null
        $homeMarker = Get-ObjectValue -Object $capture -Name 'homeMarkerVisible' -Default $null
        if ($setMarker -ne $true) { [void]$problems.Add(('{0}：扩展集页签标志控件 4131 不可见（页签没切过去，这张图不是本页的画面）' -f $name)) }
        if ($makeMarker -eq $true) { [void]$problems.Add(('{0}：制作页签标志控件却可见（页签没切干净）' -f $name)) }
        if ($homeMarker -eq $true) { [void]$problems.Add(('{0}：首页标志控件却可见（页签没切干净）' -f $name)) }
    }
    $driverFailure = [string](Get-ObjectValue -Object $snapshot -Name 'failure' -Default '')
    if ($driverFailure) { [void]$problems.Add(('驱动本次失败：{0}' -f $driverFailure)) }
    Add-Check -Id ($prefix + '-E') -Title '扩展集页签快照完整性（文件 / 尺寸 / 非空白 / 页签标志控件）' -Ok ($problems.Count -eq 0) `
        -Details $(if ($problems.Count -eq 0) {
            @(('{0} 张 set 抓图；客户区 {1}x{2}；构建指纹 {3}' -f $setCaptures.Count, $clientWidth, $clientHeight,
                    [string](Get-ObjectValue -Object $snapshot -Name 'appFingerprint' -Default '?')))
        } else { @($problems) })

    # ---------- 正对照：扩展集页签的画面必须与首页/制作页**显著不同** ----------
    $controlProblem = @()
    $ratios = [ordered]@{}
    foreach ($reference in @('make-baseline', 'home-startup'))
    {
        if (-not $byName.ContainsKey($reference) -or -not $byName.ContainsKey('set-baseline'))
        {
            $controlProblem += ('快照里缺 {0} 或 set-baseline，正对照做不了' -f $reference)
            continue
        }
        $diff = Compare-HarnessBmp -BaselinePath ([string](Get-ObjectValue -Object $byName['set-baseline'] -Name 'bmp' -Default '')) `
            -TargetPath ([string](Get-ObjectValue -Object $byName[$reference] -Name 'bmp' -Default '')) -MaxHotspots 1
        if (-not $diff.Ok) { $controlProblem += ('set-baseline vs {0} 比对失败：{1}' -f $reference, $diff.Error); continue }
        $ratios[$reference] = $diff.DiffRatio
        if ($diff.DiffRatio -lt $minSetRefDiff)
        {
            $controlProblem += ('扩展集页签图与 {0} 只差 {1:P2}（要求 ≥ {2:P1}）—— 页签可能没切过去，或抓图是同一张' -f $reference, $diff.DiffRatio, $minSetRefDiff)
        }
    }
    Add-Check -Id ($prefix + '-F') -Title '抓图有效性（扩展集页签的画面与首页/制作页显著不同）' -Ok ($controlProblem.Count -eq 0) `
        -Details $(if ($controlProblem.Count -eq 0) {
            @($ratios.Keys | ForEach-Object { 'set-baseline vs {0}：{1:P2}（要求 ≥ {2:P1}）' -f $_, $ratios[$_], $minSetRefDiff })
        } else { $controlProblem })

    # ---------- 噪声地板：同状态连拍 ----------
    $noiseDetail = @()
    $noiseOk = $true
    if ($byName.ContainsKey('set-baseline') -and $byName.ContainsKey('set-baseline-noise'))
    {
        $noise = Compare-HarnessBmp -BaselinePath ([string](Get-ObjectValue -Object $byName['set-baseline'] -Name 'bmp' -Default '')) `
            -TargetPath ([string](Get-ObjectValue -Object $byName['set-baseline-noise'] -Name 'bmp' -Default ''))
        if (-not $noise.Ok) { $noiseOk = $false; $noiseDetail += ('比对失败：{0}' -f $noise.Error) }
        else
        {
            $noiseOk = ($noise.DiffRatio -le $noiseRatio)
            $noiseDetail += ('同状态连拍：差异 {0:N0} px / {1:P4}（阈值 {2:P3}）' -f $noise.DiffPixels, $noise.DiffRatio, $noiseRatio)
            if (-not $noiseOk) { $noiseDetail += '抓图不稳定 —— 本次测量不可判读（先看是否有闪烁光标 / 进度条 / 时间戳文字）' }
        }
    }
    else { $noiseOk = $false; $noiseDetail += '快照里缺 set-baseline / set-baseline-noise' }
    Add-Check -Id ($prefix + '-C') -Title '扩展集页签抓图稳定性（噪声地板）' -Ok $noiseOk -Details $noiseDetail

    # ---------- 主判据：扩展集页签的状态矩阵（历史无关性）----------
    $matrix = @($setCaptures | Where-Object { [string](Get-ObjectValue -Object $_ -Name 'name' -Default '') -notlike '*noise*' })
    $matrixDetail = New-Object System.Collections.ArrayList
    $hotspotDetail = New-Object System.Collections.ArrayList
    $matrixOk = $true
    $worstRatio = 0.0
    $worstPair = ''
    $visDiffCount = 0
    $matrixNames = @($matrix | ForEach-Object { [string](Get-ObjectValue -Object $_ -Name 'name' -Default '') })
    if ($matrix.Count -lt 3 -or -not ($matrixNames -contains 'set-baseline'))
    {
        $matrixOk = $false
        [void]$matrixDetail.Add(('扩展集页签抓图只有 {0} 路（{1}）—— 要求 ≥3 路且含 set-baseline' -f $matrix.Count, ($matrixNames -join ', ')))
    }
    else
    {
        for ($left = 0; $left -lt $matrix.Count; $left++)
        {
            for ($right = $left + 1; $right -lt $matrix.Count; $right++)
            {
                $leftName = [string](Get-ObjectValue -Object $matrix[$left] -Name 'name' -Default '?')
                $rightName = [string](Get-ObjectValue -Object $matrix[$right] -Name 'name' -Default '?')
                $leftBmp = [string](Get-ObjectValue -Object $matrix[$left] -Name 'bmp' -Default '')
                $rightBmp = [string](Get-ObjectValue -Object $matrix[$right] -Name 'bmp' -Default '')
                $diff = Compare-HarnessBmp -BaselinePath $leftBmp -TargetPath $rightBmp -MaxHotspots 6
                if (-not $diff.Ok)
                {
                    $matrixOk = $false
                    [void]$matrixDetail.Add(('{0} vs {1}：比对失败 {2}' -f $leftName, $rightName, $diff.Error))
                    continue
                }
                $pass = ($diff.DiffRatio -le $setStateRatio)
                if (-not $pass)
                {
                    $matrixOk = $false
                    foreach ($hotspot in $diff.Hotspots)
                    {
                        if ($hotspot.X -lt 0) { continue }
                        $owners = @()
                        if ($rectTables) { $owners = @(Resolve-HotspotOwners -Hotspot $hotspot -RectTables $rectTables -StateName $rightName -Max 3) }
                        [void]$hotspotDetail.Add(('  {0} vs {1} ← 差异热区 ({2},{3}) {4}x{5}，{6:N0} px（占全部差异 {7:P0}）' -f `
                                    $leftName, $rightName, $hotspot.X, $hotspot.Y, $hotspot.W, $hotspot.H, $hotspot.Pixels, $hotspot.Share))
                        foreach ($owner in $owners) { [void]$hotspotDetail.Add(('      · 疑似旧位置：{0}' -f $owner)) }
                    }
                }
                if ($diff.DiffRatio -gt $worstRatio) { $worstRatio = $diff.DiffRatio; $worstPair = ('{0} vs {1}' -f $leftName, $rightName) }
                [void]$matrixDetail.Add(('{0} vs {1}：差异 {2:N0} px / {3:P3}（阈值 {4:P2}）{5}' -f `
                            $leftName, $rightName, $diff.DiffPixels, $diff.DiffRatio, $setStateRatio, $(if ($pass) { ' OK' } else { ' **超阈值**' })))
                if (-not $pass -and $DiffDir -and $visDiffCount -lt 20)
                {
                    $visBmp = Join-Path $DiffDir ('set-diff-{0}--{1}.bmp' -f $leftName, $rightName)
                    [void](Compare-HarnessBmp -BaselinePath $leftBmp -TargetPath $rightBmp -DiffVisPath $visBmp -MaxHotspots 6)
                    [void](Convert-BmpToPng -BmpPath $visBmp -PngPath ([System.IO.Path]::ChangeExtension($visBmp, '.png')))
                    $visDiffCount++
                }
            }
        }
        if ($worstPair -eq '') { $worstPair = '（所有组合都是 0 差异像素）' }
        [void]$matrixDetail.Add(('最差组合：{0} = {1:P3}；阈值 {2:P2}（{3} 路 set 抓图，{4} 次两两比对）' -f `
                    $worstPair, $worstRatio, $setStateRatio, $matrix.Count, ($matrix.Count * ($matrix.Count - 1) / 2)))
        [void]$matrixDetail.Add('判据口径与 E-11 相同：逐像素比对全客户区、阈值必须在可信区间、失败给热区与控件归属。标定依据见 README「E-12」一节。')
    }
    Add-Check -Id ($prefix + '-a') -Title '扩展集页签：不同到达路径的画面必须一致（像素幽灵主判据）' -Ok $matrixOk `
        -Details (@($matrixDetail) + @($hotspotDetail))

    return [pscustomobject]@{ WorstPair = $worstPair; WorstRatio = $worstRatio; StateThreshold = $setStateRatio; Captures = $setCaptures.Count }
}

function New-HarnessPatternBmp
{
    param([Parameter(Mandatory)][string]$Path, [int]$Width = 800, [int]$Height = 600, [int]$Variant = 0)
    $image = [Harness.Pixels]::Pattern($Width, $Height, $Variant)
    Write-HarnessBmp -Image $image -Path $Path
    return $Path
}

# 把 OverlayPath 的一块区域贴到 BasePath 上 → 造"幽灵图"（自检用）。
function New-HarnessGhostBmp
{
    param(
        [Parameter(Mandatory)][string]$BasePath,
        [Parameter(Mandatory)][string]$OverlayPath,
        [Parameter(Mandatory)][string]$OutPath,
        [Parameter(Mandatory)][int[]]$Rect      # x,y,w,h
    )

    $base = Read-HarnessBmp -Path $BasePath
    $overlay = Read-HarnessBmp -Path $OverlayPath
    if (-not $base -or -not $overlay) { throw '合成幽灵图：有输入图读不出来' }
    if ($Rect.Count -lt 4) { throw '合成幽灵图：-Rect 需要 x,y,w,h 四项' }
    [Harness.Pixels]::PasteRegion($base, $overlay, $Rect[0], $Rect[1], $Rect[2], $Rect[3])
    Write-HarnessBmp -Image $base -Path $OutPath
    return $OutPath
}

# 单色图（自检：验证"全黑图 = 假通过"会被抓住）。
function New-HarnessSolidBmp
{
    param([Parameter(Mandatory)][string]$Path, [int]$Width = 800, [int]$Height = 600, [byte]$B = 0, [byte]$G = 0, [byte]$R = 0)
    Write-HarnessBmp -Image ([Harness.Pixels]::Solid($Width, $Height, $B, $G, $R)) -Path $Path
    return $Path
}

# ==================================================================
# 自检夹具：合成快照（含图片），喂给 assert-tab-pixels.ps1 验鉴别力
# ==================================================================
#
# 场景（每个都对应一种"断言退化"的可能）：
#   clean          全部一致 → 必须全绿（证明断言不是永远失败）
#   ghost          制作页某几路叠了首页的旧像素 → **只有 E-11a 炸**
#                  （可见性元数据全部正确：E-11e 必须**不**炸 —— 这就是"盲区"的证明）
#   home-ghost     首页回去后不干净 → 只有 E-11b 炸
#   unstable       同状态连拍不一致（噪声地板破）→ 只有 E-11C 炸
#   blank          所有抓图都是单色（两张图差异 0，最容易假通过）→ E-11F 必须炸
#   frozen         页签没切过去（"制作页图"其实是首页图，且驱动谎报切成功）→ E-11F 必须炸
#   crossprocess   另一个进程的同一路径不一致 → 只有 E-11d 炸
#   tampered       把阈值调到 50% 想让断言永远通过 → E-11T 必须炸
#
# 图片尺寸默认 800x600（比真实客户区小，跑得快，判据与尺寸无关）。

# 取字段一律用 Get-ObjectValue（StrictMode 下直接点不存在的属性会抛）。
function New-CaptureRecord
{
    param([string]$Name, [string]$Page, [string]$Path, [bool]$MakeMarker = $true, [bool]$HomeMarker = $false, [bool]$SetMarker = $false, [int]$Width = 800, [int]$Height = 600)

    $image = Read-HarnessBmp -Path $Path
    $stats = Get-HarnessBmpStats -Image $image
    return [pscustomobject][ordered]@{
        name              = $Name
        page              = $Page
        history           = $Name
        bmp               = $Path
        png               = ''
        width             = $stats.Width
        height            = $stats.Height
        colors            = $stats.Colors
        modalShare        = $stats.ModalShare
        sha256            = (Get-Sha256Hex -Path $Path)
        makeMarkerVisible = $MakeMarker
        homeMarkerVisible = $HomeMarker
        setMarkerVisible  = $SetMarker
    }
}

function New-SyntheticTabPixelSnapshot
{
    param(
        [Parameter(Mandatory)][string]$Dir,
        [Parameter(Mandatory)][ValidateSet('clean', 'ghost', 'home-ghost', 'unstable', 'blank', 'frozen', 'crossprocess', 'tampered',
            'set-ghost', 'set-unstable', 'set-frozen', 'set-tampered')][string]$Scenario,
        [int]$Width = 800,
        [int]$Height = 600
    )

    if (Test-Path -LiteralPath $Dir) { Remove-Item -LiteralPath $Dir -Recurse -Force }
    New-Item -ItemType Directory -Path $Dir -Force | Out-Null

    # "首页"/"制作页"/"扩展集页"三张不同的合成图（变体 0 / 3 / 6 两两差 > 正对照要求的 5%）
    $homeSrc = Join-Path $Dir 'src-home.bmp'
    $makeSrc = Join-Path $Dir 'src-make.bmp'
    $setSrc = Join-Path $Dir 'src-set.bmp'
    [void](New-HarnessPatternBmp -Path $homeSrc -Width $Width -Height $Height -Variant 0)
    [void](New-HarnessPatternBmp -Path $makeSrc -Width $Width -Height $Height -Variant 3)
    [void](New-HarnessPatternBmp -Path $setSrc -Width $Width -Height $Height -Variant 6)

    # "旧位置"的仿真实测热区（修复前实测最大热区是 (8,472) 560x56，这里按比例缩到 800x600）
    $ghostRects = @(@(20, 340, 240, 60), @(300, 180, 220, 48), @(60, 420, 300, 40))
    $homeGhostRects = @(@(30, 90, 260, 50), @(330, 300, 200, 44))

    $makePath = Join-Path $Dir 'make.bmp'
    Copy-Item -LiteralPath $makeSrc -Destination $makePath -Force
    $homePath = Join-Path $Dir 'home.bmp'
    Copy-Item -LiteralPath $homeSrc -Destination $homePath -Force

    $ghostedMake = Join-Path $Dir 'make-ghosted.bmp'
    Copy-Item -LiteralPath $makeSrc -Destination $ghostedMake -Force
    foreach ($rect in $ghostRects) { [void](New-HarnessGhostBmp -BasePath $ghostedMake -OverlayPath $homeSrc -OutPath $ghostedMake -Rect $rect) }

    $ghostedHome = Join-Path $Dir 'home-ghosted.bmp'
    Copy-Item -LiteralPath $homeSrc -Destination $ghostedHome -Force
    foreach ($rect in $homeGhostRects) { [void](New-HarnessGhostBmp -BasePath $ghostedHome -OverlayPath $makeSrc -OutPath $ghostedHome -Rect $rect) }

    # 扩展集页签的三张图：正常 / 叠了制作页旧像素 / 叠了首页旧像素
    $setPath = Join-Path $Dir 'set.bmp'
    Copy-Item -LiteralPath $setSrc -Destination $setPath -Force
    $setGhostedMake = Join-Path $Dir 'set-ghosted-make.bmp'
    Copy-Item -LiteralPath $setSrc -Destination $setGhostedMake -Force
    foreach ($rect in $ghostRects) { [void](New-HarnessGhostBmp -BasePath $setGhostedMake -OverlayPath $makeSrc -OutPath $setGhostedMake -Rect $rect) }
    $setGhostedHome = Join-Path $Dir 'set-ghosted-home.bmp'
    Copy-Item -LiteralPath $setSrc -Destination $setGhostedHome -Force
    foreach ($rect in $homeGhostRects) { [void](New-HarnessGhostBmp -BasePath $setGhostedHome -OverlayPath $homeSrc -OutPath $setGhostedHome -Rect $rect) }

    $blankPath = Join-Path $Dir 'blank.bmp'
    [void](New-HarnessSolidBmp -Path $blankPath -Width $Width -Height $Height -B 0 -G 0 -R 0)

    # ---------- 场景取值 ----------
    $fileOf = @{
        'home-startup'           = $homePath
        'make-baseline'          = $makePath
        'make-baseline-noise'    = $makePath
        'home-visit1'            = $homePath
        'make-after-home'        = $makePath
        'set-visit1'             = $homePath
        'make-after-set'         = $makePath
        'log-open'               = $homePath
        'log-closed'             = $homePath
        'make-after-log'         = $makePath
        'home-visit2'            = $homePath
        'make-baseline-process2' = $makePath
        'set-baseline'           = $setPath
        'set-baseline-noise'     = $setPath
        'set-after-home'         = $setPath
        'set-after-make'         = $setPath
        'set-after-log'          = $setPath
    }
    switch ($Scenario)
    {
        'ghost'
        {
            $fileOf['make-after-home'] = $ghostedMake
            $fileOf['make-after-set'] = $ghostedMake
            $fileOf['make-after-log'] = $ghostedMake
        }
        'home-ghost' { $fileOf['home-visit2'] = $ghostedHome }
        'unstable' { $fileOf['make-baseline-noise'] = $ghostedMake }
        'crossprocess' { $fileOf['make-baseline-process2'] = $ghostedMake }
        'blank'
        {
            foreach ($key in @($fileOf.Keys)) { $fileOf[$key] = $blankPath }
        }
        'frozen'
        {
            # 页签没切过去：所有"制作页"抓图其实都是首页图（而驱动**谎报**切成功了）
            foreach ($key in @($fileOf.Keys)) { if ($key -like 'make-*') { $fileOf[$key] = $homePath } }
        }
        'set-ghost'
        {
            # 扩展集页签的幽灵：从别的页签切过来时旧像素留在表面上（E-12-a 必须炸）
            $fileOf['set-after-home'] = $setGhostedHome
            $fileOf['set-after-make'] = $setGhostedMake
            $fileOf['set-after-log'] = $setGhostedMake
        }
        'set-unstable' { $fileOf['set-baseline-noise'] = $setGhostedMake }
        'set-frozen'
        {
            # 页签没切过去：所有 set 抓图其实都是首页图（E-12-F 必须炸）
            foreach ($key in @($fileOf.Keys)) { if ($key -like 'set-*') { $fileOf[$key] = $homePath } }
        }
    }

    $pageOf = @{
        'home-startup' = 'home'; 'make-baseline' = 'make'; 'make-baseline-noise' = 'make'; 'home-visit1' = 'home'
        'make-after-home' = 'make'; 'set-visit1' = 'set'; 'make-after-set' = 'make'; 'log-open' = 'home'
        'log-closed' = 'home'; 'make-after-log' = 'make'; 'home-visit2' = 'home'; 'make-baseline-process2' = 'make'
        'set-baseline' = 'set'; 'set-baseline-noise' = 'set'; 'set-after-home' = 'set'
        'set-after-make' = 'set'; 'set-after-log' = 'set'
    }

    $captures = New-Object System.Collections.ArrayList
    foreach ($name in @('home-startup', 'make-baseline', 'make-baseline-noise', 'home-visit1', 'make-after-home',
            'set-visit1', 'make-after-set', 'log-open', 'log-closed', 'make-after-log', 'home-visit2', 'make-baseline-process2',
            'set-baseline', 'set-baseline-noise', 'set-after-home', 'set-after-make', 'set-after-log'))
    {
        $page = [string]$pageOf[$name]
        # 可见性元数据一律"正确"：制作页上制作标志可见、首页标志不可见 —— 幽灵场景下这就是
        # "可见性全绿、像素不干净"的现场（E-11e 必须通过，E-11a 必须炸）。
        $makeMarker = ($page -eq 'make')
        $homeMarker = ($page -eq 'home')
        $setMarker = ($page -eq 'set')
        [void]$captures.Add((New-CaptureRecord -Name $name -Page $page -Path ([string]$fileOf[$name]) `
                    -MakeMarker $makeMarker -HomeMarker $homeMarker -SetMarker $setMarker -Width $Width -Height $Height))
    }

    # 控件矩形表：幽灵热区落在"首页控件的旧位置"上，归属断言才有东西可报
    $makeRects = New-Object System.Collections.ArrayList
    [void]$makeRects.Add([ordered]@{ id = 4101; class = 'Button'; text = '生成当前游戏扩展集'; visible = $true; x = 20; y = 200; w = 240; h = 32 })
    [void]$makeRects.Add([ordered]@{ id = 4104; class = 'Button'; text = '目录'; visible = $true; x = 20; y = 300; w = 240; h = 120 })
    [void]$makeRects.Add([ordered]@{ id = 4014; class = 'Button'; text = '辅助操作'; visible = $false; x = 20; y = 340; w = 240; h = 60 })
    [void]$makeRects.Add([ordered]@{ id = 4051; class = 'Button'; text = '步骤2：加载动态Hash收集模块'; visible = $false; x = 300; y = 180; w = 220; h = 48 })
    [void]$makeRects.Add([ordered]@{ id = 4060; class = 'Button'; text = '检查状态'; visible = $false; x = 60; y = 420; w = 300; h = 40 })
    $homeRects = New-Object System.Collections.ArrayList
    [void]$homeRects.Add([ordered]@{ id = 4014; class = 'Button'; text = '辅助操作'; visible = $true; x = 20; y = 340; w = 240; h = 60 })
    [void]$homeRects.Add([ordered]@{ id = 4051; class = 'Button'; text = '步骤2：加载动态Hash收集模块'; visible = $true; x = 300; y = 180; w = 220; h = 48 })
    [void]$homeRects.Add([ordered]@{ id = 4060; class = 'Button'; text = '检查状态'; visible = $true; x = 60; y = 420; w = 300; h = 40 })
    [void]$homeRects.Add([ordered]@{ id = 4101; class = 'Button'; text = '生成当前游戏扩展集'; visible = $false; x = 20; y = 200; w = 240; h = 32 })

    $setRects = New-Object System.Collections.ArrayList
    [void]$setRects.Add([ordered]@{ id = 4131; class = 'Button'; text = '刷新列表'; visible = $true; x = 20; y = 120; w = 200; h = 32 })
    [void]$setRects.Add([ordered]@{ id = 4101; class = 'Button'; text = '生成当前游戏扩展集'; visible = $false; x = 20; y = 340; w = 240; h = 60 })
    [void]$setRects.Add([ordered]@{ id = 4051; class = 'Button'; text = '步骤2：加载动态Hash收集模块'; visible = $false; x = 300; y = 180; w = 220; h = 48 })
    [void]$setRects.Add([ordered]@{ id = 4060; class = 'Button'; text = '检查状态'; visible = $false; x = 60; y = 420; w = 300; h = 40 })

    $rectTables = [ordered]@{}
    foreach ($name in @('home-startup', 'home-visit1', 'log-open', 'log-closed', 'home-visit2')) { $rectTables[$name] = @($homeRects) }
    foreach ($name in @('make-baseline', 'make-baseline-noise', 'make-after-home', 'make-after-set', 'make-after-log', 'make-baseline-process2')) { $rectTables[$name] = @($makeRects) }
    foreach ($name in @('set-baseline', 'set-baseline-noise', 'set-after-home', 'set-after-make', 'set-after-log', 'set-visit1')) { $rectTables[$name] = @($setRects) }

    $thresholds = [ordered]@{
        stateDiffRatio        = 0.0010
        setStateDiffRatio     = 0.0010
        noiseDiffRatio        = 0.0001
        crossProcessDiffRatio = 0.0010
        homeHistoryDiffRatio  = 0.0100
        minHomeMakeDiffRatio  = 0.05
        minSetRefDiffRatio    = 0.05
        minColors             = 50
        maxModalShare         = 0.95
    }
    if ($Scenario -eq 'tampered')
    {
        $thresholds['stateDiffRatio'] = 0.5
        $thresholds['noiseDiffRatio'] = 0.3
        $thresholds['homeHistoryDiffRatio'] = 0.3
        $thresholds['minHomeMakeDiffRatio'] = 0.0
    }
    if ($Scenario -eq 'set-tampered')
    {
        $thresholds['setStateDiffRatio'] = 0.5
        $thresholds['minSetRefDiffRatio'] = 0.0
    }

    $snapshotPath = Join-Path $Dir 'tab-pixels.json'
    Write-JsonFile -Path $snapshotPath -Object ([ordered]@{
            case             = ('synthetic-' + $Scenario)
            startedUtc       = (Get-Date).ToUniversalTime().ToString('o')
            finishedUtc      = (Get-Date).ToUniversalTime().ToString('o')
            appExe           = '(合成夹具：没有真实被测程序)'
            appFingerprint   = 'SYNTHETIC'
            clientWidth      = $Width
            clientHeight     = $Height
            thresholds       = $thresholds
            captures         = @($captures)
            controlRects     = $rectTables
            dialogs          = @()
            failure          = ''
            notes            = @(('自检场景 {0}：图片是合成图，判的是断言读得对不对' -f $Scenario))
        })
    return $snapshotPath
}

# 用**修复前的真实抓图**造一份快照（lead / harness 现场探针抓的客户区图）。
# 这些图是"缺陷现场"：四路制作页抓图互不相同（实测 1.37% / 9.28% / 5.76%）→ E-11a 必须炸。
# 图片不在仓库里时返回 ''（调用方按"跳过该场景"处理，不静默当成通过）。
function New-PrefixTabPixelSnapshot
{
    param([Parameter(Mandatory)][string]$EvidenceDir, [Parameter(Mandatory)][string]$Dir)

    $mapping = [ordered]@{
        'home-startup'           = 'home-startup.png'
        'home-visit1'            = 'home-visit1.png'
        'make-baseline'          = 'make-baseline.png'
        # 同状态连拍：现场探针实测"连拍两张 0 差异像素"，这里直接引用同一张图
        #（自证噪声地板为 0 的那次实测见 evidence\tab-pixels-prefix\README.txt）
        'make-baseline-noise'    = 'make-baseline.png'
        'make-after-home'        = 'make-after-home.png'
        'set-visit1'             = 'set-visit1.png'
        'make-after-set'         = 'make-after-set.png'
        'make-after-log'         = 'make-after-log.png'
        # 跨进程复现：修复前那次探针只有一个进程，引用同一张图（E-11d 在本场景里不承载判据）
        'make-baseline-process2' = 'make-baseline.png'
    }
    foreach ($file in $mapping.Values)
    {
        if (-not (Test-Path -LiteralPath (Join-Path $EvidenceDir $file))) { return '' }
    }

    if (Test-Path -LiteralPath $Dir) { Remove-Item -LiteralPath $Dir -Recurse -Force }
    New-Item -ItemType Directory -Path $Dir -Force | Out-Null

    $pageOf = @{
        'home-startup' = 'home'; 'home-visit1' = 'home'
        'make-baseline' = 'make'; 'make-baseline-noise' = 'make'; 'make-after-home' = 'make'
        'set-visit1' = 'set'; 'make-after-set' = 'make'; 'make-after-log' = 'make'
        'make-baseline-process2' = 'make'
    }
    $captures = New-Object System.Collections.ArrayList
    $width = 0
    $height = 0
    foreach ($name in $mapping.Keys)
    {
        # 证据图存的是 PNG（每张 ~60 KB；BMP 每张 2.8 MB，仓库里放不下）。
        # 比对前用 Pillow 无损转成 BMP —— PNG/BMP 都是无损 RGB，像素一一对应（转完会自检像素数）。
        $source = Join-Path $EvidenceDir ([string]$mapping[$name])
        $path = Join-Path $Dir ([System.IO.Path]::GetFileNameWithoutExtension([string]$mapping[$name]) + '.bmp')
        if (-not (Convert-PngToBmp -PngPath $source -BmpPath $path)) { return '' }
        $page = [string]$pageOf[$name]
        [void]$captures.Add((New-CaptureRecord -Name $name -Page $page -Path $path `
                    -MakeMarker ($page -eq 'make') -HomeMarker ($page -eq 'home')))
        if ($width -eq 0)
        {
            $image = Read-HarnessBmp -Path $path
            $width = $image.Width
            $height = $image.Height
        }
    }

    $snapshotPath = Join-Path $Dir 'tab-pixels.json'
    Write-JsonFile -Path $snapshotPath -Object ([ordered]@{
            case           = 'prefix-real-capture'
            startedUtc     = (Get-Date).ToUniversalTime().ToString('o')
            appExe         = (Join-Path $EvidenceDir 'YuaExtract.exe.prefix.txt')
            appFingerprint = '6B664840DD6EF1BB（修复前）'
            clientWidth    = $width
            clientHeight   = $height
            thresholds     = [ordered]@{
                stateDiffRatio = 0.0010; noiseDiffRatio = 0.0001; crossProcessDiffRatio = 0.0010
                homeHistoryDiffRatio = 0.0100; minHomeMakeDiffRatio = 0.05; minColors = 50; maxModalShare = 0.95
            }
            captures       = @($captures)
            controlRects   = [ordered]@{}
            failure        = ''
            notes          = @('修复前现场抓图（PrintWindow 客户区）：四路制作页抓图互不相同 = 缺陷现场；断言必须炸。')
        })
    return $snapshotPath
}

# ---------- PNG → 24 位 BMP（无损；交给自带的 Python + Pillow）----------
#
# 抓图原生产物是 BMP（精确像素、无压缩）；PNG 只用来当"给人看/长期留证"。
# 历史证据图（修复前现场抓图）按 PNG 存（每张 ~60 KB），比对前再转回 BMP：
# PNG 与 BMP 都是无损 RGB，转回来像素一一对应 —— 前提是别用有损中间格式。
function Convert-PngToBmp
{
    param(
        [Parameter(Mandatory)][string]$PngPath,
        [Parameter(Mandatory)][string]$BmpPath,
        [string]$PythonPath = 'C:\Users\Administrator\.dsh\dsh-runtimes\dsh-primary-runtime\dependencies\python\python.exe'
    )

    if (-not (Test-Path -LiteralPath $PngPath)) { return $false }
    $python = $null
    if (Test-Path -LiteralPath $PythonPath) { $python = $PythonPath }
    else
    {
        $found = Get-Command python -ErrorAction SilentlyContinue
        if ($found) { $python = $found.Source }
    }
    if (-not $python) { return $false }

    $directory = Split-Path -Parent $BmpPath
    if ($directory -and -not (Test-Path -LiteralPath $directory)) { New-Item -ItemType Directory -Path $directory -Force | Out-Null }
    # convert('RGB') 丢掉 alpha：BMP 24 位没有透明通道，带 alpha 的 PNG 直接存会变 32 位。
    $code = 'import sys;from PIL import Image;Image.open(sys.argv[1]).convert("RGB").save(sys.argv[2])'
    try { & $python @('-c', $code, $PngPath, $BmpPath) 2>$null | Out-Null }
    catch { return $false }
    return (Test-Path -LiteralPath $BmpPath)
}
