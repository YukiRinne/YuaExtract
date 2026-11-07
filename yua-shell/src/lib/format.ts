export function fmtTime(ts: number) {
  const d = new Date(ts);
  return [d.getHours(), d.getMinutes(), d.getSeconds()].map((n) => String(n).padStart(2, "0")).join(":");
}

export function fmtInt(n: number) {
  return n.toLocaleString("zh-CN");
}

export function fmtPct(n: number | null | undefined, digits = 2) {
  if (n == null || Number.isNaN(n)) return "—";
  return `${(n * 100).toFixed(digits)}%`;
}

export function fmtDur(sec: number) {
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  return [h, m, s].map((n) => String(n).padStart(2, "0")).join(":");
}

export function prettyPath(p: string | null | undefined, fallback = "（未选择）") {
  return p || fallback;
}

/** Middle-truncate a path, keeping drive/root and the filename. */
export function midTrunc(p?: string | null, max = 48) {
  if (!p) return "—";
  if (p.length <= max) return p;
  const parts = p.split(/[/\\]/).filter(Boolean);
  const name = parts[parts.length - 1] || p;
  const head = parts[0]?.endsWith(":") ? `${parts[0]}\\${parts[1] || ""}` : parts[0] || "";
  const out = `${head}\\…\\${name}`;
  if (out.length <= max + 8) return out;
  const keep = Math.max(10, max - head.length - 6);
  return `${head}\\…\\${name.slice(-keep)}`;
}
