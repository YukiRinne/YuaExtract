import { useEffect } from "react";
import { useStore } from "../store";
import type { ExtractMode } from "../types";

export type DirId = "a" | "b" | "c" | "d" | "e" | "f";
export type DirScene = "home" | "restore" | "library";
export type AppRoute = "home" | "make" | "library" | "about";

export function parseDirSearch() {
  const q = new URLSearchParams(window.location.search);
  const raw = q.get("dir");
  const dir: DirId | null = raw === "a" || raw === "b" || raw === "c" || raw === "d" || raw === "e" || raw === "f" ? raw : null;
  const sc = q.get("scene");
  const scene: DirScene = sc === "restore" ? "restore" : sc === "library" ? "library" : "home";
  return { dir, scene };
}

export function useHydrateDir(scene: DirScene, theme: "light" | "dark" = "dark") {
  const s = useStore();
  useEffect(() => {
    let alive = true;
    (async () => {
      s.set({
        theme,
        locale: "zh",
        tab: scene === "library" ? "library" : "home",
        selectedPublisher: "Wamsoft",
        selectedTitle: "星夜回廊",
        logOpen: false,
        overlays: {},
        toasts: [],
        palette: false,
        confirm: null,
      });
      s.send({ type: "refreshLibrary" });
      await delay(40);
      if (!alive) return;
      s.send({ type: "browseExe" });
      await delay(120);
      if (!alive) return;
      if (scene === "restore") {
        s.set({ overlays: { restore: true } });
        s.send({
          type: "startRestore",
          opts: {
            outputDir: "Restored_Extractor_Output",
            threads: 4,
            keepUnresolved: false,
            inferNames: false,
          },
        });
      }
    })();
    return () => {
      alive = false;
    };
  }, [scene]);
  return s;
}

function delay(ms: number) {
  return new Promise((r) => setTimeout(r, ms));
}

export function leaf(p?: string | null) {
  if (!p) return "—";
  const parts = p.split(/[/\\]/).filter(Boolean);
  return parts[parts.length - 1] || p;
}

export function midTrunc(p?: string | null) {
  if (!p) return "—";
  if (p.length <= 52) return p;
  const parts = p.split(/[/\\]/).filter(Boolean);
  const name = parts[parts.length - 1];
  const drive = parts[0]?.endsWith(":") ? `${parts[0]}\\${parts[1] || ""}` : parts[0];
  return `${drive}\\…\\${name}`;
}

export async function copyText(text: string) {
  try {
    await navigator.clipboard.writeText(text);
  } catch {
    /* ignore in shot env */
  }
}

export function modeLabel(mode: ExtractMode) {
  if (mode === "extension") return "该作扩展集";
  if (mode === "studio") return "会社合集";
  return "传统动态";
}

export const MODE_HELP: Record<ExtractMode, string> = {
  extension: "用已安装的该作扩展集展开静态 Hash 表，再解包、还原，不必跑游戏做动态采集。",
  studio: "二期预留：跨同一会社的作品复用扩展集。一期不可用。",
  classic: "解包 XP3 → 运行游戏由 Dumper 收集 Hash → 用动态 Hash 还原资源名。",
};

export function pipelineOf(s: ReturnType<typeof useStore>) {
  const w = s.workspace;
  if (s.mode === "extension") {
    return [
      { n: 1 as const, key: "static", label: "静态 Hash 表", ready: w.staticReady, detail: w.staticReady ? "已生成" : "未开始", verb: "生成静态 Hash 表" },
      { n: 2 as const, key: "xp3", label: "动态 XP3 输出", ready: w.extractReady, detail: w.extractReady ? leaf(w.extractDir) : "未开始", verb: "提取动态 XP3" },
      { n: 3 as const, key: "rst", label: "资源名还原", ready: w.restoreReady, detail: pct(w.restoreRate), verb: "打开资源名还原" },
    ];
  }
  return [
    { n: 1 as const, key: "xp3", label: "动态 XP3 输出", ready: w.extractReady, detail: w.extractReady ? leaf(w.extractDir) : "未开始", verb: "开始解包 XP3" },
    { n: 2 as const, key: "hash", label: "动态 Hash 输出", ready: w.hashReady, detail: w.hashReady ? `${w.hashDirEntries} 目录 / ${w.hashFileEntries} 文件` : "未开始", verb: "启动 Hash 收集" },
    { n: 3 as const, key: "rst", label: "资源名还原", ready: w.restoreReady, detail: pct(w.restoreRate), verb: "打开资源名还原" },
  ];
}

export function nextAction(s: ReturnType<typeof useStore>) {
  if (s.mode === "studio") return { label: "会社合集未实现", disabled: true, step: null as 1 | 2 | 3 | "browse" | null };
  if (!s.workspace.exe) return { label: "选择游戏 exe", disabled: false, step: "browse" as const };
  const next = pipelineOf(s).find((st) => !st.ready);
  if (!next) return { label: "三步已完成", disabled: true, step: null };
  return { label: next.verb, disabled: false, step: next.n };
}

export function pct(n: number | null | undefined) {
  if (n == null || Number.isNaN(n)) return "未开始";
  return `${(n * 100).toFixed(2)}%`;
}

export function rstLabel(status: string) {
  return ({ queued: "排队", running: "运行", done: "完成", failed: "失败", mining: "推理" } as Record<string, string>)[status] || status;
}

export function nfmt(n: number) {
  return Math.round(n).toLocaleString("en-US");
}

export function jobText(s: ReturnType<typeof useStore>) {
  if (s.job.status === "running") return s.job.text || "运行中";
  if (s.job.status === "error") return s.job.error || "出错";
  return "空闲";
}

export function parseCounts(detail: string) {
  const m = detail.match(/(\d[\d,]*)\s*\/\s*(\d[\d,]*)/);
  if (!m) return null;
  return { done: m[1], total: m[2] };
}

export function fmtHms(ts: number) {
  const d = new Date(ts);
  return [d.getHours(), d.getMinutes(), d.getSeconds()].map((n) => String(n).padStart(2, "0")).join(":");
}

export function PathField({ path, label }: { path?: string | null; label?: string }) {
  if (!path) return <span className="path-field">{label ? `${label} —` : "—"}</span>;
  return (
    <span className="path-field" title={path}>
      {label ? <span className="path-lab">{label}</span> : null}
      <code className="mono-path">{midTrunc(path)}</code>
      <button type="button" className="copy-btn" onClick={() => void copyText(path)} aria-label="复制路径">
        复制
      </button>
    </span>
  );
}
