import { useEffect } from "react";
import type { DirId, DirScene } from "./shared";
import { parseDirSearch } from "./shared";
import { DirA } from "./a";
import { DirB } from "./b";
import { DirC } from "./c";
import { DirD } from "./d";
import { DirE } from "./e";
import { DirF } from "./f";
import "../styles/dirs.css";
import "../styles/dirs-light.css";

export function DirApp({ dir, scene }: { dir: DirId; scene: DirScene }) {
  useEffect(() => {
    document.documentElement.dataset.dir = dir;
    document.documentElement.dataset.scene = scene;
    document.documentElement.setAttribute("data-theme", dir === "d" || dir === "e" || dir === "f" ? "light" : "dark");
    return () => {
      delete document.documentElement.dataset.dir;
      delete document.documentElement.dataset.scene;
    };
  }, [dir, scene]);

  if (dir === "a") return <DirA scene={scene} />;
  if (dir === "b") return <DirB scene={scene} />;
  if (dir === "c") return <DirC scene={scene} />;
  if (dir === "d") return <DirD scene={scene} />;
  if (dir === "e") return <DirE scene={scene} />;
  return <DirF scene={scene} />;
}

export function readDirRoute() {
  return parseDirSearch();
}
