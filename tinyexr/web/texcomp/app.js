/*
 * TinyEXR texcomp browser demo.
 *
 * Drives the WASM module (tcw_*) built from web/texcomp/texcomp_web.c.
 * Copyright (c) 2014-2026 Syoyo Fujita and TinyEXR authors
 * SPDX-License-Identifier: Apache-2.0
 */
import Module from "./texcomp_web.mjs";

const M = await Module();

/* Codec ids must match the enum in texcomp_web.c. */
const CODECS = [
  { id: 0,  name: "BC1",       bpp: 4,  note: "RGB, 4 colours/block" },
  { id: 1,  name: "BC3",       bpp: 8,  note: "RGBA (BC1 + BC4 alpha)" },
  { id: 2,  name: "BC5",       bpp: 8,  note: "2 channels (normals)" },
  { id: 3,  name: "BC7",       bpp: 8,  note: "RGBA, best LDR desktop" },
  { id: 4,  name: "BC6H",      bpp: 8,  note: "HDR RGB", hdr: true },
  { id: 5,  name: "ETC2 RGB",  bpp: 4,  note: "mobile" },
  { id: 6,  name: "ETC2 RGBA", bpp: 8,  note: "mobile + EAC alpha" },
  { id: 7,  name: "EAC R11",   bpp: 4,  note: "1 channel" },
  { id: 8,  name: "EAC RG11",  bpp: 8,  note: "2 channels (normals)" },
  { id: 9,  name: "ASTC",      bpp: 8,  note: "LDR, variable block" },
  { id: 10, name: "ASTC HDR",  bpp: 8,  note: "HDR RGB", hdr: true },
  { id: 11, name: "uni",       bpp: 8,  note: "UASTC intermediate → transcode" },
];
const ASTC_ID = 9, ASTC_HDR_ID = 10;

const $ = (id) => document.getElementById(id);
const state = { loaded: false, name: "" };

/* ----------------------------------------------------------------- wasm --- */

function msg() { return M.UTF8ToString(M._tcw_message()); }

function loadEXR(bytes, name) {
  const p = M._malloc(bytes.length);
  M.HEAPU8.set(bytes, p);
  const ok = M._tcw_load_exr(p, bytes.length);
  M._free(p);
  if (!ok) throw new Error(msg());
  state.loaded = true;
  state.name = name;
  $("srcInfo").textContent = `${name} — ${msg()}`;
}

/* Browser-decoded PNG/JPEG via canvas. */
async function loadImageFile(file) {
  const bmp = await createImageBitmap(file);
  const cv = new OffscreenCanvas(bmp.width, bmp.height);
  const cx = cv.getContext("2d");
  cx.drawImage(bmp, 0, 0);
  const d = cx.getImageData(0, 0, bmp.width, bmp.height).data;
  const p = M._malloc(d.length);
  M.HEAPU8.set(d, p);
  const ok = M._tcw_load_rgba8(p, bmp.width, bmp.height);
  M._free(p);
  if (!ok) throw new Error(msg());
  state.loaded = true;
  state.name = file.name;
  $("srcInfo").textContent = `${file.name} — ${msg()}`;
}

/* Copy a tonemapped preview out of wasm into a canvas. */
function draw(canvasId, which, exposure, gamma, errScale) {
  const w = M._tcw_work_width(), h = M._tcw_work_height();
  if (!w || !h) return;
  const ptr = M._tcw_preview(which, exposure, gamma, errScale || 1);
  if (!ptr) return;
  const px = new Uint8ClampedArray(M.HEAPU8.buffer, ptr, w * h * 4);
  const cv = $(canvasId);
  cv.width = w;
  cv.height = h;
  cv.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(px), w, h), 0, 0);
}

function download(bytes, name, mime) {
  const blob = new Blob([bytes], { type: mime || "application/octet-stream" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = name;
  a.click();
  URL.revokeObjectURL(a.href);
}

/* Compress + decode + measure, on the current working image. */
function runCodec(codec, quality, bx, by, srgb, normalWeighted) {
  const bytes = M._tcw_compress(codec, quality, bx, by, srgb ? 1 : 0,
                                normalWeighted ? 1 : 0);
  if (!bytes) throw new Error(msg());
  if (!M._tcw_decode(codec, bx, by)) throw new Error(msg());
  const w = M._tcw_work_width(), h = M._tcw_work_height();
  return {
    bytes,
    raw: w * h * 4,
    psnr: M._tcw_psnr(3),
    encMs: M._tcw_encode_ms(),
    decMs: M._tcw_decode_ms(),
  };
}

/* -------------------------------------------------------------- controls --- */

for (const c of CODECS) {
  const o = document.createElement("option");
  o.value = c.id;
  o.textContent = `${c.name} — ${c.note}`;
  $("codec").appendChild(o);
}
$("codec").value = 3; /* BC7 */

$("codec").addEventListener("change", () => {
  const isAstc = +$("codec").value === ASTC_ID;
  $("astcBlockWrap").classList.toggle("hidden", !isAstc);
});

document.querySelectorAll(".tab").forEach((t) => {
  t.addEventListener("click", () => {
    document.querySelectorAll(".tab").forEach((x) => x.classList.remove("active"));
    document.querySelectorAll(".panel").forEach((x) => x.classList.remove("active"));
    t.classList.add("active");
    $(t.dataset.panel).classList.add("active");
  });
});

function astcBlock() {
  const isAstc = +$("codec").value === ASTC_ID;
  if (!isAstc) return [4, 4];
  return $("astcBlock").value.split(",").map(Number);
}

function need() {
  if (!state.loaded) throw new Error("Load an image first");
}

function setStats(el, r, label) {
  const ratio = (r.raw / r.bytes).toFixed(1);
  el.innerHTML =
    `<span><b>${label}</b></span>` +
    `<span>PSNR <b>${r.psnr.toFixed(2)} dB</b></span>` +
    `<span>${(r.bytes / 1024).toFixed(1)} KiB <span class="dim">(${ratio}:1 vs RGBA8)</span></span>` +
    `<span class="dim">encode ${r.encMs.toFixed(1)} ms · decode ${r.decMs.toFixed(1)} ms</span>`;
}

/* ------------------------------------------------------ resize + compress --- */

/* Put the source into the working slot: either as loaded, or resized. The
 * filter and the sRGB-aware toggle only mean anything when we actually resample,
 * so they are disabled at original size. */
function prepareWork() {
  const size = +$("size").value;
  if (size === 0) {
    if (!M._tcw_use_source()) throw new Error(msg());
    return;
  }
  if (!M._tcw_resize(size, size, +$("filter").value, 0,
                     $("srgbAware").checked ? 1 : 0, 0))
    throw new Error(msg());
}

function syncResizeControls() {
  const off = +$("size").value === 0;
  $("filter").disabled = off;
  $("srgbAware").disabled = off;
}
$("size").addEventListener("change", syncResizeControls);
syncResizeControls();

$("btnRun").addEventListener("click", () => {
  try {
    need();
    const [bx, by] = astcBlock();
    const codec = +$("codec").value;
    const isHdr = M._tcw_codec_is_hdr(codec);
    prepareWork();
    const srgb = +$("size").value !== 0 && $("srgbAware").checked;
    const r = runCodec(codec, +$("quality").value, bx, by, srgb, false);
    const gamma = isHdr || M._tcw_src_is_hdr() ? 2.2 : 1.0;
    draw("cA", 0, 1, gamma, 1);
    draw("cB", 1, 1, gamma, 1);
    draw("cC", 2, 1, 1, +$("errScale").value);
    setStats($("stats"), r, CODECS.find((c) => c.id === codec).name);
    $("ktxInfo").textContent = "";
  } catch (e) {
    $("stats").textContent = "⚠ " + e.message;
  }
});

$("errScale").addEventListener("input", () => {
  $("errX").textContent = $("errScale").value;
  if (M._tcw_work_width()) draw("cC", 2, 1, 1, +$("errScale").value);
});

/* Containers: run the real texpipe pipeline (mips + KTX2/DDS). */
function writeContainer(container, ext, mime) {
  try {
    need();
    const codec = +$("codec").value;
    const [bx, by] = astcBlock();
    prepareWork();
    const srgb = +$("size").value !== 0 && $("srgbAware").checked;
    const n = M._tcw_write_container(codec, container, 0, +$("quality").value,
                                     bx, by, srgb ? 1 : 0, 1);
    if (!n) throw new Error(msg());
    const ptr = M._tcw_container_ptr();
    const bytes = new Uint8Array(M.HEAPU8.buffer, ptr, n).slice();
    if (container === 1)
      $("ktxInfo").textContent = M.UTF8ToString(M._tcw_ktx2_info(ptr, n));
    download(bytes, state.name.replace(/\.[^.]+$/, "") + ext, mime);
  } catch (e) {
    $("ktxInfo").textContent = "⚠ " + e.message;
  }
}
$("btnKtx2").addEventListener("click", () => writeContainer(1, ".ktx2", "image/ktx2"));
$("btnDds").addEventListener("click", () => writeContainer(0, ".dds", "image/vnd-ms.dds"));

/* ------------------------------------------------------------------- HDR --- */

function hdrExposure() { return Math.pow(2, +$("hdrExp").value); }

$("hdrExp").addEventListener("input", () => {
  $("hdrExpV").textContent = (+$("hdrExp").value).toFixed(1) + " EV";
  if (M._tcw_work_width()) {
    draw("hA", 0, hdrExposure(), 2.2, 1);
    draw("hB", 1, hdrExposure(), 2.2, 1);
  }
});

$("btnHdr").addEventListener("click", () => {
  try {
    need();
    if (!M._tcw_src_is_hdr())
      $("hdrStats").textContent =
        "note: this source is LDR-range — load a scene-linear EXR to see the real difference. ";
    const codec = +$("hdrCodec").value;
    if (!M._tcw_resize(256, 256, 5, 0, 0, 0)) throw new Error(msg());
    const r = runCodec(codec, 1, 4, 4, false, false);
    draw("hA", 0, hdrExposure(), 2.2, 1);
    draw("hB", 1, hdrExposure(), 2.2, 1);
    draw("hC", 2, 1, 1, 16);
    const label = CODECS.find((c) => c.id === codec).name;
    setStats($("hdrStats"), r, label);
  } catch (e) {
    $("hdrStats").textContent = "⚠ " + e.message;
  }
});

/* --------------------------------------------------------------- normals --- */

/* BC5 and EAC_RG11 keep only X,Y — the shader rebuilds Z, so their angular
 * error must be measured the same way. */
const NORMAL_CODECS = [
  { id: 2,  name: "BC5",       recon: 1 },
  { id: 8,  name: "EAC RG11",  recon: 1 },
  { id: 3,  name: "BC7",       recon: 0 },
  { id: 9,  name: "ASTC 4×4",  recon: 0 },
  { id: 0,  name: "BC1",       recon: 0 },
];

$("btnNormal").addEventListener("click", () => {
  try {
    need();
    const rows = [];
    for (const nc of NORMAL_CODECS) {
      if (!M._tcw_resize(256, 256, 5, 0, 0, 0)) throw new Error(msg());
      if (!M._tcw_normal_from_height(+$("nStrength").value)) throw new Error(msg());
      const r = runCodec(nc.id, 2, 4, 4, false, nc.id === 3);
      rows.push({
        name: nc.name,
        deg: M._tcw_normal_angular_error(nc.recon),
        psnr: r.psnr,
        kib: r.bytes / 1024,
      });
      if (nc.id === 2) { /* show BC5, the one that wins */
        draw("nA", 0, 1, 1, 1);
        draw("nB", 1, 1, 1, 1);
        draw("nC", 2, 1, 1, 16);
      }
    }
    rows.sort((a, b) => a.deg - b.deg);
    $("nTable").innerHTML =
      "<tr><th>codec</th><th>mean angular error</th><th>RGB PSNR</th><th>size</th></tr>" +
      rows.map((r, i) =>
        `<tr class="${i === 0 ? "best" : ""}"><td>${r.name}</td>` +
        `<td><b>${r.deg.toFixed(2)}°</b></td>` +
        `<td class="dim">${r.psnr.toFixed(1)} dB</td>` +
        `<td class="dim">${r.kib.toFixed(0)} KiB</td></tr>`).join("");
  } catch (e) {
    $("nTable").innerHTML = "⚠ " + e.message;
  }
});

/* ----------------------------------------------------------- environment --- */

$("btnEnv").addEventListener("click", () => {
  try {
    need();
    const proj = +$("envProj").value;
    const size = +$("envSize").value;
    const codec = +$("envCodec").value;
    const exp = Math.pow(2, +$("envExp").value);
    if (!M._tcw_project_env(proj, size)) throw new Error(msg());
    const projMsg = msg();
    const r = runCodec(codec, 1, 4, 4, false, false);
    draw("eA", 0, exp, 2.2, 1);
    draw("eB", 1, exp, 2.2, 1);
    draw("eC", 2, 1, 1, 16);
    setStats($("envStats"), r, projMsg);
  } catch (e) {
    $("envStats").textContent = "⚠ " + e.message;
  }
});

/* ------------------------------------------------------------------ mips --- */

$("btnMips").addEventListener("click", () => {
  try {
    need();
    if (!M._tcw_resize(256, 256, 5, 0, $("mipSrgb").checked ? 1 : 0, 0))
      throw new Error(msg());
    const n = M._tcw_build_mips(+$("mipContent").value, 0,
                                $("mipSrgb").checked ? 1 : 0);
    if (!n) throw new Error(msg());
    const strip = $("mipStrip");
    strip.innerHTML = "";
    for (let l = 0; l < n; ++l) {
      if (!M._tcw_select_mip(l)) continue;
      const w = M._tcw_mip_width(l), h = M._tcw_mip_height(l);
      const fig = document.createElement("figure");
      const cap = document.createElement("figcaption");
      cap.textContent = `${w}×${h}`;
      const cv = document.createElement("canvas");
      cv.id = "mip" + l;
      fig.appendChild(cv);
      fig.appendChild(cap);
      strip.appendChild(fig);
      draw("mip" + l, 0, 1, 1, 1);
      cv.style.width = Math.max(16, w) + "px";
    }
    /* leave the working image at level 0 */
    M._tcw_select_mip(0);
  } catch (e) {
    $("mipStrip").textContent = "⚠ " + e.message;
  }
});

/* -------------------------------------------------------- sample sources --- */

$("btnSample").addEventListener("click", async () => {
  try {
    const resp = await fetch("sample.exr");
    if (!resp.ok) throw new Error("sample.exr not found");
    loadEXR(new Uint8Array(await resp.arrayBuffer()), "sample.exr");
  } catch (e) {
    $("srcInfo").textContent = "⚠ " + e.message;
  }
});

$("fileIn").addEventListener("change", async (ev) => {
  const f = ev.target.files[0];
  if (!f) return;
  try {
    if (/\.exr$/i.test(f.name))
      loadEXR(new Uint8Array(await f.arrayBuffer()), f.name);
    else
      await loadImageFile(f);
  } catch (e) {
    $("srcInfo").textContent = "⚠ " + e.message;
  }
});

/* openexr-images: list the repo tree, fetch the .exr the user picks. Same
 * source the TinyEXR viewer demo uses. */
const OEXR_REPO = "AcademySoftwareFoundation/openexr-images";
const OEXR_TREE = `https://api.github.com/repos/${OEXR_REPO}/git/trees/main?recursive=1`;
const OEXR_RAW = `https://raw.githubusercontent.com/${OEXR_REPO}/main/`;
let oexrFiles = null;

$("btnOpenExr").addEventListener("click", async () => {
  const list = $("oexrList");
  list.classList.toggle("hidden");
  if (oexrFiles || list.classList.contains("hidden")) return;
  list.textContent = "fetching index…";
  try {
    const r = await fetch(OEXR_TREE);
    if (!r.ok) throw new Error(r.status + " " + r.statusText);
    const t = await r.json();
    oexrFiles = t.tree
      .filter((e) => e.type === "blob" && /\.exr$/i.test(e.path))
      .sort((a, b) => a.path.localeCompare(b.path));
    list.innerHTML = "";
    /* The equirect ones are the interesting sources for the env panel, so
     * surface them first with a hint. */
    for (const f of oexrFiles) {
      const b = document.createElement("button");
      b.className = "oexr-item";
      b.textContent = f.path;
      if (/latlong|equirect|env/i.test(f.path)) b.classList.add("hint");
      b.addEventListener("click", async () => {
        list.classList.add("hidden");
        $("srcInfo").textContent = "fetching " + f.path + "…";
        try {
          const resp = await fetch(OEXR_RAW + encodeURI(f.path));
          if (!resp.ok) throw new Error(resp.status + " " + resp.statusText);
          loadEXR(new Uint8Array(await resp.arrayBuffer()), f.path.split("/").pop());
        } catch (e) {
          $("srcInfo").textContent = "⚠ " + e.message;
        }
      });
      list.appendChild(b);
    }
  } catch (e) {
    list.textContent = "⚠ could not fetch the openexr-images index: " + e.message;
  }
});

/* ------------------------------------------------------------------ boot --- */

$("meta").textContent =
  `wasm ready · texcomp backend: ${M.UTF8ToString(M._tcw_backend())} · ` +
  `all encode/decode runs locally`;
$("btnSample").click();
