# security-core 第一批安全回归

针对第一批（路径/读取边界、清理与解压、PE 资源）的 headless 生产函数回归。
全部源文件只在本目录；所有 exe / obj / 临时夹具 / 日志落到已忽略目录：

```
_verify/restore-harness/_build/security-core/
  bin/     可执行程序
  obj/     编译中间件
  logs/    每个用例的 build/stdout 日志
  scratch/ 每个用例独占的一次性夹具（结束时保留，便于复查）
```

## 运行

```powershell
pwsh -NoProfile -File '_verify\restore-harness\safety-tests\run-security-core.ps1'
```

- 任一用例失败，脚本返回非 0，并保留该用例全部 stdout（`logs\*.stdout.log`）。
- `-NoBuild` 只重跑已有 exe。
- 构建用 VS2022 BuildTools v143 / C++20 / `/utf-8` / `/MT`，不运行 GUI、不联网、不安装证书。

## 用例与覆盖

| 用例 | checks | 覆盖 |
|---|---|---|
| `security-core-path-test` | 44 | PathSafety 正常中文/空根域/多层；`../`、绝对路径、ADS、设备名、尾点尾空白拒绝；组件边界；同源/嵌套；junction 解析拒绝；Probe/Remove 不跟随链接、循环不 hang、根 junction 只摘链接、外部 sentinel 不变 |
| `security-core-restorer-test` | 34 | 白盒调用真实 `RestoreSinglePackage`：完整映射层级保留、空根域、中文；恶意映射载入拒绝并计数；内核二次防线；同源/嵌套拒绝；冲突不覆盖、`__dup1`；同内容重跑记录 delivery；两遍式按 SOURCE→实际目标清理且不通配删用户文件；MinedNames 历史优先 + 空结果不丢旧 + 原子替换失败保留旧字节 + 4 线程并发写不互相覆盖 |
| `security-core-xp3-test` | 13 | 真实 `parse_xp3/rebuild_archive`：畸形 64 位 index/comp/chunk size、info name 拒绝；多段归档 rebuild 与等价单段逐字节一致；真实 `Crypto::zlib_decompress` 有效流/截断/坏 Adler/FDICT 全通过 |
| `security-core-pe-test` | 25 | PeReader：SizeOfOptionalHeader/NumberOfSections/节 raw 范围、RVA→真实文件边界；PeResource：合法三层 root-relative + 非中性语言读取、目录/名字/数据项/大小越界拒绝；PackerDetect/UnpackCore：TLS AddressOfCallBacks/callback 按 VA−ImageBase，畸形 VA 与巨大 PayloadSize 失败不崩 |
| `security-core-stream-test` | 11 | 真实 `ExtractCore::TryDecryptText`：1 字节/缺 mode/非法 mode、奇数字节、声明长度大于真实可读、Seek 失败、mode0/1 正常变换、mode2 头截断 |

## 第一批未覆盖 / 后续批次

- `ExtractPackageTo` 的封包文件名拒绝（`..`/`.`）已用同一 `PathSafety` 规则实现，但没有构造宿主 TVP 索引；由第二批 UI/CLI 验收补。
- `YuaExtractWorkspace::RemoveDirectoryTree` 的取消参数没有专门用例（接口可用，取消路径与 `DeleteRecursive` 同一检查点）。
- 手写 inflate 已整体替换为 vendored zlib 流式实现（64 MiB cap、必须 `Z_STREAM_END`）。超出 cap 的炸弹流没有构造（会占用 ~64 MiB 临时内存），留作后续压力用例。
- Loader 全工程构建当前被其它并行 scope 的 `ExtensionSet.cpp` 临时编译错误阻塞；本目录的 restorer/workspace 是用真实源文件单独编译链接验证的。
- `CxdecRepacker` Debug 工程本身 `/ZI` 与 `/guard:cf` 冲突（其它 scope 的工程设置），本批用 Release 构建 + 测试宿主验证 `xp3.cpp/pack.cpp`。
