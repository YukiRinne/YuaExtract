import { useState } from "react";
import {
  MODE_HELP, PathField, fmtHms, jobText, leaf, midTrunc, modeLabel, nextAction, nfmt,
  parseCounts, pipelineOf, rstLabel, useHydrateDir, type AppRoute, type DirScene,
} from "./shared";
import { useStore } from "../store";
import type { ExtractMode } from "../types";

export function DirC({ scene }: { scene: DirScene }) {
  const s = useHydrateDir(scene);
  const [route, setRoute] = useState<AppRoute>(scene === "library" ? "library" : "home");
  const loaded = Boolean(s.workspace.exe);
  const restoring = scene === "restore";
  const running = s.job.kind === "restore" && s.job.status === "running";
  return (
    <div className="dir dir-c" data-loaded={loaded ? "1" : "0"} data-running={running ? "1" : "0"}>
      <aside className="c-nav">
        <div className="c-brand">YuaExtract</div>
        {([
          ["home", "首页"],
          ["make", "制作"],
          ["library", "扩展集"],
          ["about", "关于"],
        ] as const).map(([id, label]) => (
          <button key={id} className={!restoring && route === id ? "on" : ""} onClick={() => setRoute(id)}>{label}</button>
        ))}
        <span className="c-nav-foot">{jobText(s)}</span>
        <span className="proto">prototype</span>
      </aside>
      <div className="c-stage">
        {restoring ? <CRestore /> : route === "library" ? <CLibrary /> : <CHome />}
      </div>
    </div>
  );
}

function CHome() {
  const s = useStore();
  const w = s.workspace;
  const steps = pipelineOf(s);
  const next = nextAction(s);
  const pubs = [...new Set(s.library.map((e) => e.publisher))];
  const titles = s.library.filter((e) => e.publisher === s.selectedPublisher);
  const setMode = (mode: ExtractMode) => {
    s.set({ mode });
    s.send({ type: "setMode", mode });
  };
  const runNext = () => {
    if (next.step === "browse") s.send({ type: "browseExe" });
    else if (next.step) s.send({ type: "runStep", step: next.step });
  };
  return (
    <>
      <header className="c-head">
        <div>
          <h1>{w.title || "未选择游戏"}</h1>
          <p>{s.selectedPublisher || "—"} · {modeLabel(s.mode)} · {jobText(s)}</p>
        </div>
        <div className="cta-row">
          <button className="c-ghost" onClick={() => s.send({ type: "browseExe" })}>更改目标</button>
          <button className="cta" disabled={next.disabled} onClick={runNext}>{next.label}</button>
        </div>
      </header>
      <div className="c-home">
        <div className="mode-line">
          <div className="seg" role="radiogroup" aria-label="模式">
            {([
              ["extension", "该作扩展集"],
              ["studio", "会社合集"],
              ["classic", "传统动态"],
            ] as const).map(([id, label]) => (
              <button key={id} role="radio" aria-checked={s.mode === id} disabled={id === "studio"} data-on={s.mode === id} onClick={() => setMode(id)}>{label}</button>
            ))}
          </div>
          <button className="help" title={MODE_HELP[s.mode]} aria-label="模式说明">?</button>
          <PathField path={w.exe} />
        </div>
        <section>
          <h2>进度</h2>
          <ul className="c-steps">
            {steps.map((st) => (
              <li key={st.key} data-current={next.step === st.n}>
                <span className="n">{st.n}</span>
                <span className="grow">{st.label}</span>
                <span>{st.ready ? "完成" : next.step === st.n ? "下一步" : "未开始"}</span>
                {st.ready && st.detail !== "完成" ? <span className="mute">{st.detail}</span> : null}
              </li>
            ))}
          </ul>
        </section>
        <section>
          <h2>目录</h2>
          <ul className="c-files">
            <li><PathField label="Extractor" path={w.extractDir} /></li>
            <li><PathField label="Hash" path={s.mode === "extension" ? w.staticHashDir : w.hashDir} /></li>
            <li><PathField label="Restored" path={w.restoreDir} /></li>
          </ul>
          <h2>扩展集</h2>
          <div className="c-ext">
            <div>
              {pubs.map((p) => (
                <button key={p} data-on={p === s.selectedPublisher} onClick={() => {
                  const first = s.library.find((e) => e.publisher === p);
                  s.set({ selectedPublisher: p, selectedTitle: first?.title ?? null });
                  s.send({ type: "selectExtension", publisher: p, title: first?.title ?? null });
                }}>{p}</button>
              ))}
            </div>
            <ul>
              {titles.map((e) => (
                <li key={e.id} data-on={e.title === s.selectedTitle} onClick={() => {
                  s.set({ selectedTitle: e.title });
                  s.send({ type: "selectExtension", publisher: e.publisher, title: e.title });
                }}>
                  <b>{e.title}</b>
                  <span>{nfmt(e.fileCount)} 文件</span>
                </li>
              ))}
            </ul>
          </div>
        </section>
      </div>
    </>
  );
}

function CLibrary() {
  const s = useStore();
  const current = s.library.find((e) => e.publisher === s.selectedPublisher && e.title === s.selectedTitle);
  const updates = s.remotes.filter((r) => r.status === "update" || r.status === "new").length;
  return (
    <>
      <header className="c-head">
        <div>
          <h1>扩展集</h1>
          <p>{s.library.length} 份档案</p>
        </div>
      </header>
      <div className="tool-row pad">
        <button className="tb" onClick={() => s.send({ type: "refreshLibrary" })}>刷新</button>
        <button className="tb">打开目录</button>
        <button className="tb">打开选中</button>
        <button className="tb">环境自检</button>
        <span className="grow" />
        <button className="tb secondary">在线更新{updates ? <span className="badge">{updates}</span> : null}</button>
      </div>
      <div className="c-split">
        <ul className="c-list">
          {s.library.map((e) => (
            <li key={e.id} data-on={e.title === s.selectedTitle && e.publisher === s.selectedPublisher} onClick={() => s.set({ selectedPublisher: e.publisher, selectedTitle: e.title })}>
              <b>{e.title}</b>
              <span>{e.publisher} · {nfmt(e.fileCount)}</span>
            </li>
          ))}
        </ul>
        <div className="c-detail">
          {current ? (
            <dl>
              <div><dt>会社</dt><dd>{current.publisher}</dd></div>
              <div><dt>作品</dt><dd>{current.title}</dd></div>
              <div><dt>规模</dt><dd>目录 {nfmt(current.dirCount)} / 文件 {nfmt(current.fileCount)}</dd></div>
              <div><dt>哈希</dt><dd><code className="mono-path">{current.archiveSha.slice(0, 16)}…</code></dd></div>
              <div><dt>路径</dt><dd><PathField path={current.path} /></dd></div>
            </dl>
          ) : <p className="mute">从左侧选择一份扩展集</p>}
        </div>
      </div>
    </>
  );
}

function CRestore() {
  const s = useStore();
  const running = s.job.kind === "restore" && s.job.status === "running";
  return (
    <>
      <header className="c-head">
        <div>
          <h1>资源名还原</h1>
          <p className="counts">总 {nfmt(s.restoreStats.total)} · 运行 {nfmt(s.restoreStats.running)} · 排队 {nfmt(s.restoreStats.queued)} · 完成 {nfmt(s.restoreStats.done)} · 失败 {nfmt(s.restoreStats.failed)} · 线程 4</p>
        </div>
        <div className="task-actions">
          <button className="tb">打开输出</button>
          {running
            ? <button className="cta" onClick={() => s.send({ type: "stopRestore" })}>取消</button>
            : <button className="cta">开始</button>}
        </div>
      </header>
      <div className="c-restore">
        <div className="c-dest"><PathField label="输出" path={s.workspace.restoreDir || "Restored_Extractor_Output"} /></div>
        <p className="chk-line">
          <label className="chk" title="未还原文件按 Hash 加后缀落盘"><input type="checkbox" disabled /> 保留未还原文件</label>
          <label className="chk" title="需先有 Hash Seed；验证失败不写"><input type="checkbox" disabled /> 推断性还原</label>
        </p>
        <ul className="c-xfer">
          {s.restoreTasks.map((task) => {
            const c = parseCounts(task.detail);
            return (
              <li key={task.id} data-st={task.status}>
                <div className="c-xfer-h">
                  <b>{task.pack}</b>
                  <span>{rstLabel(task.status)} · {Math.round(task.progress * 100)}%</span>
                </div>
                <div className="meter thick"><i style={{ width: `${Math.round(task.progress * 100)}%` }} /></div>
                <div className="c-xfer-f">
                  <span className="clip" title={task.current}>{task.current ? midTrunc(task.current) : "等待开始"}</span>
                  <span className="num">{c ? `${c.done}/${c.total}` : task.detail.replace(/\s+/g, " ")}</span>
                </div>
              </li>
            );
          })}
        </ul>
        <div className="c-log">
          {s.logs.slice(-40).map((line) => (
            <div key={line.id}><span>{fmtHms(line.ts)}</span> {line.message}</div>
          ))}
        </div>
      </div>
    </>
  );
}
