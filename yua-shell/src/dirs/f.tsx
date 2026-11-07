import { useState } from "react";
import {
  CloudDownload, Flower2, FolderOpen, Home, Info, Library, MessageSquare,
  RefreshCw, Search, ShieldCheck, Wrench,
} from "lucide-react";
import { PathField, jobText, leaf, modeLabel, nfmt, useHydrateDir, type AppRoute, type DirScene } from "./shared";
import { DirRows, Ico, LibraryDetail, LogPane, ModeSeg, NextButton, ProgressTable, RecentLogs, RestoreActions, RestoreCounts, RestoreTable, ROUTES, updateCount } from "./light-ui";
import { useStore } from "../store";

export function DirF({ scene }: { scene: DirScene }) {
  const s = useHydrateDir(scene, "light");
  const [route, setRoute] = useState<AppRoute>(scene === "library" ? "library" : "home");
  const restoring = scene === "restore";
  const running = s.job.kind === "restore" && s.job.status === "running";
  return (
    <div className="dir dir-f" data-loaded={s.workspace.exe ? "1" : "0"} data-running={running ? "1" : "0"}>
      <header className="f-top">
        <span className="f-logo">
          <img src="/brand/ameto.png" alt="" />
          <span>YuaExtract</span>
        </span>
        <nav>
          {ROUTES.map((r) => (
            <button key={r.id} data-on={!restoring && route === r.id} onClick={() => setRoute(r.id)}>
              <Ico icon={r.id === "home" ? Home : r.id === "make" ? Wrench : r.id === "library" ? Library : Info} />
              {r.label}
            </button>
          ))}
        </nav>
        <span className="f-chip"><Ico icon={Flower2} size={14} /> {s.workspace.title || "未选择"}</span>
        <span className="proto">prototype</span>
      </header>
      {restoring ? <FRestore /> : route === "library" ? <FLibrary /> : <FHome />}
    </div>
  );
}

function FHome() {
  const s = useStore();
  return (
    <div className="f-page l-stage">
      <div className="f-hero">
        <img src="/brand/ameto.png" alt="" />
        <div>
          <h1>{s.workspace.title || "未选择游戏"}</h1>
          <p>{s.selectedPublisher || "—"} · {modeLabel(s.mode)} · {jobText(s)}</p>
          <PathField path={s.workspace.exe} />
        </div>
        <NextButton />
      </div>
      <ModeSeg />
      <section>
        <h2>进度</h2>
        <ProgressTable />
      </section>
      <div className="f-cols">
        <section>
          <h2>目录</h2>
          <DirRows />
        </section>
        <section>
          <h2>扩展集</h2>
          <ul className="f-ext">
            {s.library.map((e) => (
              <li key={e.id} data-on={e.title === s.selectedTitle} onClick={() => {
                s.set({ selectedPublisher: e.publisher, selectedTitle: e.title });
                s.send({ type: "selectExtension", publisher: e.publisher, title: e.title });
              }}>
                <b>{e.title}</b>
                <span>{e.publisher} · {nfmt(e.fileCount)}</span>
              </li>
            ))}
          </ul>
        </section>
      </div>
      <RecentLogs />
    </div>
  );
}

function FLibrary() {
  const s = useStore();
  const n = updateCount(s);
  return (
    <div className="f-page l-stage">
      <div className="f-hero slim">
        <div>
          <h1>扩展集</h1>
          <p>{s.library.length} 份档案，按会社整理</p>
        </div>
      </div>
      <div className="tool-row">
        <button className="tb" onClick={() => s.send({ type: "refreshLibrary" })}><Ico icon={RefreshCw} /> 刷新</button>
        <button className="tb"><Ico icon={FolderOpen} /> 打开目录</button>
        <button className="tb"><Ico icon={ShieldCheck} /> 环境自检</button>
        <button className="tb"><Ico icon={MessageSquare} /> 留言</button>
        <span className="grow" />
        <button className="tb secondary"><Ico icon={CloudDownload} /> 在线更新{n ? <span className="badge">{n}</span> : null}</button>
      </div>
      <div className="f-lib">
        <div>
          <label className="l-search"><Ico icon={Search} /><input placeholder="搜索作品" readOnly /></label>
          <ul className="f-ext">
            {s.library.map((e) => (
              <li key={e.id} data-on={e.title === s.selectedTitle} onClick={() => s.set({ selectedPublisher: e.publisher, selectedTitle: e.title })}>
                <b>{e.title}</b>
                <span>{e.publisher}</span>
              </li>
            ))}
          </ul>
        </div>
        <LibraryDetail />
      </div>
    </div>
  );
}

function FRestore() {
  const s = useStore();
  return (
    <div className="f-page">
      <div className="f-hero slim">
        <div>
          <h1>资源名还原</h1>
          <RestoreCounts />
        </div>
        <RestoreActions />
      </div>
      <div className="l-rest-opt">
        <PathField label="输出" path={s.workspace.restoreDir || "Restored_Extractor_Output"} />
        <span>线程 4</span>
        <label className="chk" title="未还原文件按 Hash 加后缀落盘"><input type="checkbox" disabled /> 保留未还原文件</label>
        <label className="chk" title="需先有 Hash Seed；验证失败不写"><input type="checkbox" disabled /> 推断性还原</label>
      </div>
      <ul className="f-xfer">
        {s.restoreTasks.map((task) => (
          <li key={task.id}>
            <div>
              <b>{task.pack}</b>
              <span>{task.status === "running" ? "运行" : task.status === "queued" ? "排队" : task.status} · {Math.round(task.progress * 100)}%</span>
            </div>
            <div className="meter thick"><i style={{ width: `${Math.round(task.progress * 100)}%` }} /></div>
            <small title={task.current}>{task.current || "等待开始"}</small>
          </li>
        ))}
      </ul>
      <LogPane />
    </div>
  );
}
