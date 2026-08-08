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
  <script src="/ui/point-cloud-viewer.js" defer></script>
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
      <p class="viewer-description">&#x7070;&#x8272;&#x4E3A;&#x8F93;&#x5165;&#x70B9;&#x4E91;&#xFF0C;&#x7EA2;&#x8272;&#x4E3A;&#x710A;&#x7F1D; overlay&#x3002;</p>
      <div class="viewer-shell"><canvas id="inspection-viewer" aria-label="inspection 3D viewer"></canvas><p id="inspection-viewer-status" class="viewer-status" aria-live="polite">3D viewer loads after upload.</p><div class="viewer-controls"><button id="inspection-reset-view" type="button" class="secondary">Reset view</button><label>Point size <input id="inspection-point-size" type="range" min="1" max="10" value="3"></label><label><input id="inspection-input-layer" type="checkbox" checked> Input</label><label><input id="inspection-overlay-layer" type="checkbox" checked> Weld overlay</label></div></div>
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
      <p class="viewer-description">&#x663E;&#x793A;&#x8F93;&#x5165;&#x70B9;&#x4E91;&#x3001;&#x8D77;&#x70B9;&#x3001;&#x7EC8;&#x70B9;&#x3001;&#x62D0;&#x70B9;&#x4E0E;&#x710A;&#x63A5;&#x8DEF;&#x5F84;&#x3002;&#x65B9;&#x5411;&#x8F74;&#x4EE5; start &#x4E3A;&#x663E;&#x793A;&#x951A;&#x70B9;&#xFF0C;&#x4E0D;&#x4EE3;&#x8868; WeldAgent &#x7B97;&#x6CD5;&#x5750;&#x6807;&#x7CFB;&#x539F;&#x70B9;&#x3002;</p>
      <div class="viewer-shell"><canvas id="guidance-viewer" aria-label="guidance 3D viewer"></canvas><p id="guidance-viewer-status" class="viewer-status" aria-live="polite">3D viewer loads after upload.</p><div class="viewer-controls"><button id="guidance-reset-view" type="button" class="secondary">Reset view</button><label>Point size <input id="guidance-point-size" type="range" min="1" max="10" value="3"></label><label><input id="guidance-input-layer" type="checkbox" checked> Input</label><label><input id="guidance-geometry-layer" type="checkbox" checked> Geometry</label></div></div>
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
.viewer-description { margin: 16px 0 0; color: #526179; }
.result { margin-top: 18px; display: grid; gap: 10px; }
.viewer-shell { margin-top: 20px; padding: 10px; border: 1px solid #d9e1ee; border-radius: 8px; background: #f8fafc; }
.viewer-shell canvas { display: block; width: 100%; height: 360px; min-height: 220px; border-radius: 6px; background: #111827; touch-action: none; }
.viewer-status { min-height: 1.25rem; margin: 8px 0 4px; color: #526179; }
.viewer-controls { display: flex; flex-wrap: wrap; align-items: center; gap: 10px; font-size: .9rem; }
.viewer-controls .secondary { margin-left: 0; padding: 6px 10px; }
.result-card { border: 1px solid #d9e1ee; border-radius: 8px; padding: 12px; }
.result-card dl { display: grid; grid-template-columns: minmax(130px, 1fr) 2fr; gap: 6px 14px; margin: 0; }
.result-card dt { color: #526179; }
.result-card dd { margin: 0; overflow-wrap: anywhere; }
.result-card a { color: #174bc1; }
footer { margin-top: 20px; font-size: .9rem; }
@media (max-width: 600px) { .shell { padding: 16px 12px 32px; } .result-card dl { grid-template-columns: 1fr; gap: 2px; } }
)CSS";

constexpr char kViewerJavaScript[] = R"JS((() => {
  "use strict";
  const MAX_DISPLAY_POINTS = 500000;
  const XYZ_MEDIA = "application/vnd.iaisf.pointcloud.xyz-f32le";
  const PLY_MEDIA = "application/vnd.iaisf.pointcloud.ply";
  const finite = (value) => Number.isFinite(value);
  const point = (value) => Array.isArray(value) && value.length === 3 && value.every(finite);
  const artifactId = (value) => typeof value === "string" && /^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$/.test(value);
  const canonicalDownload = (url, id) => {
    if (!artifactId(id) || typeof url !== "string") return false;
    const match = /^\/api\/artifacts\/v1\/files\/([A-Za-z0-9][A-Za-z0-9._-]{0,127})$/.exec(url);
    return Boolean(match && match[1] === id);
  };
  const DECIMAL_INTEGER = /^(?:0|[1-9][0-9]*)$/;
  const DECIMAL_FLOAT = /^[+-]?(?:(?:[0-9]+(?:\.[0-9]*)?)|(?:\.[0-9]+))(?:[eE][+-]?[0-9]+)?$/;
  const tokenMatches = (token, grammar) => typeof token === "string" && token.length > 0 && token.length <= 64 && grammar.test(token);
  const parseUnsignedInteger = (token) => {
    if (!tokenMatches(token, DECIMAL_INTEGER)) return null;
    const value = Number(token);
    return Number.isSafeInteger(value) ? value : null;
  };
  const parseFiniteDecimal = (token) => {
    if (!tokenMatches(token, DECIMAL_FLOAT)) return null;
    const value = Number(token);
    return Number.isFinite(value) ? value : null;
  };
  const setText = (node, text) => { if (node) node.textContent = String(text).slice(0, 256); };
  const artifactBytes = async (artifact, signal, media) => {
    const kindValid = media === XYZ_MEDIA ? artifact && artifact.kind === "point_cloud"
      : media === PLY_MEDIA && artifact && artifact.kind === "weld_points";
    if (!artifact || typeof artifact !== "object" || !kindValid ||
        !artifactId(artifact.artifact_id) || !/^[0-9a-f]{64}$/.test(artifact.sha256 || "") ||
        !canonicalDownload(artifact.download_url, artifact.artifact_id) ||
        typeof artifact.download_url !== "string" || typeof artifact.media_type !== "string" || artifact.media_type !== media ||
        typeof artifact.coordinate_frame !== "string" || artifact.coordinate_frame.length === 0 ||
        typeof artifact.unit !== "string" || artifact.unit.length === 0 ||
        !Number.isSafeInteger(artifact.size_bytes) || artifact.size_bytes <= 0 ||
        !Number.isSafeInteger(artifact.point_count) || artifact.point_count <= 0) {
      throw new Error("3D Artifact metadata is invalid");
    }
    const response = await fetch(artifact.download_url, { headers: { Accept: media }, signal });
    if (!response.ok) throw new Error("3D Artifact download failed");
    const contentType = (response.headers.get("content-type") || "").split(";", 1)[0].trim().toLowerCase();
    if (contentType !== media) throw new Error("3D Artifact media type is invalid");
    const bytes = await response.arrayBuffer();
    if (bytes.byteLength !== artifact.size_bytes) throw new Error("3D Artifact size is invalid");
    return bytes;
  };
  const parseXyz = (bytes, artifact) => {
    if (!Number.isSafeInteger(artifact.point_count) || artifact.point_count <= 0 ||
        artifact.size_bytes !== artifact.point_count * 12 || bytes.byteLength !== artifact.size_bytes) {
      throw new Error("XYZ Artifact point count is invalid");
    }
    const view = new DataView(bytes);
    const count = artifact.point_count;
    const bbox = { min: [Infinity, Infinity, Infinity], max: [-Infinity, -Infinity, -Infinity] };
    for (let i = 0; i < count; ++i) {
      for (let axis = 0; axis < 3; ++axis) {
        const value = view.getFloat32(i * 12 + axis * 4, true);
        if (!finite(value)) throw new Error("XYZ Artifact contains non-finite data");
        bbox.min[axis] = Math.min(bbox.min[axis], value);
        bbox.max[axis] = Math.max(bbox.max[axis], value);
      }
    }
    const extent = Math.max(bbox.max[0] - bbox.min[0], bbox.max[1] - bbox.min[1], bbox.max[2] - bbox.min[2]);
    if (!finite(extent) || extent <= 0) throw new Error("XYZ Artifact has no display extent");
    const center = bbox.min.map((value, axis) => (value + bbox.max[axis]) / 2);
    const stride = Math.max(1, Math.ceil(count / MAX_DISPLAY_POINTS));
    const shown = Math.ceil(count / stride);
    const positions = new Float32Array(shown * 3);
    let output = 0;
    for (let i = 0; i < count; i += stride) {
      const base = i * 12;
      for (let axis = 0; axis < 3; ++axis) {
        positions[output * 3 + axis] = (view.getFloat32(base + axis * 4, true) - center[axis]) / extent;
      }
      ++output;
    }
    return { positions, count: shown, totalCount: count, bbox, center, extent };
  };
  const parsePly = (bytes, artifact, cloud, expectedCount) => {
    if (!Number.isSafeInteger(artifact.point_count) || artifact.point_count <= 0 ||
        artifact.point_count !== expectedCount) throw new Error("PLY point count is inconsistent");
    const text = new TextDecoder("utf-8", { fatal: true }).decode(bytes);
    const headerMatch = /(?:^|\r?\n)end_header\r?\n/.exec(text);
    if (!headerMatch || headerMatch.index + headerMatch[0].length > 16384) throw new Error("PLY header is invalid");
    const headerEnd = headerMatch.index + headerMatch[0].length;
    const header = text.slice(0, headerEnd).split(/\r?\n/);
    if (header.length > 0 && header[header.length - 1] === "") header.pop();
    if (header.some((line) => line.trim().length === 0)) throw new Error("PLY header whitespace is invalid");
    if (header[0] !== "ply" || header[1] !== "format ascii 1.0") throw new Error("PLY format is unsupported");
    const elements = header.filter((line) => line.startsWith("element "));
    const vertex = header.find((line) => line.startsWith("element vertex "));
    const vertexCount = parseUnsignedInteger(vertex ? vertex.slice(15) : null);
    if (elements.length !== 1 || !vertex || vertexCount === null || vertexCount !== expectedCount) throw new Error("PLY vertex count is invalid");
    const properties = header.filter((line) => line.startsWith("property "));
    const required = ["property float x", "property float y", "property float z", "property int label", "property float confidence"];
    if (properties.length !== required.length || properties.some((value, index) => value !== required[index])) throw new Error("PLY properties are unsupported");
    const rows = text.slice(headerEnd).split(/\r?\n/);
    if (rows.length > 0 && rows[rows.length - 1] === "") rows.pop();
    if (rows.some((line) => line.trim().length === 0)) throw new Error("PLY body whitespace is invalid");
    if (rows.length !== expectedCount) throw new Error("PLY body length is invalid");
    const stride = Math.max(1, Math.ceil(expectedCount / MAX_DISPLAY_POINTS));
    const shown = Math.ceil(expectedCount / stride);
    const positions = new Float32Array(shown * 3);
    const epsilon = Math.max(1e-4, cloud.extent * 1e-5);
    for (let i = 0; i < rows.length; ++i) {
      const values = rows[i].trim().split(/\s+/);
      if (values.length !== 5 || values[3] !== "0") throw new Error("PLY row is invalid");
      const xyz = values.slice(0, 3).map(parseFiniteDecimal);
      const confidence = parseFiniteDecimal(values[4]);
      if (!xyz.every((value) => value !== null) || confidence === null || confidence < 0 || confidence > 1) throw new Error("PLY row is invalid");
      for (let axis = 0; axis < 3; ++axis) {
        if (xyz[axis] < cloud.bbox.min[axis] - epsilon || xyz[axis] > cloud.bbox.max[axis] + epsilon) throw new Error("PLY overlay is outside input cloud");
        if (i % stride === 0) positions[Math.floor(i / stride) * 3 + axis] = (xyz[axis] - cloud.center[axis]) / cloud.extent;
      }
    }
     return { positions, count: shown, totalCount: expectedCount };
   };
)JS"
R"JS(
   const compile = (gl, type, source) => {
    const shader = gl.createShader(type); gl.shaderSource(shader, source); gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) { gl.deleteShader(shader); throw new Error("WebGL shader compilation failed"); }
    return shader;
  };
  const create = (canvas, statusNode) => {
    const gl = canvas.getContext("webgl2", { antialias: true, alpha: false });
    const state = { input: null, overlay: null, geometry: null, inputVisible: true, overlayVisible: true, geometryVisible: true, pointSize: 3, zoom: 1.6, pan: [0, 0], rotation: [0.25, -0.35], frame: null, disposed: false, dirty: true, raf: 0 };
    const status = (text) => setText(statusNode, text);
    let program = null; let buffer = null; let positionLocation = -1; let rotationLocation = null; let zoomLocation = null; let panLocation = null; let sizeLocation = null; let colorLocation = null; let resizeObserver = null;
    if (!gl) { status("WebGL2 is unavailable; text results and downloads remain available"); }
    const vertexSource = "#version 300 es\nin vec3 a_position; uniform vec2 u_rotation; uniform float u_zoom; uniform vec2 u_pan; uniform float u_point_size; void main(){float cx=cos(u_rotation.x),sx=sin(u_rotation.x),cy=cos(u_rotation.y),sy=sin(u_rotation.y); vec3 p=a_position; p=vec3(p.x, p.y*cx-p.z*sx, p.y*sx+p.z*cx); p=vec3(p.x*cy+p.z*sy,p.y,-p.x*sy+p.z*cy); gl_Position=vec4(p.xy*u_zoom+u_pan,p.z,1.0); gl_PointSize=u_point_size;}";
    const fragmentSource = "#version 300 es\nprecision mediump float; uniform vec4 u_color; out vec4 outColor; void main(){outColor=u_color;}";
    if (gl) {
      try {
        const shaders = [compile(gl, gl.VERTEX_SHADER, vertexSource), compile(gl, gl.FRAGMENT_SHADER, fragmentSource)];
        program = gl.createProgram(); shaders.forEach((shader) => gl.attachShader(program, shader)); gl.linkProgram(program); shaders.forEach((shader) => gl.deleteShader(shader));
        if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error("WebGL program link failed");
        buffer = gl.createBuffer(); positionLocation = gl.getAttribLocation(program, "a_position"); rotationLocation = gl.getUniformLocation(program, "u_rotation"); zoomLocation = gl.getUniformLocation(program, "u_zoom"); panLocation = gl.getUniformLocation(program, "u_pan"); sizeLocation = gl.getUniformLocation(program, "u_point_size"); colorLocation = gl.getUniformLocation(program, "u_color");
      } catch (_) { status("3D renderer initialization failed; text results and downloads remain available"); }
    }
    const schedule = () => { state.dirty = true; if (!state.raf && !state.disposed) state.raf = window.requestAnimationFrame(render); };
    const upload = (layer) => { if (!gl || !program || !layer) return; gl.bindBuffer(gl.ARRAY_BUFFER, buffer); gl.bufferData(gl.ARRAY_BUFFER, layer.positions, gl.STATIC_DRAW); };
    const render = () => { state.raf = 0; if (state.disposed || !gl || !program) return; if (!state.dirty) return; state.dirty = false; const dpr = Math.min(window.devicePixelRatio || 1, 2); const width = Math.max(1, Math.floor(canvas.clientWidth * dpr)); const height = Math.max(1, Math.floor(canvas.clientHeight * dpr)); if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; } gl.viewport(0, 0, width, height); gl.clearColor(0.067,0.094,0.153,1); gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT); gl.useProgram(program); gl.uniform2fv(rotationLocation, state.rotation); gl.uniform1f(zoomLocation, state.zoom); gl.uniform2fv(panLocation, state.pan); gl.enableVertexAttribArray(positionLocation); gl.bindBuffer(gl.ARRAY_BUFFER, buffer); gl.vertexAttribPointer(positionLocation, 3, gl.FLOAT, false, 0, 0); const draw = (layer, color, mode, size) => { if (!layer) return; upload(layer); gl.uniform4fv(colorLocation, color); gl.uniform1f(sizeLocation, size); gl.drawArrays(mode, 0, layer.count); }; if (state.inputVisible) draw(state.input, [0.72,0.78,0.88,0.52], gl.POINTS, state.pointSize); if (state.overlayVisible) draw(state.overlay, [1,0.25,0.08,1], gl.POINTS, state.pointSize + 2); if (state.geometryVisible && state.geometry) { draw(state.geometry.path, [0.1,0.75,1,1], gl.LINE_STRIP, 2); draw(state.geometry.markers, [1,0.85,0.1,1], gl.POINTS, state.pointSize + 4); if (state.geometry.axes) state.geometry.axes.forEach((axis) => draw(axis, axis.color, gl.LINES, 2)); } };
    const resetView = () => { state.zoom = 1.6; state.pan = [0, 0]; state.rotation = [0.25, -0.35]; schedule(); };
    const clear = () => { state.input = null; state.overlay = null; state.geometry = null; state.frame = null; resetView(); status("3D scene cleared"); };
  const loadInputArtifact = async (artifact, signal) => { try { const bytes = await artifactBytes(artifact, signal, XYZ_MEDIA); state.input = parseXyz(bytes, artifact); state.frame = artifact; schedule(); status(`input cloud: ${state.input.count} / ${state.input.totalCount} points`); return state.input; } catch (error) { state.input = null; schedule(); if (error && error.name === "AbortError") throw error; status(error instanceof Error ? error.message : "input 3D parsing failed"); throw error; } };
    const setInspectionOverlay = async (artifact, expectedCount, signal) => { if (!artifact) { state.overlay = null; schedule(); return; } try { if (!state.frame || artifact.coordinate_frame !== state.frame.coordinate_frame || artifact.unit !== state.frame.unit) throw new Error("overlay coordinate metadata mismatch"); const bytes = await artifactBytes(artifact, signal, PLY_MEDIA); state.overlay = parsePly(bytes, artifact, state.input, expectedCount); schedule(); status("PTV2 weld overlay loaded"); } catch (error) { if (error && error.name === "AbortError") throw error; state.overlay = null; schedule(); status(error instanceof Error ? error.message : "PTV2 overlay parsing failed"); } };
    const setGuidanceGeometry = (result) => { try { if (!state.input || result.coordinate_frame !== state.frame.coordinate_frame || result.unit !== state.frame.unit || !point(result.start) || !point(result.end)) throw new Error("guidance geometry metadata or points are invalid"); const pathPoints = [result.start]; if (result.weld_type === "l") { if (!point(result.corner)) throw new Error("L result has no corner"); pathPoints.push(result.corner); } else if (result.corner !== null && result.corner !== undefined) throw new Error("corner is not allowed for this weld type"); pathPoints.push(result.end); const outside = (value) => value.some((item, axis) => item < state.input.bbox.min[axis] - Math.max(1, state.input.extent * .02) || item > state.input.bbox.max[axis] + Math.max(1, state.input.extent * .02)); if (pathPoints.some(outside)) throw new Error("guidance geometry is outside input cloud"); const normalize = (value) => new Float32Array(value.map((item, axis) => (item - state.input.center[axis]) / state.input.extent)); const path = new Float32Array(pathPoints.length * 3); pathPoints.forEach((value, index) => path.set(normalize(value), index * 3)); const markers = new Float32Array(path); let axes = null; const vectors = [result.x_axis, result.y_axis, result.z_axis]; if (vectors.every(point)) { const norms = vectors.map((value) => Math.hypot(...value)); const dots = [vectors[0][0]*vectors[1][0]+vectors[0][1]*vectors[1][1]+vectors[0][2]*vectors[1][2], vectors[0][0]*vectors[2][0]+vectors[0][1]*vectors[2][1]+vectors[0][2]*vectors[2][2], vectors[1][0]*vectors[2][0]+vectors[1][1]*vectors[2][1]+vectors[1][2]*vectors[2][2]]; const cross = [vectors[0][1]*vectors[1][2]-vectors[0][2]*vectors[1][1], vectors[0][2]*vectors[1][0]-vectors[0][0]*vectors[1][2], vectors[0][0]*vectors[1][1]-vectors[0][1]*vectors[1][0]]; const handed = cross[0]*vectors[2][0]+cross[1]*vectors[2][1]+cross[2]*vectors[2][2]; if (norms.every((value) => Math.abs(value - 1) <= .05) && dots.every((value) => Math.abs(value) <= .05) && handed >= .8) { const anchor = normalize(result.start); const axisLength = .15; const colors = [[1,0,0,1],[0,1,0,1],[0,0,1,1]]; axes = vectors.map((value, index) => { const positions = new Float32Array(6); positions.set(anchor, 0); positions.set(anchor.map((item, axis) => item + value[axis] * axisLength), 3); return { positions, count: 2, color: colors[index] }; }); } } state.geometry = { path: { positions: path, count: pathPoints.length }, markers: { positions: markers, count: pathPoints.length }, axes }; schedule(); status(axes ? "guidance path and display-anchor axes loaded" : "guidance path loaded; axes failed validation"); } catch (error) { state.geometry = null; schedule(); status(error instanceof Error ? error.message : "guidance geometry parsing failed"); } };
    const setLayerVisibility = (values) => { if (!values || typeof values !== "object") return; if (typeof values.input === "boolean") state.inputVisible = values.input; if (typeof values.overlay === "boolean") state.overlayVisible = values.overlay; if (typeof values.geometry === "boolean") state.geometryVisible = values.geometry; schedule(); };
    const setPointSize = (value) => { if (Number.isFinite(Number(value))) { state.pointSize = Math.min(10, Math.max(1, Number(value))); schedule(); } };
    const onPointer = (event) => { if (!state.drag) return; const dx = event.clientX - state.drag.x; const dy = event.clientY - state.drag.y; state.drag.x = event.clientX; state.drag.y = event.clientY; if (event.buttons === 1) { state.rotation[1] += dx * .01; state.rotation[0] += dy * .01; } else { state.pan[0] += dx / Math.max(1, canvas.clientWidth); state.pan[1] -= dy / Math.max(1, canvas.clientHeight); } schedule(); };
    const onPointerDown = (event) => { state.drag = { x: event.clientX, y: event.clientY }; canvas.setPointerCapture(event.pointerId); };
    const onPointerUp = (event) => { state.drag = null; if (canvas.hasPointerCapture(event.pointerId)) canvas.releasePointerCapture(event.pointerId); };
    const onPointerCancel = (event) => { state.drag = null; if (canvas.hasPointerCapture(event.pointerId)) canvas.releasePointerCapture(event.pointerId); };
    const onLostPointerCapture = () => { state.drag = null; };
    const onContextMenu = (event) => event.preventDefault();
    const onWheel = (event) => { event.preventDefault(); state.zoom = Math.min(8, Math.max(.2, state.zoom * (event.deltaY < 0 ? 1.1 : .9))); schedule(); };
    const onContextLost = (event) => { event.preventDefault(); state.dirty = false; status("WebGL context lost; text results and downloads remain available"); };
    canvas.addEventListener("pointerdown", onPointerDown); canvas.addEventListener("pointermove", onPointer); canvas.addEventListener("pointerup", onPointerUp); canvas.addEventListener("pointercancel", onPointerCancel); canvas.addEventListener("lostpointercapture", onLostPointerCapture); canvas.addEventListener("contextmenu", onContextMenu); canvas.addEventListener("wheel", onWheel, { passive: false }); canvas.addEventListener("webglcontextlost", onContextLost);
    resizeObserver = typeof ResizeObserver === "function" ? new ResizeObserver(schedule) : null; if (resizeObserver) resizeObserver.observe(canvas); resetView();
    return { loadInputArtifact, setInspectionOverlay, setGuidanceGeometry, setLayerVisibility, setPointSize, resetView, clear, dispose: () => { if (state.disposed) return; state.disposed = true; if (state.raf) { window.cancelAnimationFrame(state.raf); state.raf = 0; } if (resizeObserver) { resizeObserver.disconnect(); resizeObserver = null; } canvas.removeEventListener("pointerdown", onPointerDown); canvas.removeEventListener("pointermove", onPointer); canvas.removeEventListener("pointerup", onPointerUp); canvas.removeEventListener("pointercancel", onPointerCancel); canvas.removeEventListener("lostpointercapture", onLostPointerCapture); canvas.removeEventListener("contextmenu", onContextMenu); canvas.removeEventListener("wheel", onWheel); canvas.removeEventListener("webglcontextlost", onContextLost); if (gl && buffer) gl.deleteBuffer(buffer); if (gl && program) gl.deleteProgram(program); buffer = null; program = null; state.input = null; state.overlay = null; state.geometry = null; state.frame = null; } };
  };
  window.IaisfPointCloudViewer = { create };
})();)JS";

constexpr char kJavaScript[] = R"JS((() => {
  "use strict";
  const MAX_UPLOAD_BYTES = 64 * 1024 * 1024;
  const POLL_INTERVAL_MS = 1000;
  const POLL_TIMEOUT_MS = 10 * 60 * 1000;
  const MAX_ERROR_TEXT = 256;
  const state = { view: "inspection", operation: null, generation: 0 };
  const $ = (id) => document.getElementById(id);
  const viewers = {
    weld_inspection: window.IaisfPointCloudViewer ? window.IaisfPointCloudViewer.create($("inspection-viewer"), $("inspection-viewer-status")) : null,
    welding_guidance: window.IaisfPointCloudViewer ? window.IaisfPointCloudViewer.create($("guidance-viewer"), $("guidance-viewer-status")) : null
  };
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
  const renderVisualization = async (application, result, operation) => {
    const viewer = viewers[application];
    if (!viewer || !isCurrent(operation)) return;
    try {
      if (application === "weld_inspection") {
        await viewer.setInspectionOverlay(result.weld_points || null, result.weld_point_count, operation.controller.signal);
      } else {
        viewer.setGuidanceGeometry(result);
      }
    } catch (error) {
      if (error && error.name === "AbortError") throw error;
    }
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
        if (application === "weld_inspection") operation.weldPointCount = result.weld_point_count;
        renderResult(application, result, operation);
        await renderVisualization(application, result, operation);
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
    const operation = { application, generation: ++state.generation, controller: new AbortController(), inputPointCount: null, weldPointCount: null };
    state.operation = operation;
    const signal = operation.controller.signal;
    if (viewers[application]) viewers[application].clear();
    setControls(application, true); setStatus("\u6b63\u5728\u4e0a\u4f20\u70b9\u4e91\u2026");
    try {
      const uploadResponse = await fetch("/api/artifacts/v1/pointclouds", { method: "POST", headers: { "Content-Type": "text/plain; charset=utf-8" }, body: file, signal });
      const upload = await jsonResponse(uploadResponse);
      const artifact = strictArtifact(upload.artifact);
      if (canonicalDownloadId(upload.download_url) !== artifact.artifact_id) throw new Error("\u4e0a\u4f20\u54cd\u5e94\u4e0b\u8f7d\u5730\u5740\u65e0\u6548");
      operation.inputPointCount = artifact.point_count;
      if (viewers[application]) {
        const viewerArtifact = { ...artifact, download_url: upload.download_url };
        try { await viewers[application].loadInputArtifact(viewerArtifact, signal); }
        catch (error) { if (error && error.name === "AbortError") throw error; }
      }
      const contract = application === "weld_inspection"
        ? { schema_version: "1.0", input_artifacts: [artifact], requested_outputs: [$("inspection-segmentation").checked ? "segmentation" : null, $("inspection-geometry").checked ? "geometry" : null].filter(Boolean) }
        : { schema_version: "1.0", input_artifacts: [artifact], weld_type: $("guidance-type").value === "auto" ? { mode: "auto" } : { mode: "requested", requested: $("guidance-type").value }, review_policy: { human_checkpoint: "required" } };
      setStatus("\u6b63\u5728\u63d0\u4ea4\u4efb\u52a1\u2026");
      const submitPath = application === "weld_inspection" ? "/api/weld-inspection/v1/jobs" : "/api/welding-guidance/v1/jobs";
      const submitResponse = await fetch(submitPath, { method: "POST", headers: { "Content-Type": "application/json; charset=utf-8", Accept: "application/json" }, body: JSON.stringify(contract), signal });
      const accepted = validateAccepted(application, await jsonResponse(submitResponse));
      setStatus(`\u4efb\u52a1 ${accepted.jobId} \u5df2\u63d0\u4ea4\uff0c\u6b63\u5728\u5904\u7406\u2026`);
      const finalPhase = await poll(application, accepted, signal, operation);
      if (isCurrent(operation) && finalPhase !== null) setStatus(application === "weld_inspection" && operation.weldPointCount === 0 ? "\u672a\u68c0\u6d4b\u5230\u710a\u7f1d\u70b9\uff0c\u4ecd\u53ef\u4e0b\u8f7d\u5408\u6cd5\u8f93\u51fa" : "\u7ed3\u679c\u5df2\u51c6\u5907\uff0c\u53ef\u4ee5\u4e0b\u8f7d\u8f93\u51fa\u6587\u4ef6");
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
    if (viewers[state.view]) viewers[state.view].clear();
    state.view = tab.dataset.view;
    document.querySelectorAll("[data-view]").forEach((item) => item.classList.toggle("is-active", item === tab));
    document.querySelectorAll("[data-panel]").forEach((panel) => panel.classList.toggle("is-hidden", panel.id !== state.view));
    if (!stopped) setStatus("\u8bf7\u9009\u62e9\u70b9\u4e91\u6587\u4ef6\u5e76\u5f00\u59cb\u64cd\u4f5c");
  }));
  $("inspection-submit").addEventListener("click", () => submit("weld_inspection"));
  $("guidance-submit").addEventListener("click", () => submit("welding_guidance"));
  $("inspection-stop").addEventListener("click", stopOperation);
  $("guidance-stop").addEventListener("click", stopOperation);
  $("inspection-reset-view").addEventListener("click", () => viewers.weld_inspection && viewers.weld_inspection.resetView());
  $("guidance-reset-view").addEventListener("click", () => viewers.welding_guidance && viewers.welding_guidance.resetView());
  $("inspection-point-size").addEventListener("input", (event) => viewers.weld_inspection && viewers.weld_inspection.setPointSize(event.target.value));
  $("guidance-point-size").addEventListener("input", (event) => viewers.welding_guidance && viewers.welding_guidance.setPointSize(event.target.value));
  $("inspection-input-layer").addEventListener("change", (event) => viewers.weld_inspection && viewers.weld_inspection.setLayerVisibility({ input: event.target.checked }));
  $("inspection-overlay-layer").addEventListener("change", (event) => viewers.weld_inspection && viewers.weld_inspection.setLayerVisibility({ overlay: event.target.checked }));
  $("guidance-input-layer").addEventListener("change", (event) => viewers.welding_guidance && viewers.welding_guidance.setLayerVisibility({ input: event.target.checked }));
  $("guidance-geometry-layer").addEventListener("change", (event) => viewers.welding_guidance && viewers.welding_guidance.setLayerVisibility({ geometry: event.target.checked }));
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
    auto viewer = make_resource(
        limits_, "application/javascript; charset=utf-8", kViewerJavaScript);
    if (!viewer) {
        return Result<void>::failure(std::move(viewer).error());
    }
    auto javascript = make_resource(
        limits_, "application/javascript; charset=utf-8", kJavaScript);
    if (!javascript) {
        return Result<void>::failure(std::move(javascript).error());
    }
    resources_ = Resources{
        std::move(html).value(),
        std::move(css).value(),
        std::move(viewer).value(),
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
        auto viewer = add("/ui/point-cloud-viewer.js", resources_.viewer);
        if (!viewer) {
            return viewer;
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
