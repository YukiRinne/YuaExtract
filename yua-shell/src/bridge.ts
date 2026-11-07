import type { HostEvent, HostRequest } from "./types";

export const PROTOCOL_VERSION = 1;

// Demo/mock code is a build-time property, not a runtime query flag: a production
// `vite build` compiles IS_DEMO_BUILD to `false`, so no query flag can turn the
// mock backend on inside the shipped shell (only `?nomock=1` can turn it off in
// dev/demo/test builds).
export const IS_DEMO_BUILD = Boolean(import.meta.env.DEV || import.meta.env.MODE === "demo" || import.meta.env.MODE === "test");

export type HostBridgeMode = "webview2" | "mock" | "unavailable";

export interface HostBridge {
  available: boolean;
  mode: HostBridgeMode;
  post(msg: HostRequest): void;
  subscribe(fn: (ev: HostEvent) => void): () => void;
}

declare global {
  interface Window {
    chrome?: { webview?: WebView2 };
    __YUA_MOCK__?: boolean;
    __YUA_SHOT__?: (name: string) => Promise<void> | void;
    __YUA_STATE__?: unknown;
  }
}

interface WebView2 {
  postMessage: (data: unknown) => void;
  addEventListener: (type: "message", fn: (ev: { data: unknown }) => void) => void;
  removeEventListener: (type: "message", fn: (ev: { data: unknown }) => void) => void;
}

const EVENT_TYPES: ReadonlySet<string> = new Set<HostEvent["type"]>([
  "hello",
  "workspace",
  "library",
  "remoteIndex",
  "job",
  "log",
  "toast",
  "restoreTasks",
  "collect",
  "batch",
  "selfCheck",
  "notes",
  "picked",
  "overlayHint",
  "uiSync",
]);

function parseEvent(data: unknown): HostEvent | null {
  try {
    const raw: unknown = typeof data === "string" ? JSON.parse(data) : data;
    if (!raw || typeof raw !== "object" || Array.isArray(raw)) return null;
    const type = (raw as { type?: unknown }).type;
    if (typeof type !== "string" || !EVENT_TYPES.has(type)) return null;
    return raw as HostEvent;
  } catch {
    return null;
  }
}

export function createHostBridge(): HostBridge {
  const wv = window.chrome?.webview;
  // Raw compile-time expression (not the IS_DEMO_BUILD constant) so that a
  // production `vite build` folds it to `false` and statically eliminates the
  // mock bridge branch below from the shipped bundle.
  const allowMock = (import.meta.env.DEV || import.meta.env.MODE === "demo" || import.meta.env.MODE === "test") && !wv && new URLSearchParams(window.location.search).get("nomock") !== "1";

  if (wv) {
    return {
      available: true,
      mode: "webview2",
      post(msg) {
        wv.postMessage(JSON.stringify({ protocolVersion: PROTOCOL_VERSION, ...msg }));
      },
      subscribe(fn) {
        const handler = (ev: { data: unknown }) => {
          const parsed = parseEvent(ev.data);
          if (parsed) fn(parsed);
        };
        wv.addEventListener("message", handler);
        return () => wv.removeEventListener("message", handler);
      },
    };
  }

  if (!allowMock) {
    // Fail-closed: no host, no fabricated replies. Nothing is posted or emitted.
    return {
      available: false,
      mode: "unavailable",
      post() {},
      subscribe() {
        return () => {};
      },
    };
  }

  // Mock path is installed by mock/backend.ts
  const listeners = new Set<(ev: HostEvent) => void>();
  const mock: HostBridge = {
    available: false,
    mode: "mock",
    post(msg: HostRequest) {
      window.dispatchEvent(new CustomEvent("yua-host-request", { detail: msg }));
    },
    subscribe(fn: (ev: HostEvent) => void) {
      listeners.add(fn);
      return () => listeners.delete(fn);
    },
  };
  window.addEventListener("yua-host-event", ((ev: CustomEvent<HostEvent>) => {
    listeners.forEach((l) => l(ev.detail));
  }) as EventListener);
  return mock;
}

export function emitHostEvent(ev: HostEvent) {
  window.dispatchEvent(new CustomEvent("yua-host-event", { detail: ev }));
}

export function nextReqId() {
  return `r_${Date.now().toString(36)}_${Math.random().toString(36).slice(2, 8)}`;
}
