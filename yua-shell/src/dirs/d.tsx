import { useState } from "react";
import {
  CloudDownload, FolderOpen, HelpCircle, Home, Info, Library, MessageSquare,
  RefreshCw, Search, ShieldCheck, Wrench,
} from "lucide-react";
import { PathField, jobText, leaf, useHydrateDir, type AppRoute, type DirScene } from "./shared";
import { Brand, DirRows, ExtPicker, Ico, LibraryDetail, LogPane, ModeSeg, NextButton, ProgressTable, RecentLogs, RestoreActions, RestoreCounts, RestoreTable, ROUTES, updateCount } from "./light-ui";
import { useStore } from "../store";

export function DirD({ scene }: { scene: DirScene }) {
  const s = useHydrateDir(scene, "light");
  const [route, setRoute] = useState<AppRoute>(scene === "library" ? "library" : "home");
  const restoring = scene === "restore";
  const running = s.job.kind === "restore" && s.job.status === "running";
  return (
    <div className="dir dir-d" data-loaded={s.workspace.exe ? "1" : "0"} data-running={running ? "1" : "0"}>
      <header className="d-top">
        <Brand src="/brand/ameto.png" name="YuaExtract" />
        <nav>
          {ROUTES.map((r) => (
            <button key={r.id} data-on={!restoring && route === r.id} onClick={() => setRoute(r.id)}>
              <Ico icon={r.id === "home" ? Home : r.id === "make" ? Wrench : r.id === "library" ? Library : Info} />
              {r.label}
            </button>
          ))}
        </nav>
        <span className="d-crumb">{leaf(s.workspace.exe)} · {s.selectedTitle || "未选择"}</span>
        <span className="proto">prototype</span>
      </header>
      {restoring ? <DRestore /> : (
        <div className="d-body">
          <aside className="d-side">
            {route === "library" ? <DLibSide /> : (
              <>
                <div className="d-block">
                  <h3>游戏</h3>
                  <b>{leaf(s.workspace.exe)}</b>
                  <PathField path={s.workspace.exe} />
                  <button className="tb" onClick={() => s.send({ type: "browseExe" })}>浏览…</button>
                </div>
                <div className="d-block">
                  <h3>模式</h3>
                  <ModeSeg />
                </div>
                <div className="d-block grow">
                  <h3>扩展集</h3>
                  <ExtPicker />
                </div>
              </>
            )}
          </aside>
          <main className="l-stage" key={route}>
            {route === "library" ? <DLibrary /> : <DHome />}
          </main>
        </div>
      )}
      <footer className="d-foot">
        <span>{jobText(s)}</span>
        <PathField path={s.workspace.workspace} />
        <span className="grow" />
        <span>prototype · mock backend</span>
      </footer>
    </div>
  );
}

function DHome() {
  const s = useStore();
  return (
    <>
      <div className="l-pagehead">
        <div>
          <h1>{s.workspace.title || "未选择游戏"}</h1>
          <p>{s.selectedPublisher || "—"} · {jobText(s)}</p>
        </div>
        <NextButton />
      </div>
      <section>
        <h2>进度</h2>
        <ProgressTable />
      </section>
      <div className="d-split">
        <section>
          <h2>目录</h2>
          <DirRows />
        </section>
        <section>
          <h2>选中扩展集</h2>
          <LibraryDetail />
        </section>
      </div>
      <RecentLogs />
    </>
  );
}

function DLibSide() {
  const s = useStore();
  return (
    <>
      <div className="d-block">
        <h3>筛选</h3>
        <label className="l-search"><Ico icon={Search} /><input placeholder="搜索作品" readOnly /></label>
      </div>
      <div className="d-block grow">
        <h3>列表</h3>
        <ul className="l-plain">
          {s.library.map((e) => (
            <li key={e.id} data-on={e.title === s.selectedTitle && e.publisher === s.selectedPublisher} onClick={() => s.set({ selectedPublisher: e.publisher, selectedTitle: e.title })}>
              <b>{e.title}</b>
              <small>{e.publisher}</small>
            </li>
          ))}
        </ul>
      </div>
    </>
  );
}

function DLibrary() {
  const s = useStore();
  const n = updateCount(s);
  return (
    <>
      <div className="tool-row">
        <button className="tb" onClick={() => s.send({ type: "refreshLibrary" })}><Ico icon={RefreshCw} /> 刷新</button>
        <button className="tb"><Ico icon={FolderOpen} /> 打开目录</button>
        <button className="tb"><Ico icon={ShieldCheck} /> 环境自检</button>
        <button className="tb"><Ico icon={MessageSquare} /> 留言</button>
        <span className="grow" />
        <button className="tb secondary"><Ico icon={CloudDownload} /> 在线更新{n ? <span className="badge">{n}</span> : null}</button>
      </div>
      <LibraryDetail />
      <div className="l-empty soft">
        <Ico icon={HelpCircle} size={20} />
        <div>
          <b>尚未运行环境自检</b>
          <p>只读检查，不会改动游戏或扩展集文件。</p>
        </div>
        <button className="tb">运行自检</button>
      </div>
    </>
  );
}

function DRestore() {
  const s = useStore();
  return (
    <div className="l-restore">
      <div className="task-head">
        <div>
          <b>资源名还原</b>
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
      <div className="l-rest-split">
        <RestoreTable />
        <LogPane />
      </div>
    </div>
  );
}
