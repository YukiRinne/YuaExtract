import { useEffect, useMemo, useRef, useState } from "react";
import { FileArchive, FileQuestion, FolderOpen, HelpCircle, Sparkles, Square, Trash2, X } from "lucide-react";
import { fmtDur, fmtInt, fmtPct, fmtTime, midTrunc } from "../lib/format";
import { VirtualList } from "../lib/virtualList";
import { Meter, PathLine, PetalBurst } from "../ui/widgets";
import { Ring } from "../ui/desk";
import { SettingsScreen } from "../screens/screens";
import { localizeText } from "../i18n";
import { useStore } from "../store";
import type { OverlayId, RestoreOptions } from "../types";

export function OverlayRoot() {
  const s = useStore();
  return (
    <>
      {s.overlays.restore && <RestoreOverlay />}
      {s.overlays.collect && <CollectOverlay />}
      {s.overlays.batch && <BatchOverlay />}
      {s.overlays.online && <OnlineOverlay />}
      {s.overlays.meta && <MetaOverlay />}
      {s.overlays.legacy && <LegacyOverlay />}
      {s.overlays.notes && <NotesOverlay />}
      {s.overlays.settings && <SettingsOverlay />}
      {s.palette && <Palette />}
      {s.confirm && <ConfirmDialog />}
      <Toasts />
    </>
  );
}

function useFocusTrap(active: boolean) {
  const ref = useRef<HTMLDivElement>(null);
  useEffect(() => {
    if (!active) return;
    const root = ref.current;
    if (!root) return;
    const sel = "a[href],button:not([disabled]),input:not([disabled]),textarea:not([disabled]),select:not([disabled]),[tabindex]:not([tabindex=\"-1\"])";
    const list = () => [...root.querySelectorAll<HTMLElement>(sel)];
    const first = list()[0];
    first?.focus();
    const onKey = (e: KeyboardEvent) => {
      if (e.key !== "Tab") return;
      const items = list();
      if (!items.length) return;
      const i = items.indexOf(document.activeElement as HTMLElement);
      if (e.shiftKey && i <= 0) { e.preventDefault(); items[items.length - 1].focus(); }
      else if (!e.shiftKey && (i === items.length - 1 || i < 0)) { e.preventDefault(); items[0].focus(); }
    };
    root.addEventListener("keydown", onKey);
    return () => root.removeEventListener("keydown", onKey);
  }, [active]);
  return ref;
}

function Frame({
  id, title, children, footer, narrow,
}: {
  id: OverlayId;
  title: string;
  children: React.ReactNode;
  footer?: React.ReactNode;
  narrow?: boolean;
}) {
  const s = useStore();
  const ref = useFocusTrap(true);
  return (
    <div className="scrim" onMouseDown={(e) => { if (e.target === e.currentTarget) s.openOverlay(id, false); }}>
      <div ref={ref} className={`dialog ${narrow ? "narrow" : ""}`} role="dialog" aria-modal="true" aria-labelledby={`${id}-title`}>
        <header>
          <h3 id={`${id}-title`}>{title}</h3>
          <button className="icon-btn" aria-label={s.t.close} onClick={() => s.openOverlay(id, false)}><X size={16} /></button>
        </header>
        <div className="body-pad">{children}</div>
        {footer && <footer>{footer}</footer>}
      </div>
    </div>
  );
}

function RestoreOverlay() {
  const s = useStore();
  const t = s.t;
  const [opts, setOpts] = useState<RestoreOptions>({
    outputDir: s.workspace.restoreDir || "Restored_Extractor_Output",
    threads: 4,
    keepUnresolved: false,
    inferNames: false,
  });
  const running = s.restoreTasks.some((x) => x.status === "running" || x.status === "mining" || x.status === "queued")
    || (s.job.kind === "restore" && (s.job.status === "running" || s.job.status === "cancelling"));
  const done = s.job.kind === "restore" && s.job.status === "success" && !s.restoreTasks.some((x) => x.status === "running" || x.status === "mining" || x.status === "queued");
  const failed = s.job.kind === "restore" && s.job.status === "error";
  const [burst, setBurst] = useState(0);
  const [optHelp, setOptHelp] = useState(false);
  useEffect(() => {
    if (done) setBurst((n) => n + 1);
  }, [done]);
  const total = s.restoreStats.hashed || s.restoreStats.total || 0;
  const processed = s.restoreStats.restored + s.restoreStats.kept + s.restoreStats.discarded;
  const overall = done && s.restoreStats.finalRate != null
    ? s.restoreStats.finalRate
    : s.restoreTasks.length
      ? s.restoreTasks.reduce((a, task) => a + task.progress, 0) / s.restoreTasks.length
      : (running ? s.job.progress : 0);
  return (
    <>
      <PetalBurst fire={burst} />
      <Frame
        id="restore"
        title={t.restoreTitle}
        footer={
          <>
            <span className="note num">
              {t.statTotal} {fmtInt(s.restoreStats.total)} · {t.statRunning} {fmtInt(s.restoreStats.running)} · {t.statQueued} {fmtInt(s.restoreStats.queued)} · {t.statDone} {fmtInt(s.restoreStats.done)} · {t.statFailed} {fmtInt(s.restoreStats.failed)}
            </span>
            <div className="row">
              <button className="btn" onClick={() => s.send({ type: "openRestoreDir" })}>{t.openRestore}</button>
              {running ? (
                <button className="btn-danger" onClick={() => s.send({ type: "stopRestore" })}>{t.stopRestore}</button>
              ) : (
                <button className="btn-primary" style={{ width: "auto" }} onClick={() => s.send({ type: "startRestore", opts })}>
                  <span>{t.start}</span>
                </button>
              )}
              <button className="btn" onClick={() => { if (running) s.send({ type: "stopRestore" }); s.openOverlay("restore", false); }}>{t.back}</button>
            </div>
          </>
        }
      >
        <div className="row">
          <label className="grow">{t.outputDir}<input className="field" value={opts.outputDir} onChange={(e) => setOpts({ ...opts, outputDir: e.target.value })} /></label>
          <label>{t.threads}<input className="field" type="number" min={1} max={16} value={opts.threads} onChange={(e) => setOpts({ ...opts, threads: Number(e.target.value) || 1 })} style={{ width: 88 }} /></label>
        </div>
        <div className="opt-toggles">
          <button type="button" className="chip" aria-pressed={opts.keepUnresolved} onClick={() => setOpts({ ...opts, keepUnresolved: !opts.keepUnresolved })}>
            <FileQuestion size={14} /><span>{s.locale === "zh" ? "保留未还原" : t.keepUnresolved}</span>
          </button>
          <button type="button" className="chip" aria-pressed={opts.inferNames} onClick={() => setOpts({ ...opts, inferNames: !opts.inferNames })}>
            <Sparkles size={14} /><span>{s.locale === "zh" ? "推测文件名" : t.infer}</span>
          </button>
          <button type="button" className="icon-btn" aria-expanded={optHelp} aria-controls="restore-opt-help" onClick={() => setOptHelp((v) => !v)}>
            <HelpCircle size={15} />
            <span className="note">{s.locale === "zh" ? "说明" : "Help"}</span>
          </button>
        </div>
        {optHelp && (
          <p className="opt-help note" id="restore-opt-help" tabIndex={0}>
            {t.keepUnresolved} {t.infer}
          </p>
        )}
        <div className="restore-hero">
          <Ring
            value={overall}
            label={t.result}
            sub={failed ? localizeText(s.job.error || s.job.text, s.locale) : done && s.restoreStats.finalRate != null
              ? `${fmtPct(s.restoreStats.finalRate)} · ${fmtInt(s.restoreStats.restored)} / ${fmtInt(s.restoreStats.hashed)}`
              : `${fmtInt(Math.max(processed, s.restoreStats.done))} / ${fmtInt(total || s.restoreStats.total)}`}
          />
          {s.mining && <p className="note">{localizeText(s.mining, s.locale)}</p>}
        </div>
        <div className="split-lib" style={{ marginTop: 8, height: 360 }}>
          <div>
            <h2 style={{ fontSize: 13, margin: "0 0 8px" }}>{t.taskOverview}</h2>
            <div className="pack-list">
              {s.restoreTasks.map((task, i) => (
                <div key={task.id} className="pack-row" data-tone={i % 6}>
                  <span className="idx" style={{ width: 32, height: 32, borderRadius: 10, display: "grid", placeItems: "center", background: "color-mix(in srgb, var(--hibiscus-ink) 50%, transparent)" }}><FileArchive size={15} /></span>
                  <span>
                    <b>{task.pack}</b>
                    <small title={task.current}>{statusLabel(t, task.status)} · {midTrunc(task.current, 28)}</small>
                  </span>
                  <span className="pct num">{Math.round(task.progress * 100)}%</span>
                </div>
              ))}
              {!s.restoreTasks.length && <div className="empty">{s.locale === "zh" ? "点「开始」后出现分包进度。" : "Packs appear after you start."}</div>}
            </div>
          </div>
          <div>
            <h2 style={{ fontSize: 13, margin: "0 0 8px" }}>{t.detailLog}</h2>
            <VirtualList
              className="log-list"
              items={s.logs.filter((l) => l.source === "Restore" || l.source === "YuaExtract").slice(-400)}
              rowHeight={22}
              empty={<div className="empty">{t.emptyLogs}</div>}
              render={(l) => (
                <div className="log-row">
                  <span>{fmtTime(l.ts)}</span>
                  <span className={`lvl-${l.level}`}>{l.level}</span>
                  <span>{localizeText(l.message, s.locale)}</span>
                </div>
              )}
            />
          </div>
        </div>
      </Frame>
    </>
  );
}

function statusLabel(t: ReturnType<typeof useStore>["t"], st: string) {
  if (st === "queued") return t.queued;
  if (st === "running") return t.running;
  if (st === "done") return t.done;
  if (st === "failed") return t.failed;
  if (st === "mining") return t.mining;
  return st;
}

function CollectOverlay() {
  const s = useStore();
  const t = s.t;
  const c = s.collect;
  const rate = c.fileTotal ? c.fileHit / c.fileTotal : 0;
  const [tab, setTab] = useState<"file" | "dir">("file");
  const lines = tab === "file" ? c.lines.file : c.lines.dir;
  return (
    <Frame
      id="collect"
      title={t.collectTitle}
      footer={
        <>
          <span className="note">{c.running ? `${t.running} · PID ${c.pid}` : t.idle}</span>
          <div className="row">
            <button className="btn-danger" onClick={() => s.send({ type: "stopGame" })} title={t.stopGame} aria-label={t.stopGame}><Square size={14} /><span>{s.locale === "zh" ? "关闭" : "Stop"}</span></button>
            <button className="btn" onClick={() => s.send({ type: "clearCollectFiles" })} title={t.clearFiles} aria-label={t.clearFiles}><Trash2 size={14} /><span>{s.locale === "zh" ? "清空" : "Clear"}</span></button>
            <button className="btn" onClick={() => s.send({ type: "openCollectDir" })} title={t.openOut} aria-label={t.openOut}><FolderOpen size={14} /><span>{s.locale === "zh" ? "打开" : "Open"}</span></button>
            <button className="btn" onClick={() => s.openOverlay("collect", false)}>{t.back}</button>
          </div>
        </>
      }
    >
      <div className="split-lib" style={{ height: "auto", marginBottom: 16 }}>
        <div className="sketch" style={{ padding: 14 }}>
          <dl className="kv">
            <dt>{t.game}</dt><dd><PathLine path={s.workspace.exe} /></dd>
            <dt>{t.workspace}</dt><dd><PathLine path={s.workspace.workspace} /></dd>
            <dt>{t.pid}</dt><dd className="num">{c.pid || "—"}</dd>
            <dt>{t.uptime}</dt><dd className="num">{fmtDur(c.uptime)}</dd>
            <dt>{t.denom}</dt><dd><PathLine path={s.workspace.extractDir} /></dd>
          </dl>
        </div>
        <div className="sketch" style={{ padding: 14 }}>
          <p>DirectoryHash <span className="num">{fmtInt(c.dirHit)} / {fmtInt(c.dirTotal)} · {fmtPct(c.dirTotal ? c.dirHit / c.dirTotal : 0, 0)}</span></p>
          <Meter value={c.dirTotal ? c.dirHit / c.dirTotal : 0} />
          <p style={{ marginTop: 10 }}>FileNameHash <span className="num">{fmtInt(c.fileHit)} / {fmtInt(c.fileTotal)} · {fmtPct(c.fileTotal ? c.fileHit / c.fileTotal : 0, 0)}</span></p>
          <Meter value={c.fileTotal ? c.fileHit / c.fileTotal : 0} />
          <p style={{ marginTop: 12 }}>{t.rate}：<span className="num">{fmtPct(rate, 1)}</span></p>
        </div>
      </div>
      <div className="row">
        <button className="chip" aria-pressed={tab === "file"} onClick={() => setTab("file")}>FileNameHash {t.liveLog}</button>
        <button className="chip" aria-pressed={tab === "dir"} onClick={() => setTab("dir")}>DirectoryHash {t.liveLog}</button>
      </div>
      <VirtualList
        className="log-list"
        items={lines}
        rowHeight={22}
        empty={<div className="empty">{t.collectReady}</div>}
        render={(line) => <div className="path">{line}</div>}
      />
    </Frame>
  );
}

function BatchOverlay() {
  const s = useStore();
  const t = s.t;
  const [out, setOut] = useState(s.workspace.extractDir || "Extractor_Output");
  const [threads, setThreads] = useState(4);
  const [hot, setHot] = useState(false);
  const running = s.job.kind === "extract" && s.job.status === "running" && s.overlays.batch;
  const addFiles = (names: string[]) => s.send({ type: "enqueueXp3", names });
  return (
    <Frame
      id="batch"
      title={t.batchTitle}
      footer={
        <>
          <span className="note num">{t.statTotal} {fmtInt(s.batchStats.total)} · {t.statRunning} {fmtInt(s.batchStats.running)} · {t.statDone} {fmtInt(s.batchStats.done)} · {t.statFailed} {fmtInt(s.batchStats.failed)}</span>
          <div className="row">
            {running ? (
              <button className="btn-danger" onClick={() => s.send({ type: "stopBatch" })}>{t.cancel}</button>
            ) : (
              <button className="btn-primary" style={{ width: "auto" }} onClick={() => s.send({ type: "startBatch", outputDir: out, threads })}>{t.start}</button>
            )}
            <button className="btn" onClick={() => s.openOverlay("batch", false)}>{t.back}</button>
          </div>
        </>
      }
    >
      <div
        className="sketch drop-stage empty"
        data-hot={hot}
        style={{ minHeight: 120, marginBottom: 12 }}
        onDragOver={(e) => { e.preventDefault(); setHot(true); }}
        onDragLeave={() => setHot(false)}
        onDrop={(e) => {
          e.preventDefault();
          setHot(false);
          const names = [...e.dataTransfer.files]
            .map((f) => (f as File & { path?: string }).path || f.name)
            .filter((n) => n.toLowerCase().endsWith(".xp3"));
          if (names.length) addFiles(names);
        }}
        onClick={() => addFiles(window.__YUA_MOCK__ ? ["data.xp3", "patch.xp3", "voice.xp3"] : [])}
        role="button"
        tabIndex={0}
        onKeyDown={(e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); addFiles([]); } }}
      >
        <h2 style={{ fontSize: 16 }}>{t.dropXp3}</h2>
      </div>
      <div className="row">
        <label className="grow">{t.output}<input className="field" value={out} onChange={(e) => setOut(e.target.value)} /></label>
        <label>{t.threads}<input className="field" type="number" min={1} max={16} value={threads} onChange={(e) => setThreads(Number(e.target.value) || 1)} style={{ width: 88 }} /></label>
      </div>
      <div className="pack-list" style={{ marginTop: 12 }}>
        {s.batchTasks.map((task, i) => (
          <div key={task.id} className="pack-row" data-tone={["rose", "lilac", "mint", "peach"][i % 4]} data-status={task.status}>
            <FileArchive size={16} />
            <div className="grow">
              <b>{task.file}</b>
              <small>{statusLabel(t, task.status)} · {task.detail} · {task.size}</small>
              <Meter value={task.progress} />
            </div>
            <span className="num">{Math.round(task.progress * 100)}%</span>
          </div>
        ))}
        {!s.batchTasks.length && <p className="note">{t.noXp3}</p>}
      </div>
    </Frame>
  );
}

function OnlineOverlay() {
  const s = useStore();
  const t = s.t;
  const [endpoint, setEndpoint] = useState(s.remoteBase || s.info?.onlineBase || "");
  const [sel, setSel] = useState<string | null>(null);
  const selected = s.remotes.find((r) => r.id === sel);
  const running = s.job.status === "running" && s.job.kind.startsWith("online");
  return (
    <Frame
      id="online"
      title={t.onlineTitle}
      footer={
        <>
          <span className="note">{running ? s.job.text : t.liveEndpoint.replace("{url}", s.remoteBase || endpoint)}</span>
          <div className="row">
            <button className="btn" disabled={running} onClick={() => s.send({ type: "fetchIndex", endpoint })}>{t.fetchIndex}</button>
            <button className="btn-primary" style={{ width: "auto" }} disabled={!sel || running} onClick={async () => {
              if (selected?.status === "local-richer") {
                const ok = await s.ask({
                  title: t.overwriteTitle,
                  body: t.overwriteBody.replace("{local}", String(selected.fileCount)).replace("{remote}", String(selected.remoteFileCount)),
                  ok: t.overwriteOk,
                  cancel: t.cancel,
                  danger: true,
                });
                if (!ok) return;
                s.send({ type: "downloadRemote", entryId: sel!, force: true });
              } else if (sel) s.send({ type: "downloadRemote", entryId: sel });
            }}>{t.downloadSel}</button>
            <button className="btn" onClick={() => s.openOverlay("online", false)}>{t.close}</button>
          </div>
        </>
      }
    >
      <label>{t.endpoint}<input className="field" value={endpoint} onChange={(e) => setEndpoint(e.target.value)} /></label>
      <p className="note" style={{ marginTop: 8 }}>{t.onlineNote}</p>
      {s.remoteError && <p className="note" style={{ color: "var(--bad)" }}>{s.remoteError}</p>}
      {running && <div style={{ marginTop: 10 }}><Meter value={s.job.progress} /></div>}
      <table className="table" style={{ marginTop: 12 }}>
        <thead><tr><th>{t.publisher}</th><th>{t.title}</th><th>{t.remoteCount}</th><th>{t.st}</th></tr></thead>
        <tbody>
          {s.remotes.map((r) => (
            <tr key={r.id} onClick={() => setSel(r.id)} style={{ background: sel === r.id ? "var(--blush)" : undefined, cursor: "pointer" }}>
              <td>{r.publisher}</td>
              <td>{r.title}</td>
              <td className="num">{fmtInt(r.remoteDirCount)} / {fmtInt(r.remoteFileCount)}</td>
              <td>
                <span className={`badge ${r.status === "new" ? "hi" : r.status === "update" ? "warn" : r.status === "local-richer" ? "bad" : "good"}`}>
                  {r.status === "new" ? t.remoteNew : r.status === "update" ? t.remoteUpdate : r.status === "local-richer" ? t.remoteRicher : t.remoteSame}
                </span>
              </td>
            </tr>
          ))}
          {!s.remotes.length && <tr><td colSpan={4} className="note">{t.fetchIndexFirst}</td></tr>}
        </tbody>
      </table>
    </Frame>
  );
}

function MetaOverlay() {
  const s = useStore();
  const t = s.t;
  const [meta, setMeta] = useState({
    publisher: s.selectedPublisher || "",
    title: s.selectedTitle || "",
    contributor: "meika",
    version: "1.3.0",
    date: "2026-11-06",
    comment: "",
  });
  return (
    <Frame
      id="meta"
      title={t.metaTitle}
      narrow
      footer={
        <>
          <span />
          <div className="row">
            <button className="btn" onClick={() => s.openOverlay("meta", false)}>{t.cancel}</button>
            <button className="btn-primary" style={{ width: "auto" }} onClick={() => { s.send({ type: "saveMeta", meta }); s.openOverlay("meta", false); }}>{t.ok}</button>
          </div>
        </>
      }
    >
      {(["publisher", "title", "contributor", "version", "date"] as const).map((k) => (
        <label key={k} style={{ display: "block", marginBottom: 10 }}>
          {k === "publisher" ? t.publisher : k === "title" ? t.title : k === "contributor" ? t.contributor : k === "version" ? t.version : t.date}
          <input className="field" value={meta[k]} onChange={(e) => setMeta({ ...meta, [k]: e.target.value })} />
        </label>
      ))}
      <label>{t.comment}<textarea className="field" rows={4} value={meta.comment} onChange={(e) => setMeta({ ...meta, comment: e.target.value })} /></label>
    </Frame>
  );
}

function LegacyOverlay() {
  const s = useStore();
  const t = s.t;
  const actions = [
    ["extractor", t.extractor],
    ["dumper", t.dumper],
    ["hashRestore", t.hashRestore],
    ["keyStatic", t.keyStatic],
    ["repack", t.repack],
  ] as const;
  return (
    <Frame id="legacy" title={t.legacyTitle} narrow>
      <p className="note">{t.legacyNote}</p>
      <div className="stack" style={{ marginTop: 12 }}>
        {actions.map(([id, label]) => (
          <button key={id} className="btn btn-wide" onClick={() => s.send({ type: "openLegacy", action: id })}>{label}</button>
        ))}
      </div>
      {s.job.kind === "static-key" && s.job.status === "running" && (
        <div style={{ marginTop: 16 }}><Meter value={s.job.progress} /></div>
      )}
    </Frame>
  );
}

function NotesOverlay() {
  const s = useStore();
  return (
    <Frame id="notes" title={s.t.notes} narrow>
      <pre className="note" style={{ whiteSpace: "pre-wrap", fontFamily: "var(--mono)" }}>{s.notes}</pre>
    </Frame>
  );
}

function SettingsOverlay() {
  const s = useStore();
  return (
    <Frame id="settings" title={s.t.settings} narrow>
      <SettingsScreen />
    </Frame>
  );
}

function Palette() {
  const s = useStore();
  const trap = useFocusTrap(true);
  const [q, setQ] = useState("");
  const [i, setI] = useState(0);
  const cmds = useMemo(() => {
    const all = [
      { id: "home", label: s.t.tabs.home, run: () => s.set({ tab: "home", palette: false }) },
      { id: "make", label: s.t.tabs.make, run: () => s.set({ tab: "make", palette: false }) },
      { id: "lib", label: s.t.tabs.library, run: () => s.set({ tab: "library", palette: false }) },
      { id: "about", label: s.t.tabs.about, run: () => s.set({ tab: "about", palette: false }) },
      { id: "settings", label: s.t.settings, run: () => { s.set({ palette: false }); s.openOverlay("settings"); } },
      { id: "browse", label: s.t.browse, run: () => { s.send({ type: "browseExe" }); s.set({ palette: false }); } },
      { id: "s1", label: s.locale === "zh" ? "解包 XP3" : "Extract XP3", run: () => { s.send({ type: "runStep", step: 1 }); s.set({ palette: false }); } },
      { id: "s2", label: s.locale === "zh" ? "收集 Hash" : "Collect hashes", run: () => { s.send({ type: "runStep", step: 2 }); s.set({ palette: false }); } },
      { id: "s3", label: s.locale === "zh" ? "还原资源名" : "Restore names", run: () => { s.send({ type: "runStep", step: 3 }); s.set({ palette: false }); } },
      { id: "online", label: s.t.online, run: () => { s.openOverlay("online"); s.send({ type: "fetchIndex" }); s.set({ palette: false }); } },
      { id: "theme", label: s.theme === "dark" ? s.t.light : s.t.dark, run: () => s.set({ theme: s.theme === "dark" ? "light" : "dark", palette: false }) },
      { id: "lang", label: s.locale === "zh" ? "English" : "中文", run: () => s.set({ locale: s.locale === "zh" ? "en" : "zh", palette: false }) },
      { id: "log", label: s.logOpen ? s.t.hideLog : s.t.showLog, run: () => s.set({ logOpen: !s.logOpen, palette: false }) },
    ];
    // Demo failure injection exists only in a demo/test build with the mock backend
    // attached. The compile-time guard (raw expression) folds to `false` in a
    // production build, so the `failDemo` entry is stripped from the shipped bundle.
    if ((import.meta.env.DEV || import.meta.env.MODE === "demo" || import.meta.env.MODE === "test") && Boolean(window.__YUA_MOCK__)) {
      all.push({ id: "fail", label: s.t.demoFail, run: () => { s.send({ type: "failDemo" }); s.set({ palette: false, tab: "home" }); } });
    }
    const qq = q.trim().toLowerCase();
    return qq ? all.filter((c) => c.label.toLowerCase().includes(qq)) : all;
  }, [q, s]);
  return (
    <div className="scrim heavy" onMouseDown={(e) => { if (e.target === e.currentTarget) s.set({ palette: false }); }}>
      <div ref={trap} className="dialog narrow palette" role="dialog" aria-modal="true" aria-label={s.t.search}>
        <input autoFocus placeholder={s.t.search} value={q} onChange={(e) => { setQ(e.target.value); setI(0); }}
          onKeyDown={(e) => {
            if (e.key === "ArrowDown") { e.preventDefault(); setI((n) => Math.min(cmds.length - 1, n + 1)); }
            if (e.key === "ArrowUp") { e.preventDefault(); setI((n) => Math.max(0, n - 1)); }
            if (e.key === "Enter") cmds[i]?.run();
            if (e.key === "Escape") s.set({ palette: false });
          }} />
        <div className="body-pad" style={{ padding: 8, maxHeight: 360 }}>
          {cmds.map((c, idx) => (
            <button key={c.id} className="cmd" data-active={idx === i} onMouseEnter={() => setI(idx)} onClick={c.run}>
              {c.label}
            </button>
          ))}
          {!cmds.length && <div className="empty">{s.t.noMatch}</div>}
        </div>
      </div>
    </div>
  );
}

function ConfirmDialog() {
  const s = useStore();
  const c = s.confirm!;
  const ref = useFocusTrap(true);
  return (
    <div className="scrim">
      <div ref={ref} className="dialog narrow" role="alertdialog" aria-modal="true" aria-labelledby="confirm-title">
        <header><h3 id="confirm-title">{c.title}</h3></header>
        <div className="body-pad"><p className="note" style={{ whiteSpace: "pre-wrap" }}>{c.body}</p></div>
        <footer>
          <span />
          <div className="row">
            <button className="btn" onClick={() => c.resolve(false)}>{c.cancel}</button>
            <button className={c.danger ? "btn-danger" : "btn-primary"} style={{ width: "auto" }} onClick={() => c.resolve(true)}>{c.ok}</button>
          </div>
        </footer>
      </div>
    </div>
  );
}

function Toasts() {
  const s = useStore();
  useEffect(() => {
    if (!s.toasts.length) return;
    const id = s.toasts[0].id;
    const timer = window.setTimeout(() => s.dismissToast(id), 3800);
    return () => window.clearTimeout(timer);
  }, [s.toasts, s]);
  const modalOpen = s.palette || !!s.confirm || Object.values(s.overlays).some(Boolean);
  return (
    <div className={`toasts${modalOpen ? " over-modal" : ""}`} aria-live="polite">
      {s.toasts.map((toast) => (
        <button key={toast.id} className="toast" data-level={toast.level} onClick={() => s.dismissToast(toast.id)}>
          <b>{toast.title}</b>
          {toast.body && <span className="note">{toast.body}</span>}
        </button>
      ))}
    </div>
  );
}

export function LogDrawer() {
  const s = useStore();
  const last = s.logs[s.logs.length - 1];
  const taskOpen = s.overlays.restore || s.overlays.collect || s.overlays.batch;
  return (
    <div className="log-drawer" data-open={s.logOpen ? "true" : "false"} style={{ height: s.logOpen ? s.logHeight : 32 }}>
      <button type="button" className="log-h" onClick={() => s.set({ logOpen: !s.logOpen })}>
        <div className="row">
          <span>&gt;_ {s.t.logLabel}</span>
          <span style={{ opacity: 0.7 }}>{taskOpen ? s.t.logInWindow : last ? localizeText(last.message, s.locale) : s.t.emptyLogs}</span>
        </div>
        <span>{s.logOpen ? "▾" : "▴"} · {s.logs.length}</span>
      </button>
      {s.logOpen && (
        <VirtualList
          className="log-body"
          items={s.logs}
          rowHeight={20}
          empty={<div className="empty">{s.t.emptyLogs}</div>}
          render={(l) => (
            <div className="log-row">
              <span>{fmtTime(l.ts)}</span>
              <span className={`lvl-${l.level}`}>{l.source}</span>
              <span className={`lvl-${l.level}`}>{localizeText(l.message, s.locale)}</span>
            </div>
          )}
        />
      )}
    </div>
  );
}
