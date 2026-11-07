import React from "react";
import ReactDOM from "react-dom/client";
import { StoreProvider } from "./store";

const params = new URLSearchParams(window.location.search);
const hasWebView = Boolean(window.chrome?.webview);
// Compile-time demo switch: this raw expression (not a re-exported constant) is
// required so that `vite build` folds it to `false` in a production build and
// statically eliminates every guarded branch, including the mock backend's
// dynamic import chunk below.
const IS_DEMO_BUILD = import.meta.env.DEV || import.meta.env.MODE === "demo" || import.meta.env.MODE === "test";
const allowMock = IS_DEMO_BUILD && !hasWebView && params.get("nomock") !== "1";

const root = document.getElementById("root")!;

function BridgeDiagnostic() {
  return (
    <div className="desk bridge-diag">
      <div className="bridge-diag-card">
        <p className="bridge-diag-kicker">YuaExtract · shell</p>
        <h1>无法连接宿主 / Host bridge unavailable</h1>
        <p className="note">
          缺少 <code>window.chrome.webview</code>。此页面必须由 YuaExtract 桌面宿主（WebView2）承载，
          不能直接在浏览器中打开。
        </p>
        <p className="note lang-en">
          <code>window.chrome.webview</code> is missing. This page must be hosted by the YuaExtract desktop
          host (WebView2) and cannot be opened directly in a browser.
        </p>
        <hr className="bridge-diag-rule" />
        <p className="note">
          生产构建已启用 fail-closed：mock 后端被禁用，不会伪造任何宿主事件。
          因此界面在此停止，而不是显示一份假的成功状态。
        </p>
        <p className="note lang-en">
          The production build runs fail-closed: the mock backend is disabled and never fabricates host
          events. The UI stops here instead of showing a fake success state.
        </p>
        <ol className="bridge-diag-steps">
          <li>
            通过 <code>YuaExtract.exe</code> 启动，不要双击 <code>index.html</code>。
            <span className="lang-en">Launch via <code>YuaExtract.exe</code>, not by opening <code>index.html</code>.</span>
          </li>
          <li>
            确认已安装 WebView2 Runtime（x86 版本）。
            <span className="lang-en">Verify the WebView2 Runtime is installed (x86).</span>
          </li>
          <li>
            确认程序目录下存在完整的 <code>shell/</code> 资源（<code>index.html</code>、<code>assets/</code>、<code>brand/</code>）。
            <span className="lang-en">Verify the <code>shell/</code> assets are complete (<code>index.html</code>, <code>assets/</code>, <code>brand/</code>).</span>
          </li>
        </ol>
        <p className="note bridge-diag-foot">
          排障后可重新启动程序。 / Relaunch the app after fixing the above.
        </p>
      </div>
    </div>
  );
}

async function boot() {
  if (!hasWebView && !allowMock) {
    await import("./styles/hibiscus.css");
    ReactDOM.createRoot(root).render(<BridgeDiagnostic />);
    return;
  }

  // Demo-only backend stays out of the production bundle: the dynamic import
  // sits inside a branch that folds to `false` at compile time in a production
  // build (MODE=production), so Rollup tree-shakes the whole chunk away.
  if (IS_DEMO_BUILD && allowMock) {
    const { attachMockBackend } = await import("./mock/backend");
    attachMockBackend();
  }

  const dir = params.get("dir");
  const scene = params.get("scene") === "restore" ? "restore" : params.get("scene") === "library" ? "library" : "home";

  if (dir === "a" || dir === "b" || dir === "c" || dir === "d" || dir === "e" || dir === "f") {
    await import("./styles/app.css");
    const { DirApp } = await import("./dirs/DirApp");
    ReactDOM.createRoot(root).render(
      <React.StrictMode>
        <StoreProvider>
          <DirApp dir={dir} scene={scene} />
        </StoreProvider>
      </React.StrictMode>,
    );
    return;
  }
  await import("./styles/hibiscus.css");
  const { App } = await import("./app");
  ReactDOM.createRoot(root).render(
    <React.StrictMode>
      <StoreProvider>
        <App />
      </StoreProvider>
    </React.StrictMode>,
  );
}

void boot();
