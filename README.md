# YuaExtract

**面向 Wamsoft / KiriKiri Z Hxv4 (2021.11+) / CxdecV2 系列加密游戏的动态分析与资源整理工具**

解包 · Hash 收集 · 资源名还原 · 扩展集（免跑游戏重复还原）

[![build](https://img.shields.io/badge/platform-Windows%207%20SP1%2B-blue)](#环境)
[![arch](https://img.shields.io/badge/arch-x86%20(Win32)-informational)](#环境)
[![license](https://img.shields.io/badge/license-see%20LICENSE-green)](LICENSE)

---

## 这是什么

YuaExtract 是 **KrkrExtractForCxdecV2Extra**（KrkrExtractV2 for CxdecV2）的重制版：界面全部重做，并在上游功能之上加了一条
**「解包 → 动态 Hash 收集 → 离线还原资源名」** 的三步流程。最新版本又加上了**扩展集**：
把一部游戏已经整理好的「文件名 ↔ 哈希」成果留档，**同一部游戏以后重解包时不再需要跑游戏做动态采集**。

```
传统三步（首次）：  ① 解包 XP3  →  ② 跑游戏收集 Hash  →  ③ 还原资源名
扩展集（以后）：    ① 从扩展集生成静态 Hash 表  →  ② 解包 XP3  →  ③ 还原资源名
                                              ↑ 不用再跑游戏
```

## 主要功能

- **XP3 动态解包** — 批量解包游戏封包，产出哈希名的 `Extractor_Output`
- **动态 Hash 收集** — 启动游戏并注入模块，实时记录「明文名 ↔ Hash」对应关系
- **资源名还原** — 按收集到的映射离线还原目录名、文件名、后缀，实测成功率 90% 以上
- **静态 Key 提取** — 不启动游戏、不注入、不依赖 Frida/Python，直接从游戏 EXE 派生密钥材料
- **扩展集**（新增）— 「名字 ↔ 哈希」知识档案（`rules.int`）：留档、干跑测试、装库、
  **从社区仓库在线更新**，同一部游戏重解包免跑采集

## 扩展集（本版本的旗舰功能）

一部游戏的扩展集 = 这部游戏「哪些哈希对应哪些名字」的知识档案（`rules.int`，只含名字与哈希，
**不含任何游戏资源**）。

- **制作**：「制作扩展集」页签一键生成 —— 合并采集/还原成果 → 归一化 → 生成期自校验（每条
  「名字+盐→哈希」必须算得回去）→ 压缩留档（一部游戏约 200 KB）
- **复用**：以后重解包同一部游戏，首页「该作扩展集提取」直接离线展开静态 Hash 表，**免跑游戏**
- **在线更新**：从社区扩展集仓库 [YukiRinne/Yua-Extensions](https://github.com/YukiRinne/Yua-Extensions)
  浏览、下载、安装别人整理好的扩展集。安全设计：
  - 仅 HTTPS、不跟随重定向、三段超时、体积上限
  - 下载后 **sha256 必须与索引一致**，容器结构 / CRC / 内嵌摘要逐层校验，任一不过即拒且本地零改动
  - **绝不自动加载、绝不后台联网** —— 入库只是入库，用不用仍由归属校验把关
- **如实说明**：档案里不存哈希层，工具**验不了"名字对不对"**；信任根是仓库索引，
  名字的最终把关是**还原率这个事实**

## 快速开始

1. 从 [Releases](../../releases) 下载 `YuaExtract.exe`（或自行构建，见下）
2. 打开工具，选择目标游戏的主程序 exe
3. 首次：首页选「传统动态模式」，按提示跑完三步 → 在「制作扩展集」页签把成果**生成扩展集并安装到库**
4. 以后重解包同一部游戏：首页选「该作扩展集提取」→ 扩展集页签「在线更新」拿到档案 → 三步跑完

> 详细口径与设计文档见 [`docs/`](docs/)：[扩展集设计](docs/扩展集设计.md) ·
> [UI 升级说明](docs/YuaExtract-UI升级说明.md) · [交接说明](docs/交接说明.md)

## 构建

```powershell
# 前置：Visual Studio 2022（MSVC v143）+ Windows 10 SDK 10.0.19041.0+
git clone https://github.com/YukiRinne/YuaExtract.git
cd YuaExtract
Get-Process YuaExtract -ErrorAction SilentlyContinue | Stop-Process -Force   # 防止文件占用
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe' `
  YuaExtract.sln /p:Configuration=Release /p:Platform=x86 /m /v:minimal /nologo
```

产物在 `Release\YuaExtract.exe`（附 `Release\CxdecExtractordll\` 下的各功能模块）。

| 环境 | 要求 |
|---|---|
| 系统 | Windows 7 SP1 x64 及以上（推荐 Win10 / Win11） |
| 工具链 | MSVC 2022 (v143)，C++20，`/utf-8` |
| 平台 | x86 / Win32 |
| 第三方依赖 | **无**（BLAKE2s 手写实现；zlib 为仓库内置的 vendored 副本） |

## 仓库结构

```
├─ CxdecExtractorLoader/    主程序 + 扩展集内核（ExtensionSet.*）+ 在线更新层（OnlineUpdate.*）
├─ CxdecExtractor/          XP3 动态解包核心
├─ CxdecStringDumper/       动态 Hash 收集模块（注入游戏）
├─ CxdecHashRestore/        Hook 撞库恢复 Hash 映射
├─ CxdecKeyStatic/          静态 Key 提取
├─ CxdecRepacker/           目录重封包 XP3（含 vendored zlib）
├─ docs/                    设计文档与各轮交接说明
└─ _verify/restore-harness/ 验证脚手架（合成工作区 + UI 驱动 + 断言 + 假绿护栏）
```

（其余上游模块与完整模块说明见 [docs/YuaExtract-UI升级说明.md](docs/YuaExtract-UI升级说明.md) §4。）

## 相关仓库

- **[YukiRinne/Yua-Extensions](https://github.com/YukiRinne/Yua-Extensions)** — 社区扩展集仓库。
  工具内置基址指向这里；投稿只需 PR 一份 `rules.int`，CI 自动校验（详见其 README）。

## 免责声明

本项目仅供**学习研究、兼容性分析、个人合法备份、汉化与资源修复**等用途。
请在你合法拥有的游戏副本上使用；本项目及其产物**不包含、也不应用于传播**任何游戏资源本体。

## 致谢

- **KrkrExtractForCxdecV2Extra**（KrkrExtractV2 for CxdecV2）—— 上游项目，本仓库在其基础上重做界面并扩展功能
- 各 `Cxdec*` 模块的原作者与贡献者
- 参考项目：[GARbro](https://github.com/crskycode/GARbro)、[GARbro2](https://github.com/UserUnknownFactor/GARbro2)、
  [cxdec-hxv4-static-analysis](https://github.com/hktkqj/cxdec-hxv4-static-analysis)
- 扩展集仓库的第一份档案由 [Rinne](https://github.com/YukiRinne) 整理

## License

见 [LICENSE](LICENSE)。
