import { useEffect, useMemo, useRef, useState } from "react";
import { AnimatePresence, motion } from "motion/react";
import {
  Activity, ArchiveRestore, Check, ChevronRight, Clock, Database, FileTerminal,
  FolderOpen, FolderOutput, HelpCircle, Library, Lock, MoreHorizontal, RefreshCw, Search,
  Trash2, Upload, Wrench, X, CloudDownload, Pencil, FlaskConical, Download,
} from "lucide-react";
import { fmtInt, fmtPct, midTrunc } from "../lib/format";
import { localizeText } from "../i18n";
import { ChipFilter, Meter, ModeSeg, NeoToggle, PageFrame, PathLine, StatusWord, reduced } from "../ui/widgets";
import { DropArt, Ring } from "../ui/desk";
import { useStore } from "../store";
import type { ExtractMode } from "../types";

export function nextAction(s: ReturnType<typeof useStore>) {
  const t = s.t;
  const w = s.workspace;
  if (s.job.status === "running" || s.job.status === "cancelling") {
    return { label: s.job.status === "cancelling" ? t.cancelling : t.cancel, run: () => s.send({ type: "cancelJob" }), cancel: true };
  }
  if (!w.exe) return { label: t.pickGame, run: () => s.send({ type: "browseExe" }) };
  if (s.mode === "studio") return { label: t.studioSoon, run: () => s.notify(t.studioSoon), disabled: true };
  if (s.mode === "extension") {
    if (!s.selectedTitle) return { label: t.pickSet, run: () => s.notify(t.pickSet) };
    if (!w.staticReady) return { label: t.startStatic, run: () => s.send({ type: "runStep", step: 1 }) };
    if (!w.extractReady) return { label: t.startXp3, run: () => s.send({ type: "runStep", step: 2 }) };
    if (!w.restoreReady) return { label: t.startRestore, run: () => s.send({ type: "runStep", step: 3 }) };
    return { label: t.startRestore, run: () => s.send({ type: "runStep", step: 3 }) };
  }
  if (!w.extractReady) return { label: t.startXp3, run: () => s.send({ type: "runStep", step: 1 }) };
  if (!w.hashReady) return { label: t.startHash, run: () => s.send({ type: "runStep", step: 2 }) };
  return { label: t.startRestore, run: () => s.send({ type: "runStep", step: 3 }) };
}

function pipeItems(s: ReturnType<typeof useStore>) {
  const w = s.workspace;
  const t = s.t;
  const running = s.job.status === "running" || s.job.status === "cancelling";
  const live = `${t.inProgress} · ${Math.round(s.job.progress * 100)}%`;
  const extractJob = running && (s.job.kind === "extract" || s.job.kind === "precheck");
  const staticJob = running && s.job.kind === "static-table";
  const restoreJob = running && s.job.kind === "restore";
  if (s.mode === "extension") {
    const s1 = w.staticReady ? t.generated : staticJob ? live : t.nextStep;
    const s2 = w.extractReady ? t.generated : !w.staticReady ? t.prereq : extractJob ? live : t.nextStep;
    const s3 = w.restoreRate != null ? fmtPct(w.restoreRate) : restoreJob ? live : t.prereq;
    return [
      { n: 1 as const, label: t.pipeStatic, ready: w.staticReady, hint: s1, stat: w.staticReady ? t.generated : staticJob ? live : t.waiting, p: w.staticReady ? 1 : 0 },
      { n: 2 as const, label: t.pipeXp3, ready: w.extractReady, hint: s2, stat: w.extractReady ? midTrunc(w.extractDir, 36) : extractJob ? live : t.waiting, p: w.extractReady ? 1 : 0 },
      { n: 3 as const, label: t.pipeRestore, ready: w.restoreReady, hint: s3, stat: w.restoreRate != null ? `${fmtPct(w.restoreRate)} · ${fmtInt(w.restoredCount)} / ${fmtInt(w.hashedCount)}` : restoreJob ? live : t.waiting, p: w.restoreRate ?? 0 },
    ];
  }
  if (s.mode === "studio") {
    return [1, 2, 3].map((n) => ({ n: n as 1 | 2 | 3, label: t.stepStudio.replace("{n}", String(n)), ready: false, hint: t.modeStudioFull, stat: t.prereq, p: 0 }));
  }
  const h1 = w.extractReady ? t.generated : extractJob ? live : t.nextStep;
  const h2 = w.hashReady ? `${fmtInt(w.hashDirEntries)} / ${fmtInt(w.hashFileEntries)}` : !w.extractReady ? t.prereq : t.nextStep;
  const h3 = w.restoreRate != null ? fmtPct(w.restoreRate) : restoreJob ? live : t.prereq;
  return [
    { n: 1 as const, label: t.pipeXp3, ready: w.extractReady, hint: h1, stat: w.extractReady ? midTrunc(w.extractDir, 36) : extractJob ? live : t.waiting, p: w.extractReady ? 1 : 0 },
    { n: 2 as const, label: t.pipeHash, ready: w.hashReady, hint: h2, stat: w.hashReady ? `${fmtInt(w.hashDirEntries)} / ${fmtInt(w.hashFileEntries)}` : t.waiting, p: w.hashReady ? 1 : 0 },
    { n: 3 as const, label: t.pipeRestore, ready: w.restoreReady, hint: h3, stat: w.restoreRate != null ? `${fmtPct(w.restoreRate)} · ${fmtInt(w.restoredCount)} / ${fmtInt(w.hashedCount)}` : restoreJob ? live : t.waiting, p: w.restoreRate ?? 0 },
  ];
}

function nextIndex(s: ReturnType<typeof useStore>) {
  const items = pipeItems(s);
  const i = items.findIndex((x) => !x.ready);
  return i < 0 ? items.length - 1 : i;
}

export function HomeScreen() {
  const s = useStore();
  const w = s.workspace;
  const t = s.t;
  const next = nextAction(s);
  const items = pipeItems(s);
  const ni = nextIndex(s);
  const [open, setOpen] = useState<number>(ni);
  const [help, setHelp] = useState(false);
  const [q, setQ] = useState("");
  const pubs = useMemo(() => {
    const map = new Map<string, number>();
    for (const e of s.library) map.set(e.publisher, (map.get(e.publisher) || 0) + 1);
    return [...map.entries()].map(([id, count]) => ({ id, label: id, count }));
  }, [s.library]);
  const titles = s.library.filter((e) => {
    const pubOk = !s.selectedPublisher || e.publisher === s.selectedPublisher;
    const qq = q.trim().toLowerCase();
    const textOk = !qq || e.title.toLowerCase().includes(qq) || e.publisher.toLowerCase().includes(qq);
    return pubOk && textOk;
  });
  const setMode = (mode: ExtractMode) => { s.set({ mode }); s.send({ type: "setMode", mode }); };
  const helpText = s.mode === "extension"
    ? t.helpExt
    : s.mode === "studio"
      ? t.studioSoon
      : t.helpClassic;

  return (
    <>
      <div className="col-main">
        <div
          className={`sketch drop-stage${w.exe ? "" : " empty"}`}
          data-hot={s.dropping}
          onClick={() => { if (!w.exe) s.send({ type: "browseExe" }); }}
          onDragOver={(e) => { e.preventDefault(); s.set({ dropping: true }); }}
          onDragLeave={() => s.set({ dropping: false })}
          onDrop={(e) => {
            e.preventDefault();
            s.set({ dropping: false });
            const file = [...e.dataTransfer.files][0];
            const path = (file as File & { path?: string })?.path || file?.name;
            if (path) s.send({ type: "setTarget", path: path.toLowerCase().endsWith(".exe") ? path : `${path}` });
          }}
          role="button"
          tabIndex={0}
          aria-label={w.exe ? t.target : t.dropStrong}
          onKeyDown={(e) => { if (!w.exe && (e.key === "Enter" || e.key === " ")) { e.preventDefault(); s.send({ type: "browseExe" }); } }}
        >
          {!w.exe ? (
            <>
              <DropArt />
              <div className="drop-ico"><Upload size={28} /></div>
              <h2>{t.dropStrong}</h2>
              <p>{t.dropSub}</p>
            </>
          ) : (
            <>
              <div style={{ position: "absolute", top: 12, right: 12, display: "flex", gap: 6 }}>
                <button className="icon-btn" aria-label={t.browse} onClick={(e) => { e.stopPropagation(); s.send({ type: "browseExe" }); }}><FolderOpen size={15} /></button>
                <button className="icon-btn" aria-label={t.clearGame} onClick={(e) => { e.stopPropagation(); s.set({ workspace: { ...s.workspace, exe: null, title: null } }); }}><X size={15} /></button>
              </div>
              <div className="file-row">
                <div className="file-ico"><FileTerminal size={22} /></div>
                <div className="grow">
                  <div style={{ display: "flex", alignItems: "baseline", gap: 8, marginBottom: 4 }}>
                    <h2>{w.exe.split(/[/\\]/).pop()}</h2>
                    <span className="badge">{t.loaded}</span>
                  </div>
                  <PathLine path={w.exe} />
                </div>
              </div>
              <ModeSeg value={s.mode} onChange={setMode} />
            </>
          )}
        </div>

        {w.exe && s.mode === "extension" && (
          <div className="sketch picker">
            <div className="picker-h">
              <b><Library size={15} /> {t.pickSet}</b>
              <label className="search">
                <Search size={14} />
                <input value={q} onChange={(e) => setQ(e.target.value)} placeholder={t.searchSet} />
              </label>
            </div>
            <ChipFilter
              items={pubs}
              selected={s.selectedPublisher}
              onSelect={(id) => {
                const first = s.library.find((e) => e.publisher === id);
                s.set({ selectedPublisher: id, selectedTitle: first?.title ?? null });
                s.send({ type: "selectExtension", publisher: id, title: first?.title ?? null });
              }}
            />
            <ul className="title-list">
              <AnimatePresence initial={false}>
                {titles.map((e) => (
                  <motion.li key={e.id} initial={reduced() ? false : { opacity: 0, y: 6 }} animate={{ opacity: 1, y: 0 }} exit={{ opacity: 0 }} transition={{ duration: 0.16 }}>
                    <button
                      className="title-item"
                      aria-selected={e.title === s.selectedTitle && e.publisher === s.selectedPublisher}
                      onClick={() => {
                        s.set({ selectedPublisher: e.publisher, selectedTitle: e.title });
                        s.send({ type: "selectExtension", publisher: e.publisher, title: e.title });
                      }}
                    >
                      <span>
                        <b>{e.title}</b>
                        <small>{e.publisher}</small>
                      </span>
                      <span className="num">{fmtInt(e.fileCount)} {t.filesUnit}</span>
                    </button>
                  </motion.li>
                ))}
              </AnimatePresence>
              {!titles.length && <li className="empty">{t.emptySel}</li>}
            </ul>
          </div>
        )}

        {w.exe && (
          <div className="stamps">
            {[
              { icon: <FolderOutput size={16} />, name: "Extractor", path: w.extractDir },
              { icon: <Database size={16} />, name: s.mode === "extension" ? "Static" : "Hash", path: s.mode === "extension" ? w.staticHashDir : w.hashDir },
              { icon: <ArchiveRestore size={16} />, name: "Restored", path: w.restoreDir },
            ].map((d) => (
              <div key={d.name} className="stamp" tabIndex={0}>
                <i aria-hidden>{d.icon}</i>
                <span>
                  <em>{d.name}</em>
                  <PathLine path={d.path} max={42} />
                </span>
              </div>
            ))}
          </div>
        )}

        {w.exe && (
          <div className="sketch facts">
            <h3>{t.workspace}</h3>
            <dl className="kv">
              <dt>{t.title}</dt><dd>{w.title || w.exe.split(/[/\\]/).pop()?.replace(/\.exe$/i, "")}</dd>
              <dt>{t.publisher}</dt><dd>{w.publisher || s.selectedPublisher || t.unselected}</dd>
              <dt>{t.workspace}</dt><dd><PathLine path={w.workspace} /></dd>
              <dt>{t.attribution}</dt>
              <dd>{w.attribution === "passed" ? t.attrPass : w.attribution === "forced" ? t.attrForced : w.attribution === "rejected" ? t.attrReject : t.attrNone}</dd>
            </dl>
          </div>
        )}
      </div>

      <aside className="col-side">
        <div className="next-card">
          <div className="stripe" />
          <div className="next-top">
            <button className="help" onClick={() => setHelp((v) => !v)} aria-expanded={help}>
              <HelpCircle size={14} /> {t.flowHelp}
            </button>
            <div className="row">
              <button className="ghost" onClick={() => s.send({ type: "checkStatus" })} title={t.check} aria-label={t.check}>
                <Activity size={14} /><span>{t.check}</span>
              </button>
              <StatusWord status={s.job.status} />
            </div>
          </div>
          {help && <div className="help-pop">{helpText}</div>}
          <button className="btn-primary" disabled={next.disabled} aria-busy={s.job.status === "running" || s.job.status === "cancelling"} onClick={next.run}>
            <span>{next.label}</span>
            {!next.cancel && <ChevronRight size={20} />}
          </button>
          {s.job.status === "error" && (
            <p className="note" style={{ color: "var(--bad)", marginTop: 12 }}>{localizeText(s.job.error || s.job.text, s.locale)}</p>
          )}
          <div className="next-ring-wrap">
            {(() => {
              const running = s.job.status === "running" || s.job.status === "cancelling";
              if (running && (s.job.kind === "extract" || s.job.kind === "precheck")) {
                return <Ring value={s.job.progress} label={s.locale === "zh" ? "解包进度" : "Extract"} sub={`${Math.round(s.job.progress * 100)}%`} />;
              }
              if (running && s.job.kind === "static-table") {
                return <Ring value={s.job.progress} label={s.locale === "zh" ? "静态表" : "Static table"} sub={`${Math.round(s.job.progress * 100)}%`} />;
              }
              if (running && s.job.kind === "restore") {
                return <Ring value={s.job.progress} label={t.result} sub={`${Math.round(s.job.progress * 100)}%`} />;
              }
              if (w.restoreRate != null) {
                return <Ring value={w.restoreRate} label={t.result} sub={`${fmtInt(w.restoredCount)} / ${fmtInt(w.hashedCount)}`} />;
              }
              if (running) {
                return <Ring value={s.job.progress} label={localizeText(s.job.text, s.locale) || t.current} sub={`${Math.round(s.job.progress * 100)}%`} />;
              }
              return <Ring value={0} label={s.locale === "zh" ? "还原尚未开始" : "Restore not started"} sub={t.notStarted} muted />;
            })()}
          </div>
        </div>
        <div className="spine">
          <h3>{t.pipeline}</h3>
          <div className="stat-stack">
            {items.map((it, i) => {
              const expanded = open === it.n;
              return (
                <div key={it.n}>
                  <button
                    className="stat-row"
                    data-tone={i}
                    data-state={it.ready ? "ready" : i === ni ? "next" : "idle"}
                    onClick={() => setOpen(expanded ? 0 : it.n)}
                    aria-expanded={expanded}
                  >
                    <span className="idx">{it.n}</span>
                    <span>
                      <b>{it.label}</b>
                      <small>{it.hint}</small>
                      {expanded && it.stat !== it.hint && <small style={{ marginTop: 6, display: "block" }}>{it.stat}</small>}
                    </span>
                    <span className="spark" aria-hidden>
                      {it.ready ? <Check size={18} /> : i === ni ? <Clock size={18} /> : <Lock size={18} />}
                    </span>
                  </button>
                </div>
              );
            })}
          </div>
        </div>
          <div className="side-dock">
            <button className="btn dock-btn" onClick={() => s.send({ type: "openWorkspace" })} title={t.openWs} aria-label={t.openWs}>
              <FolderOpen size={15} /><span>{t.openWs}</span>
            </button>
            <button className="btn dock-btn" onClick={async () => {
              const ok = await s.ask({ title: t.clean, body: t.cleanBody, ok: t.clean, cancel: t.cancel, danger: true });
              if (ok) s.send({ type: "cleanDirs" });
            }} title={t.clean} aria-label={t.clean}>
              <Trash2 size={15} /><span>{t.clean}</span>
            </button>
            <button className="btn dock-btn" onClick={() => s.openOverlay("legacy")} title={t.legacyTitle} aria-label={t.legacyTitle}>
              <Wrench size={15} /><span>{t.legacyShort}</span>
            </button>
          </div>
      </aside>
    </>
  );
}

export function MakeScreen() {
  const s = useStore();
  const t = s.t;
  const busy = s.job.status === "running" && (s.job.kind === "generate-set" || s.job.kind === "dry-run");
  return (
    <PageFrame>
      <div className="toolbar">
        <div>
          <h1 style={{ margin: 0, fontSize: 22 }}>{t.makeTitle}</h1>
          <p className="note" style={{ margin: "6px 0 0" }}>{t.makeLead}</p>
        </div>
      </div>
      <div className="split-lib">
        <div className="sketch" style={{ padding: 16 }}>
          <div className="stack">
            <button className="btn-primary btn-wide" disabled={busy} onClick={() => s.send({ type: "generateSet" })}>
              <span>{busy && s.job.kind === "generate-set" ? localizeText(s.job.text, s.locale) : t.generate}</span>
              <ChevronRight size={18} />
            </button>
            {busy && <Meter value={s.job.progress} />}
            <div className="grid-2">
              <button className="btn" onClick={() => s.openOverlay("meta")} title={t.editInfo} aria-label={t.editInfo}><Pencil size={14} /><span>{s.locale === "zh" ? "信息" : "Meta"}</span></button>
              <button className="btn" onClick={() => s.send({ type: "dryRunSet" })} title={t.testSet} aria-label={t.testSet}><FlaskConical size={14} /><span>{s.locale === "zh" ? "测试" : "Test"}</span></button>
              <button className="btn" onClick={() => s.send({ type: "installSet" })} title={t.install} aria-label={t.install}><Download size={14} /><span>{s.locale === "zh" ? "安装" : "Install"}</span></button>
              <button className="btn" onClick={() => s.send({ type: "openDraftDir" })} title={t.openDraft} aria-label={t.openDraft}><FolderOpen size={14} /><span>{s.locale === "zh" ? "草稿" : "Draft"}</span></button>
              <button className="btn" onClick={() => s.send({ type: "openLibraryRoot" })} title={t.openLib} aria-label={t.openLib}><Library size={14} /><span>{s.locale === "zh" ? "打开库" : "Library"}</span></button>
              <button className="btn" onClick={() => s.send({ type: "pickSourceDir" })} title={t.pickSource} aria-label={t.pickSource}><FolderOutput size={14} /><span>{s.locale === "zh" ? "来源" : "Source"}</span></button>
              <button className="btn" onClick={() => s.send({ type: "pickTestDir" })} title={t.pickTest} aria-label={t.pickTest}><FolderOutput size={14} /><span>{s.locale === "zh" ? "测试目录" : "Test dir"}</span></button>
            </div>
          </div>
        </div>
        <div className="sketch detail" style={{ padding: 16 }}>
          <h2 style={{ marginTop: 0, fontSize: 14 }}>{t.recent}</h2>
          <p className="note">{["generate-set", "dry-run"].includes(s.job.kind) || /扩展集|干跑|安装|生成|Generate|Dry|Install/i.test(s.lastAction || "") ? (localizeText(s.lastAction, s.locale) || t.notStarted) : t.notStarted}</p>
          <h2 style={{ fontSize: 14 }}>{t.testResult}</h2>
          <p className="note">{localizeText(s.dryRunNote, s.locale) || t.noDry}</p>
          <dl className="kv" style={{ marginTop: 16 }}>
            <dt>{t.sourceDir}</dt><dd><PathLine path={s.sourceOverride || s.workspace.hashDir} fallback={`${t.default}`} /></dd>
            <dt>{t.testDir}</dt><dd><PathLine path={s.testOverride} fallback={t.follow} /></dd>
            <dt>{t.archivePath}</dt><dd><PathLine path={s.workspace.workspace ? `${s.workspace.workspace}\\rules.int` : null} /></dd>
          </dl>
        </div>
      </div>
    </PageFrame>
  );
}

export function LibraryScreen() {
  const s = useStore();
  const t = s.t;
  const [q, setQ] = useState("");
  const [more, setMore] = useState(false);
  const asked = useRef(false);
  useEffect(() => {
    if (asked.current) return;
    asked.current = true;
    // 原生宿主里切页签只读本地状态：索引必须由「检查更新」显式触发，
    // 不在后台自动发网络请求；只有 demo/mock 构建才会自动拉一份演示索引。
    if (Boolean(window.__YUA_MOCK__) && !s.remotes.length) s.send({ type: "fetchIndex" });
  }, [s]);
  const updates = s.remotes.filter((r) => r.status === "update" || r.status === "new").length;
  const pubs = useMemo(() => {
    const map = new Map<string, number>();
    for (const e of s.library) map.set(e.publisher, (map.get(e.publisher) || 0) + 1);
    return [...map.entries()].map(([id, count]) => ({ id, label: id, count }));
  }, [s.library]);
  const titles = s.library.filter((e) => {
    const pubOk = !s.selectedPublisher || e.publisher === s.selectedPublisher;
    const qq = q.trim().toLowerCase();
    return pubOk && (!qq || e.title.toLowerCase().includes(qq) || e.publisher.toLowerCase().includes(qq));
  });
  const current = s.library.find((e) => e.publisher === s.selectedPublisher && e.title === s.selectedTitle);
  return (
    <PageFrame>
      <div className="toolbar">
        <label className="search grow">
          <Search size={14} />
          <input value={q} onChange={(e) => setQ(e.target.value)} placeholder={t.searchSet} />
        </label>
        <button className="btn" onClick={() => s.send({ type: "refreshLibrary" })} aria-label={t.refresh} title={t.refresh}><RefreshCw size={14} /><span>{t.refresh}</span></button>
        <button className="btn" onClick={() => s.send({ type: "openLibraryRoot" })} aria-label={t.openLibRoot} title={t.openLibRoot}><FolderOpen size={14} /><span>{t.openLibRoot}</span></button>
        <button className={`btn${updates ? "" : " ghost"}`} onClick={() => { s.openOverlay("online"); s.send({ type: "fetchIndex" }); }} aria-label={t.updates} title={t.updates}>
          <CloudDownload size={14} /><span>{t.updates}</span>{updates > 0 && <span className="badge hi">{updates}</span>}
        </button>
        <div className="more-wrap">
          <button className="btn" aria-haspopup="menu" aria-expanded={more} aria-label={s.locale === "zh" ? "更多" : "More"} onClick={() => setMore((v) => !v)}>
            <MoreHorizontal size={16} />
          </button>
          {more && (
            <div className="more-menu" role="menu">
              <button role="menuitem" onClick={() => { setMore(false); s.send({ type: "openSelectedSet" }); }}>{t.openSelected}</button>
              <button role="menuitem" onClick={() => { setMore(false); s.send({ type: "selfCheck" }); }}>{t.selfCheck}</button>
              <button role="menuitem" onClick={() => { setMore(false); s.send({ type: "readNotes" }); }}>{t.notes}</button>
            </div>
          )}
        </div>
      </div>
      <ChipFilter
        items={pubs}
        selected={s.selectedPublisher}
        onSelect={(id) => {
          const first = s.library.find((e) => e.publisher === id);
          s.set({ selectedPublisher: id, selectedTitle: first?.title ?? null });
          s.send({ type: "selectExtension", publisher: id, title: first?.title ?? null });
        }}
      />
      <div className="split-lib" style={{ height: "calc(100% - 96px)" }}>
        <div className="sketch" style={{ overflow: "auto" }}>
          {!s.library.length && (
            <div className="empty">
              <DropArt />
              <p>{t.emptyLib}</p>
            </div>
          )}
          <ul className="title-list">
            {titles.map((e) => {
              const remote = s.remotes.find((r) => r.publisher === e.publisher && r.title === e.title);
              return (
                <li key={e.id}>
                  <button
                    className="title-item"
                    aria-selected={e.title === s.selectedTitle && e.publisher === s.selectedPublisher}
                    onClick={() => {
                      s.set({ selectedPublisher: e.publisher, selectedTitle: e.title });
                      s.send({ type: "selectExtension", publisher: e.publisher, title: e.title });
                    }}
                  >
                    <span>
                      <b>{e.title} {remote?.status === "update" && <span className="badge warn">{t.remoteUpdate}</span>}</b>
                      <small>{e.publisher}</small>
                    </span>
                    <span className="num">{fmtInt(e.fileCount)} {t.filesUnit}</span>
                  </button>
                </li>
              );
            })}
          </ul>
        </div>
        <div className="sketch detail" style={{ padding: 16, overflow: "auto" }}>
          <h2 style={{ marginTop: 0, fontSize: 16 }}>{t.libDetail}</h2>
          {!current && <p className="note">{s.library.length ? t.emptySel : t.emptyLibHint}</p>}
          {current && (
            <dl>
              <dt>{t.publisher}</dt><dd>{current.publisher}</dd>
              <dt>{t.title}</dt><dd>{current.title}</dd>
              <dt>{t.labelEntries}</dt><dd className="num">{t.labelDir} {fmtInt(current.dirCount)} / {t.labelFile} {fmtInt(current.fileCount)} / {t.labelExceptions} {current.exceptions}</dd>
              <dt>{t.labelArchiveSha}</dt><dd className="path" title={current.archiveSha}>{current.archiveSha}</dd>
              <dt>{t.labelTargetExe}</dt><dd className="path" title={current.targetSha}>{current.targetSha}</dd>
              <dt>{t.labelSaltSha}</dt><dd className="path" title={current.saltSha}>{current.saltSha}</dd>
              <dt>{t.labelGeneratedInstalled}</dt><dd>{current.generatedAt} · {current.installedAt}</dd>
              <dt>{t.labelSource}</dt><dd>{localizeText(current.source, s.locale)}</dd>
              <dt>{t.labelPath}</dt><dd><PathLine path={current.path} /></dd>
            </dl>
          )}
          <p className="note" style={{ marginTop: 16 }}>{t.cannotVerify}</p>
          <h2 style={{ fontSize: 14 }}>{t.selfCheck}</h2>
          {!s.selfCheck.length && <p className="note">{t.selfCheckNever}</p>}
          {s.selfCheck.map((it) => (
            <div key={it.id} className="row" style={{ marginTop: 8, alignItems: "flex-start" }}>
              <span className={`badge ${it.ok ? "good" : "bad"}`}>{it.ok ? t.pass : t.fail}</span>
              <div>
                <b>{localizeText(it.title, s.locale)}</b>
                <div className="note">{localizeText(it.detail, s.locale)}</div>
              </div>
            </div>
          ))}
        </div>
      </div>
    </PageFrame>
  );
}

export function AboutScreen() {
  const s = useStore();
  const t = s.t;
  const f = s.info?.fingerprint;
  const mods = [
    ["CxdecExtractor.dll", t.mod1],
    ["CxdecExtractorUI.dll", t.mod2],
    ["CxdecStringDumper.dll", t.mod3],
    ["CxdecHashRestore.dll", t.mod4],
    ["CxdecKeyStatic.dll", t.mod5],
    ["CxdecRepacker.dll", t.mod6],
  ];
  return (
    <PageFrame>
      <h1 style={{ marginTop: 0, fontSize: 22 }}>{t.product} {s.info?.version}</h1>
      <p className="note">{t.aboutLead}</p>
      <div className="split-lib" style={{ marginTop: 16, height: "auto" }}>
        <div className="sketch" style={{ padding: 16 }}>
          <h2 style={{ marginTop: 0, fontSize: 14 }}>{t.modules}</h2>
          <div className="about-mods">
            {mods.map(([n, d]) => (
              <div className="mod" key={n}><b className="mono" style={{ fontFamily: "var(--mono)" }}>{n}</b><span className="note">{d}</span></div>
            ))}
          </div>
        </div>
        <div className="sketch" style={{ padding: 16 }}>
          <h2 style={{ marginTop: 0, fontSize: 14 }}>{t.dirPolicy}</h2>
          <dl className="kv">
            <dt>{t.labelToolDir}</dt><dd><PathLine path={s.info?.loaderDir} /></dd>
            <dt>{t.labelLogDir}</dt><dd><PathLine path={s.info?.logDir} /></dd>
            <dt>{t.labelModuleDir}</dt><dd><PathLine path={s.info?.moduleDir} /></dd>
            <dt>{t.labelLibRoot}</dt><dd><PathLine path={s.info?.libraryRoot} /></dd>
          </dl>
          <h2 style={{ fontSize: 14 }}>{t.fingerprint}</h2>
          <dl className="kv">
            <dt>{t.labelProgramFile}</dt><dd>{f?.fileName}</dd>
            <dt>{t.labelFileSize}</dt><dd className="num">{f ? `${fmtInt(f.size)} ${t.bytesUnit}` : "—"}</dd>
            <dt>{t.labelModified}</dt><dd>{f?.modified}</dd>
            <dt>SHA-256</dt><dd className="path" title={f?.sha256}>{f ? `${f.sha256.slice(0, 16)}…` : "—"}</dd>
          </dl>
        </div>
      </div>
      <div className="sketch" style={{ padding: 16, marginTop: 16 }}>
        <h2 style={{ marginTop: 0, fontSize: 14 }}>{t.disclaimer}</h2>
        <p className="note">{t.disclaimerBody}</p>
      </div>
    </PageFrame>
  );
}

export function SettingsScreen() {
  const s = useStore();
  return (
    <div className="stack" style={{ gap: 16 }}>
      <p className="note">{s.t.settingsLead}</p>
      <div>
        <div className="note" style={{ marginBottom: 8 }}>{s.t.theme}</div>
        <NeoToggle checked={s.theme === "dark"} onChange={(on) => s.set({ theme: on ? "dark" : "light" })} label={s.t.darkMode} />
      </div>
      <div>
        <div className="note" style={{ marginBottom: 8 }}>{s.t.lang}</div>
        <div className="row">
          <button className="chip" aria-pressed={s.locale === "zh"} onClick={() => s.set({ locale: "zh" })}>中文</button>
          <button className="chip" aria-pressed={s.locale === "en"} onClick={() => s.set({ locale: "en" })}>English</button>
        </div>
      </div>
    </div>
  );
}

export function HomeControls() {
  return null;
}
