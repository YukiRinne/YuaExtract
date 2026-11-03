# security-extension 回归（扩展集内核 / 在线更新安全与事务）

本目录只放**应入库**的 cpp / ps1 / vcxproj / md。编译产物、stdout、check JSON 不在这里：

| 产物 | 路径 |
|---|---|
| exe / obj | `_verify\restore-harness\_build\security-extension\` |
| stdout log + `checks-security-extension.json` | `_verify\restore-harness\Run\security-extension\` |

## 跑法

```powershell
pwsh -File "_verify\restore-harness\security-extension\run-security-extension.ps1"
# 只跑已构建的 exe：
pwsh -File "_verify\restore-harness\security-extension\run-security-extension.ps1" -NoBuild
```

构建只走独立 `security-extension-tests.vcxproj`（VS2022 BuildTools，v143，Win32，C++20，
`/utf-8`），输出目录写死在 vcxproj 的 `OutDir/IntDir`，**不碰解决方案默认 Release**。

## 测什么（真实执行生产代码）

测试 exe 直接编入 `ExtensionSet.cpp` / `OnlineUpdate.cpp` / `Blake2s.cpp` / `siphash.cpp` /
vendored zlib，所有用例调用真实公开入口；不复制算法、不 grep 源码。

- 编解码往返：中文会社/作品、空串或非空盐、名字层展开。
- 畸形输入：2 MiB 无效 stored（不得大分配）、zlib 垃圾/截断、CRC 篡改。
- 上限：64 MiB + 1 的压缩炸弹拒绝；恰好 64 MiB 的合法压缩 payload 接受。
- 安装事务：首次 / 幂等 / 更新（清单始终一行）；坏清单（解析失败、仅"坏行被跳过"、
  `manifest.txt` 是同名目录）全部拒绝且磁盘零变化。
- commit 失败回滚：用 `FILE_SHARE_READ` 锁住 `manifest.txt`（不给 DELETE/WRITE），
  使 `ReplaceFileW` 在 rules/info 已提交后失败；核对旧 rules/info/manifest 的
  **字节与最后写入时间**逐项保持，且无 `.yua-tmp` / `.yua-bak` 残留。
- 故障 seam（仅测试构建宏启用）：模拟 `ReplaceFileW` 半途（旧目标已进 backup、目标缺失）、
  "替换成功后抛 bad_alloc"、以及"完整 commit 后 Finalize 抛异常"；同时覆盖
  `InstallBytesToLibrary` 外层与 `WriteFileAtomic` 单文件 helper 两种消费方式，
  分别验证失败项自身回滚、
  rollback 失败时 backup 保留、已成功写入只返回 true 并带清理提示（绝不回滚已删 backup）。
- `atomicfile-header-selfcheck.cpp` 只 include `Common/AtomicFile.h`，防止头文件依赖其它
  TU 的包含顺序。
- Discard 在祖先被换成 junction 时保留 temp 路径，不顺着链接删外部同名文件。
- 槽位口径：`Game:A` 与 `GameA` 被 `LibrarySlotKey` 判成同一物理槽位（Update / LocalNewer，不 New）。
- 并发：两个 writer 同时安装不同槽位，清单不丢行。
- 字节绑定：`VerifyArchiveBytes(A)` 之后即使磁盘上的 path 被换成 B，
  `InstallBytesToLibrary(A)` 装入的仍是 A。
- staging：owned session 正常清理；父级被换成 junction 时拒绝穿越并保留登记，
  复原路径后仍可清理；下载在 owned session 建好后失败会自动清理该 session；
  未登记的 explicit 目录拒绝清理；空串 no-op。

## 边界

- 不发网络请求；在线下载路径只验证到 staging / 校验 / 安装的本地部分。
- 不改系统 ACL / 证书库；junction 只在测试自造的临时夹具里创建。
- 不承诺进程崩溃 / 掉电时三文件跨文件 crash-safe；这里验证的是普通 I/O 失败可回滚。
