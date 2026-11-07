import { useState } from "react";
import {
  MODE_HELP, PathField, fmtHms, jobText, leaf, midTrunc, modeLabel, nextAction, nfmt,
  parseCounts, pipelineOf, rstLabel, useHydrateDir, type AppRoute, type DirScene,
} from "./shared";
import { useStore } from "../store";
import type { ExtractMode } from "../types";

export function DirB({ scene }: { scene: DirScene }) {
  const s = useHydrateDir(scene);
  const [route, setRoute] = useState<AppRoute>(scene === "library" ? "library" : "home");
  const loaded = Boolean(s.workspace.exe);
  const restoring = scene === "restore";
  const running = s.job.kind === "restore" && s.job.status === "running";
  return (
    <div className="dir dir-b" data-loaded={loaded ? "1" : "0"} data-running={running ? "1" : "0"}>
      <header className="b-top">
        <span className="b-word">YUAEXTRACT</span>
        <nav className="b-tabs">
          {([
            ["home", "首页"],
            ["make", "制作"],
            ["library", "扩展集"],
            ["about", "关于"],
          ] as const).map(([id, label]) => (
            <button key={id} className={!restoring && route === id ? "on" : ""} onClick={() => setRoute(id)}>{label}</button>
          ))}
        </nav>
        <span className="b-env">{s.workspace.title || "未选择"} · {modeLabel(s.mode)}</span>
        <span className="proto">prototype</span>
      </header>
      {restoring ? <BRestore /> : route === "library" ? <BLibrary /> : <BHome />}
    </div>
  );
}

function BHome() {
  const s = useStore();
  const w = s.workspace;
  const pubs = [...new Set(s.library.map((e) => e.publisher))];
  const titles = s.library.filter((e) => e.publisher === s.selectedPublisher);
  const steps = pipelineOf(s);
  const next = nextAction(s);
  const setMode = (mode: ExtractMode) => {
    s.set({ mode });
    s.send({ type: "setMode", mode });
  };
  const runNext = () => {
    if (next.step === "browse") s.send({ type: "browseExe" });
    else if (next.step) s.send({ type: "runStep", step: next.step });
  };
  return (
    <div className="b-body">
      <aside className="b-col">
        <div className="b-col-h">扩展集</div>
        {pubs.map((p) => (
          <div key={p}>
            <button className="b-tree-h" data-on={p === s.selectedPublisher} onClick={() => {
              const first = s.library.find((e) => e.publisher === p);
              s.set({ selectedPublisher: p, selectedTitle: first?.title ?? null });
              s.send({ type: "selectExtension", publisher: p, title: first?.title ?? null });
            }}>{p}</button>
            {p === s.selectedPublisher && titles.map((e) => (
              <button key={e.id} className="b-tree" data-on={e.title === s.selectedTitle} onClick={() => {
                s.set({ selectedTitle: e.title });
                s.send({ type: "selectExtension", publisher: e.publisher, title: e.title });
              }}>
                <span>{e.title}</span>
                <em>{nfmt(e.fileCount)}</em>
              </button>
            ))}
          </div>
        ))}
      </aside>
      <div className="b-work">
        <div className="b-composer">
          <div className="seg" role="radiogroup" aria-label="模式">
            {([
              ["extension", "EXT"],
              ["studio", "STUDIO"],
              ["classic", "CLASSIC"],
            ] as const).map(([id, label]) => (
              <button key={id} role="radio" aria-checked={s.mode === id} disabled={id === "studio"} data-on={s.mode === id} onClick={() => setMode(id)} title={MODE_HELP[id]}>{label}</button>
            ))}
          </div>
          <button className="help" title={MODE_HELP[s.mode]} aria-label="模式说明">?</button>
          <div className="b-url">
            <b>{leaf(w.exe)}</b>
            <PathField path={w.exe} />
          </div>
          <button className="cta" disabled={next.disabled} onClick={runNext}>{next.label}</button>
        </div>
        <div className="b-params">
          <table>
            <thead><tr><th>#</th><th>阶段</th><th>状态</th><th>值</th></tr></thead>
            <tbody>
              {steps.map((st) => (
                <tr key={st.key}>
                  <td>{st.n}</td>
                  <td>{st.label}</td>
                  <td>{st.ready ? "完成" : next.step === st.n ? "下一步" : "未开始"}</td>
                  <td>{st.detail}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
        <div className="b-response">
          <div className="b-resp-h">
            <span>{jobText(s)}</span>
            <span>{s.selectedPublisher || "—"} / {s.selectedTitle || "—"}</span>
          </div>
          <ul className="b-dirs">
            <li><PathField label="Extractor" path={w.extractDir} /></li>
            <li><PathField label="Hash" path={s.mode === "extension" ? w.staticHashDir : w.hashDir} /></li>
            <li><PathField label="Restored" path={w.restoreDir} /></li>
          </ul>
        </div>
      </div>
    </div>
  );
}

function BLibrary() {
  const s = useStore();
  const current = s.library.find((e) => e.publisher === s.selectedPublisher && e.title === s.selectedTitle);
  const updates = s.remotes.filter((r) => r.status === "update" || r.status === "new").length;
  return (
    <div className="b-body">
      <aside className="b-col">
        <div className="b-col-h">筛选</div>
        <input className="a-search" placeholder="搜索" readOnly />
        {s.library.map((e) => (
          <button key={e.id} className="b-tree" data-on={e.title === s.selectedTitle && e.publisher === s.selectedPublisher} onClick={() => s.set({ selectedPublisher: e.publisher, selectedTitle: e.title })}>
            <span>{e.title}</span>
            <em>{nfmt(e.fileCount)}</em>
          </button>
        ))}
      </aside>
      <div className="b-work">
        <div className="tool-row">
          <button className="tb" onClick={() => s.send({ type: "refreshLibrary" })}>刷新</button>
          <button className="tb">打开目录</button>
          <button className="tb">打开选中</button>
          <button className="tb">环境自检</button>
          <span className="grow" />
          <button className="tb secondary">在线更新{updates ? <span className="badge">{updates}</span> : null}</button>
        </div>
        {current && (
          <dl className="b-meta">
            <div><dt>会社</dt><dd>{current.publisher}</dd></div>
            <div><dt>作品</dt><dd>{current.title}</dd></div>
            <div><dt>文件</dt><dd className="num">{nfmt(current.fileCount)}</dd></div>
            <div><dt>哈希</dt><dd><code className="mono-path">{current.archiveSha.slice(0, 16)}…</code></dd></div>
            <div><dt>路径</dt><dd><PathField path={current.path} /></dd></div>
          </dl>
        )}
      </div>
    </div>
  );
}

function BRestore() {
  const s = useStore();
  const running = s.job.kind === "restore" && s.job.status === "running";
  return (
    <div className="b-body restore">
      <aside className="b-col">
        <div className="b-col-h">封包</div>
        {s.restoreTasks.map((task) => (
          <button key={task.id} className="b-tree" data-on={task.status === "running"}>
            <span className={`stxt ${task.status}`}>{rstLabel(task.status)}</span>
            <span>{task.pack}</span>
            <em>{Math.round(task.progress * 100)}</em>
          </button>
        ))}
      </aside>
      <div className="b-work">
        <div className="task-head">
          <div>
            <b>还原</b>
            <span className="counts">总 {nfmt(s.restoreStats.total)} · 运行 {nfmt(s.restoreStats.running)} · 排队 {nfmt(s.restoreStats.queued)} · 完成 {nfmt(s.restoreStats.done)} · 失败 {nfmt(s.restoreStats.failed)}</span>
          </div>
          <div className="task-actions">
            <button className="tb">打开输出</button>
            {running
              ? <button className="cta" onClick={() => s.send({ type: "stopRestore" })}>取消</button>
              : <button className="cta">开始</button>}
          </div>
        </div>
        <div className="b-toolbar-slim">
          <PathField label="输出" path={s.workspace.restoreDir || "Restored_Extractor_Output"} />
          <span>线程 4</span>
          <label className="chk" title="未还原文件按 Hash 加后缀落盘"> <input type="checkbox" disabled /> 保留未还原文件</label>
          <label className="chk" title="需先有 Hash Seed；验证失败不写"> <input type="checkbox" disabled /> 推断性还原</label>
        </div>
        <div className="b-params">
          <table>
            <thead><tr><th>封包</th><th>状态</th><th>进度</th><th>当前条目</th><th>已处理</th></tr></thead>
            <tbody>
              {s.restoreTasks.map((task) => {
                const c = parseCounts(task.detail);
                return (
                  <tr key={task.id}>
                    <td>{task.pack}</td>
                    <td>{rstLabel(task.status)}</td>
                    <td className="num">
                      <span className="meter"><i style={{ width: `${Math.round(task.progress * 100)}%` }} /></span>
                      {Math.round(task.progress * 100)}%
                    </td>
                    <td className="clip" title={task.current}>{task.current ? midTrunc(task.current) : "等待开始"}</td>
                    <td className="num">{c ? `${c.done}/${c.total}` : task.detail.replace(/\s+/g, " ")}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </div>
        <div className="b-response log">
          <div className="b-log">
            {s.logs.slice(-70).map((line) => (
              <div key={line.id}><span className="ts">{fmtHms(line.ts)}</span>{line.message}</div>
            ))}
          </div>
        </div>
      </div>
    </div>
  );
}
