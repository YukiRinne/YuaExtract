# YuaExtract shell

WebView2 front-end shell for [YuaExtract](https://github.com/YukiRinne/YuaExtract). Chinese-first UI, optional English toggle, mock backend when `window.chrome.webview` is missing.

## Run

```bash
cd /workspace/yua-shell
npm install
npm run dev          # http://127.0.0.1:5173
npm run build && npm run preview   # http://127.0.0.1:4173
```

Screenshots (Playwright, 1440×900):

```bash
npx playwright install chromium   # once
npm run build && npm run preview  # in another terminal
node scripts/shots.mjs v1         # or r1
```

## Bridge

See [BRIDGE.md](BRIDGE.md) and `src/bridge.ts` / `src/types.ts`. The C++ loader should host `dist/` and speak JSON over `chrome.webview`.
