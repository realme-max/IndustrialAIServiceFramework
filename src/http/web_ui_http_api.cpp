#include "iaisf/http/web_ui_http_api.hpp"

#include <exception>
#include <new>
#include <string>
#include <string_view>
#include <utility>

#include "iaisf/core/error.hpp"

namespace iaisf::web_ui {
namespace {

constexpr char kHtml[] = R"HTML(<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>&#x5DE5;&#x4E1A;&#x710A;&#x63A5;&#x5206;&#x6790;</title>
  <link rel="stylesheet" href="/ui/app.css">
  <script src="/ui/app.js" defer></script>
</head>
<body>
  <main class="shell">
    <header class="hero">
      <p class="eyebrow">Industrial AI Service</p>
      <h1>&#x5DE5;&#x4E1A;&#x710A;&#x63A5;&#x5206;&#x6790;</h1>
      <p class="lead">&#x9009;&#x62E9;&#x4E00;&#x4E2A;&#x72EC;&#x7ACB;&#x4E1A;&#x52A1;&#xFF0C;&#x4E0A;&#x4F20;&#x70B9;&#x4E91;&#x5E76;&#x67E5;&#x770B;&#x7ED3;&#x6784;&#x5316;&#x7ED3;&#x679C;&#x3002;</p>
    </header>
    <nav class="tabs" aria-label="&#x4E1A;&#x52A1;&#x9009;&#x62E9;">
      <button type="button" class="tab is-active" data-view="inspection">&#x710A;&#x540E;&#x710A;&#x7F1D;&#x5206;&#x5272;</button>
      <button type="button" class="tab" data-view="guidance">&#x710A;&#x524D;&#x5EFA;&#x7CFB;&#x4E0E;&#x710A;&#x63A5;&#x7279;&#x5F81;</button>
    </nav>
    <p id="global-status" class="status" role="status" aria-live="polite">&#x8BF7;&#x9009;&#x62E9;&#x4E1A;&#x52A1;&#x5E76;&#x4E0A;&#x4F20; XYZ/TXT/PTS &#x70B9;&#x4E91;&#x3002;</p>
    <section id="inspection" class="panel" data-panel>
      <h2>&#x710A;&#x540E;&#x710A;&#x7F1D;&#x5206;&#x5272;</h2>
      <p>&#x5206;&#x6790;&#x5DF2;&#x5B8C;&#x6210;&#x710A;&#x63A5;&#x5DE5;&#x4EF6;&#xFF0C;&#x751F;&#x6210;&#x710A;&#x7F1D;&#x4E0E;&#x51E0;&#x4F55;&#x76F8;&#x5173;&#x8F93;&#x51FA;&#x3002;</p>
      <label class="field">&#x70B9;&#x4E91;&#x6587;&#x4EF6;
        <input id="inspection-file" type="file" accept=".xyz,.txt,.pts,text/plain">
      </label>
      <fieldset>
        <legend>&#x8BF7;&#x6C42;&#x8F93;&#x51FA;&#xFF08;&#x81F3;&#x5C11;&#x9009;&#x62E9;&#x4E00;&#x9879;&#xFF09;</legend>
        <label><input id="inspection-segmentation" type="checkbox" checked> segmentation</label>
        <label><input id="inspection-geometry" type="checkbox" checked> geometry</label>
      </fieldset>
      <button id="inspection-submit" type="button" class="primary">&#x4E0A;&#x4F20;&#x5E76;&#x5F00;&#x59CB;&#x5206;&#x6790;</button>
      <button id="inspection-stop" type="button" class="secondary" hidden>&#x505C;&#x6B62;&#x7B49;&#x5F85;</button>
      <div class="visualization-placeholder" aria-label="3D &#x53EF;&#x89C6;&#x5316;&#x9884;&#x7559;&#x533A;&#x57DF;">3D &#x53EF;&#x89C6;&#x5316;&#x5C06;&#x5728; Phase 10C &#x63D0;&#x4F9B;&#x3002;</div>
      <div id="inspection-result" class="result" aria-live="polite"></div>
    </section>
    <section id="guidance" class="panel is-hidden" data-panel>
      <h2>&#x710A;&#x524D;&#x5EFA;&#x7CFB;&#x4E0E;&#x710A;&#x63A5;&#x7279;&#x5F81;</h2>
      <p>&#x5206;&#x6790;&#x5F85;&#x710A;&#x5DE5;&#x4EF6;&#x5E76;&#x751F;&#x6210;&#x710A;&#x63A5;&#x5F15;&#x5BFC;&#x5019;&#x9009;&#xFF0C;&#x63D0;&#x4EA4;&#x540E;&#x9700;&#x8981;&#x4EBA;&#x5DE5;&#x590D;&#x6838;&#x3002;</p>
      <label class="field">&#x70B9;&#x4E91;&#x6587;&#x4EF6;
        <input id="guidance-file" type="file" accept=".xyz,.txt,.pts,text/plain">
      </label>
      <label class="field">&#x710A;&#x7F1D;&#x7C7B;&#x578B;
        <select id="guidance-type">
          <option value="auto">auto</option>
          <option value="straight">straight</option>
          <option value="corner">corner</option>
          <option value="l">l</option>
        </select>
      </label>
      <p class="notice">&#x4EBA;&#x5DE5;&#x590D;&#x6838;&#xFF1A;required&#x3002;&#x6B64;&#x9875;&#x9762;&#x4E0D;&#x4F1A;&#x6267;&#x884C;&#x673A;&#x5668;&#x4EBA;&#x63A7;&#x5236;&#x3002;</p>
      <button id="guidance-submit" type="button" class="primary">&#x4E0A;&#x4F20;&#x5E76;&#x5F00;&#x59CB;&#x5206;&#x6790;</button>
      <button id="guidance-stop" type="button" class="secondary" hidden>&#x505C;&#x6B62;&#x7B49;&#x5F85;</button>
      <div class="visualization-placeholder" aria-label="3D &#x53EF;&#x89C6;&#x5316;&#x9884;&#x7559;&#x533A;&#x57DF;">3D &#x53EF;&#x89C6;&#x5316;&#x5C06;&#x5728; Phase 10C &#x63D0;&#x4F9B;&#x3002;</div>
      <div id="guidance-result" class="result" aria-live="polite"></div>
    </section>
    <footer><p>PTV2 &#x4E0E; WeldAgent &#x4E3A;&#x4E24;&#x4E2A;&#x72EC;&#x7ACB;&#x4E1A;&#x52A1;&#x3002;&#x8D28;&#x91CF;&#x8BC4;&#x4EF7;&#x5C1A;&#x672A;&#x5B9E;&#x73B0;&#xFF1B;&#x673A;&#x5668;&#x4EBA;&#x6267;&#x884C;&#x59CB;&#x7EC8;&#x5173;&#x95ED;&#x3002;</p></footer>
  </main>
</body>
</html>
)HTML";

constexpr char kCss[] = R"CSS(:root {
  color-scheme: light;
  font-family: system-ui, -apple-system, "Segoe UI", sans-serif;
  background: #f3f6fa;
  color: #172033;
}
* { box-sizing: border-box; }
body { margin: 0; min-width: 320px; }
.shell { width: min(960px, 100%); margin: 0 auto; padding: 24px 16px 48px; }
.hero { padding: 18px 0; }
.eyebrow { margin: 0; color: #526179; font-size: .82rem; letter-spacing: .08em; text-transform: uppercase; }
h1, h2 { margin: .35rem 0 .6rem; }
.lead, .panel p, footer { color: #526179; }
.tabs { display: flex; flex-wrap: wrap; gap: 8px; margin: 8px 0 16px; }
.tab, button, select, input { font: inherit; }
.tab, .primary { border: 1px solid #8ea1bf; border-radius: 8px; padding: 10px 14px; cursor: pointer; background: #fff; }
.tab.is-active, .primary { background: #1e5eff; color: #fff; border-color: #1e5eff; }
.tab:disabled, .primary:disabled, .secondary:disabled { cursor: wait; opacity: .6; }
.secondary { margin-left: 8px; border: 1px solid #8ea1bf; border-radius: 8px; padding: 10px 14px; cursor: pointer; background: #fff; color: #172033; }
.status { min-height: 1.5rem; padding: 10px 12px; border-radius: 8px; background: #e7edf7; }
.panel { margin-top: 16px; padding: 20px; background: #fff; border: 1px solid #d9e1ee; border-radius: 12px; box-shadow: 0 6px 20px #17203312; }
.is-hidden { display: none; }
.field { display: grid; gap: 6px; margin: 14px 0; font-weight: 600; }
input[type="file"], select { width: 100%; max-width: 520px; padding: 9px; border: 1px solid #b9c7dc; border-radius: 7px; background: #fff; }
fieldset { border: 1px solid #d9e1ee; border-radius: 8px; margin: 14px 0; padding: 12px; display: flex; flex-wrap: wrap; gap: 14px; }
.notice { padding: 10px; border-left: 4px solid #e6a700; background: #fff8dd; }
.visualization-placeholder { display: grid; place-items: center; min-height: 120px; margin-top: 20px; color: #526179; border: 1px dashed #9cafc9; border-radius: 8px; }
.result { margin-top: 18px; display: grid; gap: 10px; }
.result-card { border: 1px solid #d9e1ee; border-radius: 8px; padding: 12px; }
.result-card dl { display: grid; grid-template-columns: minmax(130px, 1fr) 2fr; gap: 6px 14px; margin: 0; }
.result-card dt { color: #526179; }
.result-card dd { margin: 0; overflow-wrap: anywhere; }
.result-card a { color: #174bc1; }
footer { margin-top: 20px; font-size: .9rem; }
@media (max-width: 600px) { .shell { padding: 16px 12px 32px; } .result-card dl { grid-template-columns: 1fr; gap: 2px; } }
)CSS";

constexpr char kJavaScript[] = R"JS((() => {
  "use strict";
  const MAX_UPLOAD_BYTES = 64 * 1024 * 1024;
  const POLL_INTERVAL_MS = 1000;
  const POLL_TIMEOUT_MS = 10 * 60 * 1000;
  const MAX_ERROR_TEXT = 256;
  const state = { view: "inspection", operation: null, generation: 0 };
  const $ = (id) => document.getElementById(id);
  const statusNode = $("global-status");
  const setStatus = (message) => { statusNode.textContent = message; };
  const setResultText = (id, message) => {
    const node = $(id);
    while (node.firstChild) node.removeChild(node.firstChild);
    const paragraph = document.createElement("p");
    paragraph.textContent = message;
    node.appendChild(paragraph);
  };
  const genericErrors = {
    400: "\u8bf7\u6c42\u53c2\u6570\u6216\u70b9\u4e91\u683c\u5f0f\u9519\u8bef",
    409: "\u4efb\u52a1\u5f53\u524d\u72b6\u6001\u4e0d\u5141\u8bb8\u6b64\u64cd\u4f5c",
    413: "\u8bf7\u6c42\u6216\u54cd\u5e94\u8d85\u8fc7\u5927\u5c0f\u9650\u5236",
    415: "\u4e0a\u4f20\u6587\u4ef6\u7c7b\u578b\u4e0d\u53d7\u652f\u6301",
    422: "\u70b9\u4e91\u5750\u6807\u65e0\u6cd5\u901a\u8fc7\u6821\u9a8c",
    500: "\u670d\u52a1\u6682\u65f6\u4e0d\u53ef\u7528",
    503: "\u670d\u52a1\u6b63\u5728\u505c\u6b62\u6216\u6682\u65f6\u4e0d\u53ef\u7528"
  };
  const boundedString = (value) => {
    if (typeof value !== "string" || value.length === 0 || value.length > MAX_ERROR_TEXT) return null;
    return value.replace(/[\u0000-\u001f\u007f]/g, " ");
  };
  const httpError = (status, body) => {
    const fallback = genericErrors[status] || "\u8bf7\u6c42\u672a\u6210\u529f";
    const detail = body && typeof body === "object" && !Array.isArray(body) ? body.error : null;
    const code = detail && typeof detail === "object" && !Array.isArray(detail) ? boundedString(detail.code) : null;
    const message = detail && typeof detail === "object" && !Array.isArray(detail) ? boundedString(detail.message) : null;
    const error = new Error(message || fallback);
    error.status = status;
    error.code = code || "http_error";
    return error;
  };
  const jsonResponse = async (response) => {
    const contentType = response.headers.get("content-type") || "";
    if (!contentType.toLowerCase().includes("application/json")) throw httpError(response.status, null);
    let body;
    try { body = await response.json(); } catch (_) { throw httpError(response.status, null); }
    if (!response.ok) throw httpError(response.status, body);
    if (!body || typeof body !== "object" || Array.isArray(body)) throw new Error("\u54cd\u5e94\u5951\u7ea6\u683c\u5f0f\u9519\u8bef");
    return body;
  };
  const delay = (milliseconds, signal) => new Promise((resolve, reject) => {
    let settled = false;
    let timer = 0;
    const cleanup = () => signal.removeEventListener("abort", onAbort);
    const finish = () => {
      if (settled) return;
      settled = true;
      cleanup();
      resolve();
    };
    const onAbort = () => {
      if (settled) return;
      settled = true;
      window.clearTimeout(timer);
      cleanup();
      reject(new DOMException("aborted", "AbortError"));
    };
    if (signal.aborted) { onAbort(); return; }
    timer = window.setTimeout(finish, milliseconds);
    signal.addEventListener("abort", onAbort, { once: true });
    if (signal.aborted) onAbort();
  });
  const strictArtifact = (artifact) => {
    const keys = ["artifact_id", "sha256", "size_bytes", "kind", "media_type", "coordinate_frame", "unit", "point_count"];
    if (!artifact || typeof artifact !== "object" || Array.isArray(artifact) ||
        Object.keys(artifact).length !== keys.length ||
        keys.some((key) => !Object.prototype.hasOwnProperty.call(artifact, key))) {
      throw new Error("\u4e0a\u4f20\u54cd\u5e94\u5951\u7ea6\u683c\u5f0f\u9519\u8bef");
    }
    if (!["artifact_id", "sha256", "kind", "media_type", "coordinate_frame", "unit"].every((key) => typeof artifact[key] === "string") ||
        !/^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(artifact.artifact_id) || !/^[0-9a-f]{64}$/.test(artifact.sha256) ||
        !Number.isSafeInteger(artifact.size_bytes) || artifact.size_bytes < 0 ||
        !Number.isSafeInteger(artifact.point_count) || artifact.point_count <= 0) {
      throw new Error("\u4e0a\u4f20\u54cd\u5e94\u5951\u7ea6\u683c\u5f0f\u9519\u8bef");
    }
    return {
      artifact_id: artifact.artifact_id,
      sha256: artifact.sha256,
      size_bytes: artifact.size_bytes,
      kind: artifact.kind,
      media_type: artifact.media_type,
      coordinate_frame: artifact.coordinate_frame,
      unit: artifact.unit,
      point_count: artifact.point_count
    };
  };
  const jobPattern = (application) => application === "weld_inspection" ? /^wi_[0-9a-f]{32}$/ : /^wg_[0-9a-f]{32}$/;
  const statusUrlFor = (application, jobId) => application === "weld_inspection"
    ? `/api/weld-inspection/v1/jobs/${jobId}`
    : `/api/welding-guidance/v1/jobs/${jobId}`;
  const resultUrlFor = (application, jobId) => application === "weld_inspection"
    ? `/api/weld-inspection/v1/results/${jobId}`
    : `/api/welding-guidance/v1/results/${jobId}`;
  const validateAccepted = (application, accepted) => {
    if (!accepted || typeof accepted.job_id !== "string" || !jobPattern(application).test(accepted.job_id) ||
        typeof accepted.status_url !== "string" || accepted.status_url.length > 256 ||
        accepted.status_url !== statusUrlFor(application, accepted.job_id)) {
      throw new Error("\u4efb\u52a1\u63d0\u4ea4\u54cd\u5e94\u5951\u7ea6\u683c\u5f0f\u9519\u8bef");
    }
    return { jobId: accepted.job_id, statusUrl: accepted.status_url };
  };
  const canonicalDownloadId = (url) => {
    if (typeof url !== "string") return null;
    const match = url.match(/^\/api\/artifacts\/v1\/files\/([A-Za-z0-9][A-Za-z0-9._-]{0,127})$/);
    return match ? match[1] : null;
  };
  const renderLinks = (root, result) => {
    if (!result || !Array.isArray(result.output_artifacts)) throw new Error("\u7ed3\u679c\u8f93\u51fa\u5951\u7ea6\u683c\u5f0f\u9519\u8bef");
    const artifacts = [];
    artifacts.push(...result.output_artifacts);
    ["weld_points", "prediction"].forEach((key) => {
      if (result[key] === null || result[key] === undefined) return;
      if (typeof result[key] !== "object" || Array.isArray(result[key])) throw new Error("\u7ed3\u679c\u8f93\u51fa\u5951\u7ea6\u683c\u5f0f\u9519\u8bef");
      artifacts.push(result[key]);
    });
    const list = document.createElement("ul");
    const seen = new Set();
    artifacts.forEach((artifact) => {
      if (!artifact || typeof artifact.artifact_id !== "string") throw new Error("\u7ed3\u679c\u4e2d\u7684 Artifact \u65e0\u6548");
      const downloadId = canonicalDownloadId(artifact.download_url);
      if (downloadId !== artifact.artifact_id) throw new Error("\u7ed3\u679c\u4e2d\u7684\u4e0b\u8f7d\u5730\u5740\u65e0\u6548");
      if (seen.has(artifact.artifact_id)) return;
      seen.add(artifact.artifact_id);
      const item = document.createElement("li");
      const link = document.createElement("a");
      link.href = artifact.download_url;
      link.textContent = artifact.artifact_id;
      link.setAttribute("download", "");
      item.appendChild(link);
      list.appendChild(item);
    });
    if (list.childNodes.length > 0) root.appendChild(list);
  };
)JS"
R"JS(
  const renderResult = (application, result, operation) => {
    if (!result || typeof result !== "object" || Array.isArray(result)) throw new Error("\u7ed3\u679c\u54cd\u5e94\u5951\u7ea6\u683c\u5f0f\u9519\u8bef");
    if (application === "weld_inspection" && result.quality_assessment !== "not_implemented") throw new Error("\u8d28\u91cf\u8bc4\u4ef7\u54cd\u5e94\u5951\u7ea6\u683c\u5f0f\u9519\u8bef");
    if (application === "welding_guidance" && result.robot_execution_allowed !== false) throw new Error("\u673a\u5668\u4eba\u6267\u884c\u54cd\u5e94\u5951\u683c\u5f0f\u9519\u8bef");
    const root = $(application === "weld_inspection" ? "inspection-result" : "guidance-result");
    while (root.firstChild) root.removeChild(root.firstChild);
    const card = document.createElement("div");
    card.className = "result-card";
    const list = document.createElement("dl");
    const values = application === "weld_inspection"
      ? [["\u8f93\u5165\u70b9\u6570", operation.inputPointCount], ["\u710a\u7f1d\u70b9\u6570", result.weld_point_count], ["\u710a\u7f1d\u6bd4\u4f8b", result.weld_ratio], ["\u957f\u5ea6", result.length_mm], ["\u63a8\u7406\u8017\u65f6", result.inference_time_ms], ["\u603b\u8017\u65f6", result.total_time_ms], ["\u8d28\u91cf\u8bc4\u4ef7", result.quality_assessment]]
      : [["\u710a\u7f1d\u7c7b\u578b", result.weld_type], ["\u5750\u6807\u7cfb", result.coordinate_frame], ["\u5355\u4f4d", result.unit], ["\u72b6\u6001", result.disposition], ["\u53ef\u4fe1\u5ea6", result.confidence], ["\u4eba\u5de5\u590d\u6838\u539f\u56e0", result.waiting_reason], ["\u673a\u5668\u4eba\u6267\u884c", String(result.robot_execution_allowed)]];
    values.forEach(([label, value]) => {
      if (value === undefined || value === null) return;
      const dt = document.createElement("dt"); dt.textContent = label;
      const dd = document.createElement("dd"); dd.textContent = String(value);
      list.appendChild(dt); list.appendChild(dd);
    });
    ["start", "end", "corner", "x_axis", "y_axis", "z_axis"].forEach((key) => {
      if (!result[key]) return;
      const dt = document.createElement("dt"); dt.textContent = key;
      const dd = document.createElement("dd"); dd.textContent = JSON.stringify(result[key]);
      list.appendChild(dt); list.appendChild(dd);
    });
    card.appendChild(list);
    renderLinks(card, result);
    root.appendChild(card);
  };
  const pollingStates = new Set(["accepted", "queued", "dispatching", "running", "cancelling", "succeeded", "waiting_human", "failed", "cancelled", "timed_out", "worker_lost"]);
  const poll = async (application, accepted, signal, operation) => {
    const started = Date.now();
    const jobId = accepted.jobId;
    const statusUrl = accepted.statusUrl;
    while (Date.now() - started <= POLL_TIMEOUT_MS) {
      const response = await fetch(statusUrl, { headers: { Accept: "application/json" }, signal });
      const status = await jsonResponse(response);
      const phase = status.state;
      if (typeof phase !== "string" || !pollingStates.has(phase)) throw new Error("\u72b6\u6001\u54cd\u5e94\u5951\u7ea6\u683c\u5f0f\u9519\u8bef");
      if (isCurrent(operation)) setStatus(`\u4efb\u52a1 ${jobId}\uff1a${phase}`);
      if (phase === "succeeded" || phase === "waiting_human") {
        if (!isCurrent(operation)) return null;
        const resultResponse = await fetch(resultUrlFor(application, jobId), { headers: { Accept: "application/json" }, signal });
        const result = await jsonResponse(resultResponse);
        if (!isCurrent(operation)) return null;
        renderResult(application, result, operation);
        return phase;
      }
      if (["failed", "cancelled", "timed_out", "worker_lost"].includes(phase)) {
        throw new Error("\u4efb\u52a1\u672a\u6210\u529f\u5b8c\u6210\uff0c\u8bf7\u68c0\u67e5\u8f93\u5165\u540e\u91cd\u8bd5");
      }
      await delay(POLL_INTERVAL_MS, signal);
    }
    throw new Error("\u4efb\u52a1\u7b49\u5f85\u8d85\u65f6\uff0c\u8bf7\u7a0d\u540e\u67e5\u8be2\u72b6\u6001");
  };
  const setControls = (application, running) => {
    const button = $(application === "weld_inspection" ? "inspection-submit" : "guidance-submit");
    const stop = $(application === "weld_inspection" ? "inspection-stop" : "guidance-stop");
    button.disabled = running;
    stop.hidden = !running;
    stop.disabled = !running;
  };
  const isCurrent = (operation) => state.operation === operation && state.generation === operation.generation;
  const stopOperation = () => {
    const operation = state.operation;
    if (!operation) return false;
    state.operation = null;
    operation.controller.abort();
    setControls(operation.application, false);
    setStatus("\u5df2\u505c\u6b62\u9875\u9762\u7b49\u5f85\uff0c\u670d\u52a1\u5668\u4efb\u52a1\u53ef\u80fd\u4ecd\u5728\u7ee7\u7eed");
    return true;
  };
  const submit = async (application) => {
    if (state.operation) return;
    const file = $(application === "weld_inspection" ? "inspection-file" : "guidance-file").files[0];
    if (!file) { setStatus("\u8bf7\u9009\u62e9\u70b9\u4e91\u6587\u4ef6"); return; }
    if (file.size === 0) { setStatus("\u6587\u4ef6\u4e0d\u80fd\u4e3a\u7a7a"); return; }
    if (file.size > MAX_UPLOAD_BYTES) { setStatus("\u6587\u4ef6\u8d85\u8fc7\u670d\u52a1\u7aef\u5141\u8bb8\u7684\u5927\u5c0f"); return; }
    if (application === "weld_inspection" && !$("inspection-segmentation").checked && !$("inspection-geometry").checked) { setStatus("\u81f3\u5c11\u9009\u62e9\u4e00\u9879\u8f93\u51fa"); return; }
    if (application === "welding_guidance" && !["auto", "straight", "corner", "l"].includes($("guidance-type").value)) { setStatus("\u710a\u7f1d\u7c7b\u578b\u4e0d\u53d7\u652f\u6301"); return; }
    const operation = { application, generation: ++state.generation, controller: new AbortController(), inputPointCount: null };
    state.operation = operation;
    const signal = operation.controller.signal;
    setControls(application, true); setStatus("\u6b63\u5728\u4e0a\u4f20\u70b9\u4e91\u2026");
    try {
      const uploadResponse = await fetch("/api/artifacts/v1/pointclouds", { method: "POST", headers: { "Content-Type": "text/plain; charset=utf-8" }, body: file, signal });
      const upload = await jsonResponse(uploadResponse);
      const artifact = strictArtifact(upload.artifact);
      if (canonicalDownloadId(upload.download_url) !== artifact.artifact_id) throw new Error("\u4e0a\u4f20\u54cd\u5e94\u4e0b\u8f7d\u5730\u5740\u65e0\u6548");
      operation.inputPointCount = artifact.point_count;
      const contract = application === "weld_inspection"
        ? { schema_version: "1.0", input_artifacts: [artifact], requested_outputs: [$("inspection-segmentation").checked ? "segmentation" : null, $("inspection-geometry").checked ? "geometry" : null].filter(Boolean) }
        : { schema_version: "1.0", input_artifacts: [artifact], weld_type: $("guidance-type").value === "auto" ? { mode: "auto" } : { mode: "requested", requested: $("guidance-type").value }, review_policy: { human_checkpoint: "required" } };
      setStatus("\u6b63\u5728\u63d0\u4ea4\u4efb\u52a1\u2026");
      const submitPath = application === "weld_inspection" ? "/api/weld-inspection/v1/jobs" : "/api/welding-guidance/v1/jobs";
      const submitResponse = await fetch(submitPath, { method: "POST", headers: { "Content-Type": "application/json; charset=utf-8", Accept: "application/json" }, body: JSON.stringify(contract), signal });
      const accepted = validateAccepted(application, await jsonResponse(submitResponse));
      setStatus(`\u4efb\u52a1 ${accepted.jobId} \u5df2\u63d0\u4ea4\uff0c\u6b63\u5728\u5904\u7406\u2026`);
      const finalPhase = await poll(application, accepted, signal, operation);
      if (isCurrent(operation) && finalPhase !== null) setStatus("\u7ed3\u679c\u5df2\u51c6\u5907\uff0c\u53ef\u4ee5\u4e0b\u8f7d\u8f93\u51fa\u6587\u4ef6");
    } catch (error) {
      if (error && error.name === "AbortError") return;
      if (isCurrent(operation)) {
        const message = error && error.name === "TypeError"
          ? "\u7f51\u7edc\u8fde\u63a5\u4e0d\u53ef\u7528\uff0c\u8bf7\u68c0\u67e5\u670d\u52a1\u72b6\u6001"
          : error instanceof Error
            ? error.message
            : "\u8bf7\u6c42\u5931\u8d25\uff0c\u8bf7\u7a0d\u540e\u91cd\u8bd5";
        setStatus(message);
        setResultText(application === "weld_inspection" ? "inspection-result" : "guidance-result", "\u672c\u6b21\u64cd\u4f5c\u672a\u5b8c\u6210");
      }
    } finally {
      if (isCurrent(operation)) { state.operation = null; setControls(application, false); }
    }
  };
  document.querySelectorAll("[data-view]").forEach((tab) => tab.addEventListener("click", () => {
    const stopped = stopOperation();
    state.view = tab.dataset.view;
    document.querySelectorAll("[data-view]").forEach((item) => item.classList.toggle("is-active", item === tab));
    document.querySelectorAll("[data-panel]").forEach((panel) => panel.classList.toggle("is-hidden", panel.id !== state.view));
    if (!stopped) setStatus("\u8bf7\u9009\u62e9\u70b9\u4e91\u6587\u4ef6\u5e76\u5f00\u59cb\u64cd\u4f5c");
  }));
  $("inspection-submit").addEventListener("click", () => submit("weld_inspection"));
  $("guidance-submit").addEventListener("click", () => submit("welding_guidance"));
  $("inspection-stop").addEventListener("click", stopOperation);
  $("guidance-stop").addEventListener("click", stopOperation);
})();
)JS";

Result<std::shared_ptr<const http::HttpResponse>> make_resource(
    const http::HttpLimits& limits,
    const std::string_view content_type,
    const std::string_view body) {
    try {
        http::HttpResponse response;
        for (const auto& header : {
                 std::pair<std::string_view, std::string_view>{
                     "Cache-Control", "no-store"},
                 {"X-Content-Type-Options", "nosniff"},
                 {"Referrer-Policy", "no-referrer"},
                 {"X-Frame-Options", "DENY"},
                 {"Content-Security-Policy",
                  "default-src 'none'; script-src 'self'; style-src 'self'; "
                  "connect-src 'self'; img-src 'none'; font-src 'none'; "
                  "object-src 'none'; base-uri 'none'; form-action 'none'; "
                  "frame-ancestors 'none'"}}) {
            auto result = response.set_header(
                std::string{header.first}, std::string{header.second});
            if (!result) {
                return Result<std::shared_ptr<const http::HttpResponse>>::failure(
                    std::move(result).error());
            }
        }
        auto type = response.set_header(
            "Content-Type", std::string{content_type});
        if (!type) {
            return Result<std::shared_ptr<const http::HttpResponse>>::failure(
                std::move(type).error());
        }
        response.set_body(std::string{body});
        auto valid = response.validate(limits);
        if (!valid) {
            return Result<std::shared_ptr<const http::HttpResponse>>::failure(
                std::move(valid).error());
        }
        auto stored = std::make_shared<const http::HttpResponse>(
            std::move(response));
        return Result<std::shared_ptr<const http::HttpResponse>>::success(
            std::move(stored));
    } catch (const std::bad_alloc&) {
        return Result<std::shared_ptr<const http::HttpResponse>>::failure(
            make_error(ErrorCode::ResourceExhausted,
                       "web UI resource allocation failed"));
    } catch (const std::length_error&) {
        return Result<std::shared_ptr<const http::HttpResponse>>::failure(
            make_error(ErrorCode::ResourceExhausted,
                       "web UI resource is too large"));
    } catch (const std::exception&) {
        return Result<std::shared_ptr<const http::HttpResponse>>::failure(
            make_error(ErrorCode::InternalError,
                       "web UI resource preparation failed"));
    }
}

}  // namespace

Result<WebUiHttpApi::Ptr> WebUiHttpApi::create(http::HttpLimits limits) {
    try {
        auto api = Ptr{new WebUiHttpApi{std::move(limits)}};
        auto prepared = api->prepare_resources();
        if (!prepared) {
            return Result<Ptr>::failure(std::move(prepared).error());
        }
        return Result<Ptr>::success(std::move(api));
    } catch (const std::bad_alloc&) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "unable to allocate web UI API"));
    } catch (const std::exception&) {
        return Result<Ptr>::failure(make_error(
            ErrorCode::InternalError,
            "unable to create web UI API"));
    }
}

Result<void> WebUiHttpApi::prepare_resources() {
    auto html = make_resource(
        limits_, "text/html; charset=utf-8", kHtml);
    if (!html) {
        return Result<void>::failure(std::move(html).error());
    }
    auto css = make_resource(
        limits_, "text/css; charset=utf-8", kCss);
    if (!css) {
        return Result<void>::failure(std::move(css).error());
    }
    auto javascript = make_resource(
        limits_, "application/javascript; charset=utf-8", kJavaScript);
    if (!javascript) {
        return Result<void>::failure(std::move(javascript).error());
    }
    resources_ = Resources{
        std::move(html).value(),
        std::move(css).value(),
        std::move(javascript).value()};
    return Result<void>::success();
}

Result<http::HttpResponse> WebUiHttpApi::copy_response(
    const std::shared_ptr<const http::HttpResponse>& resource) const {
    if (!resource) {
        return Result<http::HttpResponse>::failure(make_error(
            ErrorCode::InvalidState,
            "web UI resource is unavailable"));
    }
    try {
        return Result<http::HttpResponse>::success(*resource);
    } catch (const std::bad_alloc&) {
        return Result<http::HttpResponse>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "web UI response allocation failed"));
    } catch (const std::exception&) {
        return Result<http::HttpResponse>::failure(make_error(
            ErrorCode::InternalError,
            "web UI response construction failed"));
    }
}

Result<void> WebUiHttpApi::register_routes(http::HttpRouter& router) {
    try {
        const auto self = shared_from_this();
        auto add = [&router, self](
                       std::string path,
                       std::shared_ptr<const http::HttpResponse> resource) {
            const std::weak_ptr<WebUiHttpApi> weak = self;
            return router.add_route(
                "GET",
                std::move(path),
                [weak, resource = std::move(resource)](
                    const http::HttpRequest&) {
                    auto locked = weak.lock();
                    if (!locked) {
                        return Result<http::HttpResponse>::failure(make_error(
                            ErrorCode::InvalidState,
                            "web UI owner is unavailable"));
                    }
                    return locked->copy_response(resource);
                });
        };
        auto html = add("/", resources_.html);
        if (!html) {
            return html;
        }
        auto css = add("/ui/app.css", resources_.css);
        if (!css) {
            return css;
        }
        return add("/ui/app.js", resources_.javascript);
    } catch (const std::bad_alloc&) {
        return Result<void>::failure(make_error(
            ErrorCode::ResourceExhausted,
            "web UI route allocation failed"));
    } catch (const std::exception&) {
        return Result<void>::failure(make_error(
            ErrorCode::InvalidState,
            "web UI routes cannot be registered"));
    }
}

}  // namespace iaisf::web_ui
