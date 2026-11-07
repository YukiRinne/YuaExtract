import { useMemo, useRef, useState, type ReactNode, type UIEvent } from "react";

export function VirtualList<T>({
  items,
  rowHeight,
  render,
  empty,
  className,
  overscan = 8,
}: {
  items: T[];
  rowHeight: number;
  render: (item: T, index: number) => ReactNode;
  empty?: ReactNode;
  className?: string;
  overscan?: number;
}) {
  const ref = useRef<HTMLDivElement>(null);
  const [scroll, setScroll] = useState(0);
  const [h, setH] = useState(320);

  const onScroll = (e: UIEvent<HTMLDivElement>) => {
    setScroll(e.currentTarget.scrollTop);
    setH(e.currentTarget.clientHeight);
  };

  const { start, end, padTop, padBot } = useMemo(() => {
    const start = Math.max(0, Math.floor(scroll / rowHeight) - overscan);
    const visible = Math.ceil(h / rowHeight) + overscan * 2;
    const end = Math.min(items.length, start + visible);
    return {
      start,
      end,
      padTop: start * rowHeight,
      padBot: Math.max(0, (items.length - end) * rowHeight),
    };
  }, [scroll, h, items.length, rowHeight, overscan]);

  if (!items.length) return <div className={className}>{empty}</div>;

  return (
    <div ref={ref} className={`virt ${className ?? ""}`} onScroll={onScroll}>
      <div style={{ height: padTop }} />
      {items.slice(start, end).map((item, i) => (
        <div key={start + i} style={{ height: rowHeight }}>
          {render(item, start + i)}
        </div>
      ))}
      <div style={{ height: padBot }} />
    </div>
  );
}
