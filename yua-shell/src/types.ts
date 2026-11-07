export type Locale = "zh" | "en";
export type Theme = "dark" | "light";
export type TabId = "home" | "make" | "library" | "about";
export type ExtractMode = "extension" | "studio" | "classic";
export type JobKind =
  | "idle"
  | "precheck"
  | "static-table"
  | "extract"
  | "collect"
  | "restore"
  | "generate-set"
  | "dry-run"
  | "install-set"
  | "online-index"
  | "online-download"
  | "self-check"
  | "static-key"
  | "legacy";

export type JobStatus = "idle" | "running" | "cancelling" | "success" | "error";

export type OverlayId =
  | "restore"
  | "collect"
  | "batch"
  | "online"
  | "meta"
  | "legacy"
  | "notes"
  | "palette"
  | "confirm"
  | "settings";

export type LogLevel = "debug" | "info" | "warn" | "error";

export interface LogEntry {
  id: number;
  ts: number;
  level: LogLevel;
  source: string;
  message: string;
}

export interface ExtensionEntry {
  id: string;
  publisher: string;
  title: string;
  contributor: string;
  version: string;
  date: string;
  comment: string;
  dirCount: number;
  fileCount: number;
  exceptions: number;
  archiveSha: string;
  targetSha: string;
  siblingSha: string;
  saltSha: string;
  generatedAt: string;
  installedAt: string;
  source: string;
  path: string;
}

export interface RemoteExtension extends ExtensionEntry {
  status: "new" | "same" | "update" | "local-richer";
  remoteDirCount: number;
  remoteFileCount: number;
  remoteSha: string;
}

export interface RestoreTask {
  id: string;
  pack: string;
  status: "queued" | "running" | "done" | "failed" | "mining";
  progress: number;
  current: string;
  detail: string;
  files: number;
}

export interface ExtractTask {
  id: string;
  file: string;
  status: "queued" | "running" | "done" | "failed";
  progress: number;
  detail: string;
  size: string;
}

export interface SelfCheckItem {
  id: string;
  title: string;
  ok: boolean;
  detail: string;
}

export interface HostInfo {
  product: string;
  version: string;
  loaderDir: string;
  logDir: string;
  moduleDir: string;
  libraryRoot: string;
  fingerprint: {
    fileName: string;
    size: number;
    modified: string;
    sha256: string;
  };
  onlineBase: string;
}

export interface WorkspaceSnapshot {
  exe: string | null;
  title: string | null;
  publisher: string | null;
  workspace: string | null;
  extractDir: string | null;
  hashDir: string | null;
  restoreDir: string | null;
  reportPath: string | null;
  staticHashDir: string | null;
  extractReady: boolean;
  hashReady: boolean;
  hashDirEntries: number;
  hashFileEntries: number;
  restoreReady: boolean;
  staticReady: boolean;
  attribution: "none" | "passed" | "forced" | "rejected";
  restoreRate: number | null;
  restoredCount: number;
  hashedCount: number;
  reportReady: boolean;
}

export type DistributiveOmit<T, K extends PropertyKey> = T extends unknown ? Omit<T, K> : never;

type HostRequestMessage =
  | { id: string; type: "ready" }
  | { id: string; type: "browseExe" }
  | { id: string; type: "setTarget"; path: string }
  | { id: string; type: "setMode"; mode: ExtractMode }
  | { id: string; type: "selectExtension"; publisher: string | null; title: string | null }
  | { id: string; type: "runStep"; step: 1 | 2 | 3 }
  | { id: string; type: "checkStatus" }
  | { id: string; type: "openWorkspace" }
  | { id: string; type: "openRestoreDir" }
  | { id: string; type: "openRestoreReport" }
  | { id: string; type: "cleanDirs" }
  | { id: string; type: "cancelJob" }
  | { id: string; type: "generateSet" }
  | { id: string; type: "saveMeta"; meta: Partial<ExtensionEntry> }
  | { id: string; type: "dryRunSet" }
  | { id: string; type: "installSet" }
  | { id: string; type: "openDraftDir" }
  | { id: string; type: "openLibraryRoot" }
  | { id: string; type: "openSelectedSet" }
  | { id: string; type: "pickSourceDir"; clear?: boolean }
  | { id: string; type: "pickTestDir"; clear?: boolean }
  | { id: string; type: "refreshLibrary" }
  | { id: string; type: "selfCheck" }
  | { id: string; type: "readNotes" }
  | { id: string; type: "fetchIndex"; endpoint?: string }
  | { id: string; type: "downloadRemote"; entryId: string; force?: boolean }
  | { id: string; type: "startRestore"; opts: RestoreOptions }
  | { id: string; type: "stopRestore" }
  | { id: string; type: "startCollectDemo" }
  | { id: string; type: "stopGame" }
  | { id: string; type: "clearCollectFiles" }
  | { id: string; type: "openCollectDir" }
  | { id: string; type: "enqueueXp3"; names: string[] }
  | { id: string; type: "startBatch"; outputDir: string; threads: number }
  | { id: string; type: "stopBatch" }
  | { id: string; type: "openLegacy"; action: string; confirmed?: boolean }
  | { id: string; type: "confirmReply"; action: string; accepted: boolean }
  | { id: string; type: "failDemo" }
  | {
      id: string;
      type: "windowControl";
      action: "minimize" | "maximize" | "close" | "drag" | "resize";
      edge?: "n" | "s" | "e" | "w" | "nw" | "ne" | "sw" | "se";
      force?: boolean;
    };

/** Distributes over the request union so every message keeps its own discriminant. */
type WithProtocolVersion<T> = T extends unknown ? T & { protocolVersion?: 1 } : never;

/** Every request the shell sends carries the host protocol version. */
export type HostRequest = WithProtocolVersion<HostRequestMessage>;
export type HostPayload = DistributiveOmit<HostRequest, "id">;

export interface RestoreOptions {
  outputDir: string;
  threads: number;
  keepUnresolved: boolean;
  inferNames: boolean;
}

export type HostEvent =
  | { type: "hello"; info: HostInfo }
  | { type: "workspace"; workspace: WorkspaceSnapshot }
  | { type: "library"; entries: ExtensionEntry[] }
  | { type: "remoteIndex"; items: RemoteExtension[]; base: string; error?: string }
  | { type: "job"; kind: JobKind; status: JobStatus; progress: number; text: string; error?: string }
  | { type: "log"; entry: Omit<LogEntry, "id"> }
  | { type: "toast"; level: LogLevel; title: string; body: string }
  | { type: "restoreTasks"; tasks: RestoreTask[]; stats: RestoreStats; mining?: string }
  | { type: "collect"; running: boolean; uptime: number; pid: number; dirHit: number; dirTotal: number; fileHit: number; fileTotal: number; lines: { dir: string[]; file: string[] } }
  | { type: "batch"; tasks: ExtractTask[]; stats: BatchStats }
  | { type: "selfCheck"; items: SelfCheckItem[] }
  | { type: "notes"; text: string }
  | { type: "picked"; kind: "exe" | "source" | "test" | "output"; path: string }
  | { type: "overlayHint"; overlay: OverlayId; open: boolean }
  | {
      type: "hostConfirm";
      action: string;
      title: string;
      body: string;
      ok: string;
      cancel: string;
      danger?: boolean;
    }
  | {
      type: "uiSync";
      patch: Partial<{
        tab: TabId;
        logOpen: boolean;
        mode: ExtractMode;
        selectedPublisher: string | null;
        selectedTitle: string | null;
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
      }>;
    };

export interface RestoreStats {
  total: number;
  running: number;
  queued: number;
  done: number;
  failed: number;
  restored: number;
  hashed: number;
  kept: number;
  discarded: number;
  suffixFixed: number;
  mined: number;
  pass1Rate: number | null;
  finalRate: number | null;
}

export interface BatchStats {
  total: number;
  batch: number;
  batchTotal: number;
  running: number;
  done: number;
  failed: number;
}

export interface ConfirmSpec {
  title: string;
  body: string;
  ok: string;
  cancel: string;
  danger?: boolean;
  resolve: (ok: boolean) => void;
}
