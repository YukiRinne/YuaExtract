import { createContext, useCallback, useContext, useEffect, useMemo, useReducer, type ReactNode } from "react";
import { createHostBridge, nextReqId, type HostBridge } from "./bridge";
import { messages, type Messages } from "./i18n";
import type {
  BatchStats,
  ConfirmSpec,
  ExtractMode,
  ExtractTask,
  ExtensionEntry,
  HostEvent,
  HostInfo,
  HostPayload,
  HostRequest,
  Locale,
  LogEntry,
  OverlayId,
  RemoteExtension,
  RestoreStats,
  RestoreTask,
  SelfCheckItem,
  TabId,
  Theme,
  WorkspaceSnapshot,
} from "./types";

export interface ToastItem {
  id: number;
  level: "debug" | "info" | "warn" | "error";
  title: string;
  body: string;
}

export interface UiState {
  locale: Locale;
  theme: Theme;
  accent: string;
  ink: string;
  tab: TabId;
  mode: ExtractMode;
  info: HostInfo | null;
  workspace: WorkspaceSnapshot;
  library: ExtensionEntry[];
  selectedPublisher: string | null;
  selectedTitle: string | null;
  logs: LogEntry[];
  logOpen: boolean;
  sideWidth: number;
  logHeight: number;
  toasts: ToastItem[];
  overlays: Partial<Record<OverlayId, boolean>>;
  job: { kind: string; status: string; progress: number; text: string; error?: string };
  restoreTasks: RestoreTask[];
  restoreStats: RestoreStats;
  mining?: string;
  collect: {
    running: boolean;
    uptime: number;
    pid: number;
    dirHit: number;
    dirTotal: number;
    fileHit: number;
    fileTotal: number;
    lines: { dir: string[]; file: string[] };
  };
  batchTasks: ExtractTask[];
  batchStats: BatchStats;
  remotes: RemoteExtension[];
  remoteBase: string;
  remoteError?: string;
  selfCheck: SelfCheckItem[];
  notes: string;
  sourceOverride: string;
  testOverride: string;
  lastAction: string;
  dryRunNote: string;
  draftMeta: {
    publisher: string;
    title: string;
    contributor: string;
    version: string;
    date: string;
    comment: string;
  };
  dropping: boolean;
  confirm: ConfirmSpec | null;
  palette: boolean;
}

const emptyWs: WorkspaceSnapshot = {
  exe: null,
  title: null,
  publisher: null,
  workspace: null,
  extractDir: null,
  hashDir: null,
  restoreDir: null,
  reportPath: null,
  staticHashDir: null,
  extractReady: false,
  hashReady: false,
  hashDirEntries: 0,
  hashFileEntries: 0,
  restoreReady: false,
  staticReady: false,
  attribution: "none",
  restoreRate: null,
  restoredCount: 0,
  hashedCount: 0,
  reportReady: false,
};

const initial: UiState = {
  locale: "zh",
  theme: "light",
  accent: "#d4537e",
  ink: "#2a2440",
  tab: "home",
  mode: "classic",
  info: null,
  workspace: emptyWs,
  library: [],
  selectedPublisher: null,
  selectedTitle: null,
  logs: [],
  logOpen: false,
  sideWidth: 386,
  logHeight: 220,
  toasts: [],
  overlays: {},
  job: { kind: "idle", status: "idle", progress: 0, text: "" },
  restoreTasks: [],
  restoreStats: {
    total: 0, running: 0, queued: 0, done: 0, failed: 0,
    restored: 0, hashed: 0, kept: 0, discarded: 0, suffixFixed: 0, mined: 0,
    pass1Rate: null, finalRate: null,
  },
  collect: {
    running: false, uptime: 0, pid: 0, dirHit: 0, dirTotal: 0, fileHit: 0, fileTotal: 0,
    lines: { dir: [], file: [] },
  },
  batchTasks: [],
  batchStats: { total: 0, batch: 0, batchTotal: 0, running: 0, done: 0, failed: 0 },
  remotes: [],
  remoteBase: "",
  selfCheck: [],
  notes: "",
  sourceOverride: "",
  testOverride: "",
  lastAction: "",
  dryRunNote: "",
  draftMeta: {
    publisher: "",
    title: "",
    contributor: "",
    version: "1.0.0",
    date: new Date().toISOString().slice(0, 10),
    comment: "",
  },
  dropping: false,
  confirm: null,
  palette: false,
};

type Action =
  | { type: "patch"; patch: Partial<UiState> }
  | { type: "overlay"; id: OverlayId; open: boolean }
  | { type: "dismissToast"; id: number }
  | { type: "event"; ev: HostEvent };

let logSeq = 1;
let toastSeq = 1;

function reducer(state: UiState, action: Action): UiState {
  if (action.type === "patch") return { ...state, ...action.patch };
  if (action.type === "overlay") {
    return {
      ...state,
      overlays: { ...state.overlays, [action.id]: action.open },
      palette: action.id === "palette" ? action.open : state.palette,
    };
  }
  if (action.type === "dismissToast") {
    return { ...state, toasts: state.toasts.filter((t) => t.id !== action.id) };
  }
  const ev = action.ev;
  switch (ev.type) {
    case "hello":
      return { ...state, info: ev.info, remoteBase: ev.info.onlineBase };
    case "workspace":
      return { ...state, workspace: ev.workspace };
    case "library": {
      // 宿主重扫库后，旧选择可能已经不存在（安装/删除/改名）。保留仍然有效的选择，
      // 否则落到第一份，避免界面出现「库有内容但详情空着」的悬空状态。
      const entries = ev.entries;
      const stillThere = entries.some(
        (e) => e.publisher === state.selectedPublisher && e.title === state.selectedTitle,
      );
      if (stillThere) return { ...state, library: entries };
      const first = entries[0];
      return {
        ...state,
        library: entries,
        selectedPublisher: first?.publisher ?? null,
        selectedTitle: first?.title ?? null,
      };
    }
    case "remoteIndex":
      return { ...state, remotes: ev.items, remoteBase: ev.base, remoteError: ev.error, lastAction: ev.error ? `索引失败：${ev.error}` : `索引拉取成功：共 ${ev.items.length} 条` };
    case "job":
      return {
        ...state,
        job: { kind: ev.kind, status: ev.status, progress: ev.progress, text: ev.text, error: ev.error },
        lastAction: ev.text || state.lastAction,
        dryRunNote: ev.kind === "dry-run" && ev.status === "success" ? ev.text : state.dryRunNote,
      };
    case "log": {
      const recent = state.logs.slice(-4);
      if (recent.some((x) => x.message === ev.entry.message && x.source === ev.entry.source && x.level === ev.entry.level)) {
        return state;
      }
      return { ...state, logs: [...state.logs, { id: logSeq++, ...ev.entry }].slice(-5000) };
    }
    case "toast": {
      const next = { id: toastSeq++, ...ev };
      const filtered = state.toasts.filter((x) => x.title !== ev.title);
      return { ...state, toasts: [...filtered, next].slice(-3) };
    }
    case "restoreTasks":
      return { ...state, restoreTasks: ev.tasks, restoreStats: ev.stats, mining: ev.mining };
    case "collect":
      return { ...state, collect: ev };
    case "batch":
      return { ...state, batchTasks: ev.tasks, batchStats: ev.stats };
    case "selfCheck":
      return { ...state, selfCheck: ev.items };
    case "notes":
      return { ...state, notes: ev.text, overlays: { ...state.overlays, notes: true } };
    case "picked":
      if (ev.kind === "source") return { ...state, sourceOverride: ev.path };
      if (ev.kind === "test") return { ...state, testOverride: ev.path };
      return state;
    case "overlayHint":
      return { ...state, overlays: { ...state.overlays, [ev.overlay]: ev.open } };
    case "hostConfirm":
      return {
        ...state,
        confirm: {
          title: ev.title,
          body: ev.body,
          ok: ev.ok,
          cancel: ev.cancel,
          danger: ev.danger,
          resolve: (ok) => {
            if (!bridgeRef) bridgeRef = createHostBridge();
            bridgeRef.post({ id: nextReqId(), type: "confirmReply", action: ev.action, accepted: ok });
          },
        },
      };
    case "uiSync":
      return { ...state, ...ev.patch };
    default:
      return state;
  }
}

interface Store extends UiState {
  t: Messages;
  send: (msg: HostPayload) => void;
  set: (patch: Partial<UiState>) => void;
  openOverlay: (id: OverlayId, open?: boolean) => void;
  dismissToast: (id: number) => void;
  ask: (spec: Omit<ConfirmSpec, "resolve">) => Promise<boolean>;
  notify: (title: string, body?: string, level?: ToastItem["level"]) => void;
}

function rgbOf(hex: string): [number, number, number] {
  const raw = hex.trim().replace("#", "");
  const full = raw.length === 3 ? raw.split("").map((c) => c + c).join("") : raw;
  const n = Number.parseInt(full, 16);
  if (!Number.isFinite(n) || full.length !== 6) return [212, 83, 126];
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255];
}

function mix(a: [number, number, number], b: [number, number, number], towardB: number) {
  const c = (i: number) => Math.round(a[i] + (b[i] - a[i]) * towardB);
  return `rgb(${c(0)} ${c(1)} ${c(2)})`;
}

const Ctx = createContext<Store | null>(null);
let bridgeRef: HostBridge | null = null;

export function StoreProvider({ children }: { children: ReactNode }) {
  const [state, dispatch] = useReducer(reducer, initial);

  useEffect(() => {
    document.documentElement.dataset.theme = state.theme;
    document.documentElement.lang = state.locale === "en" ? "en" : "zh-CN";
    const accent = rgbOf(state.accent);
    const ink = rgbOf(state.ink);
    const ground = state.theme === "dark" ? rgbOf("#141018") : rgbOf("#f7f2f5");
    const wash = state.theme === "dark" ? 0.86 : 0.9;
    const root = document.documentElement.style;
    root.setProperty("--hibiscus", state.accent);
    root.setProperty("--accent", state.accent);
    root.setProperty("--ink", state.ink);
    root.setProperty("--paper", mix(accent, ground, wash));
    root.setProperty("--surface", mix(accent, ground, wash - 0.08));
    root.setProperty("--blush", mix(accent, ground, wash - 0.16));
    root.setProperty("--line", mix(accent, ground, 0.55));
    root.setProperty("--dim", mix(ink, ground, 0.42));
  }, [state.theme, state.locale, state.accent, state.ink]);

  useEffect(() => {
    const saved = localStorage.getItem("yua-shell-prefs");
    if (saved) {
      try {
        const p = JSON.parse(saved);
        dispatch({ type: "patch", patch: { theme: p.theme ?? "light", locale: p.locale ?? "zh", accent: typeof p.accent === "string" ? p.accent : "#d4537e", ink: typeof p.ink === "string" ? p.ink : "#2a2440" } });
      } catch { /* ignore */ }
    }
  }, []);

  useEffect(() => {
    localStorage.setItem("yua-shell-prefs", JSON.stringify({ theme: state.theme, locale: state.locale, accent: state.accent, ink: state.ink }));
  }, [state.theme, state.locale, state.accent, state.ink]);

  useEffect(() => {
    if (!bridgeRef) bridgeRef = createHostBridge();
    const unsub = bridgeRef.subscribe((ev) => dispatch({ type: "event", ev }));
    bridgeRef.post({ id: nextReqId(), type: "ready" });
    return unsub;
  }, []);

  const send = useCallback((msg: HostPayload) => {
    if (!bridgeRef) bridgeRef = createHostBridge();
    bridgeRef.post(Object.assign({ id: nextReqId() }, msg) as HostRequest);
  }, []);

  const set = useCallback((patch: Partial<UiState>) => dispatch({ type: "patch", patch }), []);

  const openOverlay = useCallback((id: OverlayId, open = true) => {
    dispatch({ type: "overlay", id, open });
  }, []);

  const dismissToast = useCallback((id: number) => {
    dispatch({ type: "dismissToast", id });
  }, []);

  const notify = useCallback((title: string, body = "", level: ToastItem["level"] = "info") => {
    dispatch({ type: "event", ev: { type: "toast", level, title, body } });
  }, []);

  const ask = useCallback((spec: Omit<ConfirmSpec, "resolve">) => {
    return new Promise<boolean>((resolve) => {
      dispatch({
        type: "patch",
        patch: {
          confirm: {
            ...spec,
            resolve: (ok) => {
              dispatch({ type: "patch", patch: { confirm: null } });
              resolve(ok);
            },
          },
        },
      });
    });
  }, []);

  const value = useMemo<Store>(() => ({
    ...state,
    t: messages(state.locale),
    send,
    set,
    openOverlay,
    dismissToast,
    ask,
    notify,
  }), [state, send, set, openOverlay, dismissToast, ask, notify]);

  useEffect(() => {
    window.__YUA_STATE__ = state;
  }, [state]);

  return <Ctx.Provider value={value}>{children}</Ctx.Provider>;
}

export function useStore() {
  const s = useContext(Ctx);
  if (!s) throw new Error("store");
  return s;
}
