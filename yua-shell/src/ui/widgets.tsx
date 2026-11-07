import { useLayoutEffect, useRef, useState, type ReactNode } from "react";
import { AnimatePresence, motion } from "motion/react";
import { Boxes, Building2, Check, Copy, Workflow } from "lucide-react";
import { midTrunc } from "../lib/format";
import { useStore } from "../store";
import type { ExtractMode } from "../types";

export function reduced() {
  return typeof matchMedia === "function" && matchMedia("(prefers-reduced-motion: reduce)").matches;
}

export function PathLine({ path, fallback, max = 48 }: { path?: string | null; fallback?: string; max?: number }) {
  const s = useStore();
  const [ok, setOk] = useState(false);
  if (!path) return <span className="note">{fallback ?? s.t.unselected}</span>;
  return (
    <span className="path-line">
      <span className="path" title={path}>{midTrunc(path, max)}</span>
      <button
        type="button"
        aria-label={s.t.copied}
        onClick={async (e) => {
          e.stopPropagation();
          try { await navigator.clipboard.writeText(path); } catch { /* ignore */ }
          setOk(true);
          window.setTimeout(() => setOk(false), 1200);
        }}
      >
        {ok ? <Check size={13} /> : <Copy size={13} />}
      </button>
    </span>
  );
}

export function StatusWord({ status }: { status: string }) {
  const s = useStore();
  const st = status === "running" || status === "cancelling" ? "running" : status === "success" ? "success" : status === "error" ? "error" : "idle";
  const label = st === "running" ? (status === "cancelling" ? s.t.cancelling : s.t.running) : st === "success" ? s.t.done : st === "error" ? s.t.failed : s.t.idle.replace(/[（）()]/g, "") || "空闲";
  return (
    <span className="status-word" data-st={st}>
      <i aria-hidden />
      <span>{label}</span>
    </span>
  );
}

export function ModeSeg({ value, onChange }: { value: ExtractMode; onChange: (m: ExtractMode) => void }) {
  const s = useStore();
  const modes: { id: ExtractMode; label: string; disabled?: boolean; Icon: typeof Boxes }[] = [
    { id: "extension", label: s.locale === "en" ? "Extension" : "扩展集", Icon: Boxes },
    { id: "studio", label: s.locale === "en" ? "Studio" : "合集（二期）", disabled: true, Icon: Building2 },
    { id: "classic", label: s.locale === "en" ? "Classic" : "传统动态", Icon: Workflow },
  ];
  if (s.locale === "en") {
    modes[0].label = "Extension set";
    modes[1].label = "Studio";
    modes[2].label = "Classic";
  }
  const wrap = useRef<HTMLDivElement>(null);
  const btnRefs = useRef<Array<HTMLButtonElement | null>>([]);
  const [ind, setInd] = useState({ x: 0, w: 86 });
  useLayoutEffect(() => {
    const sync = () => {
      const i = modes.findIndex((m) => m.id === value);
      const el = btnRefs.current[i];
      const root = wrap.current;
      if (!el || !root) return;
      setInd({ x: el.offsetLeft - 4, w: el.offsetWidth });
    };
    sync();
    window.addEventListener("resize", sync);
    return () => window.removeEventListener("resize", sync);
  }, [value, s.locale]);
  return (
    <div className="mode-seg" ref={wrap} role="radiogroup" aria-label={s.t.mode}>
      <div className="mode-pill" style={{ width: ind.w, transform: `translateX(${ind.x}px)` }} />
      {modes.map((m, i) => (
        <button
          key={m.id}
          ref={(el) => { btnRefs.current[i] = el; }}
          className="mode-btn"
          role="radio"
          aria-checked={value === m.id}
          aria-pressed={value === m.id}
          disabled={m.disabled}
          title={m.disabled ? s.t.studioSoon : undefined}
          onClick={() => { if (!m.disabled) onChange(m.id); }}
        >
          <m.Icon size={13} aria-hidden />
          {m.label}
        </button>
      ))}
    </div>
  );
}

export function Meter({ value, good }: { value: number; good?: boolean }) {
  const p = Math.max(0, Math.min(100, Math.round(value * 100)));
  return (
    <div className={`meter${good ? " good" : ""}`} data-nonzero={p > 0 ? "true" : "false"} role="progressbar" aria-valuemin={0} aria-valuemax={100} aria-valuenow={p}>
      <i style={{ width: `${p}%` }} />
    </div>
  );
}

export function ChipFilter({
  items,
  selected,
  onSelect,
}: {
  items: { id: string; label: string; count?: number }[];
  selected: string | null;
  onSelect: (id: string | null) => void;
}) {
  return (
    <div className="chips" role="listbox">
      <AnimatePresence initial={false}>
        {items.map((it) => (
          <motion.button
            layout
            key={it.id}
            className="chip"
            role="option"
            aria-selected={selected === it.id}
            aria-pressed={selected === it.id}
            onClick={() => onSelect(selected === it.id ? null : it.id)}
            transition={{ duration: reduced() ? 0 : 0.16 }}
          >
            {it.label}
            {it.count != null && <span className="num">{it.count}</span>}
          </motion.button>
        ))}
      </AnimatePresence>
    </div>
  );
}

export function PageFrame({ children }: { children: ReactNode }) {
  return (
    <motion.div className="page page-enter" initial={reduced() ? false : { opacity: 0, y: 8 }} animate={{ opacity: 1, y: 0 }} transition={{ duration: 0.18 }}>
      {children}
    </motion.div>
  );
}

export function PetalBurst({ fire }: { fire: number }) {
  if (!fire || reduced()) return null;
  const petals = Array.from({ length: 28 }, (_, i) => i);
  return (
    <div className="petals" aria-hidden>
      {petals.map((i) => (
        <span
          key={`${fire}-${i}`}
          className="petal"
          style={{
            left: `${6 + (i * 3.3) % 88}%`,
            background: i % 3 === 0 ? "#e94b86" : i % 3 === 1 ? "#f0c0d0" : "#ffd5e0",
            animationDuration: `${1.6 + (i % 5) * 0.18}s`,
            animationDelay: `${(i % 8) * 0.04}s`,
            ["--dx" as string]: `${(i % 2 ? 1 : -1) * (20 + (i * 13) % 60)}px`,
          }}
        />
      ))}
    </div>
  );
}

export function NeoToggle({
  checked, onChange, label,
}: {
  checked: boolean;
  onChange: (v: boolean) => void;
  label: string;
}) {
  return (
    <button type="button" className="neo-toggle" role="switch" aria-checked={checked} onClick={() => onChange(!checked)}>
      <span className="neo-toggle-track" aria-hidden><span className="neo-toggle-knob" /></span>
      <span>{label}</span>
    </button>
  );
}
