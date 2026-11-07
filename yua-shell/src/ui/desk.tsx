import { useEffect, useState } from "react";
import { reduced } from "./widgets";

export function DeskBackdrop() {
  const [pos, setPos] = useState({ x: 0, y: 0 });
  useEffect(() => {
    if (reduced()) return;
    const onMove = (e: PointerEvent) => {
      const nx = (e.clientX / window.innerWidth - 0.5) * 2;
      const ny = (e.clientY / window.innerHeight - 0.5) * 2;
      setPos({ x: nx, y: ny });
    };
    window.addEventListener("pointermove", onMove);
    return () => window.removeEventListener("pointermove", onMove);
  }, []);
  const far = { transform: `translate3d(${pos.x * 8}px, ${pos.y * 6}px, 0)` };
  const mid = { transform: `translate3d(${pos.x * -14}px, ${pos.y * -9}px, 0)` };
  const near = { transform: `translate3d(${pos.x * 18}px, ${pos.y * -12}px, 0)` };
  return (
    <div className="desk-bg" aria-hidden>
      <div className="cx-far" style={far}>
        <div className="blob blob-a" />
        <div className="blob blob-b" />
        <div className="blob blob-c" />
      </div>
      <svg className="construct cx-mid" viewBox="0 0 1440 900" preserveAspectRatio="xMidYMid slice" style={mid}>
        <defs>
          <pattern id="riso" width="7" height="7" patternUnits="userSpaceOnUse">
            <circle cx="1.2" cy="1.2" r="0.7" fill="#2a2440" fillOpacity="0.16" />
          </pattern>
          <filter id="paper" x="-20%" y="-20%" width="140%" height="140%">
            <feTurbulence type="fractalNoise" baseFrequency="0.9" numOctaves="2" stitchTiles="stitch" result="n" />
            <feColorMatrix in="n" type="saturate" values="0" result="g" />
            <feBlend in="SourceGraphic" in2="g" mode="multiply" />
          </filter>
        </defs>
        <circle cx="-40" cy="80" r="320" fill="#c9b8e8" fillOpacity="0.7" filter="url(#paper)" />
        <circle cx="1520" cy="-60" r="360" fill="#e8b7c8" fillOpacity="0.62" filter="url(#paper)" />
        <rect x="-80" y="380" width="720" height="54" transform="rotate(-15 -80 380)" fill="#2a4aa8" fillOpacity="0.38" />
        <rect width="1440" height="900" fill="url(#riso)" opacity="0.4" />
      </svg>
      <svg className="construct cx-near" viewBox="0 0 1440 900" preserveAspectRatio="xMidYMid slice" style={near}>
        <circle cx="280" cy="780" r="260" fill="#efe6dc" fillOpacity="0.7" />
        <polygon points="-40,920 640,380 80,980" fill="#e94b86" fillOpacity="0.55" />
        <rect x="1180" y="420" width="420" height="70" transform="rotate(30 1180 420)" fill="#5b46c9" fillOpacity="0.32" />
        <text
          x="1040" y="280" transform="rotate(-28 1040 280)"
          fontFamily="Arial Black, Impact, sans-serif" fontSize="200" fontWeight="900"
          fill="#2a2440" fillOpacity="0.14"
        >YUA</text>
      </svg>
      <svg className="grain" xmlns="http://www.w3.org/2000/svg">
        <filter id="grainf">
          <feTurbulence type="fractalNoise" baseFrequency="0.85" numOctaves="4" stitchTiles="stitch" />
          <feColorMatrix type="saturate" values="0" />
        </filter>
        <rect width="100%" height="100%" filter="url(#grainf)" opacity="0.55" />
      </svg>
    </div>
  );
}

export function greeting(locale: "zh" | "en", hour = new Date().getHours()) {
  if (locale === "en") {
    if (hour < 5 || hour >= 18) return "Good evening";
    if (hour < 12) return "Good morning";
    return "Good afternoon";
  }
  if (hour < 5 || hour >= 18) return "晚上好";
  if (hour < 12) return "早上好";
  return "下午好";
}

export function Ring({ value, label, sub, muted }: { value: number; label: string; sub?: string; muted?: boolean }) {
  const p = Math.max(0, Math.min(1, value));
  const r = 36;
  const c = 2 * Math.PI * r;
  const dash = `${c * p} ${c}`;
  return (
    <div className="ring" data-muted={muted ? "true" : "false"} role="img" aria-label={`${label} ${Math.round(p * 100)}%`}>
      <div className="ring-well">
        <svg width="96" height="96" viewBox="0 0 96 96">
          <circle cx="48" cy="48" r={r} fill="none" stroke="color-mix(in srgb, var(--line) 70%, transparent)" strokeWidth="8" />
          <circle
            cx="48" cy="48" r={r} fill="none" stroke="var(--hibiscus)" strokeWidth="8"
            strokeLinecap="round" strokeDasharray={dash} transform="rotate(-90 48 48)"
          />
        </svg>
        <span className="ring-disc num">{Math.round(p * 100)}%</span>
      </div>
      <div>
        <b>{label}</b>
        {sub && <small>{sub}</small>}
      </div>
    </div>
  );
}

export function DropArt() {
  return (
    <svg className="drop-art" viewBox="0 0 160 88" aria-hidden width="160" height="88">
      <rect x="18" y="28" width="52" height="44" rx="8" fill="#f3d5e2" stroke="#e94b86" strokeWidth="2" strokeDasharray="5 4" />
      <path d="M34 28 v-8 h20 v8" fill="none" stroke="#e94b86" strokeWidth="2" />
      <rect x="86" y="22" width="56" height="50" rx="8" fill="#e8e0ff" stroke="#5b46c9" strokeWidth="2" />
      <path d="M98 38 h32 M98 48 h24 M98 58 h18" stroke="#5b46c9" strokeWidth="3" strokeLinecap="round" />
      <path d="M72 48 h10" stroke="#2a2440" strokeWidth="2" strokeLinecap="round" />
      <path d="M78 42 l8 6 -8 6" fill="none" stroke="#2a2440" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  );
}
