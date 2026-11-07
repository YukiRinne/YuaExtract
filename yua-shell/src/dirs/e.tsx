import { useState } from "react";
import {
  CloudDownload, FolderOpen, Home, Info, Library, MessageSquare,
  RefreshCw, Search, ShieldCheck, Wrench,
} from "lucide-react";
import { PathField, jobText, leaf, modeLabel, useHydrateDir, type AppRoute, type DirScene } from "./shared";
import { DirRows, ExtPicker, Ico, LibraryDetail, LogPane, ModeSeg, NextButton, ProgressTable, RecentLogs, RestoreActions, RestoreCounts, RestoreTable, ROUTES, updateCount } from "./light-ui";
import { useStore } from "../store";

export function DirE({ scene }: { scene: DirScene }) {
  const s = useHydrateDir(scene, "light");
  const [route, setRoute] = useState<AppRoute>(scene === "library" ? "library" : "home");
  const restoring = scene === "restore";
  const running = s.job.kind === "restore" && s.job.status === "running";
  return (
    <div className="dir dir-e" data-loaded={s.workspace.exe ? "1" : "0"} data-running={running ? "1" : "0"}>
      <aside className="e-rail">
        <img className="e-face" src="/brand/yua.png" alt="" />
        {ROUTES.map((r) => (
          <button key={r.id} data-on={!restoring && route === r.id} title={r.label} onClick={() => setRoute(r.id)}>
            <Ico icon={r.id === "home" ? Home : r.id === "make" ? Wrench : r.id === "library" ? Library : Info} size={18} />
            <span>{r.label}</span>
          </button>
        ))}
        <span className="proto">prototype</span>
      </aside>
      {restoring ? <ERestore /> : route === "library" ? <ELibrary /> : <EHome />}
    </div>
  );
}

function EHome() {
  const s = useStore();
  return (
    <div className="e-work">
      <header className="e-head">
        <div>
          <h1>{s.workspace.title || "未选择游戏"}</h1>
          <p>{s.selectedPublisher || "—"} · {modeLabel(s.mode)} · {jobText(s)}</p>
        </div>
        <NextButton />
      </header>
      <div className="e-grid">
        <section className="e-col">
          <h2>进度</h2>
          <ModeSeg />
          <PathField path={s.workspace.exe} />
          <ProgressTable />
        </section>
        <section className="e-col">
          <h2>目录</h2>
          <DirRows />
          <h2>扩展集</h2>
          <ExtPicker />
        </section>
      </div>
      <RecentLogs />
    </div>
  );
}

function ELibrary() {
  const s = useStore();
  const n = updateCount(s);
  return (
    <div className="e-work">
      <header className="e-head">
        <div>
          <h1>扩展集</h1>
          <p>{s.library.length} 份档案</p>
        </div>
      </header>
      <div className="tool-row">
        <button className="tb" onClick={() => s.send({ type: "refreshLibrary" })}><Ico icon={RefreshCw} /> 刷新</button>
        <button className="tb"><Ico icon={FolderOpen} /> 打开目录</button>
        <button className="tb"><Ico icon={ShieldCheck} /> 环境自检</button>
        <button className="tb"><Ico icon={MessageSquare} /> 留言</button>
        <span className="grow" />
        <button className="tb secondary"><Ico icon={CloudDownload} /> 在线更新{n ? <span className="badge">{n}</span> : null}</button>
      </div>
      <div className="e-lib">
        <div className="e-lib-side">
          <label className="l-search"><Ico icon={Search} /><input placeholder="搜索" readOnly /></label>
          <ul className="l-plain">
            {s.library.map((e) => (
              <li key={e.id} data-on={e.title === s.selectedTitle} onClick={() => s.set({ selectedPublisher: e.publisher, selectedTitle: e.title })}>
                <b>{e.title}</b>
                <small>{e.publisher}</small>
              </li>
            ))}
          </ul>
        </div>
        <LibraryDetail />
      </div>
    </div>
  );
}

function ERestore() {
  const s = useStore();
  return (
    <div className="e-work">
      <header className="e-head">
        <div>
          <h1>资源名还原</h1>
          <RestoreCounts />
        </div>
        <RestoreActions />
      </header>
      <div className="l-rest-opt">
        <PathField label="输出" path={s.workspace.restoreDir || "Restored_Extractor_Output"} />
        <span>线程 4</span>
        <label className="chk" title="未还原文件按 Hash 加后缀落盘"><input type="checkbox" disabled /> 保留未还原文件</label>
        <label className="chk" title="需先有 Hash Seed；验证失败不写"><input type="checkbox" disabled /> 推断性还原</label>
      </div>
      <div className="l-rest-split e-rest">
        <RestoreTable />
        <LogPane />
      </div>
    </div>
  );
}
