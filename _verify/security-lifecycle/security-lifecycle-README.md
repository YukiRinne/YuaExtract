# security-lifecycle 无头测试

本目录只放**源测试与脚本**；编译产物与日志全部落在已忽略的
`_verify/restore-harness/_build/security-lifecycle/` 下。

## 测试清单

| 源文件 | 编入的生产实现 | 覆盖 |
| --- | --- | --- |
| `security-lifecycle-job-lifecycle-test.cpp` | `YuaExtractMainWindow.cpp` | 任务 generation、close 超时保留句柄、迟到 Done、busy 门禁；online 下载在 cancelled / ClosePending / Closing / JobDoneUi 下即使 `job.Ok=true` 也不入库；覆盖确认期间新到达的 ClosePending/Cancel 在最终复核仍阻止入库；NOTE/OPEN_LIB/ADVANCED/AUX 等 sensitive 命令一致 |
| `security-lifecycle-wait-test.cpp` | `YuaExtractHost.h` 的 `Detail::WaitForHandleResponsive` | 有界等待超时、已 signal 成功、WM_CLOSE / WM_QUIT 只探测不消费（保留给原消息循环）、自建 sleep 子进程超时/退出 |
| `security-lifecycle-steamswap-test.cpp` | `CxdecExtractorLoader/SteamApiSwap.h` | 唯一 `.yua-bak` / `.yua-stage`（CREATE_NEW）、显式/析构回滚、替换源缺失不动原文件、固定 `api.yua-new` 哨兵不被删改 |
| `security-lifecycle-restore-stop-test.cpp` | `YuaRestoreWindow.cpp` | `PopNextTask` FIFO、StopEvent 后不再取任务并清空队列、无 HWND 时进度/终态写入只改记录且自释放载荷 |

所有测试都不创建真实游戏进程、不注入模块、不写用户目录；Steam 事务只在自造临时目录里跑。

## 构建与运行

```powershell
pwsh -NoProfile -File _verify/security-lifecycle/security-lifecycle-run-tests.ps1
```

脚本会：

1. 调用 VS2022 `vcvars32.bat`，把每个测试编到
   `_verify/restore-harness/_build/security-lifecycle/<name>/{obj,bin}`；
2. 删除旧 exe 后再构建（freshness：exe 必须是本次产物，且 build log 存在）；
3. 运行并检查 `RESULT PASS (0 failures)` 与约定的 CHECK 名称；
4. 任一项失败以非 0 退出。

期望输出：四个 `PASS <name>`，最后 `[security-lifecycle] ALL PASS`。

## 定向 Loader 构建

Debug / Release 都用独立 OutDir/IntDir，不写默认 `Debug\` / `Release\`：

```text
msbuild CxdecExtractorLoader\CxdecExtractorLoader.vcxproj /t:Rebuild
  /p:Configuration=Debug|Release /p:Platform=Win32
  /p:SolutionDir=<repo>\
  /p:OutDir=<repo>\_verify\restore-harness\_build\security-lifecycle\loader-dbg|loader-rel\bin\
  /p:IntDir=<repo>\_verify\restore-harness\_build\security-lifecycle\loader-dbg|loader-rel\obj\
```

日志：`loader-dbg-build.log` / `loader-rel-build.log`。
完整解决方案的默认 Release/Debug 构建仍由主会话统一执行。

## 未动态验证的界面项

- 在线更新弹窗的关闭轮询、staging 清理的**真实 GUI 时序**（门禁谓词已 headless 覆盖）；
- 反篡改预检 60 秒超时、Steam API 替换回滚的真实**游戏目录**文件；
- 注入远程 `LoadLibraryW` 返回 0 / `ResumeThread` 失败的真实子进程；
- 所有 GUI 二次确认、拖放/失焦重入与视觉布局。
