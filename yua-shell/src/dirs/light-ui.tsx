import type { LucideIcon } from "lucide-react";
import {
  MODE_HELP, PathField, fmtHms, midTrunc, modeLabel, nextAction, nfmt, parseCounts,
  pipelineOf, rstLabel, type AppRoute,
} from "./shared";
import { useStore } from "../store";
import type { ExtractMode } from "../types";

const stroke = { size: 16, strokeWidth: 1.75 };

export function Ico({ icon: Icon, size = 16 }: { icon: LucideIcon; size?: number }) {
  return <Icon size={size} strokeWidth={1.75} aria-hidden />;
}

export function Brand({ src, name }: { src: string; name: string }) {
  return (
    <span className="l-brand">
      <img src={src} alt="" width={28} height={28} />
      <b>{name}</b>
    </span>
  );
}

export function ModeSeg() {
  const s = useStore();
  const setMode = (mode: ExtractMode) => {
    s.set({ mode });
    s.send({ type: "setMode", mode });
  };
  return (
    <div className="l-mode">
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
    </div>
  );
}

export function NextButton({ extraClass = "cta" }: { extraClass?: string }) {
  const s = useStore();
  const next = nextAction(s);
  const run = () => {
    if (next.step === "browse") s.send({ type: "browseExe" });
    else if (next.step) s.send({ type: "runStep", step: next.step });
  };
  return (
    <button className={extraClass} disabled={next.disabled} onClick={run}>{next.label}</button>
  );
}

export function ProgressTable() {
  const s = useStore();
  const next = nextAction(s);
  const steps = pipelineOf(s);
  return (
    <table className="l-table">
      <thead><tr><th>#</th><th>阶段</th><th>状态</th><th>详情</th></tr></thead>
      <tbody>
        {steps.map((st) => (
          <tr key={st.key} data-current={next.step === st.n}>
            <td className="num">{st.n}</td>
            <td>{st.label}</td>
            <td>{st.ready ? "完成" : next.step === st.n ? "下一步" : "未开始"}</td>
            <td>{st.ready ? st.detail : "—"}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export function DirRows() {
  const s = useStore();
  const w = s.workspace;
  return (
    <ul className="l-dirs">
      <li><PathField label="Extractor" path={w.extractDir} /></li>
      <li><PathField label="Hash" path={s.mode === "extension" ? w.staticHashDir : w.hashDir} /></li>
      <li><PathField label="Restored" path={w.restoreDir} /></li>
    </ul>
  );
}

export function ExtPicker() {
  const s = useStore();
  const pubs = [...new Set(s.library.map((e) => e.publisher))];
  const titles = s.library.filter((e) => e.publisher === s.selectedPublisher);
  return (
    <div className="l-ext">
      <div className="l-ext-pubs">
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
            <span className="num">{nfmt(e.fileCount)} 文件</span>
          </li>
        ))}
      </ul>
    </div>
  );
}

export function RestoreCounts() {
  const s = useStore();
  return (
    <span className="counts">
      总 {nfmt(s.restoreStats.total)} · 运行 {nfmt(s.restoreStats.running)} · 排队 {nfmt(s.restoreStats.queued)} · 完成 {nfmt(s.restoreStats.done)} · 失败 {nfmt(s.restoreStats.failed)}
    </span>
  );
}

export function RestoreActions() {
  const s = useStore();
  const running = s.job.kind === "restore" && s.job.status === "running";
  return (
    <div className="task-actions">
      <button className="tb" onClick={() => s.send({ type: "openRestoreDir" })}>打开输出</button>
      {running
        ? <button className="cta" onClick={() => s.send({ type: "stopRestore" })}>取消</button>
        : <button className="cta" onClick={() => s.send({ type: "startRestore", opts: { outputDir: s.workspace.restoreDir || "Restored_Extractor_Output", threads: 4, keepUnresolved: false, inferNames: false } })}>开始</button>}
    </div>
  );
}

export function RestoreTable() {
  const s = useStore();
  return (
    <table className="l-table dense">
      <thead><tr><th>封包</th><th>状态</th><th>进度</th><th>当前</th><th className="num">已处理</th></tr></thead>
      <tbody>
        {s.restoreTasks.map((task) => {
          const c = parseCounts(task.detail);
          return (
            <tr key={task.id} data-st={task.status}>
              <td>{task.pack}</td>
              <td>{rstLabel(task.status)}</td>
              <td>
                <span className="meter"><i style={{ width: `${Math.round(task.progress * 100)}%` }} /></span>
                <span className="num">{Math.round(task.progress * 100)}%</span>
              </td>
              <td className="clip" title={task.current}>{task.current ? midTrunc(task.current) : "等待开始"}</td>
              <td className="num">{c ? `${c.done}/${c.total}` : task.detail.replace(/\s+/g, " ")}</td>
            </tr>
          );
        })}
      </tbody>
    </table>
  );
}

export function LogPane() {
  const s = useStore();
  return (
    <div className="l-log">
      {s.logs.slice(-80).map((line) => (
        <div key={line.id}><span className="ts">{fmtHms(line.ts)}</span>{line.message}</div>
      ))}
    </div>
  );
}

export function LibraryDetail() {
  const s = useStore();
  const current = s.library.find((e) => e.publisher === s.selectedPublisher && e.title === s.selectedTitle);
  if (!current) {
    return (
      <div className="l-empty">
        <p>从左侧选择一份扩展集</p>
        <small>档案按会社分组，点选后可以打开目录或跑自检。</small>
      </div>
    );
  }
  return (
    <dl className="l-kv">
      <div><dt>会社</dt><dd>{current.publisher}</dd></div>
      <div><dt>作品</dt><dd>{current.title}</dd></div>
      <div><dt>规模</dt><dd>目录 {nfmt(current.dirCount)} / 文件 {nfmt(current.fileCount)} / 例外 {current.exceptions}</dd></div>
      <div><dt>来源</dt><dd>{current.source}</dd></div>
      <div><dt>生成</dt><dd>{current.generatedAt}</dd></div>
      <div><dt>安装</dt><dd>{current.installedAt}</dd></div>
      <div><dt>哈希</dt><dd><code className="mono-path">{current.archiveSha.slice(0, 16)}…</code></dd></div>
      <div><dt>路径</dt><dd><PathField path={current.path} /></dd></div>
    </dl>
  );
}

export function updateCount(s: ReturnType<typeof useStore>) {
  return s.remotes.filter((r) => r.status === "update" || r.status === "new").length;
}

export const ROUTES: { id: AppRoute; label: string }[] = [
  { id: "home", label: "首页" },
  { id: "make", label: "制作" },
  { id: "library", label: "扩展集" },
  { id: "about", label: "关于" },
];

export { stroke, modeLabel };


export function RecentLogs() {
  const s = useStore();
  return (
    <section className="l-recent">
      <h2>最近日志</h2>
      <LogPane />
    </section>
  );
}
