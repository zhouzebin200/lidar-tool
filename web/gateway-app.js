"use strict";

const $ = (id) => document.getElementById(id);
const state = {
  formsLoaded: false,
  connected: false,
  packetHistory: Array(60).fill(0),
  previousPackets: null,
  previousAt: null,
  latest: null,
  toastTimer: null,
};

function escapeHtml(value) {
  return String(value ?? "")
    .replaceAll("&", "&amp;").replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;").replaceAll('"', "&quot;").replaceAll("'", "&#039;");
}

function number(value) {
  return Number.isFinite(Number(value)) ? Number(value) : 0;
}

function formatNumber(value) {
  return new Intl.NumberFormat("zh-CN", { notation: number(value) >= 1000000 ? "compact" : "standard", maximumFractionDigits: 1 }).format(number(value));
}

function formatBytes(value) {
  let amount = Math.max(0, number(value));
  const units = ["B", "KB", "MB", "GB", "TB"];
  let index = 0;
  while (amount >= 1024 && index < units.length - 1) { amount /= 1024; index += 1; }
  const digits = index === 0 ? 0 : amount >= 100 ? 0 : amount >= 10 ? 1 : 2;
  return `${amount.toFixed(digits)} ${units[index]}`;
}

function formatDuration(value) {
  let seconds = Math.max(0, Math.floor(number(value)));
  const days = Math.floor(seconds / 86400); seconds %= 86400;
  const hours = Math.floor(seconds / 3600); seconds %= 3600;
  const minutes = Math.floor(seconds / 60);
  if (days) return `${days} 天 ${hours} 小时`;
  if (hours) return `${hours} 小时 ${minutes} 分`;
  if (minutes) return `${minutes} 分 ${seconds % 60} 秒`;
  return `${seconds} 秒`;
}

function parseIntegerList(value) {
  return String(value).split(/[，,\s]+/).map((item) => item.trim()).filter(Boolean).map((item) => {
    const parsed = Number.parseInt(item, 10);
    if (!Number.isInteger(parsed)) throw new Error(`“${item}”不是有效端口`);
    return parsed;
  });
}

function parseStringList(value) {
  return String(value).split(/[，,\s]+/).map((item) => item.trim()).filter(Boolean);
}

function setText(id, value) {
  const element = $(id);
  if (element) element.textContent = value;
}

function setBadge(id, text, mode = "") {
  const element = $(id);
  if (!element) return;
  element.textContent = text;
  element.className = `state-badge${mode ? ` ${mode}` : ""}`;
}

function toast(message, error = false) {
  const element = $("toast");
  element.textContent = message;
  element.className = `toast show${error ? " error" : ""}`;
  clearTimeout(state.toastTimer);
  state.toastTimer = setTimeout(() => { element.className = "toast"; }, 3300);
}

async function readJson(response) {
  let value = {};
  try { value = await response.json(); } catch (_) { /* response without JSON */ }
  if (!response.ok) throw new Error(value.error || `请求失败 (${response.status})`);
  return value;
}

async function postJson(path, body = {}) {
  return readJson(await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  }));
}

function capturePacketTotal(status) {
  if (status.capture) return number(status.capture.accepted_packets || status.capture.packets);
  return (status.listeners || []).reduce((sum, item) => sum + number(item.accepted_packets || item.packets), 0);
}

function updateTraffic(total) {
  const now = performance.now();
  let rate = 0;
  if (state.previousPackets !== null && total >= state.previousPackets) {
    rate = (total - state.previousPackets) / Math.max(.25, (now - state.previousAt) / 1000);
  }
  state.previousPackets = total;
  state.previousAt = now;
  state.packetHistory.push(rate);
  state.packetHistory.splice(0, state.packetHistory.length - 60);
  setText("packetRate", `${formatNumber(rate)} 包/秒`);
  setText("trafficRate", `${formatNumber(rate)} pkt/s`);

  const values = state.packetHistory;
  const maximum = Math.max(1, ...values) * 1.12;
  const width = 720, top = 14, bottom = 190;
  const points = values.map((value, index) => {
    const x = index * width / Math.max(1, values.length - 1);
    const y = bottom - (Math.min(maximum, value) / maximum) * (bottom - top);
    return [x, y];
  });
  const line = points.map(([x, y], index) => `${index ? "L" : "M"}${x.toFixed(1)} ${y.toFixed(1)}`).join(" ");
  $("trafficLine").setAttribute("d", line || `M0 ${bottom} L${width} ${bottom}`);
  $("trafficArea").setAttribute("d", `${line} L${width} ${bottom} L0 ${bottom} Z`);
}

function updateConnection(connected) {
  state.connected = connected;
  $("sideStatusDot").classList.toggle("online", connected);
  document.querySelector(".live-pill").classList.toggle("connected", connected);
  setText("liveText", connected ? "网关在线" : "连接中断");
}

function renderSystem(system = {}) {
  const hostname = system.hostname || "OpenWrt";
  setText("sideHostname", hostname);
  setText("heroHostname", hostname);
  setText("systemArch", system.machine || "unknown");
  setText("kernelValue", system.kernel || "--");
  const loads = system.load_average || [];
  const load = number(loads[0]);
  setText("loadValue", loads.length ? loads.join(" / ") : "--");
  $("loadBar").style.width = `${Math.min(100, load * 25)}%`;

  const memoryTotal = number(system.memory_total_bytes);
  const memoryUsed = Math.max(0, memoryTotal - number(system.memory_available_bytes));
  const memoryPercent = memoryTotal ? memoryUsed / memoryTotal * 100 : 0;
  setText("memoryValue", memoryTotal ? `${Math.round(memoryPercent)}% · ${formatBytes(memoryUsed)}` : "--");
  $("memoryBar").style.width = `${memoryPercent}%`;

  const disk = system.root_storage || {};
  const diskTotal = number(disk.total_bytes);
  const diskUsed = Math.max(0, diskTotal - number(disk.free_bytes));
  const diskPercent = diskTotal ? diskUsed / diskTotal * 100 : 0;
  setText("storageValue", diskTotal ? `${Math.round(diskPercent)}% · ${formatBytes(diskUsed)}` : "--");
  $("storageBar").style.width = `${diskPercent}%`;
}

function renderDevices(devices) {
  setText("deviceCount", devices.length);
  const models = [...new Set(devices.map((item) => item.model || item.vendor).filter(Boolean))];
  setText("deviceModels", models.length ? models.join(" · ") : "等待设备数据");
  if (!devices.length) {
    $("deviceList").innerHTML = '<div class="empty-state"><span>⌁</span><b>等待雷达上线</b><small>发现数据后会自动显示在这里</small></div>';
    return;
  }
  $("deviceList").innerHTML = devices.map((item) => `
    <div class="device-item">
      <span class="device-symbol">⌁</span>
      <span class="device-main"><b>${escapeHtml(item.model || "未知型号")}</b><span>${escapeHtml(item.vendor || "LiDAR")} · ${escapeHtml(item.ip || "--")}${item.source_mac ? ` · ${escapeHtml(item.source_mac)}` : ""}</span></span>
      <span class="device-rate">${formatNumber(item.packets)} 包<br>${escapeHtml(item.destination_port || "--")}/UDP</span>
    </div>`).join("");
}

function renderFlows(capture) {
  const flows = capture?.flows || [];
  setText("flowCount", `${flows.length} 条流`);
  if (!flows.length) {
    $("flowTable").innerHTML = '<tr><td colspan="6" class="table-empty">暂无活跃流量</td></tr>';
    return;
  }
  $("flowTable").innerHTML = flows.slice(0, 40).map((flow) => {
    const vlans = (flow.vlan_ids || []).length ? flow.vlan_ids.join(" / ") : "无";
    return `<tr>
      <td>${escapeHtml(flow.source_ip)}<br><small>${escapeHtml(flow.interface || "")}</small></td>
      <td>${escapeHtml(flow.destination_ip)}</td>
      <td>${escapeHtml(flow.source_port)} → ${escapeHtml(flow.destination_port)}</td>
      <td><span class="vlan-chip">${escapeHtml(vlans)}</span></td>
      <td>${formatNumber(flow.packets)}</td>
      <td>${number(flow.last_seen_seconds).toFixed(1)}s</td>
    </tr>`;
  }).join("");
}

function renderCapture(status) {
  const capture = status.capture;
  const interfaces = capture?.interfaces || [];
  const active = capture ? Boolean(capture.active) : (status.listeners || []).some((item) => item.state === "listening");
  const backend = capture?.backend || (status.listeners?.length ? "UDP socket" : "未启动");
  setText("captureBackend", backend);
  setText("interfaceCount", `${interfaces.length || status.listeners?.length || 0} 个网络接口`);
  setText("uptimeText", `运行 ${formatDuration(status.uptime_seconds)}`);
  setBadge("captureStateBadge", active ? "正在监听" : "异常", active ? "" : "error");
  setText("captureHint", capture?.error ? capture.error : "配置变更会短暂重启抓包服务");
}

function renderForwarding(gateway) {
  const config = gateway.config || {};
  const stats = gateway.forwarding || {};
  const active = Boolean(stats.active);
  setText("forwardState", active ? "转发中" : "已停止");
  setText("forwardSummary", config.target_ip ? `${config.target_ip}:${config.target_port || "原端口"}` : "尚未配置目标");
  setBadge("forwardBadge", active ? "转发中" : "已停止", active ? "" : "muted");
  setText("forwardPackets", formatNumber(stats.packets));
  setText("forwardBytes", formatBytes(stats.bytes));
  setText("forwardDropped", formatNumber(stats.dropped_packets));
  $("forwardError").classList.toggle("hidden", !stats.last_error);
  $("forwardError").textContent = stats.last_error || "";
}

function renderRecording(recording = {}) {
  const active = Boolean(recording.active);
  setText("recordState", active ? "录包中" : recording.state === "error" ? "异常" : "待机");
  setText("recordSummary", active ? formatBytes(recording.bytes) : recording.error || "等待外部存储");
  setBadge("recordBadge", active ? "正在录制" : recording.state === "error" ? "录包异常" : "待机", active ? "" : recording.state === "error" ? "error" : "muted");
  setText("recordBytes", formatBytes(recording.bytes));
  setText("recordPackets", formatNumber(recording.packets));
  setText("recordQueued", formatNumber(recording.queued_packets));
  setText("recordFile", recording.current_file || recording.error || "尚未创建文件");
  setText("recordLiveText", active ? "正在写入 PCAP" : recording.error ? "录包已停止" : "等待开始");
  $("recordPulse").parentElement.classList.toggle("active", active);
  $("stopRecord").disabled = !active;
  $("startRecord").disabled = active;
}

function updateInterfaceOptions(ptp) {
  const select = $("ptpInterface");
  const current = select.value || ptp.config?.interface || "";
  const interfaces = ptp.interfaces || [];
  select.innerHTML = '<option value="">请选择网口</option>' + interfaces.map((item) =>
    `<option value="${escapeHtml(item.name)}">${escapeHtml(item.name)} · ${escapeHtml(item.state)} · ${item.hardware_timestamping ? "硬件时间戳" : "软件时间戳"}</option>`
  ).join("");
  if ([...select.options].some((item) => item.value === current)) select.value = current;
  updatePhcHint(ptp);
}

function updatePhcHint(ptp = state.latest?.gateway?.ptp || {}) {
  const chosen = (ptp.interfaces || []).find((item) => item.name === $("ptpInterface").value);
  setText("phcHint", !chosen ? "选择网口后显示时间戳能力" : chosen.hardware_timestamping ? `检测到 PHC：${chosen.phc}` : "此网口未检测到 PHC，将使用软件时间戳");
}

function renderPtp(ptp = {}) {
  const active = Boolean(ptp.active);
  const config = ptp.config || {};
  setBadge("ptpBadge", active ? "同步运行中" : ptp.state === "error" ? "运行异常" : "已停止", active ? "" : ptp.state === "error" ? "error" : "muted");
  setText("ptpAvailability", ptp.available ? "linuxptp 已安装，可用" : "未检测到 linuxptp / 当前平台不可用");
  setText("ptpMode", active ? `${config.role === "master" ? "主时钟" : "从时钟"} · ${ptp.effective_timestamping === "hardware" ? "硬件" : "软件"}` : "未运行");
  setText("ptpStatusText", active ? "时间同步服务运行中" : ptp.state === "error" ? "PTP 服务异常" : "等待启动");
  setText("ptpStatusDetail", ptp.error || (active ? `${config.interface || "--"} · Domain ${config.domain ?? 0} · ${config.transport || "L2"}` : "选择网口并启用服务"));
  setText("ptpUptime", active ? formatDuration(ptp.uptime_seconds) : "--");
  setText("ptpPid", ptp.ptp4l_pid || "--");
  setText("ptpLogs", (ptp.logs || []).join("\n") || "暂无 PTP 日志");
  updateInterfaceOptions(ptp);
}

function renderStorage(storage = []) {
  const select = $("recordDirectory");
  const current = select.value;
  select.innerHTML = storage.length ? storage.map((item) =>
    `<option value="${escapeHtml(item.mountpoint)}">${escapeHtml(item.mountpoint)} · 可用 ${formatBytes(item.free_bytes)}</option>`
  ).join("") : '<option value="">未检测到外部存储</option>';
  if (storage.some((item) => item.mountpoint === current)) select.value = current;
  setText("storageTitle", storage.length ? `已发现 ${storage.length} 个外部存储` : "未检测到外部存储");
  setText("storageDescription", storage.length ? storage.map((item) => `${item.device} · ${formatBytes(item.free_bytes)} 可用`).join("；") : "请插入并挂载 USB 存储设备后刷新");
}

function loadForms(status, gateway) {
  if (state.formsLoaded) return;
  const config = status.config || {};
  $("autoDiscovery").checked = Boolean(config.auto_discovery);
  $("arpAssist").checked = Boolean(config.auto_arp_assist);
  $("listenIp").value = config.listen_ip || "0.0.0.0";
  $("udpPorts").value = (config.udp_ports || []).join(", ");
  $("sourceNetworks").value = (config.source_networks || []).join(", ");
  updateManualCapture();

  const forward = gateway.config || {};
  $("forwardEnabled").checked = Boolean(forward.enabled);
  $("targetIp").value = forward.target_ip || "";
  $("targetPort").value = forward.target_port ?? 0;
  $("sourcePorts").value = (forward.source_ports || []).join(", ");

  const ptp = gateway.ptp || {};
  const ptpConfig = ptp.config || {};
  updateInterfaceOptions(ptp);
  $("ptpEnabled").checked = Boolean(ptpConfig.enabled);
  $("ptpInterface").value = ptpConfig.interface || "";
  $("ptpRole").value = ptpConfig.role || "master";
  $("ptpTransport").value = ptpConfig.transport || "L2";
  $("ptpTimestamp").value = ptpConfig.timestamping || "auto";
  $("ptpDelay").value = ptpConfig.delay_mechanism || "E2E";
  $("ptpDomain").value = ptpConfig.domain ?? 0;
  $("ptpPriority").value = ptpConfig.priority1 ?? 128;
  updatePhcHint(ptp);
  state.formsLoaded = true;
}

function updateManualCapture() {
  const automatic = $("autoDiscovery").checked;
  $("manualCapture").querySelectorAll("input").forEach((input) => { input.disabled = automatic; });
  $("arpAssist").disabled = !automatic;
}

function renderAll(status, devicesResult, gateway) {
  state.latest = { status, devices: devicesResult.devices || [], gateway };
  updateConnection(true);
  setText("sideVersion", `v${status.version || "--"} · 服务在线`);
  setText("footerVersion", `v${status.version || ""}`);
  updateTraffic(capturePacketTotal(status));
  renderSystem(gateway.system);
  renderDevices(devicesResult.devices || []);
  renderFlows(status.capture);
  renderCapture(status);
  renderForwarding(gateway);
  renderRecording(gateway.recording);
  renderStorage(gateway.storage);
  renderPtp(gateway.ptp);
  loadForms(status, gateway);
}

async function refresh() {
  try {
    const [status, devices, gateway] = await Promise.all([
      fetch("/api/status", { cache: "no-store" }).then(readJson),
      fetch("/api/devices", { cache: "no-store" }).then(readJson),
      fetch("/api/gateway", { cache: "no-store" }).then(readJson),
    ]);
    renderAll(status, devices, gateway);
  } catch (error) {
    updateConnection(false);
    setText("captureBackend", "无法连接服务");
  }
}

async function submitCapture(event) {
  event.preventDefault();
  try {
    await postJson("/api/config", {
      auto_discovery: $("autoDiscovery").checked,
      auto_arp_assist: $("arpAssist").checked,
      listen_ip: $("listenIp").value.trim(),
      udp_ports: parseIntegerList($("udpPorts").value),
      source_networks: parseStringList($("sourceNetworks").value),
    });
    toast("网络接入配置已保存"); await refresh();
  } catch (error) { toast(error.message, true); }
}

async function submitForward(event) {
  event.preventDefault();
  try {
    await postJson("/api/gateway/config", {
      enabled: $("forwardEnabled").checked,
      target_ip: $("targetIp").value.trim(),
      target_port: Number.parseInt($("targetPort").value || "0", 10),
      source_ports: parseIntegerList($("sourcePorts").value),
    });
    toast("UDP 转发配置已应用"); await refresh();
  } catch (error) { toast(error.message, true); }
}

async function submitRecord(event) {
  event.preventDefault();
  try {
    await postJson("/api/gateway/recording/start", {
      directory: $("recordDirectory").value,
      prefix: $("recordPrefix").value.trim(),
      split_mb: Number.parseInt($("splitMb").value, 10),
      min_free_mb: 256,
    });
    toast("USB 录包已开始"); await refresh();
  } catch (error) { toast(error.message, true); }
}

async function stopRecord() {
  try { await postJson("/api/gateway/recording/stop"); toast("录包已安全停止"); await refresh(); }
  catch (error) { toast(error.message, true); }
}

async function submitPtp(event) {
  event.preventDefault();
  try {
    await postJson("/api/ptp/config", {
      enabled: $("ptpEnabled").checked,
      interface: $("ptpInterface").value,
      role: $("ptpRole").value,
      transport: $("ptpTransport").value,
      timestamping: $("ptpTimestamp").value,
      delay_mechanism: $("ptpDelay").value,
      domain: Number.parseInt($("ptpDomain").value, 10),
      priority1: Number.parseInt($("ptpPriority").value, 10),
    });
    toast("PTP 配置已应用"); await refresh();
  } catch (error) { toast(error.message, true); }
}

function initNavigation() {
  const links = [...document.querySelectorAll(".side-nav a")];
  links.forEach((link) => link.addEventListener("click", () => {
    links.forEach((item) => item.classList.toggle("active", item === link));
  }));
  const observer = new IntersectionObserver((entries) => {
    const visible = entries.filter((entry) => entry.isIntersecting).sort((a, b) => b.intersectionRatio - a.intersectionRatio)[0];
    if (!visible) return;
    links.forEach((link) => link.classList.toggle("active", link.hash === `#${visible.target.id}`));
  }, { rootMargin: "-18% 0px -70% 0px", threshold: [0, .1] });
  document.querySelectorAll("main > section[id]").forEach((section) => observer.observe(section));
}

function updateClock() {
  setText("clockText", new Intl.DateTimeFormat("zh-CN", { hour: "2-digit", minute: "2-digit", second: "2-digit", hour12: false }).format(new Date()));
}

$("autoDiscovery").addEventListener("change", updateManualCapture);
$("ptpInterface").addEventListener("change", () => updatePhcHint());
$("captureForm").addEventListener("submit", submitCapture);
$("forwardForm").addEventListener("submit", submitForward);
$("recordForm").addEventListener("submit", submitRecord);
$("stopRecord").addEventListener("click", stopRecord);
$("ptpForm").addEventListener("submit", submitPtp);
$("refreshStorage").addEventListener("click", async () => {
  try { const value = await fetch("/api/gateway", { cache: "no-store" }).then(readJson); renderStorage(value.storage); toast("存储设备列表已刷新"); }
  catch (error) { toast(error.message, true); }
});
$("toggleLogs").addEventListener("click", () => {
  const logs = $("ptpLogs"); logs.classList.toggle("collapsed");
  $("toggleLogs").textContent = logs.classList.contains("collapsed") ? "展开" : "收起";
});

initNavigation();
updateClock();
setInterval(updateClock, 1000);
refresh();
setInterval(refresh, 1500);
setInterval(() => {
  if (state.latest?.gateway?.storage) renderStorage(state.latest.gateway.storage);
}, 5000);
