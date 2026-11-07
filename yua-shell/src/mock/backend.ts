import { emitHostEvent } from "../bridge";
import type {
  ExtensionEntry,
  ExtractTask,
  HostInfo,
  HostRequest,
  RemoteExtension,
  RestoreOptions,
  RestoreTask,
  SelfCheckItem,
  WorkspaceSnapshot,
} from "../types";

const info: HostInfo = {
  product: "YuaExtract",
  version: "1.4.0-shell",
  loaderDir: "D:\\Tools\\YuaExtract",
  logDir: "D:\\Tools\\YuaExtract\\Log",
  moduleDir: "D:\\Tools\\YuaExtract\\CxdecExtractordll",
  libraryRoot: "D:\\Tools\\YuaExtract\\ExtensionLibrary",
  fingerprint: {
    fileName: "YuaExtract.exe",
    size: 1843200,
    modified: "2026-10-15 21:06:11",
    sha256: "a7c31e90b4f6d218c9e01a55d0b8e447c1f23aa09e11b77c",
  },
  onlineBase: "https://raw.githubusercontent.com/YukiRinne/Yua-Extensions/main/",
};

const seedLibrary: ExtensionEntry[] = [
  {
    id: "wamsoft/starlight-corridor",
    publisher: "Wamsoft",
    title: "星夜回廊",
    contributor: "Rinne",
    version: "1.2.0",
    date: "2026-09-18",
    comment: "目录覆盖较全；voice 包仍有少量 64 位哈希未还原。请用还原率而不是档案本身判断名字对不对。",
    dirCount: 1840,
    fileCount: 61220,
    exceptions: 0,
    archiveSha: "9b1c0e77a1d4f290",
    targetSha: "c4e19a80bb21de03",
    siblingSha: "aa9012ccde44b170",
    saltSha: "71ff0a29ce11b004",
    generatedAt: "2026-09-18 13:22:08",
    installedAt: "2026-10-02 09:14:33",
    source: "社区仓库 Yua-Extensions",
    path: "D:\\Tools\\YuaExtract\\ExtensionLibrary\\Wamsoft\\星夜回廊\\rules.int",
  },
  {
    id: "wamsoft/white-iris",
    publisher: "Wamsoft",
    title: "白虹之庭",
    contributor: "meika",
    version: "1.0.1",
    date: "2026-08-02",
    comment: "本作 PSB 明文较多，推理补充还原收益明显。",
    dirCount: 960,
    fileCount: 33410,
    exceptions: 0,
    archiveSha: "12ab99e0c3ddef18",
    targetSha: "88c0a1b2d3e4f506",
    siblingSha: "",
    saltSha: "0c19aa77b201ee54",
    generatedAt: "2026-08-02 19:01:44",
    installedAt: "2026-08-03 10:22:01",
    source: "本机生成",
    path: "D:\\Tools\\YuaExtract\\ExtensionLibrary\\Wamsoft\\白虹之庭\\rules.int",
  },
  {
    id: "morningstar///cyan-feather",
    publisher: "晨星工作室",
    title: "青羽日记",
    contributor: "anon",
    version: "0.9.0",
    date: "2026-07-11",
    comment: "",
    dirCount: 412,
    fileCount: 15880,
    exceptions: 2,
    archiveSha: "55ee10ab99cc21df",
    targetSha: "de44aa0199bbcc12",
    siblingSha: "",
    saltSha: "abc11900ffeedc01",
    generatedAt: "2026-07-11 08:40:12",
    installedAt: "2026-07-12 16:05:09",
    source: "本机生成",
    path: "D:\\Tools\\YuaExtract\\ExtensionLibrary\\晨星工作室\\青羽日记\\rules.int",
  },
];

const packs = ["data.xp3", "patch.xp3", "voice.xp3", "bgimage.xp3", "system.xp3", "scenario.xp3"];

function now() {
  return Date.now();
}

function log(level: "debug" | "info" | "warn" | "error", source: string, message: string) {
  emitHostEvent({ type: "log", entry: { ts: now(), level, source, message } });
}

function note(title: string, body = "") {
  log("info", "YuaExtract", body ? `${title}：${body}` : title);
}
function toast(level: "debug" | "info" | "warn" | "error", title: string, body: string) {
  log(level === "debug" ? "info" : level, "YuaExtract", body ? `${title}：${body}` : title);
  emitHostEvent({ type: "toast", level, title, body });
}

function job(kind: Parameters<typeof emitHostEvent>[0] extends never ? never : any, status: any, progress: number, text: string, error?: string) {
  emitHostEvent({ type: "job", kind, status, progress, text, error });
}

export function attachMockBackend() {
  window.__YUA_MOCK__ = true;
  let library = seedLibrary.map((e) => ({ ...e }));
  let workspace: WorkspaceSnapshot = emptyWorkspace();
  let mode: "extension" | "studio" | "classic" = "classic";
  let selected = { publisher: "Wamsoft", title: "星夜回廊" };
  let sourceOverride = "";
  let testOverride = "";
  let meta: Partial<ExtensionEntry> = {
    publisher: "Wamsoft",
    title: "星夜回廊",
    contributor: "meika",
    version: "1.3.0",
    date: "2026-11-06",
    comment: "",
  };
  let cancelFlag = false;
  let timers: number[] = [];
  let collectTimer = 0;
  let restoreTimer = 0;
  let batchTimer = 0;
  let collect = {
    running: false,
    uptime: 0,
    pid: 0,
    dirHit: 0,
    dirTotal: 1840,
    fileHit: 0,
    fileTotal: 60133,
    lines: { dir: [] as string[], file: [] as string[] },
  };
  let restoreTasks: RestoreTask[] = [];
  let restoreStats = zeroRestore();
  let batchTasks: ExtractTask[] = [];

  const schedule = (fn: () => void, ms: number) => {
    const id = window.setTimeout(fn, ms);
    timers.push(id);
    return id;
  };

  const pushWs = () => emitHostEvent({ type: "workspace", workspace: { ...workspace } });
  const pushLib = () => emitHostEvent({ type: "library", entries: library.map((e) => ({ ...e })) });

  const applyExe = (path: string) => {
    const name = path.split(/[/\\]/).pop() ?? path;
    const dir = path.replace(/[/\\][^/\\]+$/, "");
    workspace = {
      ...workspace,
      exe: path,
      title: name.replace(/\.exe$/i, ""),
      publisher: selected.publisher,
      workspace: dir,
      extractDir: `${dir}\\Extractor_Output`,
      hashDir: `${dir}\\StringHashDumper_Output`,
      restoreDir: `${dir}\\Restored_Extractor_Output`,
      reportPath: `${dir}\\Restored_Extractor_Output\\RestoreReport.txt`,
      staticHashDir: `${dir}\\StaticHash_Output`,
    };
    log("info", "YuaExtract", `目标游戏：${path}`);
    pushWs();
  };

  window.addEventListener("yua-host-request", ((ev: CustomEvent<HostRequest>) => {
    handle(ev.detail);
  }) as EventListener);

  function handle(req: HostRequest) {
    switch (req.type) {
      case "ready":
        emitHostEvent({ type: "hello", info });
        pushWs();
        pushLib();
        log("info", "YuaExtract", "WebView 壳已就绪（mock 后端）。");
        break;
      case "browseExe":
        applyExe("D:\\Games\\星夜回廊\\星夜回廊.exe");
        emitHostEvent({ type: "picked", kind: "exe", path: workspace.exe! });
        note("已选择目标", workspace.exe!);
        break;
      case "setTarget":
        applyExe(req.path);
        note("已选择目标", req.path);
        break;
      case "setMode":
        mode = req.mode;
        log("info", "YuaExtract", `模式切换为 ${req.mode}`);
        pushWs();
        break;
      case "selectExtension":
        selected = { publisher: req.publisher ?? "", title: req.title ?? "" };
        const hit = library.find((e) => e.publisher === req.publisher && e.title === req.title);
        if (hit) {
          workspace.publisher = hit.publisher;
          workspace.title = workspace.title || hit.title;
        }
        pushWs();
        break;
      case "runStep":
        runStep(req.step);
        break;
      case "checkStatus":
        note("检查状态完成", workspace.workspace ? `工作区 ${workspace.workspace}` : "尚未选择目标游戏。");
        pushWs();
        break;
      case "openWorkspace":
      case "openRestoreDir":
      case "openRestoreReport":
      case "openDraftDir":
      case "openLibraryRoot":
      case "openSelectedSet":
      case "openCollectDir":
        note("已在资源管理器中打开", describeOpen(req.type));
        log("info", "YuaExtract", `打开：${describeOpen(req.type)}`);
        break;
      case "cleanDirs":
        if (restoreTimer) {
          toast("warn", "无法清理", "还原窗口正在跑任务，先等它结束或关掉那个窗口，再清理目录。");
          break;
        }
        workspace.extractReady = false;
        workspace.hashReady = false;
        workspace.hashDirEntries = 0;
        workspace.hashFileEntries = 0;
        workspace.restoreReady = false;
        workspace.staticReady = false;
        workspace.restoreRate = null;
        workspace.restoredCount = 0;
        note("已清理产物目录", "Extractor / Hash / Restore / StaticHash 输出已清空（mock）。");
        pushWs();
        break;
      case "cancelJob":
        cancelFlag = true;
        job("idle", "cancelling", 0, "正在取消……（内核按条检查取消标志）");
        schedule(() => {
          job("idle", "idle", 0, "任务已取消。已经写出的文件保留。");
          toast("warn", "已取消", "后台任务被取消。");
        }, 400);
        break;
      case "generateSet":
        simulateProgress("generate-set", "正在采集并生成扩展集档案……", 8, () => {
          toast("info", "已生成扩展集档案", "rules.int 已写出，自校验例外 0 条。");
          log("info", "YuaExtract", "生成扩展集：目录 1,840 / 文件 61,220，例外 0");
        });
        break;
      case "saveMeta":
        meta = { ...meta, ...req.meta };
        note("扩展集信息已更新", "重新执行「生成当前游戏扩展集」后新信息才会写进档案。");
        break;
      case "dryRunSet":
        simulateProgress("dry-run", "正在只读干跑（不复制、不重命名任何文件）……", 7, () => {
          toast("info", "干跑测试完成", "预计还原率 91.46%（仅供参考）。");
        });
        break;
      case "installSet": {
        const entry: ExtensionEntry = {
          id: `${(meta.publisher ?? "demo").toLowerCase()}/${meta.title ?? "untitled"}`,
          publisher: meta.publisher || "未填会社",
          title: meta.title || "未填作品",
          contributor: meta.contributor || "meika",
          version: meta.version || "1.0.0",
          date: meta.date || "2026-11-06",
          comment: meta.comment || "",
          dirCount: 1840,
          fileCount: 61220,
          exceptions: 0,
          archiveSha: "f00c1a77beef9012",
          targetSha: "c4e19a80bb21de03",
          siblingSha: "",
          saltSha: "71ff0a29ce11b004",
          generatedAt: "2026-11-06 23:40:00",
          installedAt: "2026-11-06 23:40:02",
          source: "本机生成",
          path: `${info.libraryRoot}\\${meta.publisher}\\${meta.title}\\rules.int`,
        };
        library = [entry, ...library.filter((e) => e.id !== entry.id)];
        pushLib();
        toast("info", "已安装到扩展集库", entry.path);
        break;
      }
      case "pickSourceDir":
        sourceOverride = "D:\\Games\\星夜回廊\\StringHashDumper_Output";
        emitHostEvent({ type: "picked", kind: "source", path: sourceOverride });
        break;
      case "pickTestDir":
        testOverride = "D:\\Games\\星夜回廊\\Restored_Extractor_Output";
        emitHostEvent({ type: "picked", kind: "test", path: testOverride });
        break;
      case "refreshLibrary":
        pushLib();
        note("已刷新列表", `会社 ${new Set(library.map((e) => e.publisher)).size} 个 / 扩展集 ${library.length} 份`);
        break;
      case "selfCheck":
        emitHostEvent({ type: "selfCheck", items: mockSelfCheck() });
        note("运行环境自检", "7 项检查已完成（只读，未留下改动）。");
        break;
      case "readNotes": {
        const item = library.find((e) => e.publisher === selected.publisher && e.title === selected.title);
        const text = item
          ? `贡献者：${item.contributor}\n版本：${item.version}\n日期：${item.date}\n生成工具：YuaExtract 1.4.0\n\n${item.comment || "（这份档案没有写留言）"}`
          : "先在左侧选择会社与作品，然后点「扩展集留言」。";
        emitHostEvent({ type: "notes", text });
        break;
      }
      case "fetchIndex":
        simulateProgress("online-index", "正在拉取远端索引……", 6, () => {
          emitHostEvent({ type: "remoteIndex", items: mockRemote(library), base: req.endpoint || info.onlineBase });
          log("info", "Online", "索引拉取成功：共 4 条");
        });
        break;
      case "downloadRemote": {
        const items = mockRemote(library);
        const item = items.find((e) => e.id === req.entryId);
        if (!item) {
          toast("error", "下载失败", "先在上面的列表里选一条远端扩展集。");
          break;
        }
        if (item.status === "same") {
          note("已是同一份", "本地已经是同一份（档案摘要相同），无需下载。");
          break;
        }
        simulateProgress("online-download", `正在下载：${item.publisher} / ${item.title}`, 10, () => {
          if (item.status === "new" || item.status === "update") {
            const installed: ExtensionEntry = { ...item, installedAt: "2026-11-06 23:50:00", source: "在线更新" };
            library = [installed, ...library.filter((e) => e.id !== installed.id)];
            pushLib();
          }
          toast("info", item.status === "new" ? "已新增" : "已更新", `${item.publisher} / ${item.title}`);
        });
        break;
      }
      case "startRestore":
        startRestore(req.opts);
        break;
      case "stopRestore":
        window.clearInterval(restoreTimer);
        restoreTimer = 0;
        job("restore", "idle", 1, "用户关闭了窗口，还原已中止（已复制的文件保留）。");
        break;
      case "startCollectDemo":
        startCollect();
        break;
      case "stopGame":
        collect.running = false;
        window.clearInterval(collectTimer);
        note("已请求关闭游戏", "5 秒内没退出会强制结束进程。");
        emitHostEvent({ type: "collect", ...collect });
        break;
      case "clearCollectFiles":
        collect.lines = { dir: [], file: [] };
        collect.dirHit = 0;
        collect.fileHit = 0;
        note("清空完成", "删除 0 个文件（mock）。");
        emitHostEvent({ type: "collect", ...collect });
        break;
      case "enqueueXp3":
        for (const name of req.names) {
          batchTasks.push({
            id: `${name}-${Math.random().toString(36).slice(2, 6)}`,
            file: name,
            status: "queued",
            progress: 0,
            detail: "等待开始",
            size: `${(8 + Math.random() * 40).toFixed(1)} MB`,
          });
        }
        emitHostEvent({ type: "batch", tasks: batchTasks, stats: batchStats() });
        break;
      case "startBatch":
        startBatch();
        break;
      case "stopBatch":
        window.clearInterval(batchTimer);
        batchTimer = 0;
        job("extract", "idle", 1, "解包已中止。");
        break;
      case "openLegacy":
        emitHostEvent({ type: "overlayHint", overlay: "legacy", open: true });
        if (req.action === "keyStatic") {
          simulateProgress("static-key", "正在提取静态 Key……", 8, () => {
            toast("info", "静态 Key 已写出", "ExtractKey_Output\\Static");
          });
        }
        break;
      case "failDemo":
        job("extract", "error", 0.42, "启动已中止（反篡改补丁流程未产出 _crack.exe）", "确认目标正确后可以再点一次本步骤重试。");
        toast("error", "启动已中止", "反篡改补丁流程未产出 _crack.exe");
        break;
      default:
        break;
    }
  }

  function runStep(step: 1 | 2 | 3) {
    if (!workspace.exe) {
      applyExe("D:\\Games\\星夜回廊\\星夜回廊.exe");
      note("已弹出选择框并选用演示目标", workspace.exe!);
    }
    if (mode === "studio") {
      toast("warn", "二期预留", "「该会社合集撞新作」是二期预留功能，当前版本请改用「该作扩展集提取」或「传统动态模式」。");
      return;
    }
    if (mode === "extension") {
      if (step === 1) {
        if (!selected.title) {
          toast("warn", "未选择扩展集", "先在左侧「扩展集选择」里选择会社，再选择该作扩展集。");
          return;
        }
        simulateProgress("static-table", "正在加载扩展集档案……", 4, () => {
          workspace.staticReady = true;
          workspace.attribution = "passed";
          workspace.hashReady = true;
          workspace.hashDirEntries = 1840;
          workspace.hashFileEntries = 61220;
          pushWs();
          toast("info", "已生成静态 Hash 表", "写入 StaticHash_Output（全程没有跑游戏动态采集）。");
        });
      } else if (step === 2) {
        simulateProgress("precheck", "正在检查目标（反篡改补丁流程）……", 5, () => {
          emitHostEvent({ type: "overlayHint", overlay: "batch", open: true });
          workspace.extractReady = true;
          pushWs();
          job("extract", "success", 1, "已启动游戏并注入解包界面。请把 *.xp3 拖进解包窗口排队。");
        });
      } else {
        emitHostEvent({ type: "overlayHint", overlay: "restore", open: true });
      }
      return;
    }
    if (step === 1) {
      simulateProgress("precheck", "正在检查目标（反篡改补丁流程）……", 5, () => {
        log("info", "YuaExtract", "补丁流程已产出 星夜回廊_crack.exe，自动改用该文件作为目标继续启动");
        emitHostEvent({ type: "overlayHint", overlay: "batch", open: true });
        workspace.extractReady = true;
        pushWs();
        job("extract", "success", 1, "已启动游戏并注入解包界面。");
      });
    } else if (step === 2) {
      startCollect();
      emitHostEvent({ type: "overlayHint", overlay: "collect", open: true });
      workspace.hashReady = true;
      pushWs();
    } else {
      emitHostEvent({ type: "overlayHint", overlay: "restore", open: true });
    }
  }

  function startCollect() {
    collect = {
      running: true,
      uptime: 0,
      pid: 4288,
      dirHit: 120,
      dirTotal: 1840,
      fileHit: 860,
      fileTotal: 60133,
      lines: { dir: [], file: [] },
    };
    job("collect", "running", 0.05, "已启动游戏并注入 Hash 收集模块。");
    window.clearInterval(collectTimer);
    collectTimer = window.setInterval(() => {
      collect.uptime += 1;
      collect.dirHit = Math.min(collect.dirTotal, collect.dirHit + (Math.random() < 0.5 ? 1 : 0));
      collect.fileHit = Math.min(collect.fileTotal, collect.fileHit + Math.floor(3 + Math.random() * 18));
      const dName = `bgimage/episode${(collect.dirHit % 40) + 1}`;
      const fName = `scene_${String(collect.fileHit).padStart(5, "0")}.ks`;
      const dHash = hex(16);
      const fHash = hex(64);
      collect.lines.dir = [`${dName}##YSig##${dHash}`, ...collect.lines.dir].slice(0, 400);
      collect.lines.file = [`${fName}##YSig##${fHash}`, ...collect.lines.file].slice(0, 400);
      workspace.hashDirEntries = collect.dirHit;
      workspace.hashFileEntries = collect.fileHit;
      workspace.hashReady = true;
      emitHostEvent({ type: "collect", ...collect });
    }, 280) as unknown as number;
  }

  function startRestore(opts: RestoreOptions) {
    restoreTasks = packs.map((pack, i) => ({
      id: pack,
      pack,
      status: i < Math.min(opts.threads, packs.length) ? "running" : "queued",
      progress: 0,
      current: "等待开始",
      detail: `等待开始（${1200 + i * 800} 个文件）`,
      files: 1200 + i * 800,
    }));
    restoreStats = { ...zeroRestore(), total: packs.length, running: Math.min(opts.threads, packs.length), queued: Math.max(0, packs.length - opts.threads) };
    job("restore", "running", 0.02, "开始还原……");
    log("info", "Restore", `已准备 ${packs.length} 个封包任务，工作线程 ${opts.threads}，${opts.keepUnresolved ? "保留未还原" : "只输出已还原"}${opts.inferNames ? "，含推理补充还原" : ""}`);
    window.clearInterval(restoreTimer);
    restoreTimer = window.setInterval(() => {
      let running = 0;
      let queued = 0;
      let done = 0;
      let failed = 0;
      for (const t of restoreTasks) {
        if (t.status === "queued") {
          const active = restoreTasks.filter((x) => x.status === "running").length;
          if (active < opts.threads) t.status = "running";
        }
        if (t.status === "running") {
          t.progress = Math.min(1, t.progress + 0.04 + Math.random() * 0.05);
          t.current = `${t.pack.replace(".xp3", "")}/${hex(16)}/${hex(12)}…`;
          t.detail = `已处理 ${Math.floor(t.progress * t.files)} / ${t.files}`;
          if (t.progress >= 1) {
            if (t.pack === "voice.xp3" && !opts.keepUnresolved) {
              t.status = "done";
              t.detail = "完成（部分条目因缺目录名未输出）";
            } else if (t.pack === "system.xp3" && Math.random() < 0) {
              t.status = "failed";
              t.detail = "写出失败";
            } else {
              t.status = "done";
              t.detail = "已完成";
            }
            t.progress = 1;
          }
        }
        if (t.status === "running") running++;
        if (t.status === "queued") queued++;
        if (t.status === "done") done++;
        if (t.status === "failed") failed++;
      }
      restoreStats = {
        ...restoreStats,
        running,
        queued,
        done,
        failed,
        restored: Math.floor(done * 9000 + running * 1200),
        hashed: 60133,
        kept: opts.keepUnresolved ? done * 40 : 0,
        discarded: opts.keepUnresolved ? 0 : done * 80,
        suffixFixed: opts.keepUnresolved ? done * 40 : 0,
        mined: 0,
        pass1Rate: opts.inferNames && done === packs.length ? 0.9146 : null,
        finalRate: done === packs.length ? (opts.inferNames ? 0.9766 : 0.9146) : null,
      };
      const allDone = restoreTasks.every((t) => t.status === "done" || t.status === "failed");
      if (allDone && opts.inferNames && restoreStats.mined === 0) {
        restoreTasks[2].status = "mining";
        restoreTasks[2].detail = "挖掘中：扫描 240 个文件 / 候选 1800 / 命中 120";
        emitHostEvent({ type: "restoreTasks", tasks: restoreTasks, stats: restoreStats, mining: "挖掘中：扫描 240 个文件 / 候选 1800 / 命中 120" });
        restoreStats.mined = 3715;
        restoreStats.finalRate = 0.9766;
        schedule(() => {
          restoreTasks[2].status = "done";
          restoreTasks[2].detail = "推理补充还原完成";
          workspace.restoreReady = true;
          workspace.restoreRate = 0.9766;
          workspace.restoredCount = 58715;
          workspace.hashedCount = 60133;
          emitHostEvent({ type: "restoreTasks", tasks: restoreTasks, stats: { ...restoreStats, done: packs.length, running: 0 } });
          job("restore", "success", 1, "还原率 97.66%（已还原 58,715 / 哈希名文件 60,133，推理补充还原新增 3,715）");
          pushWs();
          window.clearInterval(restoreTimer);
          restoreTimer = 0;
        }, 1600);
        return;
      }
      emitHostEvent({ type: "restoreTasks", tasks: restoreTasks.map((t) => ({ ...t })), stats: { ...restoreStats } });
      if (allDone && !opts.inferNames) {
        workspace.restoreReady = true;
        workspace.restoreRate = 0.9146;
        workspace.restoredCount = 55000;
        workspace.hashedCount = 60133;
        job("restore", "success", 1, "还原率 91.46%（已还原 55,000 / 哈希名文件 60,133）");
        pushWs();
        window.clearInterval(restoreTimer);
        restoreTimer = 0;
      }
    }, 180) as unknown as number;
  }

  function startBatch() {
    if (!batchTasks.length) {
      toast("warn", "没有可处理的 XP3 文件。", "把 *.xp3 拖进解包窗口排队即可。");
      return;
    }
    job("extract", "running", 0.05, "正在解包……");
    workspace.extractReady = true;
    window.clearInterval(batchTimer);
    batchTimer = window.setInterval(() => {
      for (const t of batchTasks) {
        if (t.status === "queued") {
          if (batchTasks.filter((x) => x.status === "running").length < 2) t.status = "running";
        }
        if (t.status === "running") {
          t.progress = Math.min(1, t.progress + 0.08);
          t.detail = t.progress >= 1 ? "完成" : "读取索引 / 写出文件…";
          if (t.progress >= 1) t.status = t.file.includes("patch") && false ? "failed" : "done";
        }
      }
      emitHostEvent({ type: "batch", tasks: [...batchTasks], stats: batchStats() });
      if (batchTasks.every((t) => t.status === "done" || t.status === "failed")) {
        job("extract", "success", 1, "所有解包任务已完成。");
        window.clearInterval(batchTimer);
        batchTimer = 0;
        pushWs();
      }
    }, 200) as unknown as number;
  }

  function batchStats() {
    return {
      total: batchTasks.length,
      batch: 1,
      batchTotal: 1,
      running: batchTasks.filter((t) => t.status === "running").length,
      done: batchTasks.filter((t) => t.status === "done").length,
      failed: batchTasks.filter((t) => t.status === "failed").length,
    };
  }

  function simulateProgress(kind: any, text: string, steps: number, done: () => void) {
    cancelFlag = false;
    let i = 0;
    job(kind, "running", 0.05, text);
    const tick = () => {
      if (cancelFlag) return;
      i++;
      job(kind, "running", i / steps, text);
      if (i >= steps) {
        job(kind, "success", 1, text);
        done();
        schedule(() => job("idle", "idle", 0, "（空闲）"), 800);
      } else schedule(tick, 220);
    };
    schedule(tick, 220);
  }

  function describeOpen(t: string) {
    if (t === "openWorkspace") return workspace.workspace ?? info.loaderDir;
    if (t === "openRestoreDir") return workspace.restoreDir ?? "";
    if (t === "openRestoreReport") return workspace.reportPath ?? "";
    if (t === "openLibraryRoot") return info.libraryRoot;
    if (t === "openSelectedSet") return library.find((e) => e.title === selected.title)?.path ?? info.libraryRoot;
    if (t === "openCollectDir") return workspace.hashDir ?? "";
    return info.logDir;
  }

  // boot
  emitHostEvent({ type: "hello", info });
  pushWs();
  pushLib();
}

function emptyWorkspace(): WorkspaceSnapshot {
  return {
    exe: null,
    title: null,
    publisher: null,
    workspace: null,
    extractDir: null,
    hashDir: null,
    restoreDir: null,
    reportPath: null,
    staticHashDir: null,
    extractReady: false,
    hashReady: false,
    hashDirEntries: 0,
    hashFileEntries: 0,
    restoreReady: false,
    staticReady: false,
    attribution: "none",
    restoreRate: null,
    restoredCount: 0,
    hashedCount: 0,
  };
}

function zeroRestore() {
  return {
    total: 0,
    running: 0,
    queued: 0,
    done: 0,
    failed: 0,
    restored: 0,
    hashed: 0,
    kept: 0,
    discarded: 0,
    suffixFixed: 0,
    mined: 0,
    pass1Rate: null as number | null,
    finalRate: null as number | null,
  };
}

function hex(n: number) {
  let s = "";
  while (s.length < n) s += Math.floor(Math.random() * 16).toString(16);
  return s.slice(0, n);
}

function mockSelfCheck(): SelfCheckItem[] {
  return [
    { id: "1", title: "工具目录可写", ok: true, detail: "可写 — D:\\Tools\\YuaExtract" },
    { id: "2", title: "Hash 收集模块 CxdecStringDumper.dll", ok: true, detail: "32 位模块存在于 CxdecExtractordll" },
    { id: "3", title: "扩展集库目录", ok: true, detail: "存在且可写" },
    { id: "4", title: "库清单 manifest.txt", ok: true, detail: "可解析，共 3 条" },
    { id: "5", title: "库内档案", ok: true, detail: "3 份，全部可解码、无致命例外、带目标 exe 摘要（1 份含 2 条生成期例外）" },
    { id: "6", title: "Universal.log Hash Seed", ok: true, detail: "读到 Hash Seed:（有盐；没有时会回落到静态派生）" },
    { id: "7", title: "在线更新配置", ok: true, detail: "基址为 https，通过校验" },
  ];
}

function mockRemote(local: ExtensionEntry[]): RemoteExtension[] {
  const star = local.find((e) => e.title === "星夜回廊");
  return [
    {
      ...(star ?? local[0]),
      status: "same",
      remoteDirCount: 1840,
      remoteFileCount: 61220,
      remoteSha: star?.archiveSha ?? "",
    },
    {
      id: "wamsoft/new-work",
      publisher: "Wamsoft",
      title: "夜航灯塔",
      contributor: "Rinne",
      version: "1.0.0",
      date: "2026-10-30",
      comment: "新作档案，目录覆盖中等。",
      dirCount: 700,
      fileCount: 22100,
      exceptions: 0,
      archiveSha: "cafe1234babe5678",
      targetSha: "0102030405060708",
      siblingSha: "",
      saltSha: "1122334455667788",
      generatedAt: "2026-10-30 12:00:00",
      installedAt: "",
      source: "远端索引",
      path: "",
      status: "new",
      remoteDirCount: 700,
      remoteFileCount: 22100,
      remoteSha: "cafe1234babe5678",
    },
    {
      id: "wamsoft/white-iris",
      publisher: "Wamsoft",
      title: "白虹之庭",
      contributor: "meika",
      version: "1.1.0",
      date: "2026-10-20",
      comment: "",
      dirCount: 980,
      fileCount: 34102,
      exceptions: 0,
      archiveSha: "99aa00bb11cc22dd",
      targetSha: "88c0a1b2d3e4f506",
      siblingSha: "",
      saltSha: "0c19aa77b201ee54",
      generatedAt: "2026-10-20 08:00:00",
      installedAt: "",
      source: "远端索引",
      path: "",
      status: "update",
      remoteDirCount: 980,
      remoteFileCount: 34102,
      remoteSha: "99aa00bb11cc22dd",
    },
    {
      id: "morningstar/cyan-feather",
      publisher: "晨星工作室",
      title: "青羽日记",
      contributor: "anon",
      version: "0.9.0",
      date: "2026-07-11",
      comment: "",
      dirCount: 412,
      fileCount: 15880,
      exceptions: 2,
      archiveSha: "0011223344556677",
      targetSha: "de44aa0199bbcc12",
      siblingSha: "",
      saltSha: "abc11900ffeedc01",
      generatedAt: "2026-07-11 08:40:12",
      installedAt: "",
      source: "远端索引",
      path: "",
      status: "local-richer",
      remoteDirCount: 390,
      remoteFileCount: 15000,
      remoteSha: "0011223344556677",
    },
  ];
}
