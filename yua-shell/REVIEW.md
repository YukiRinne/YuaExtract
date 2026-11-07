# YuaExtract shell review

## Round 1 (self)

Visual QA against `shots/v1/` (1440×900). Every item below was fixed before recapturing `shots/r1/`.

### Major (6)

1. **Toast stack buried every modal footer.** Selecting a game (and every later shot’s `browseExe`) pushed another「已选择目标」card. Cards stacked at the bottom-right and covered Restore / Collect / Online primary actions (开始, 下载并安装选中, 返回主程序).  
   **Fix:** Deduplicate by title, cap at 3, auto-dismiss after 3.8s, move the stack under the titlebar (z-index below dialogs), clear toasts when entering a screenshot scene.

2. **「显示详细日志」floated over the main-flow steps.** The absolute toggle sat on top of 步骤2, and 辅助操作 / 主流程 looked unreachable.  
   **Fix:** Dedicated bottom status bar for the log toggle + job caption + mock/WebView2 badge. Compact sidebar sections and raise **主流程** directly under the mode picker so steps 1–3 are on the first screen at 900px.

3. **Windows paths rendered with doubled backslashes** (`D:\\Games\\星夜回廊\\…`). Mock strings were over-escaped (`D:\\\\` in source).  
   **Fix:** Normalize every host path to `D:\\Games\\…` / `D:\\Tools\\…` in `src/mock/backend.ts`.

4. **Restore table was unreadable mid-run.** Status wrapped one character per line (运 / 行 / 中), detail columns smashed, and the headline showed「还原率 0.00%（已还原 55,000 / 60,133）」while tasks were still at ~30%.  
   **Fix:** `nowrap` + ellipsis with `title` tooltips, a per-row mini progress bar, and mock stats that only publish `finalRate` when every pack has finished (running totals start from 0).

5. **Extension picker looked like a two-column table and paired the wrong title with the wrong studio.** `晨星工作室` sat on the same row as `白虹之庭` (a Wamsoft title).  
   **Fix:** Studio chips on one row; titles listed underneath with an explicit `作品 · {会社}` caption and dir/file counts.

6. **Section labels used `text-transform: uppercase`**, turning「游戏的 exe 程序」into「游戏的 EXE 程序」and shouting Latin fragments in a Chinese UI.  
   **Fix:** Drop uppercase; keep a modest letter-spacing on section titles.

### Minor (12)

1. Titlebar search control truncated to「搜索命令…」— replaced with「命令」+ `Ctrl+K` and a full tooltip.  
2. Duplicate browse toasts even for a single selection — deduped.  
3. Collect progress bars looked empty because 1–6% fills are tiny — bars are taller (10px) and keep numeric labels.  
4. Dialog footers did not wrap, so trailing actions clipped — `flex-wrap` on footer.  
5. Command palette list clipped the last commands — scrollable body (`max-height: 360px`).  
6. Truncated paths had no tooltip — `title` on the exe field and restore “当前条目”.  
7. Screenshot scenes leaked state (`library-empty` wiped the picker for `light-home`) — scenes now refresh the library and restore the default Wamsoft / 星夜回廊 selection.  
8. Escape only closed the palette — it now dismisses confirmations and the topmost overlay (notes → meta → online → legacy → batch → collect → restore).  
9. Sidebar had no bottom inset, so the last control sat under the old FAB — padding + status bar.  
10. Restore live rate / mined copy appeared before the job had a rate — gated on `finalRate`.  
11. Online-update shots fired while the index job text still said「正在拉取…」— wait lengthened; footer still shows host job text when a fetch is in flight (faithful).  
12. No persistent chrome indicating mock vs real host — status bar badge `mock` / `WebView2`.

### Leftovers for later rounds

- Restore “当前条目” hashes are still visually noisy (ellipsis helps, a monospace single-line clip could go further).  
- Light theme inherits workspace readiness from earlier flows in a long-lived mock session (shots now isolate better; a live user session is correct).  
- Focus trap / `aria-modal` cycle inside dialogs is incomplete.  
- English copy is complete for chrome but some overview paragraphs on Home stay Chinese (mode intros).

## R1 (direction 3 · Hibiscus desk)

Visual QA against `shots/R1/` (1440×900) after implementing Superdesign direction 3 as the default app, then folding in meika’s dashboard reference (floating white window, pastel blobs, colorful stat rows, restore ring) and a low-contrast constructivist backdrop (Rodchenko/Lissitzky geometry in pink / lavender / aqua / navy). Every item below was fixed before recapturing `shots/R1/`.

### Major (6)

1. **Character art sat as a square crop on top of the workspace stamps and log text**, so Extractor/Hash/Restored paths were unreadable — nothing like the reference “figure peeking off the window.”  
   **Fix:** Circular sticker on the *outside* left edge (`public/brand/ameto.png`), window inset increased (`36/48/36/96`) so the peek no longer covers controls.

2. **The shell filled almost the entire viewport**, so the constructivist circles, bars and the diagonal hibiscus wedge were invisible.  
   **Fix:** More margin around the floating 28px-radius window; pastel blobs + SVG geometry + a pink wedge now read behind it, with slow pointer parallax (disabled under `prefers-reduced-motion`).

3. **Screenshot scenes leaked mock workspace state.** After `restore-done`, later shots (e.g. `dark-home`) still showed a 98% restore ring and a “开始还原资源名” CTA because readiness flags survived. Using `cancelJob` to isolate scenes then *cancelled the next restore* (~400ms later), so `restore-done` showed a live 「开始」 while packs were still running.  
   **Fix:** `stopRestore` + `stopBatch` + `cleanDirs` at the start of each scene (no `cancelJob`); `restore-done` waits 4.2s; toasts cleared after host events settle.

4. **Idle home’s drop target was a short stripe band over a blank white void** — weaker than the “obvious animated drop zone” we owed the draft.  
   **Fix:** Empty stage is `flex: 1` with a tall dashed blush field, bouncing upload glyph, and a single primary 「选择游戏」 on the right.

5. **Sidebar auxiliary actions (打开工作区 / 清理目录 / 原版功能) were clipped**, the running CTA still used a “go” chevron on 「取消」, and 「会社合集」 looked like a live mode.  
   **Fix:** Side column scrolls; cancel hides the chevron; reserved mode is labeled 「会社合集（二期）」 and visually disabled.

6. **Late-night greeting said 「早上好」** (hour `0–4`).  
   **Fix:** 18:00–04:59 → 「晚上好」 / “Good evening”.

### Minor (12)

1. Log drawer was a square black strip that visually sawed off the rounded window — inset, 14px radius dock inside the window.  
2. Collect/restore meters were 6px and looked empty at low percentages — 10px tracks with numeric labels.  
3. Library/About reused the home greeting “从右边的下一步开始” — tab-specific subcopy.  
4. Folder stamps middle-truncated into `StringHashDumpe` — keep more of the filename (`midTrunc(..., 40)`).  
5. `HostRequest`’s non-distributive `Omit<, "id">` made `send()` reject every payload in `tsc` (and `downloadRemote` had a duplicate `id`). — `HostPayload` + `entryId`.  
6. Command-palette / settings reachable from the gear and Ctrl+K; Escape now also closes settings.  
7. Disabled 「启动游戏」 already used `disabled` + title; thicker meters make the running collect state clearer.  
8. Default theme is light; dark is a tasteful ink/paper remap (no neon glow) — `dark-home` / `dark-library`.  
9. Restore footer is state-driven: running → 「停止还原」, idle/done → 「开始」 (never an enabled Start mid-run after the isolation fix).  
10. Paths use middle truncation + hover copy + toast; hashes/logs stay IBM Plex Mono.  
11. File counts use tabular numerals (`61,220`).  
12. Petal burst fires once when a restore job reaches `success`, and honors reduced motion.

### Leftovers

- Dialogs are not a complete focus trap.  
- 「检查更新」 only grows a badge after an index fetch (no speculative badge).  
- Constructivist parallax is pointer-based, not a continuous idle drift.  
- English chrome is complete; a few overlay helper sentences remain Chinese.

## R2 (texture · liquid glass · stronger constructivism · more SVG)

Visual QA against `shots/R2/` (1440×900, every scene in light and a matching `*-dark` pass). Directives from meika / [pt10u]: tinted neutrals (no pure black/white), grain + risograph texture, light skeuomorphism / Liquid Glass / layered tinted shadows, bolder Rodchenko/Lissitzky composition with pointer parallax, more SVG and fewer words. R1 leftovers and extra review issues below were fixed before recapture. No push; no Superdesign credit spend. A new DC list was not forwarded mid-run (the existing `yuaextract-r1-critique-2026-11-07.md` was already folded in R1).

### Directives

1. **Texture / no pure B·W.** Tokens retinted (`--surface #FBF8FA`, `--ink #2A2440`, dark desk `#1A1522` plum-navy). SVG `feTurbulence` grain overlay + dotted risograph pattern on backdrop shapes. Log/toast/danger/stat index no longer use `#fff` / `#000`.
2. **Liquid glass + skeuomorphism.** Window (and dialogs / log dock) use `backdrop-filter: blur + saturate` with specular inset highlight, inner light/dark edge, refraction-like mix, and layered tinted ambient+key shadows. Buttons have a soft bevel / inner highlight and a sunk `:active`. `@supports not (backdrop-filter)` falls back to opaque `--surface`. Blur is limited to window + dialog + log (no extra titlebar/stat-card blur).
3. **Stronger constructivism.** Layered SVG: large circles, bars at 15°/30°/45°, hibiscus “Beat the Whites with the Red Wedge” homage, low-contrast rotated **YUA** / **解包**. Far/mid/near parallax; `prefers-reduced-motion` disables it.
4. **More SVG, less text.** Drop-zone illustration, nav/mode/toolbar/dock icons with accessible names + short labels, restore option chips, step glyphs. Greetings and help copy shortened.

### R1 leftovers / findings

- Pipeline sparklines are flat until a step is ready or a live job has `progress > 0`.
- Right-column actions sit in a pinned `side-dock` (打开工作区 no longer clips).
- Restore overlay is a glass dialog with colorful pack rows and an overall ring.
- Dialogs (and confirm) trap Tab and focus the first control.
- Library prefetches `fetchIndex` on mount (and library shots wait), so the update badge appears without opening the online overlay.
- `restore-done` polls `__YUA_STATE__.job` until `success` (shot now shows 6/6 at 100%).

### Major (6, beyond the directive work)

1. **Restore headline progress disagreed with pack rows.** The linear “已处理” meter used `job.progress` (~few percent) while colorful rows already sat around 30%. **Fix:** overall ring = mean of pack progress while running, `finalRate` when done.
2. **`prefers-reduced-motion` used a global `*` rule** that zeroed every transition (mode pill width jump, hover, focus). **Fix:** scope to backdrop parallax, decorative animations, and the running status pulse.
3. **Library toolbar text buttons wrapped and shoved 检查更新 / 留言 off the first row** at 1440. **Fix:** icon + short label tools with `aria-label` / `title`.
4. **Confirm (clean-dirs etc.) and the command palette were not treated as modal** — Tab could escape into the desk, and palette lacked `aria-modal`. **Fix:** shared focus trap on confirm; `aria-modal` on palette.
5. **Primary running CTA had no `aria-busy`**, so AT announced a normal button while a job was in flight. **Fix:** `aria-busy` while running/cancelling.
6. **Scene isolation still left `restore-done` mid-pack** at a fixed 4.2s wait (4 done / 2 running). **Fix:** poll job success up to ~10s, then a short petal beat.

### Minor (12)

1. Home-length greeting leaked onto Library/Make (“从右边的下一步开始”) — tab captions are now short (「库 · 更新 · 自检」, 「下一步在右侧」, 「把游戏拖进来」).
2. Mode switcher was text-only and verbose — icons + 「扩展集 / 合集（二期） / 传统动态」.
3. Nav tabs were text-only — Home / Hammer / Book / Info glyphs beside labels.
4. Restore options were long checkbox paragraphs — chip toggles 「保留未还原」 / 「推测文件名」 with the full copy in `title`.
5. Empty drop zone had only a bouncing upload glyph — added a small exe→list SVG.
6. Stat-row index chips used `rgba(255,255,255)` / `rgba(0,0,0)` — mixed from `--hibiscus-ink` / `--ink`.
7. `.btn-danger` and toast used raw white type — `var(--hibiscus-ink)`.
8. Log-drawer hover used `rgba(255,255,255,0.04)` — mixed from `--hibiscus-ink`.
9. Restore empty table said “勾选选项后点「开始」” — short empty pictogram caption.
10. Flow-help popover was a flat card — glass + tinted shadow so it matches the language.
11. Palette dialog was missing `aria-modal="true"`.
12. Dark window shadow still used a gray-black stack in places — shared `--ambient` / `--key` tokens (plum-tinted).

### Leftovers for later

- A few overlay helper sentences (online TLS note, legacy intro) stay longer than the new caption style.
- Constructivist type is intentionally very low-contrast; it reads more in the detail crops than at full-UI scale.
- English chrome is complete; some overlay helpers remain Chinese.

### DC items (R1 Hibiscus critique, folded into this same R2)

Source: `/workspace/design-critiques/yuaextract-R1-hibiscus-critique-2026-11-07.md`. Items already handled in the first R2 pass were verified, then remaining gaps were closed before recapture.

**Major**
1. **H1 Toasts vs footer/LOG** — Routine successes (选择目标、打开目录、刷新、检查状态、浏览演示…) now write to LOG only. Toasts remain for errors, warnings, and long-task completions (生成/安装/下载/干跑/静态 Key…). While a task window is open, toasts sit top-center (`over-modal`) instead of over the footer/LOG.
2. **H2 Palette over a task window** — Palette uses an opaque tinted-paper surface (no backdrop-filter) and a single heavier scrim (`ink 52%`, z-index 70) so the underlying dialog is inert. Verified in `palette-over-restore.png`. Liquid glass stays on the outer shell.
3. **H3 Pure #FFFFFF** — Hibiscus tokens deepened (`--surface #f3e8ef`, `--paper #eee3eb`, highlights `#f8eef3`); leftover `tokens.css` `--bg-panel:#ffffff` retinted; window fill raised to 88% so glass no longer washes to white. Pixel sample of recaptured shots: **0% #FFFFFF / 0% #000000**.
4. **H4 Running ring** — During unpack the ring is **解包进度** at the live % (`home-running` 60%). Restore before its stage is muted **还原尚未开始**. Title always matches the metric.
5. **H5 Fake sparklines** — Replaced with wait / lock / check glyphs until real throughput exists.
6. **H6 Library toolbar** — Three high-frequency actions (刷新 / 打开扩展集目录 / 检查更新) with SVG + short labels; 自检 / 留言 / 打开选中 in a `⋯` More menu; 检查更新 is ghost/secondary and only badges when updates exist.
7. **H7 Right-column clipping** — Primary CTA + muted/current ring pinned; pipeline scrolls; dock pinned (打开工作区 / 清理 / 原版). 检查状态 is a ghost next to the status word. Verified at 1440×900 and **1100×700** (`home-loaded-min.png`); peek hides under 1100px.

**Minor**
1. File counts show grouped digits + unit (`61,220 文件`).
2. Mode switch maps immediately to pipeline copy/actions; extension picker only appears in extension mode.
3. Collect meters use a darker fill (`#c43a72`) plus numeric percents.
4. Dialog logs are tinted-paper with 22px line height; the dark LOG dock stays for the shell.
5. Restore options are short chips plus a focusable「说明」disclosure (not hover-only).
6. Palette entries: 「解包 XP3 / 收集 Hash / 还原资源名」.
7. Library header is 「扩展集库」 + result count (`3 份`), not the home greeting.
8. LOG dock uses the shell’s large radius (20px inset).
9. Dark theme is purple-gray Hibiscus (`#17141f` / `#221c2e`), not brown charcoal.
10. Avatar has a safe outside-window zone; shrinks at 1200px and hides at ≤1100px.
11. Output path stamps use folder SVG + `PathLine` copy and expand the full path when focused.
12. 「检查状态」 is ghost/secondary beside the status it checks.
13. Texture stays on the desktop; shell/nav keep glass; dialogs, tables, and logs are solid tinted paper.
14. Elevation scale: desktop < window < dialog < opaque palette.
15. Constructivism is a few large cropped, offset, directional blocks (circle, bar, wedge) rather than extra small ornaments.
16. Motion: looping stripe/bob animations removed; transitions stay short (140–200ms, no springs); cancel is the instant primary; reduced-motion still scoped.

## R2 part 3 · Neumorphic controls

Correction: neumorphism is an *accent for interactive controls only*. The R2 liquid-glass shell, tinted-paper/glass panels, pipeline rows, path cards, library inspector, restore dialog, grain, and constructivist backdrop stay as they were.

### What changed

- **Raised controls** use a paired tinted shadow (highlight top-left `--neo-hi` `#f6eef3`, plum shadow bottom-right `--neo-lo`) on the same surface color as their surroundings. **Pressed / selected** states flip to the inset pair (`:active`, `aria-pressed`, selected chips). Short 140ms shadow transitions; no glow. Dark mode has its own purple-gray pair (`#2e273c` / deep plum, not gray-blue or black).
- **Buttons** (`.btn`, dock, toolbar) and **round icon discs** (browse, close, settings) follow that raised → inset press. The hibiscus **primary CTA** keeps the fill but uses a soft dual shadow instead of a glow; `:active` sinks inset.
- **Search / `.field`**: inset wells. **Mode segmented control** and **nav tabs**: inset track + raised sliding/selected pill.
- **Settings theme**: inset-track toggle with a raised knob (`NeoToggle`). Language stays as small raised chips.
- **Progress ring**: inset circular well + hibiscus arc + raised center disc for the percentage.
- **Chips / badges**: small raised pills; pressed = inset.
- Panels/cards were not converted to slabs; window glass was not reduced. Focus remains a hibiscus outline (`:focus-visible`). Reduced-motion scopes the new control transitions.

### Two layout fixes

1. **Restored path card clipped at 1440×900** — `.stamps` wrap and shrink (`flex: 1 1 160px; min-width: 0`) with `PathLine` middle-truncation (`max={28}`).
2. **Empty left column when loaded/running** — a workspace facts panel (real snapshot: title, publisher, workspace path, attribution, Extractor/Hash/Restored readiness and counts). No fake telemetry.

Recapture: `shots/R2/` light+dark, `home-loaded-min.png` (1100×700), `detail-neumorph.png`, `interaction.webm` (~15s).

## R3 (self)

Inspection only (no code this step). Evidence: current `shots/R2/` at 1440×900 light+dark (idle, loaded, running, error, done, restore, palette, library, settings, about, make, empty, EN, collect, batch, online, logs-open) plus `home-loaded-min.png` at 1100×700, `interaction.webm` frames at 1 fps (`/tmp/r3-frames`), and source (`screens.tsx` `pipeItems`, `RestoreOverlay` running guard, toast/cleanDirs, greet). Direction respected: neo on controls, glass panels, tinted paper, grain, constructivism, more SVG / less text.

### Major (6)

1. **Sidebar layout collapses as soon as height is tight.** At 1100×700 (`home-loaded-min`) the pinned dock overlaps pipeline step 1, steps 2–3 vanish, and the facts “Restored” row is eaten by the LOG. The same crush appears at 1440×900 once the log is opened to 220px (`logs-open.png`): only step 1 remains above the dock. “Spine scrolls independently” is technically true but undiscoverable — three step cards are no longer simultaneously readable.  
   **Fix:** Give `.spine` a real `min-height` (enough for three compact rows) and let the *window stage* scroll, or compact `.next-card` (hide/shrink the ring) while the log is open; keep the dock pinned below a scrollable 3-row pipeline. Re-verify at 1100×700 *and* with logs open.

2. **English UI is a bilingual mash, not a translation.** `en-home`: greeting `Good evening，星夜回廊` (fullwidth comma); pipeline titles stay hardcoded Chinese (`动态 XP3 输出` / `动态 Hash 输出` / `资源名还原` / `前置任务未完成` in `pipeItems`); About module blurbs are a Chinese array; LOG lines from the mock stay Chinese; the legacy dock button wraps to three lines (`Original / CxdecExtractorLoader / tools`).  
   **Fix:** Drive every pipeline/facts/about string from `i18n` (`t.stepCls1` etc. already exist); use a narrow `, ` in EN greetings; give the legacy control a short EN label + `title` for the long name; optionally localize mock log prefixes when `locale==="en"`.

3. **Restore dialog’s Start/Stop keys off the global `job.kind`, which is a single slot.** Isolated `restore-running.png` correctly shows 「停止还原」, but the interaction video at ~8s (`f08`) shows packs at 42–51% *and* a pink 「开始」 because the harness (and a real user) can still have an extract job owning `job`. `running` is `job.kind === "restore" && status running` — any other kind hides Stop and offers Start mid-restore.  
   **Fix:** Treat restore as running when `restoreTasks.some(t => t.status==="running"||t.status==="mining")` *or* `job.kind==="restore"`; disable Start while any pack is live; if a restore overlay opens, either adopt that job or refuse overlapping host jobs.

4. **Make-set is still a text-button wall, and “最近操作” lies.** One primary plus a 2×3 grid of long labels (`make-set.png`, `make-set-empty.png`) fights “more SVG, less text” and one primary per region. `lastAction` is global, so a failed home job (`启动已中止`) can surface as Make’s “最近操作”. The page also keeps the home greeting `晚上好，{game}` instead of a make-specific title.  
   **Fix:** Icon + short label for the six auxiliaries (or a More menu for rare path picks); scope `lastAction` / `dryRunNote` to make-jobs only; greet Make like Library (`制作扩展集` + short caption).

5. **Toasts still compete with the task column and dialog chrome.** `home-error.png` stacks 「已清理产物目录」 (cleanDirs still classified as an “important completion”) on top of 「启动已中止」, sitting on the next-card. Video `f12` (~12s) places the same clean toast top-center *on* the restore dialog (`over-modal`) while restore is at 71%. Error scenes also skip toast clearing (`name.includes("error")`).  
   **Fix:** Log-only for `cleanDirs`; keep toasts for failures and long *user-started* jobs; never cover dialog headers (anchor toasts to the window title-right, below overlay z, or inside the dialog); clear stale toasts when opening an overlay.

6. **Next-step is still said four ways, and the new facts panel repeats the stamps.** Loaded/idle home: greet “下一步在右侧”, primary “开始解包 XP3”, pipeline “下一步”, muted ring “还原尚未开始”. Facts then repeats title/publisher plus Extractor/Hash/Restored paths already on the stamps (`未生成 · D:\Games\…\Extractor_Output` is also misleading — a default path is not a generated output).  
   **Fix:** One verbal “next” (the CTA); pipeline hints become state words only (ready / locked / this step); drop the greet subtitle or make it non-directive; facts should show *status and counts*, not a second copy of the three paths (link/stamp is enough).

### Minor (12)

1. **Expanded pipeline rows duplicate copy** — hint and `stat` both render (“前置任务未完成” + “等待启动”; video `f05`). Show one line unless the extra line adds a path or count.  
2. **About (and Make) still use the home greeting** `晚上好，星夜回廊` — Library was special-cased; About should be the product title, not the game hello.  
3. **Library empty is text-only.** No SVG empty state; the inspector still says “先在左侧选择会社，再选择作品” when there is no list. Add a small empty illustration and inspector copy that points to Make / 在线更新.  
4. **Stamp paths are over-truncated** (`D:\Games\...\Extractor_Out_`) so the three folders are hard to tell apart. Prefer keeping the unique tail (`Extractor_Output`) and let the row wrap.  
5. **Collect footer is five controls in a row**; FileNameHash at ~1% is still a sliver even with a number. Icon+label the rare actions; keep Stop as the only strong control; thicker/minimum fill for <5%.  
6. **Batch (and Online) remain plain tables** while Restore got colorful pack rows — same-language task lists would help consistency without turning them into neo slabs.  
7. **Keyboard/AT:** LOG toggle is a clickable `<div class="log-h">`; path stamps are `tabIndex={0}` divs, not buttons. Make both `<button>`s (or add `role="button"` + Enter/Space).  
8. **Motion is slightly generic and uneven.** Every `.title-item` plays `rise 160ms` on mount (list pop-in); mode pill is 200ms while neo controls are 140ms; neo hover has almost no change, so the press inset is easy to miss and focus must do the work. Unify 140–160ms `var(--ease)` (no springs), add a 1px settle on hover, skip enter animations on palettes/lists when `reduced-motion`.  
9. **Muted restore ring uses `opacity: 0.48`**, which likely drops the “还原尚未开始” label below WCAG AA. Fade the arc, not the type; keep label at full contrast.  
10. **Neo pillows vs candy pipeline cards** read as two dialects on the same sidebar (soft same-color dock buttons against saturated stat-rows). Don’t slab the cards; slightly quieter neo elevation (`--neo-lo` lighter) so controls sit *on* glass rather than floating off it.  
11. **LOG open repeats routine lines** (`已清理产物目录` / `已刷新列表` many times) and always in Chinese — denser grouping or collapsing duplicate info lines would match the “routine results go to LOG” rule without turning LOG into noise.  
12. **`CxdecExtractorLoader 原版功能` wraps to two–three lines** in the dock (CN and EN). Short label 「原版功能」 / “Legacy tools” plus `title`/`aria-label` for the full product name.

### Motion notes (from video + CSS)

- Press-in exists (`:active` → `--neo-inset`) but 1 fps frames cannot prove the feel; a 140ms shadow-only press is easy to miss on glass — pair with a 1px saturate, not a bounce.  
- Scene cuts in `interaction.webm` are harness `applyShot` jumps, not product transitions; do not treat those cuts as UI motion bugs.  
- The real product interruption bug is Major 3 (job-slot collision), not the jump cut itself.  
- No looping decorative motion remains (stripes/bob removed in R2); keep it that way.

### Out of scope / not flagged

Large radii, floating window, peek avatar, pastel pipeline colors, greeting-on-home, restore ring language — all approved direction. Neo stays on controls; no panel-to-slab conversion.


## R3 (merged · fixed)

Merged the R3 self-review with DC’s 2026-11-07 critique (deduped). Direction kept: Hibiscus palette, large radii, illustration, grain + constructivism, glass shell/panels, **neumorphism on interactive controls only**. `tsc --noEmit` and `vite build` pass. Shots: `shots/R3/`. Video: `shots/R3/interaction.webm` (~14.8s, live clicks, no mid-roll `applyShot` jump cuts).

| ID | Source | Issue | Fix |
|---|---|---|---|
| M1 / DC-M1 | self + DC | Top-center toast covered the task-window title | Modal toasts sit bottom-left (`z-index` 80) so they miss title and close. Routine `cleanDirs` uses `note()` (LOG only). Scene `toast-over-restore` proves placement. |
| M2 / DC-M2 | self + DC | Pipeline still said「下一步」while unpacking | Current running step hint is「进行中 · n%」; greet subtitle becomes「正在解包…」/ `unpackingNow`. |
| DC-M3 | DC | Theme toggle off + label「浅色」read as “light disabled” | Fixed label `t.darkMode` (「深色模式」/ “Dark mode”); checked = dark. |
| DC-M4 | DC | Panels read as solid paper vs glass shell | `.sketch` / `.picker` / `.drop-stage` / `.stamp` / `.next-card` get light glass (`color-mix(paper 82%)` + blur 16). Logs/tables stay tinted paper. No `--neo-*` on panels. |
| M1 / DC-M5 | self + DC | At 1100×700, pipeline ②③ sat below the sidebar fold | Compact greet/next-card/ring/stat-rows; `.next-card` can shrink; `.spine` `min-height: 198px`. `home-loaded-min` shows all three steps + dock. |
| M2 | self | English UI mash (pipeline, greeting comma, About, overlays) | Pipeline labels via `t.pipe*`; EN greeting uses `", "`; About modules / overlay notes / confirms / palette demo / table headers moved to i18n. |
| M3 | self | Restore Start/Stop keyed off global `job.kind` (extract could steal the slot) | Footer follows `restoreTasks` live statuses (`running`/`mining`/`queued`), not extract’s job. `restore-after-extract` shows「停止还原」after extract has already finished its precheck. |
| M4 | self | Make-set text wall + `lastAction` leak + home greeting | Icon + short labels; recent pane only if the last action is make-related; greet title `makeTitleShort`. |
| M5 | self | Toasts vs task column (also leftover routine success toasts) | Top-right when no modal; bottom-left when a dialog is open; routine clean → LOG. |
| M6 / DC-m1 / DC-m3 | self + DC | Next-step said many ways; facts copied stamp paths | Greet follows the job while running; facts keep title / studio / workspace / attribution only. |
| m1 | self | Expanded pipeline row duplicated hint + stat | Extra line only when `stat !== hint`. |
| m2 | self | About/Make reused the Home greeting | Dedicated titles (`aboutTitleShort` / `makeTitleShort`). |
| m3 | self | Library empty was text-only | `DropArt` illustration + `emptyLibHint` in the inspector. |
| m4 | self | Stamp paths over-truncated | `PathLine` max 42. |
| m5 | self | Collect footer five buttons; 1% meters invisible | Dropped the always-disabled Start Game; icon+short Stop/Clear/Open; meters `min-width: 8px` when nonzero. |
| m6 | self | Batch (and Online headers) were plain vs Restore pack rows | Batch uses colorful `pack-row` cards + meters. Online table headers localized. |
| m7 | self | LOG header was a `<div>` | `<button type="button" class="log-h">`. |
| m8 | self | Uneven motion (title-item rise, 200ms pill, limp hover) | Dropped list `rise`; mode pill 140ms; button hover slightly stronger neo shadow; reduced-motion still scoped. |
| m9 | self | Muted ring faded the label below AA | Fade `.ring-well` only; label/small stay full contrast. |
| m10 | self | Neo pillows floated off glass | `--neo-lo` lightened `0.22 → 0.16`. |
| m11 | self | LOG repeated routine Chinese lines | Dedupes the same message/source/level within the last 4 entries. |
| m12 / DC-m6 | self + DC | Legacy dock label wrapped | Short `legacyShort` + `title`/`aria-label` = `legacyTitle`. |
| DC-m2 | DC | Palette had no focus trap | Palette root uses `useFocusTrap`. |
| DC-m4 | DC | Restore dialog log contrast risk | Dialog log list: deeper tinted paper + `--ink` text. |
| DC-m5 | DC | 「合集(二期)」disabled state too weak | Disabled mode pill opacity `0.38` (tooltip already `studioSoon`). |
| DC-m7 | DC | Online / Legacy helper sentences too long | Short `onlineNote` / `legacyNote`. |
| DC-m8 | DC | Constructivist type nearly invisible | Decorative `YUA` fillOpacity `0.10 → 0.14`; backdrop stays `aria-hidden`. |
| DC-m9 | DC | EN UI leftover hardcoded Chinese in overlays/confirms | `overwriteTitle`/`overwriteOk`/`overwriteBody`, `remoteCount`, `fetchIndexFirst`, `demoFail`, `noMatch`, `collectReady`, `cleanBody`, `liveEndpoint` all go through `t.*`. |
| DC-m10 | DC | Dual log surfaces (dock + window) | While restore/collect/batch is open, the dock preview reads `logInWindow` (「详见窗口内日志」). |

### Acceptance check

- Toast does not cover dialog title/close (`toast-over-restore.png`).
- `home-running.png`: step 1 =「进行中 · 60%」, subtitle「正在解包…」.
- Settings: off +「深色模式」.
- `home-loaded-min.png`: steps 1–3 visible at 1100×700.
- `restore-after-extract.png`: footer is「停止还原」even after extract’s precheck completed.
- Palette trap in code; panels light-glass; `#FFFFFF`/`#000000` policy unchanged (tinted tokens).

## R4 (Native frameless client-area integration & protocol fix)

Resolved real-hardware integration defects identified on `feat/hibiscus-native-ui`:

1. **Eliminated double JSON encoding on WebView2 bridge (`bridge.ts` + `YuaWebViewBridge.inl`).** `createHostBridge` previously called `wv.postMessage(JSON.stringify({ protocolVersion, ...msg }))`, causing WebView2’s `postMessage` to serialize the string a second time (`get_WebMessageAsJson` received `Kind::String` and rejected every request with `拒绝 WebView2 消息：消息不是合法 JSON 对象`). Fixed `bridge.ts` to pass a plain object to `wv.postMessage(...)` and added string-unwrapping fallback in `MessageHandler::Invoke`.
2. **Removed system titlebar & outer decorative desk inset (`hibiscus.css` + `app.tsx` + `YuaExtractMainWindow.cpp`).** Suppressed native `WS_CAPTION` via `WM_NCCALCSIZE` + 1px DWM shadow in WebView2 mode, set `.window` to `inset: 0; border-radius: 0; border: 0;`, hid outer `.desk-bg` / `.blob-*` / `.grain` / `.peek`, and added `.win-ctrls` (Minimize, Maximize/Restore, Close), titlebar drag / double-click maximize, and 8-direction `.win-resize-*` handles backed by `windowControl`. Preserved standard `WS_OVERLAPPEDWINDOW` when running with `--legacy-win32` / `YUA_LEGACY_WIN32=1` or when `FailClosed` falls back to Win32.
3. **Connected in-shell `RestoreOverlay` to the real C++ restore engine (`YuaRestoreWindow.cpp` + `YuaWebViewBridge.inl`).** Added hidden mode (`options.Hidden = true`), `StartRestoreWithOptions`, `StopRestoreRun`, and `QueryRestoreSnapshot` so Step 3 and `RestoreOverlay` enumerate real packages, stream live per-package/mining progress, and execute Pass 1 + Supplement Mining + Pass 2 without opening a duplicate Win32 popup window.
