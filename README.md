# YuaExtract

**面向 Wamsoft / KiriKiri Z Hxv4 / CxdecV2 系列游戏的资源提取与文件名还原工具。**

基于 KrkrExtractForCxdecV2Extra 重制界面，提供 XP3 解包、动态 Hash 收集、离线资源名还原，以及可复用的扩展集。

[下载发行包](https://github.com/YukiRinne/YuaExtract/releases) · [构建记录](https://github.com/YukiRinne/YuaExtract/actions) · [社区扩展集](https://github.com/YukiRinne/Yua-Extensions)

> 仅用于合法副本的学习研究、兼容性分析、个人备份、汉化与资源修复。动态提取会启动游戏并注入模块；部分密钥派生还会执行游戏内嵌代码。请只处理可信的目标文件，**本工具不是沙箱**。

## 功能

- **XP3 动态解包**：批量提取封包资源，保存到 `Extractor_Output`。
- **动态 Hash 收集**：运行游戏并记录明文资源名与哈希的对应关系。
- **离线资源名还原**：结合映射恢复目录、文件名与后缀，复制到独立输出目录，保持源解包目录不变。
- **扩展集制作与复用**：把已有名字与哈希整理为 `rules.int`，同一部游戏再次处理时可离线生成映射，省去重复动态采集。
- **扩展集在线更新**：按用户操作从社区仓库获取、校验和安装档案，不自动后台联网。
- **辅助工具**：静态 Key 提取、XP3 重封包、参数文件管理及命令行调试。

兼容性取决于游戏的引擎版本、加密实现与可用映射；不保证所有游戏都能处理，也不保证固定还原率。

## 下载与运行

从 [Releases](https://github.com/YukiRinne/YuaExtract/releases) 下载完整发行压缩包，解压后运行 `YuaExtract.exe`。不要只复制主程序：各功能依赖相邻的模块目录。

```text
YuaExtract.exe
CxdecExtractordll/
  CxdecExtractor.dll
  CxdecStringDumper.dll
  CxdecKeyStatic.dll
  CxdecRepacker.dll
  CxdecCli.exe
  ...
```

建议使用 Windows 10 / 11。项目沿用 Windows 7 SP1 的目标设置，但本轮验证在 Windows 11 上完成，未重新验证 Windows 7 兼容性。程序与注入模块均为 **x86 / Win32**，不面向 64 位游戏进程。

## 使用流程

### 首次处理：传统动态模式

1. 打开工具，选择目标游戏主程序，在首页选择「传统动态模式」。
2. **步骤 1：解包 XP3。** 提取结果进入工作区的 `Extractor_Output`。
3. **步骤 2：收集 Hash。** 按界面提示运行游戏，访问需要还原的资源以收集名字映射。
4. **步骤 3：还原资源名。** 在独立还原窗口选择映射和输出目录，运行后查看 `RestoreReport.txt`。

默认只输出已还原的哈希名资源，同时保留原本就是明文名的文件。若要保留未知资源，勾选「保留未还原文件名的文件」；工具会在可识别时按文件头补后缀。

还原采用复制方式，不改写源解包目录。输出目录不得与源目录相同或互相嵌套。目标重名会尝试使用 `__dupN` 保留两份数据，无法找到可用名字时明确报错，不覆盖已有文件。

### 再次处理：使用该作扩展集

完成首次采集与还原后，可在「制作扩展集」页签生成档案并安装到本地库。已有同作扩展集时：

1. 安装自己的档案，或在扩展集页签手动在线获取社区档案。
2. 首页选择「该作扩展集提取」，通过归属检查后生成静态 Hash 表。
3. 解包 XP3，再使用生成的映射离线还原。

**扩展集省去的是重复 Hash 采集，不是动态解包本身。** 解包仍可能需要启动游戏并注入模块。

### 扩展集是什么

`rules.int` 保存资源名、盐、归属信息及少量例外映射，常规哈希由名字和盐重新计算；不包含图片、音频或脚本等游戏资源本体。制作时合并已有成果并进行自校验；复用时检查归属后展开映射。

在线下载仅接受 HTTPS，并设有超时与体积上限。工具对照索引检查 SHA-256，验证档案结构、CRC 与相关摘要，再用**同一份已校验字节**安装。校验与摘要不能独立证明每个名字都正确，来源可信度和实际还原结果仍需用户判断。

安装会在替换前检查现有清单，并对普通写入失败尝试回滚；无法回滚时保留备份并报告。**这不等于跨文件的断电或进程崩溃原子性保证。**

## 安全与行为变化

2026-11 的审计修复覆盖路径、解析边界、文件事务、后台任务与原生执行授权：

- **原生执行默认拒绝。** 静态 Key 提取不启动游戏主进程，但派生路径可能加载游戏内嵌 DLL 并调用其中函数。GUI 需要明确确认；CLI 需要 `--allow-native-code`。授权后代码以工具进程权限运行，不具备沙箱隔离。
- **优先复用已有参数。** 封包时可以使用已有匹配参数；指定游戏 EXE 但缺少匹配参数时，不再静默回落到无关的内置配置。
- **路径不越界。** 拒绝目录穿越、危险 Windows 名称和不安全映射；还原、临时目录清理与输出检查防止沿重解析点越出指定范围。
- **读取与解压有边界。** 对 XP3、PE、资源目录及文本流检查长度、偏移与完整读取；相关解压路径限制输出大小并检查流结束状态。
- **已有成果优先保全。** 安装与映射历史使用暂存和替换机制；停止补充扫描时不以不完整结果覆盖历史。
- **取消和关闭不等于完成。** 后台任务持有有效状态直到线程实际退出，过期任务消息被忽略；在线任务取消或关窗后不再安装。
- **替换与注入如实报错。** `steam_api.dll` 替换需先确认并创建唯一备份，失败尝试回滚；远程模块加载失败不再被报告为成功。
- **运行时模块不支持动态卸载。** 注入模块按目标进程生命周期保持加载。

建议普通权限运行；处理真实游戏前另行备份游戏目录及已有工作成果。

## 从源码构建

需要 Visual Studio 2022 或 Build Tools，安装「使用 C++ 的桌面开发」、MSVC v143 与 Windows 10 SDK。多数项目使用 C++20，Repacker / CLI 保留 C++17；zlib 与 Detours 源码已包含在仓库中。

```powershell
git clone https://github.com/YukiRinne/YuaExtract.git
cd YuaExtract

# 如使用完整 Visual Studio，请按实际安装位置调整路径。
$msbuild = 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe'
& $msbuild YuaExtract.sln /t:Rebuild /p:Configuration=Release /p:Platform=x86 /m /v:minimal /nologo
if ($LASTEXITCODE -ne 0) { throw 'Release 构建失败' }
```

构建前请正常关闭正在使用这些模块的工具与测试程序，避免文件占用。产物位于 `Release\YuaExtract.exe` 与 `Release\CxdecExtractordll\`；调试构建将配置改为 `Debug`，输出位于 `Debug\`。

GitHub Actions 在 `main` 更新时构建 Release / Debug 并提供工作流产物。**源码推送不等于发布新版 Releases**；仓库的发行流程由 `v*` 标签触发。

## 代码结构

```text
Common/                   路径安全、原子文件、原生执行授权及通用工具
CxdecExtractorLoader/     主界面、工作区、离线还原、扩展集与在线更新
CxdecExtractor/           XP3 动态提取
CxdecStringDumper/        动态 Hash 收集
CxdecHashRestore/         运行时 Hash 映射恢复
CxdecKeyStatic/           静态密钥派生
CxdecRepacker/            XP3 重封包与参数管理
CxdecCli/                 命令行辅助工具
CxdecPeUnpacker/          PE 解析与相关辅助流程
```

CLI 与 `CxdecRepacker.dll`、`CxdecKeyStatic.dll` 应位于同一目录。查看命令帮助：

```powershell
.\Release\CxdecExtractordll\CxdecCli.exe help
```

## 致谢与许可

感谢 KrkrExtractForCxdecV2Extra 上游项目及各 `Cxdec*` 模块作者与贡献者。相关参考项目包括 [GARbro](https://github.com/crskycode/GARbro)、[GARbro2](https://github.com/UserUnknownFactor/GARbro2) 和 [cxdec-hxv4-static-analysis](https://github.com/hktkqj/cxdec-hxv4-static-analysis)。

许可条款见 [LICENSE](LICENSE)。第三方组件保留各自的许可声明；本项目不包含或授权传播游戏资源。
