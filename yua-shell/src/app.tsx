import { useEffect, useRef, type DragEvent } from "react";
import { BookOpen, Hammer, Home, Info, Minus, Search, Settings, Square, X } from "lucide-react";
import { fmtInt } from "./lib/format";
import { targetInteractionBusy } from "./lib/interaction";
import { LogDrawer, OverlayRoot } from "./overlays/overlays";
import { AboutScreen, HomeScreen, LibraryScreen, MakeScreen } from "./screens/screens";
import { useStore } from "./store";
import { greeting } from "./ui/desk";
import type { TabId } from "./types";

const LOGO = "./brand/ameto.png";
const RESIZE_EDGES = ["n", "s", "e", "w", "nw", "ne", "sw", "se"] as const;

export function App() {
  const s = useStore();
  const t = s.t;
  const sRef = useRef(s);
  sRef.current = s;

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      const cur = sRef.current;
      if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "k") {
        e.preventDefault();
        cur.set({ palette: !cur.palette });
      }
      if (e.key === "Escape") {
        if (cur.palette) { cur.set({ palette: false }); return; }
        if (cur.confirm) { const fn = cur.confirm.resolve; cur.set({ confirm: null }); fn(false); return; }
        const order: Array<keyof typeof cur.overlays> = ["settings", "notes", "meta", "online", "legacy", "batch", "collect", "restore"];
        const open = order.find((id) => cur.overlays[id]);
        if (open) cur.openOverlay(open, false);
      }
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  useEffect(() => {
    // Compile-time demo guard (raw expression, so `vite build` folds it to
    // `false` in production and strips the `__YUA_SHOT__` registration and the
    // demo-only `applyShot` helper from the shipped bundle).
    if (import.meta.env.DEV || import.meta.env.MODE === "demo" || import.meta.env.MODE === "test") {
      const qa = new URLSearchParams(window.location.search).get("qa") === "1";
      if (Boolean(window.__YUA_MOCK__) || qa) {
        window.__YUA_SHOT__ = async (name: string) => {
          await applyShot(name, sRef.current);
        };
      } else {
        delete window.__YUA_SHOT__;
      }
    }
  }, []);

  const tabs: { id: TabId; label: string; icon: typeof Home }[] = [
    { id: "home", label: t.tabs.home, icon: Home },
    { id: "make", label: s.locale === "zh" ? "制作" : t.tabs.make, icon: Hammer },
    { id: "library", label: t.tabs.library, icon: BookOpen },
    { id: "about", label: t.tabs.about, icon: Info },
  ];

  const interactionBusy = targetInteractionBusy({
    jobStatus: s.job.status,
    restoreTasks: s.restoreTasks,
  });

  const onDrop = (e: DragEvent) => {
    e.preventDefault();
    s.set({ dropping: false });
    if (interactionBusy) return;
    const file = [...e.dataTransfer.files][0];
    const path = (file as File & { path?: string }).path || file?.name;
    if (!path) return;
    if (path.toLowerCase().endsWith(".xp3")) {
      s.openOverlay("batch");
      s.send({ type: "enqueueXp3", names: [...e.dataTransfer.files].map((f) => f.name) });
    } else {
      s.send({ type: "setTarget", path });
    }
  };

  const gameName = s.workspace.exe
    ? (s.workspace.title || s.workspace.exe.split(/[/\\]/).pop()?.replace(/\.exe$/i, "") || "")
    : "";
  const hi = greeting(s.locale);
  const sep = s.locale === "en" ? ", " : "，";
  const greetTitle = s.tab === "library"
    ? (s.locale === "zh" ? "扩展集库" : "Extension library")
    : s.tab === "make"
    ? s.t.makeTitleShort
    : s.tab === "about"
    ? s.t.aboutTitleShort
    : (gameName ? `${hi}${sep}${gameName}` : hi);
  const jobLive = s.job.status === "running" || s.job.status === "cancelling";
  const greetSub = s.tab === "library"
    ? (s.locale === "zh" ? `${fmtInt(s.library.length)} 份` : `${s.library.length} sets`)
    : s.tab === "make"
    ? (s.locale === "zh" ? "把这次整理成扩展集" : "Pack this run into a set.")
    : s.tab === "about"
    ? (s.t.tagline)
    : jobLive
    ? (s.job.kind === "extract" || s.job.kind === "precheck" ? s.t.unpackingNow : s.job.kind === "restore" ? s.t.restoringNow : s.t.workingNow)
    : gameName
    ? (s.locale === "zh" ? "下一步在右侧" : "Next step is on the right.")
    : (s.locale === "zh" ? "把游戏拖进来" : "Drop a game .exe here.");

  return (
    <div
      className="desk"
      data-dropping={s.dropping}
      onDragOver={(e) => { e.preventDefault(); if (!interactionBusy) s.set({ dropping: true }); }}
      onDragLeave={() => s.set({ dropping: false })}
      onDrop={onDrop}
    >
      {RESIZE_EDGES.map((edge) => (
        <div
          key={edge}
          className={`win-resize win-resize-${edge}`}
          aria-hidden
          onMouseDown={(e) => {
            if (e.button !== 0) return;
            e.preventDefault();
            s.send({ type: "windowControl", action: "resize", edge });
          }}
        />
      ))}
      <div className="window">
      <header
        className="titlebar"
        onMouseDown={(e) => {
          if (e.button !== 0) return;
          if ((e.target as HTMLElement).closest("button, input, a, nav, [role='button']")) return;
          s.send({ type: "windowControl", action: "drag" });
        }}
        onDoubleClick={(e) => {
          if ((e.target as HTMLElement).closest("button, input, a, nav, [role='button']")) return;
          s.send({ type: "windowControl", action: "maximize" });
        }}
      >
        <div className="brand">
          <img src={LOGO} alt="" width={24} height={24} />
          <span>YuaExtract</span>
          <nav className="nav" role="tablist" aria-label="pages">
            {tabs.map((tab) => (
              <button
                key={tab.id}
                className="nav-btn"
                role="tab"
                aria-selected={s.tab === tab.id}
                onClick={() => s.set({ tab: tab.id })}
              >
                <tab.icon size={14} aria-hidden />
                {tab.label}
              </button>
            ))}
          </nav>
        </div>
        <div className="title-right">
          <button className="ghost" title={t.paletteHint} onClick={() => s.set({ palette: true })}>
            <Search size={14} />
            <span className="kbd">Ctrl+K</span>
          </button>
          <button className="icon-btn" aria-label={t.settings} onClick={() => s.openOverlay("settings")}>
            <Settings size={16} />
          </button>
          <div className="win-ctrls">
            <button
              type="button"
              className="win-btn"
              aria-label={s.locale === "zh" ? "最小化" : "Minimize"}
              title={s.locale === "zh" ? "最小化" : "Minimize"}
              onClick={() => s.send({ type: "windowControl", action: "minimize" })}
            >
              <Minus size={14} />
            </button>
            <button
              type="button"
              className="win-btn"
              aria-label={s.locale === "zh" ? "最大化 / 还原" : "Maximize"}
              title={s.locale === "zh" ? "最大化 / 还原" : "Maximize"}
              onClick={() => s.send({ type: "windowControl", action: "maximize" })}
            >
              <Square size={12} />
            </button>
            <button
              type="button"
              className="win-btn win-close"
              aria-label={s.locale === "zh" ? "关闭" : "Close"}
              title={s.locale === "zh" ? "关闭" : "Close"}
              onClick={() => s.send({ type: "windowControl", action: "close" })}
            >
              <X size={15} />
            </button>
          </div>
        </div>
      </header>
      <div className="greet">
        <div>
          <h1>{greetTitle}</h1>
          <p>{greetSub}</p>
        </div>
      </div>
      <div className="stage">
        {s.tab === "home" && <HomeScreen />}
        {s.tab === "make" && (
          <div style={{ flex: 1, minWidth: 0, overflow: "hidden" }}><MakeScreen /></div>
        )}
        {s.tab === "library" && (
          <div style={{ flex: 1, minWidth: 0, overflow: "hidden" }}><LibraryScreen /></div>
        )}
        {s.tab === "about" && (
          <div style={{ flex: 1, minWidth: 0, overflow: "hidden" }}><AboutScreen /></div>
        )}
      </div>
      <LogDrawer />
      </div>
      <OverlayRoot />
      <div className="drop-veil">{t.dropStrong}</div>
    </div>
  );
}

async function applyShot(name: string, s: ReturnType<typeof useStore>) {
  const closeAll = () =>
    s.set({
      overlays: {},
      palette: false,
      confirm: null,
      logOpen: false,
      tab: "home",
      theme: "light",
      locale: "zh",
      toasts: [],
      job: { kind: "idle", status: "idle", progress: 0, text: "" },
      remoteError: undefined,
    });
  closeAll();
  const wait = (ms: number) => new Promise((r) => setTimeout(r, ms));
  s.send({ type: "stopRestore" });
  s.send({ type: "stopBatch" });
  await wait(30);
  s.send({ type: "cleanDirs" });
  await wait(30);
  if (name !== "library-empty") {
    s.set({ selectedPublisher: "Wamsoft", selectedTitle: "星夜回廊" });
    s.send({ type: "refreshLibrary" });
    await wait(30);
  }
  switch (name) {
    case "home-idle":
      break;
    case "home-loaded":
      s.send({ type: "browseExe" });
      await wait(80);
      break;
    case "home-running":
      s.send({ type: "browseExe" });
      s.set({ mode: "classic" });
      s.send({ type: "setMode", mode: "classic" });
      s.send({ type: "runStep", step: 1 });
      await wait(400);
      s.set({ overlays: {} });
      break;
    case "home-error":
      s.send({ type: "browseExe" });
      s.send({ type: "failDemo" });
      await wait(80);
      break;
    case "make-set":
      s.set({ tab: "make" });
      s.send({ type: "browseExe" });
      break;
    case "make-set-empty":
      s.set({ tab: "make", lastAction: "", dryRunNote: "" });
      break;
    case "library":
      s.set({ tab: "library" });
      s.send({ type: "fetchIndex" });
      await wait(1600);
      break;
    case "library-empty":
      s.set({ tab: "library", library: [], selectedPublisher: null, selectedTitle: null });
      break;
    case "about":
      s.set({ tab: "about" });
      break;
    case "restore-idle":
      s.send({ type: "browseExe" });
      s.set({ overlays: { restore: true } });
      break;
    case "restore-running":
      s.send({ type: "browseExe" });
      s.set({ overlays: { restore: true } });
      await wait(40);
      s.send({ type: "startRestore", opts: { outputDir: "Restored_Extractor_Output", threads: 4, keepUnresolved: false, inferNames: false } });
      await wait(600);
      break;
    case "restore-done":
      s.send({ type: "browseExe" });
      s.set({ overlays: { restore: true } });
      s.send({ type: "startRestore", opts: { outputDir: "Restored_Extractor_Output", threads: 4, keepUnresolved: true, inferNames: true } });
      for (let i = 0; i < 50; i++) {
        await wait(200);
        const job = (window as Window & { __YUA_STATE__?: { job?: { kind?: string; status?: string } } }).__YUA_STATE__?.job;
        if (job?.kind === "restore" && job.status === "success") break;
      }
      await wait(280);
      break;
    case "collect-console":
      s.send({ type: "browseExe" });
      s.send({ type: "startCollectDemo" });
      s.set({ overlays: { collect: true } });
      await wait(700);
      break;
    case "batch-extractor":
      s.send({ type: "browseExe" });
      s.set({ overlays: { batch: true } });
      s.send({ type: "enqueueXp3", names: ["data.xp3", "patch.xp3", "voice.xp3"] });
      s.send({ type: "startBatch", outputDir: "Extractor_Output", threads: 4 });
      await wait(500);
      break;
    case "batch-empty":
      s.set({ overlays: { batch: true }, batchTasks: [] });
      break;
    case "online-update":
      s.set({ overlays: { online: true } });
      s.send({ type: "fetchIndex" });
      await wait(1800);
      break;
    case "online-error":
      s.set({ overlays: { online: true }, remoteError: "失败：TLS 握手没通过（证书链不受信任、证书过期，或系统时间不对）。地址：https://example.invalid/", remotes: [] });
      break;
    case "meta-edit":
      s.set({ overlays: { meta: true } });
      break;
    case "legacy":
      s.set({ overlays: { legacy: true } });
      break;
    case "command-palette":
      s.set({ palette: true });
      break;
    case "palette-over-restore":
      s.send({ type: "browseExe" });
      s.set({ overlays: { restore: true }, palette: true });
      break;
    case "restore-after-extract":
      s.send({ type: "browseExe" });
      s.set({ mode: "classic" });
      s.send({ type: "setMode", mode: "classic" });
      s.send({ type: "runStep", step: 1 });
      await wait(450);
      s.send({ type: "startRestore", opts: { outputDir: "Restored_Extractor_Output", threads: 4, keepUnresolved: false, inferNames: false } });
      await wait(900);
      s.set({ overlays: { restore: true }, palette: false });
      await wait(160);
      break;
    case "toast-over-restore":
      s.send({ type: "browseExe" });
      s.set({ overlays: { restore: true } });
      await wait(40);
      s.send({ type: "startRestore", opts: { outputDir: "Restored_Extractor_Output", threads: 4, keepUnresolved: false, inferNames: false } });
      await wait(200);
      s.set({ toasts: [{ id: 9001, level: "error", title: s.locale === "zh" ? "下载失败" : "Download failed", body: s.locale === "zh" ? "索引请求超时" : "Index request timed out" }] });
      break;
    case "logs-open":
      s.send({ type: "browseExe" });
      s.set({ logOpen: true, logHeight: 220 });
      break;
    case "light-home":
      s.send({ type: "browseExe" });
      s.set({ theme: "light", tab: "home" });
      break;
    case "dark-home":
      s.send({ type: "browseExe" });
      s.set({ theme: "dark", tab: "home" });
      break;
    case "dark-library":
      s.set({ theme: "dark", tab: "library" });
      s.send({ type: "fetchIndex" });
      await wait(1600);
      break;
    case "settings":
      s.set({ overlays: { settings: true } });
      break;
    case "en-home":
      s.send({ type: "browseExe" });
      s.set({ locale: "en", tab: "home" });
      break;
    case "notes":
      s.send({ type: "readNotes" });
      await wait(80);
      break;
    default:
      break;
  }
  if (!name.includes("error") && name !== "online-update" && name !== "toast-over-restore") {
    await wait(80);
    s.set({ toasts: [] });
  }
}
