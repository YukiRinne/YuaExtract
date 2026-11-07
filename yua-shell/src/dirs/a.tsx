import { useState } from "react";
import {
  MODE_HELP, PathField, fmtHms, jobText, leaf, midTrunc, modeLabel, nextAction, nfmt,
  parseCounts, pipelineOf, rstLabel, useHydrateDir, type AppRoute, type DirScene,
} from "./shared";
import { useStore } from "../store";
import type { ExtractMode } from "../types";

export function DirA({ scene }: { scene: DirScene }) {
  const s = useHydrateDir(scene);
  const [route, setRoute] = useState<AppRoute>(scene === "library" ? "library" : "home");
  const loaded = Boolean(s.workspace.exe);
  const restoring = scene === "restore";
  const running = s.job.kind === "restore" && s.job.status === "running";
  return (
    <div className="dir dir-a" data-loaded={loaded ? "1" : "0"} data-running={running ? "1" : "0"}>
      <header className="a-title">
        <span className="a-brand">YuaExtract</span>
        <span className="a-crumb">
          {leaf(s.workspace.exe)}
          <i />
          {modeLabel(s.mode)}
          {restoring ? <><i />还原</> : route !== "home" ? <><i />{route === "library" ? "扩展集" : route}</> : null}
        </span>
        <span className="a-spacer" />
        <span className="proto">prototype</span>
      </header>
      {restoring ? <ARestore /> : (
        <div className="a-body">
          <nav className="a-rail" aria-label="页面">
            {([
              ["home", "主", "首页"],
              ["make", "制", "制作扩展集"],
              ["library", "库", "扩展集"],
              ["about", "关", "关于"],
            ] as const).map(([id, ch, title]) => (
              <button key={id} className={route === id ? "on" : ""} title={title} onClick={() => setRoute(id)}>{ch}</button>
            ))}
          </nav>
          {route === "library" ? <ALibrary /> : <AHome />}
        </div>
      )}
      <footer className="a-status">
        <span>{jobText(s)}</span>
        <span className="sep" />
        <PathField path={s.workspace.workspace} />
        <span className="a-spacer" />
        <span>prototype · mock backend</span>
      </footer>
    </div>
  );
}

function AHome() {
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
    <>
      <aside className="a-side">
        <div className="a-group">
          <div className="a-k">游戏</div>
          <div className="a-file">
            <b>{leaf(w.exe)}</b>
            <PathField path={w.exe} />
          </div>
          <button className="a-quiet" onClick={() => s.send({ type: "browseExe" })}>浏览…</button>
        </div>
        <div className="a-group">
          <div className="a-k">
            模式
            <button className="help" title={MODE_HELP[s.mode]} aria-label="模式说明">?</button>
          </div>
          <div className="seg" role="radiogroup" aria-label="模式">
            {([
              ["extension", "扩展集"],
              ["studio", "会社"],
              ["classic", "传统"],
            ] as const).map(([id, label]) => (
              <button key={id} role="radio" aria-checked={s.mode === id} disabled={id === "studio"} data-on={s.mode === id} onClick={() => setMode(id)}>{label}</button>
            ))}
          </div>
        </div>
        <div className="a-group grow">
          <div className="a-k">扩展集</div>
          {pubs.map((p) => (
            <button key={p} className="a-row" data-on={p === s.selectedPublisher} onClick={() => {
              const first = s.library.find((e) => e.publisher === p);
              s.set({ selectedPublisher: p, selectedTitle: first?.title ?? null });
              s.send({ type: "selectExtension", publisher: p, title: first?.title ?? null });
            }}>{p}</button>
          ))}
          <div className="a-sublist">
            {titles.map((e) => (
              <button key={e.id} className="a-row nested" data-on={e.title === s.selectedTitle} onClick={() => {
                s.set({ selectedTitle: e.title });
                s.send({ type: "selectExtension", publisher: e.publisher, title: e.title });
              }}>
                {e.title}
                <small>{nfmt(e.fileCount)}</small>
              </button>
            ))}
          </div>
        </div>
      </aside>
      <main className="a-main">
        <div className="a-head">
          <h1>{w.title || "未选择游戏"}</h1>
          <p>{modeLabel(s.mode)} · {jobText(s)}</p>
        </div>
        <div className="cta-row">
          <button className="cta" disabled={next.disabled} onClick={runNext}>{next.label}</button>
        </div>
        <section>
          <div className="a-k">进度</div>
          <table className="a-table">
            <thead><tr><th>#</th><th>阶段</th><th>状态</th><th>详情</th></tr></thead>
            <tbody>
              {steps.map((st) => (
                <tr key={st.key} data-current={!st.ready && next.step === st.n}>
                  <td>{st.n}</td>
                  <td>{st.label}</td>
                  <td>{st.ready ? "完成" : next.step === st.n ? "下一步" : "未开始"}</td>
                  <td>{st.detail}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </section>
        <section>
          <div className="a-k">目录</div>
          <ul className="a-dirs">
            <li><PathField label="Extractor" path={w.extractDir} /></li>
            <li><PathField label="Hash" path={s.mode === "extension" ? w.staticHashDir : w.hashDir} /></li>
            <li><PathField label="Restored" path={w.restoreDir} /></li>
          </ul>
        </section>
      </main>
    </>
  );
}

function ALibrary() {
  const s = useStore();
  const current = s.library.find((e) => e.publisher === s.selectedPublisher && e.title === s.selectedTitle);
  const titles = s.library.filter((e) => !s.selectedPublisher || e.publisher === s.selectedPublisher);
  const pubs = [...new Set(s.library.map((e) => e.publisher))];
  const updates = s.remotes.filter((r) => r.status === "update" || r.status === "new").length;
  return (
    <>
      <aside className="a-side">
        <div className="a-group">
          <div className="a-k">筛选</div>
          <input className="a-search" placeholder="搜索作品" readOnly value="" />
          {pubs.map((p) => (
            <button key={p} className="a-row" data-on={p === s.selectedPublisher} onClick={() => {
              const first = s.library.find((e) => e.publisher === p);
              s.set({ selectedPublisher: p, selectedTitle: first?.title ?? null });
            }}>{p}</button>
          ))}
        </div>
        <div className="a-group grow">
          <div className="a-k">列表</div>
          {titles.map((e) => (
            <button key={e.id} className="a-row" data-on={e.title === s.selectedTitle && e.publisher === s.selectedPublisher} onClick={() => s.set({ selectedPublisher: e.publisher, selectedTitle: e.title })}>
              {e.title}
              <small>{nfmt(e.fileCount)}</small>
            </button>
          ))}
        </div>
      </aside>
      <main className="a-main">
        <div className="tool-row">
          <button className="tb" onClick={() => s.send({ type: "refreshLibrary" })}>刷新</button>
          <button className="tb" onClick={() => s.send({ type: "openLibraryRoot" })}>打开目录</button>
          <button className="tb">打开选中</button>
          <button className="tb">环境自检</button>
          <button className="tb">留言</button>
          <span className="a-spacer" />
          <button className="tb secondary">
            在线更新{updates ? <span className="badge">{updates}</span> : null}
          </button>
        </div>
        {current ? (
          <dl className="a-kv">
            <div><dt>会社</dt><dd>{current.publisher}</dd></div>
            <div><dt>作品</dt><dd>{current.title}</dd></div>
            <div><dt>规模</dt><dd>目录 {nfmt(current.dirCount)} / 文件 {nfmt(current.fileCount)} / 例外 {current.exceptions}</dd></div>
            <div><dt>档案</dt><dd><code className="mono-path">{current.archiveSha.slice(0, 16)}…</code></dd></div>
            <div><dt>路径</dt><dd><PathField path={current.path} /></dd></div>
          </dl>
        ) : <p className="mute">未选择扩展集</p>}
      </main>
    </>
  );
}

function ARestore() {
  const s = useStore();
  const running = s.job.kind === "restore" && s.job.status === "running";
  return (
    <div className="a-body restore">
      <div className="a-restore">
        <div className="task-head">
          <div>
            <b>资源名还原</b>
            <span className="counts">
              总 {nfmt(s.restoreStats.total)} · 运行 {nfmt(s.restoreStats.running)} · 排队 {nfmt(s.restoreStats.queued)} · 完成 {nfmt(s.restoreStats.done)} · 失败 {nfmt(s.restoreStats.failed)}
            </span>
          </div>
          <div className="task-actions">
            <button className="tb">打开输出</button>
            {running
              ? <button className="cta" onClick={() => s.send({ type: "stopRestore" })}>取消</button>
              : <button className="cta">开始</button>}
          </div>
        </div>
        <div className="a-toolbar">
          <PathField label="输出" path={s.workspace.restoreDir || "Restored_Extractor_Output"} />
          <label className="tight">线程 <input readOnly value="4" /></label>
          <label className="chk" title="未还原文件会落成「&lt;64位Hash&gt;&lt;按文件头判定的后缀&gt;」；识别不出时落 .bin。">
            <input type="checkbox" disabled /> 保留未还原文件
          </label>
          <label className="chk" title="先普通还原，再挖候选名、哈希验证后写入。需要先跑过步骤2。验证不通过的一律不写。">
            <input type="checkbox" disabled /> 推断性还原
          </label>
        </div>
        <div className="a-split log-first">
          <div className="a-pane">
            <table className="a-table dense">
              <thead>
                <tr><th>封包</th><th>状态</th><th>进度</th><th>当前</th><th className="num">已处理</th></tr>
              </thead>
              <tbody>
                {s.restoreTasks.map((task) => {
                  const c = parseCounts(task.detail);
                  return (
                    <tr key={task.id} data-st={task.status}>
                      <td>{task.pack}</td>
                      <td>{rstLabel(task.status)}</td>
                      <td>
                        <span className="meter" aria-valuenow={Math.round(task.progress * 100)} aria-valuemin={0} aria-valuemax={100}>
                          <i style={{ width: `${Math.round(task.progress * 100)}%` }} />
                        </span>
                        <span className="num">{Math.round(task.progress * 100)}%</span>
                      </td>
                      <td className="clip" title={task.current}>{midTrunc(task.current) === "—" ? task.current : midTrunc(task.current)}</td>
                      <td className="num">{c ? `${c.done}/${c.total}` : task.detail.replace(/\s+/g, " ")}</td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
          <div className="a-log">
            {s.logs.slice(-80).map((line) => (
              <div key={line.id}>
                <span className="ts">{fmtHms(line.ts)}</span>
                <span className={`lv ${line.level}`}>{line.level}</span>
                <span>{line.message}</span>
              </div>
            ))}
          </div>
        </div>
      </div>
    </div>
  );
}
