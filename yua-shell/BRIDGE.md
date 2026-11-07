# YuaExtract WebView2 bridge

The shell talks to the Win32 loader exclusively through `window.chrome.webview`.
Every UI action is a JSON `HostRequest`. Every native notification is a JSON `HostEvent`.
When the WebView2 object is missing (browser preview), `src/mock/backend.ts` handles the same messages.

## Transport

```
UI  →  chrome.webview.postMessage({ protocolVersion: 1, ...HostRequest })
C++ →  webview.PostWebMessageAsJson(JSON_UTF16(HostEvent))
```

Important: `chrome.webview.postMessage` must receive a plain JS object, **not** `JSON.stringify(...)`. Passing a string causes WebView2 to wrap it as a JSON string literal in `get_WebMessageAsJson()`, which is not a JSON object. `YuaWebViewBridge.inl` also includes a defensive string-unwrapping fallback.

Each `HostRequest` has an `id` string the host may echo in related logs. Events are not request/response paired; the host pushes state (`workspace`, `job`, `log`, …) whenever it changes.

Types live in `src/types.ts` (`HostRequest`, `HostEvent`).

## Requests the C++ side must handle

| type | When the UI sends it | Expected host work | Events to push |
|---|---|---|---|
| `ready` | First paint | Identify build, library, online base | `hello`, `workspace`, `library` |
| `browseExe` | 「选择」 | Native open-file dialog (`*.exe`) | `picked {kind:exe}` + `workspace` |
| `setTarget` | Drag-drop or path typed | Validate PE, set workspace = game dir | `workspace`, `log` |
| `setMode` | Mode pills | Persist mutually-exclusive mode | `workspace` |
| `selectExtension` | Publisher / title lists | Remember current archive for mode 1 | `workspace` |
| `runStep` | 步骤 1/2/3 | Mode-dependent (see below) | `job`, `overlayHint`, `log`, `workspace` |
| `checkStatus` | 检查状态 | Probe output dirs / mapping counts | `workspace`, `toast` |
| `openWorkspace` / `openRestoreDir` / `openRestoreReport` | Aux buttons | `ShellExecute` the path | `toast` on failure |
| `cleanDirs` | 清理目录 | Delete Extractor / Hash / Restore / StaticHash outputs (refuse if restore window is running) | `workspace`, `toast` |
| `cancelJob` | 取消 | Cooperative cancel of generate / static-table / dry-run / online | `job {cancelling→idle}` |
| `generateSet` | 生成当前游戏扩展集 | Merge mapping logs → `rules.int` | `job`, `log` |
| `saveMeta` | 编辑信息 OK | Store publisher/title/contributor/version/date/comment | `toast` |
| `dryRunSet` | 测试当前扩展集 | Read-only restore projection | `job` |
| `installSet` | 安装到扩展集库 | Copy archive into `ExtensionLibrary\<会社>\<作品>\` | `library`, `toast` |
| `openDraftDir` / `openLibraryRoot` / `openSelectedSet` | Directory buttons | Explorer | `toast` on miss |
| `pickSourceDir` / `pickTestDir` | Custom dirs | Folder picker | `picked`, `workspace` |
| `refreshLibrary` | 刷新列表 | Rescan library + manifest | `library` |
| `selfCheck` | 运行环境自检 | Seven read-only checks | `selfCheck` |
| `readNotes` | 扩展集留言 | Decode META comment (do not decode archive on every paint) | `notes` |
| `fetchIndex` | 拉取索引 | Public HTTPS GET `index.txt`, no redirects, rejects localhost/loopback/private/reserved IPs before connecting | `remoteIndex` or error |
| `downloadRemote` | 下载并安装选中 | Download + 9-point trust chain; confirm overwrite if local-richer | `job`, `library`, `toast` |
| `startRestore` | 还原窗口「开始」 | Offline restore via hidden `YuaRestoreWindow` engine, N threads, optional keep/infer | `restoreTasks`, `job`, `workspace` |
| `stopRestore` | Close / abort | Cooperative stop, keep copied files | `restoreTasks`, `job` |
| `startCollectDemo` / `stopGame` / `clearCollectFiles` / `openCollectDir` | Collect console | Game process control + mapping logs | `collect` |
| `enqueueXp3` / `startBatch` / `stopBatch` | Batch extractor window | Queue `*.xp3`, extract into `Extractor_Output` | `batch` |
| `openLegacy` | 原版功能 | Open upstream loader dialog (`extractor`/`dumper`/`hashRestore`/`keyStatic`/`repack`) | `overlayHint {legacy}` |
| `windowControl` | Titlebar drag / resize / min / max / close | Native Win32 window state & `WM_SYSCOMMAND` move/resize | — |
| `failDemo` | Screenshot / QA only | Optional; host may ignore | — |

### `runStep` semantics

**Mode `extension` (该作扩展集提取)**

1. Load selected `rules.int`, expand static tables into `StaticHash_Output` (no game launch). May prompt: extract static key → retry, or force unverified use.
2. Launch game + inject `CxdecExtractorUI.dll` (same as classic step 1). Host should open the batch-extractor overlay via `overlayHint: batch`.
3. Open restore window (`overlayHint: restore`) using `StaticHash_Output` as the map directory.

**Mode `studio` (该会社合集撞新作)** — reserved for phase 2. UI already disables the steps; host should no-op with an explanatory toast.

**Mode `classic` (传统动态模式)**

1. Anti-tamper precheck (`EnsureTargetReadyForLaunch`); auto-switch to `*_crack.exe` when produced; inject extractor UI.
2. Launch + inject `CxdecStringDumper.dll` with `CXDEC_HASH_COLLECT_ONLY=1`; `overlayHint: collect`.
3. Offline restore window using `StringHashDumper_Output` (+ every `*.lst`).

## Events the host should push

| type | Payload | Notes |
|---|---|---|
| `hello` | `HostInfo` | Product, paths, build fingerprint, online base URL |
| `workspace` | `WorkspaceSnapshot` | Target, dirs, readiness flags, attribution, restore rate |
| `library` | `ExtensionEntry[]` | Installed archives (metadata only — no archive decode on timer) |
| `remoteIndex` | items + base + optional error | Diff classes: `new` / `same` / `update` / `local-richer` |
| `job` | kind, status, 0–1 progress, text, error | Drives the shared progress rail |
| `log` | level, source, message, ts | Append-only; UI virtualizes |
| `toast` | level, title, body | Transient |
| `restoreTasks` | tasks + `RestoreStats` + optional mining line | Push from worker threads, not the UI thread |
| `collect` | pid, uptime, two counters, two log tails | Tails may be last ~1000 lines |
| `batch` | extract tasks + stats | Same “terminal state written by worker” rule as restore |
| `selfCheck` | 7 items | Read-only; never mutate disk except a temp probe file that is deleted |
| `notes` | text | Comment / contributor block |
| `picked` | kind + path | Dialog results |
| `overlayHint` | overlay id + open | Host asks the shell to show restore/collect/batch/legacy |
| `ui` | reason + sourceDir/testDir/draftMeta/lastAction/dryRunNote + notes | Syncs MakeScreen overrides, draft metadata, dry-run summary, and note fields |

## Security notes (must stay on the C++ side)

- Online updates: Public HTTPS only (`OnlineHostPolicy` rejects `localhost`, `127.0.0.0/8`, `::1`, link-local, private, and reserved addresses before connecting), no redirects, triple timeouts, size cap, sha256 must match the index, refuse on any container/CRC/digest failure with zero local writes.
- Never auto-load a downloaded archive; installing ≠ using. Attribution still happens on homepage mode-1 step 1.
- The archive stores names, not hash layers; the tool cannot prove names are “correct” — restore rate is the ground truth.

## Hosting the shell

Ship `dist/*` in `<LoaderDir>\shell\` next to `YuaExtract.exe`. `YuaWebViewBridge.inl` maps virtual host `https://yua.local/` to `<LoaderDir>\shell\` via `SetVirtualHostNameToFolderMapping` and navigates to `https://yua.local/index.html`. Production builds (`vite build`) tree-shake `src/mock/backend.ts` out of `shell/assets/`; if `window.chrome.webview` is absent in a production bundle, the UI fails closed.
