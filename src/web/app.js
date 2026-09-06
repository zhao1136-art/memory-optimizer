// MemOptimizer 单页后台：原生 JS + Canvas 图表，无 CDN
"use strict";
const $ = s => document.querySelector(s);
const API = "/api/v1";
let selectedPid = null;

const mb = m => (m >= 1024 ? (m / 1024).toFixed(2) + " GB" : m.toFixed(0) + " MB");

async function get(path) {
  const r = await fetch(API + path);
  if (!r.ok) throw new Error(r.status);
  return r.json();
}

async function post(path, obj) {
  const r = await fetch(API + path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(obj) });
  const j = await r.json();
  if (!r.ok) throw new Error(j.error || r.status);
  return j;
}

/* Canvas 折线图 */
function drawLine(cv, series, color, label) {
  const dpr = window.devicePixelRatio || 1;
  const w = cv.clientWidth, h = parseInt(cv.getAttribute("height"), 10);
  cv.width = w * dpr; cv.height = h * dpr;
  cv.style.height = h + "px";
  const ctx = cv.getContext("2d");
  ctx.scale(dpr, dpr);
  ctx.clearRect(0, 0, w, h);
  if (!series || series.length < 2) return;
  const data = series.map(x => x.v);
  let mn = Math.min(...data), mx = Math.max(...data);
  if (mx - mn < 0.5) { mx += 0.5; mn -= 0.5; }
  const pad = 6;
  ctx.strokeStyle = color; ctx.lineWidth = 2;
  ctx.beginPath();
  const n = data.length;
  for (let i = 0; i < n; i++) {
    const x = pad + (i / (n - 1)) * (w - pad * 2);
    const y = h - pad - ((data[i] - mn) / (mx - mn)) * (h - pad * 2);
    i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
  }
  ctx.stroke();
  // 填充
  const g = ctx.createLinearGradient(0, 0, 0, h);
  g.addColorStop(0, color + "55"); g.addColorStop(1, color + "00");
  ctx.lineTo(w - pad, h - pad); ctx.lineTo(pad, h - pad); ctx.closePath();
  ctx.fillStyle = g; ctx.fill();
}

function renderSys(ov) {
  const s = ov.system;
  const pct = s.avail_pct || 0;
  $("#sysbar").innerHTML =
    `<span>可用 <b>${mb(s.avail_mb)}</b> / ${mb(s.total_mb)}</span>` +
    `<span>可用率 <b>${pct.toFixed(1)}%</b></span>` +
    `<span>提交 <b>${mb(s.commit_mb)}</b> / ${mb(s.commit_limit_mb)}</span>` +
    `<span>缓存 <b>${mb(s.cache_mb)}</b></span>` +
    `<span>句柄 <b>${s.handles}</b></span><span>线程 <b>${s.threads}</b></span>`;
  $("#sampinfo").textContent = `采样 ${ov.sampler.sample_count} 次 · 间隔 ${ov.sampler.interval_ms}ms`;
}

function stat(k, v, cls) {
  return `<div class="stat"><div class="k">${k}</div><div class="v ${cls}">${v}</div></div>`;
}

function svc(sev) { return `<span class="sev sev${sev}">${sev}</span>`; }

function renderLeaks(d) {
  const el = $("#leakList");
  if (!d.count) { el.innerHTML = `<div class="cell">未发现明显泄漏趋势 ✅</div>`; return; }
  el.innerHTML = d.leaks.map(l =>
    `<div class="cell"><b>${esc(l.name)} <span class="rt">${l.kind}</span></b>` +
    `<div class="sub">速率 ${l.rate_kb_min.toFixed(1)} KB/min · R²=${l.r2.toFixed(2)} · 历时 ${l.span_sec.toFixed(0)}s · CPU ${l.cpu_pct.toFixed(1)}% ${svc(l.severity)}</div></div>`
  ).join("");
}

function renderSuggestions(d) {
  const el = $("#sugList");
  if (!d.count) { el.innerHTML = `<div class="cell">当前无明显优化项 ✅</div>`; return; }
  el.innerHTML = d.suggestions.map(s =>
    `<div class="cell"><b>[${svc(s.severity)}] ${esc(s.title)}</b>` +
    `<div class="sub">${esc(s.reason)}</div>` +
    (s.actionable ? `<button data-act="${s.action}" data-pid="${s.pid}">执行：${esc(s.action)}</button>` : "")
  ).join("");
  el.querySelectorAll("button[data-act]").forEach(b =>
    b.onclick = async () => {
      const body = JSON.stringify({ pid: +b.dataset.pid, action: b.dataset.act });
      const r = await fetch(API + "/suggestions/apply", { method: "POST", headers: { "Content-Type": "application/json" }, body });
      const j = await r.json();
      alert(j.ok ? j.message : j.error);
    });
}

function renderRuntime(rt) {
  $("#rtList").innerHTML = rt.runtimes.filter(x => x.count > 0)
    .sort((a, b) => b.count - a.count)
    .map(x => `<div class="cell"><b>${esc(x.runtime)}</b><div class="sub">${x.count} 个进程</div></div>`).join("")
    || `<div class="cell">无</div>`;
}

function renderProcs() {
  const q = $("#search").value.trim();
  const sort = $("#sortSel").value;
  get(`/processes?search=${encodeURIComponent(q)}&top=300`).then(d => {
    const ps = (d.processes || []).slice().sort((a, b) => sort === "name" ? a.name.localeCompare(b.name) : b[sort + "_mb"] - a[sort + "_mb"]);
    const tb = $("#procsBody");
    const maxPriv = Math.max(1, ...ps.map(p => p.private_mb));
    tb.innerHTML = ps.slice(0, 120).map(p =>
      `<tr data-pid="${p.pid}" class="pt" style="cursor:pointer">` +
      `<td>${p.pid}</td><td>${esc(p.name)}</td><td><span class="rt">${esc(p.runtime)}</span></td>` +
      `<td><div class="hot" style="width:${(p.private_mb / maxPriv) * 90}px"></div> ${p.private_mb.toFixed(0)}</td>` +
      `<td>${p.ws_mb.toFixed(0)}</td><td>${p.peak_mb.toFixed(0)}</td>` +
      `<td>${p.cpu_pct ? p.cpu_pct.toFixed(1) : "0"}</td><td>${p.threads}</td><td>▶</td></tr>`
    ).join("");
    tb.querySelectorAll("tr.pt").forEach(tr => tr.onclick = () => viewDetail(+tr.dataset.pid));
  }).catch(() => {});
}

async function viewDetail(pid) {
  selectedPid = pid;
  try {
    const d = await get(`/process/${pid}`);
    const p = d.process;
    let h = `<h3>进程详情 #${pid} <button onclick="closeDetail()">关闭</button></h3>`;
    h += `<div class="sysstats">` +
      stat("名称", esc(p.name || ""), "") + stat("运行时", esc(p.runtime || ""), "") +
      stat("私有", p.private_mb.toFixed(1) + " MB", "") + stat("工作集", p.ws_mb.toFixed(1) + " MB", "") +
      stat("峰值", p.peak_mb ? p.peak_mb.toFixed(1) + " MB" : "-", "") + stat("CPU", (p.cpu_pct || 0).toFixed(1) + "%", "") +
      stat("线程/句柄", `${p.threads}/${p.handles}`, "") + stat("缺页", p.faults || 0, "") +
      `</div><canvas id="procChart" height="140"></canvas>`;
    $("#procDetail").innerHTML = h;
    const tr = await get(`/process/${pid}/trend`);
    const data = (tr.t || []).map((t, i) => ({ t, v: tr.priv_mb[i] }));
    drawLine($("#procChart"), data, "#4cc2ff");
  } catch (e) { $("#procDetail").innerHTML = `<div class="cell">无法读取进程（可能权限不足或已退出）</div>`; }
}
window.closeDetail = () => { selectedPid = null; $("#procDetail").innerHTML = ""; };

function esc(s) { const d = document.createElement("div"); d.textContent = s || ""; return d.innerHTML; }

function renderFrag() {
  const pid = +$("#fragPid").value;
  get(`/fragmentation/${pid}`).then(f => {
    $("#fragRes").innerHTML = f.ok
      ? `<div class="sysstats">` + stat("已提交区", f.region_count + " 个", "") +
        stat("已提交", mb(f.committed_mb), "") + stat("最大连续", mb(f.largest_region_mb), f.frag_idx > 0.4 ? "warn" : "good") +
        stat("碎片指数", f.frag_idx.toFixed(3), f.frag_idx > 0.5 ? "bad" : f.frag_idx > 0.3 ? "warn" : "good") +
        `</div><div class="sub">碎片指数接近 0 = 连续整块；越高碎片化越严重。</div>`
      : `<div class="cell warn">${esc(f.error)}</div>`;
  }).catch(() => $("#fragRes").innerHTML = `<div class="cell">分析失败</div>`);
}

async function refresh() {
  try {
    const ov = await get("/overview");
    renderSys(ov);
  } catch (e) {}
  renderProcs();
  get("/leaks").then(renderLeaks).catch(() => {});
  get("/suggestions").then(renderSuggestions).catch(() => {});
  get("/runtime").then(renderRuntime).catch(() => {});
  renderProtect();
}

function renderProtectList(pl) {
  const el = $("#protList");
  if (!pl.length) { el.innerHTML = `<div class="cell">暂无自定义保护进程（内置系统进程已自动保护）✅</div>`; return; }
  el.innerHTML = pl.map(n =>
    `<div class="cell"><b>${esc(n)}</b> ` +
    `<button data-rm="${esc(n)}">移除</button></div>`
  ).join("");
  el.querySelectorAll("button[data-rm]").forEach(b =>
    b.onclick = async () => {
      try { await post("/protect", { op: "remove", name: b.dataset.rm }); refresh(); }
      catch (e) { alert("移除失败：" + e.message); }
    });
}

async function renderProtect() {
  try {
    const j = await post("/protect", { op: "list" });
    renderProtectList(j.names || []);
  } catch (e) { $("#protList").innerHTML = `<div class="cell">加载失败</div>`; }
}

async function addProtect() {
  const name = $("#protName").value.trim();
  if (!name) { alert("请输入进程名，如 python.exe"); return; }
  try {
    await post("/protect", { op: "add", name });
    $("#protName").value = "";
    renderProtect();
  } catch (e) { alert("加入失败：" + e.message); }
}

async function doOneClick() {
  if (!confirm("一键优化将执行：\n1) 关闭僵尸进程（长时间低 CPU 且高驻留内存）\n2) 回收高驻留进程的工作集\n3) 清理系统 standby 缓存\n\n受保护进程将一律跳过（不关闭、不回收）。\n确定执行？")) return;
  const btn = $("#btnOneClick");
  btn.disabled = true;
  try {
    const r = await fetch(API + "/optimize", { method: "POST", headers: { "Content-Type": "application/json" }, body: "{}" });
    const j = await r.json();
    let msg = "一键优化完成\n" +
      "回收工作集：" + j.reclaimed + " 个（约 " + (j.reclaimed_mb || 0).toFixed(0) + "MB）\n" +
      "关闭僵尸进程：" + j.killed + " 个\n";
    if (j.killed > 0) {
      msg += "已关闭：" + (j.killed_list || []).map(k => k.name + "(" + k.pid + ")").join("、") + "\n";
    }
    msg += "清理缓存：" + (j.cache_purged ? "成功" : (j.cache_error || "未执行"));
    alert(msg);
    refresh();
  } catch (e) {
    alert("一键优化失败：" + e.message);
  } finally {
    btn.disabled = false;
  }
}
function doExport() {
  const fmt = $("#reportFmt").value;
  const btn = $("#btnReport");
  const old = btn.textContent;
  btn.disabled = true;
  btn.textContent = "导出中...";
  if (fmt === "html") {
    window.open("/api/v1/report?format=html");
    btn.disabled = false;
    btn.textContent = old;
    return;
  }
  fetch("/api/v1/report?format=" + encodeURIComponent(fmt))
    .then(resp => {
      if (!resp.ok) return resp.json().then(e => { throw new Error((e && e.error) || "导出失败(HTTP " + resp.status + ")"); });
      return resp.blob();
    })
    .then(blob => {
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = "memopt-report." + (fmt === "xls" ? "xls" : fmt);
      document.body.appendChild(a);
      a.click();
      // 不能立刻 revoke：大文件(如 PDF 数MB) 若 blob 地址过早被撤销，
      // 浏览器下载会中断导致文件不完整、无法打开。延迟到下载开始后再回收。
      setTimeout(() => { a.remove(); URL.revokeObjectURL(url); }, 5000);
    })
    .catch(e => alert("导出失败：" + e.message))
    .finally(() => { btn.disabled = false; btn.textContent = old; });
}
function boot() {
  $("#btnOneClick").onclick = doOneClick;
  $("#btnReport").onclick = doExport;
  $("#search").addEventListener("input", renderProcs);
  $("#sortSel").addEventListener("change", renderProcs);
  $("#btnFrag").onclick = renderFrag;
  $("#btnProtAdd").onclick = addProtect;
  $("#protName").addEventListener("keydown", e => { if (e.key === "Enter") addProtect(); });
  refresh();
  setInterval(refresh, 2000);
}
document.addEventListener("DOMContentLoaded", boot);