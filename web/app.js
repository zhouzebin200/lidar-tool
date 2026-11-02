"use strict";

const $ = (id) => document.getElementById(id);
const canvas = $("pointCloud");
const gl = canvas.getContext("webgl2", { antialias: true, alpha: false });

const state = {
  socket: null,
  points: 0,
  lastFrameAt: 0,
  frameCounter: 0,
  fpsCounter: 0,
  fpsAt: performance.now(),
  yaw: -0.74,
  pitch: 0.56,
  distance: 42,
  colorMode: 0,
  pointSize: 2,
  maxDistance: 200,
  zMin: -3,
  zMax: 3,
  intensityMax: 1,
  dragging: false,
  lastX: 0,
  lastY: 0,
  activeDevice: null,
  selectedDevice: null,
  configLoaded: false,
  pcapUploading: false,
  gatewayLoaded: false,
  storageLoadedAt: 0,
  networkReloading: false,
};

function formatBytes(value) {
  const bytes = Number(value) || 0;
  if (bytes < 1024) return `${bytes} B`;
  const units = ["KB", "MB", "GB", "TB"];
  let amount = bytes / 1024, index = 0;
  while (amount >= 1024 && index < units.length - 1) { amount /= 1024; index++; }
  return `${amount.toFixed(amount >= 100 ? 0 : amount >= 10 ? 1 : 2)} ${units[index]}`;
}

function compileShader(type, source) {
  const shader = gl.createShader(type);
  gl.shaderSource(shader, source);
  gl.compileShader(shader);
  if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(shader));
  return shader;
}

function createProgram(vertex, fragment) {
  const program = gl.createProgram();
  gl.attachShader(program, compileShader(gl.VERTEX_SHADER, vertex));
  gl.attachShader(program, compileShader(gl.FRAGMENT_SHADER, fragment));
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
  return program;
}

let pointProgram = null;
let lineProgram = null;
let rendererError = "";
try {
if (gl) {
pointProgram = createProgram(`#version 300 es
  precision highp float;
  layout(location=0) in vec3 aPosition;
  layout(location=1) in float aIntensity;
  layout(location=2) in float aRing;
  uniform mat4 uViewProjection;
  uniform float uPointSize;
  uniform float uMaxDistance;
  uniform float uZMin;
  uniform float uZMax;
  uniform float uIntensityMax;
  uniform highp int uColorMode;
  out float vValue;
  out float vVisible;
  void main() {
    gl_Position = uViewProjection * vec4(aPosition, 1.0);
    gl_PointSize = uPointSize;
    float range = length(aPosition);
    vVisible = range <= uMaxDistance ? 1.0 : 0.0;
    if (uColorMode == 0) vValue = clamp((aPosition.z - uZMin) / max(.01, uZMax - uZMin), 0.0, 1.0);
    else if (uColorMode == 1) vValue = clamp(aIntensity / max(.01, uIntensityMax), 0.0, 1.0);
    else if (uColorMode == 2) vValue = clamp(range / uMaxDistance, 0.0, 1.0);
    else vValue = fract(aRing * .61803398875);
  }
`, `#version 300 es
  precision highp float;
  uniform highp int uColorMode;
  in float vValue;
  in float vVisible;
  out vec4 outColor;
  vec3 turbo(float x) {
    x = clamp(x, 0.0, 1.0);
    if (x < .25) return mix(vec3(.08, .08, .45), vec3(0.0, .75, 1.0), x * 4.0);
    if (x < .50) return mix(vec3(0.0, .75, 1.0), vec3(.1, .95, .35), (x - .25) * 4.0);
    if (x < .75) return mix(vec3(.1, .95, .35), vec3(1.0, .85, .05), (x - .50) * 4.0);
    return mix(vec3(1.0, .85, .05), vec3(.85, .05, .03), (x - .75) * 4.0);
  }
  vec3 categorical(float h) {
    vec3 rgb = clamp(abs(mod(h * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return mix(vec3(1.0), rgb, .78);
  }
  void main() {
    if (vVisible < .5) discard;
    vec2 p = gl_PointCoord * 2.0 - 1.0;
    if (dot(p, p) > 1.0) discard;
    vec3 color;
    if (uColorMode == 1) color = vec3(pow(vValue, .55));
    else if (uColorMode == 3) color = categorical(vValue);
    else color = turbo(vValue);
    outColor = vec4(color, 0.96);
  }
`);

lineProgram = createProgram(`#version 300 es
  precision highp float;
  layout(location=0) in vec3 aPosition;
  layout(location=1) in vec3 aColor;
  uniform mat4 uViewProjection;
  out vec3 vColor;
  void main() { gl_Position = uViewProjection * vec4(aPosition, 1.0); vColor = aColor; }
`, `#version 300 es
  precision highp float;
  in vec3 vColor;
  out vec4 outColor;
  void main() { outColor = vec4(vColor, 0.58); }
`);
}
} catch (error) {
  rendererError = error instanceof Error ? error.message : String(error);
  console.error("ULV WebGL renderer initialization failed:", error);
}

const rendererReady = Boolean(gl && pointProgram && lineProgram);
const pointBuffer = rendererReady ? gl.createBuffer() : null;
const gridPositionBuffer = rendererReady ? gl.createBuffer() : null;
const gridColorBuffer = rendererReady ? gl.createBuffer() : null;
let gridVertexCount = 0;

function buildGrid() {
  const positions = [];
  const colors = [];
  const add = (a, b, color) => { positions.push(...a, ...b); colors.push(...color, ...color); };
  for (let i = -40; i <= 40; i += 5) {
    const major = i === 0;
    const color = major ? [0.12, 0.28, 0.27] : [0.07, 0.11, 0.13];
    add([i, -40, 0], [i, 40, 0], color);
    add([-40, i, 0], [40, i, 0], color);
  }
  add([0, 0, 0], [6, 0, 0], [0.7, 0.12, 0.18]);
  add([0, 0, 0], [0, 6, 0], [0.10, 0.42, 0.70]);
  add([0, 0, 0], [0, 0, 6], [0.16, 0.72, 0.56]);
  gridVertexCount = positions.length / 3;
  gl.bindBuffer(gl.ARRAY_BUFFER, gridPositionBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(positions), gl.STATIC_DRAW);
  gl.bindBuffer(gl.ARRAY_BUFFER, gridColorBuffer);
  gl.bufferData(gl.ARRAY_BUFFER, new Float32Array(colors), gl.STATIC_DRAW);
}

function perspective(out, fovy, aspect, near, far) {
  const f = 1 / Math.tan(fovy / 2);
  out.fill(0); out[0] = f / aspect; out[5] = f; out[10] = (far + near) / (near - far);
  out[11] = -1; out[14] = (2 * far * near) / (near - far); return out;
}

function lookAt(out, eye, center, up) {
  let zx = eye[0] - center[0], zy = eye[1] - center[1], zz = eye[2] - center[2];
  let len = Math.hypot(zx, zy, zz) || 1; zx /= len; zy /= len; zz /= len;
  let xx = up[1] * zz - up[2] * zy, xy = up[2] * zx - up[0] * zz, xz = up[0] * zy - up[1] * zx;
  len = Math.hypot(xx, xy, xz) || 1; xx /= len; xy /= len; xz /= len;
  const yx = zy * xz - zz * xy, yy = zz * xx - zx * xz, yz = zx * xy - zy * xx;
  out.set([xx,yx,zx,0, xy,yy,zy,0, xz,yz,zz,0,
    -(xx*eye[0]+xy*eye[1]+xz*eye[2]),
    -(yx*eye[0]+yy*eye[1]+yz*eye[2]),
    -(zx*eye[0]+zy*eye[1]+zz*eye[2]),1]);
  return out;
}

function multiply(out, a, b) {
  const result = new Float32Array(16);
  for (let column = 0; column < 4; column++) for (let row = 0; row < 4; row++) {
    result[column*4+row] = a[row]*b[column*4] + a[4+row]*b[column*4+1] + a[8+row]*b[column*4+2] + a[12+row]*b[column*4+3];
  }
  out.set(result); return out;
}

function viewProjection() {
  const eye = [
    Math.cos(state.pitch) * Math.cos(state.yaw) * state.distance,
    Math.cos(state.pitch) * Math.sin(state.yaw) * state.distance,
    Math.sin(state.pitch) * state.distance,
  ];
  const projection = perspective(new Float32Array(16), Math.PI / 4, canvas.width / canvas.height, .1, 600);
  const view = lookAt(new Float32Array(16), eye, [0, 0, 0], [0, 0, 1]);
  return multiply(new Float32Array(16), projection, view);
}

function uploadFrame(buffer) {
  const view = new DataView(buffer);
  if (view.byteLength < 28 || view.getUint32(0, true) !== 0x31564c55) return;
  const deviceId = view.getUint32(8, true);
  if (state.selectedDevice !== null && deviceId !== state.selectedDevice) return;
  if (state.selectedDevice === null) state.selectedDevice = deviceId;
  const count = view.getUint32(16, true);
  if (view.byteLength !== 28 + count * 20) return;
  let zMin = Infinity, zMax = -Infinity, intensityMax = 0;
  const sampleStride = Math.max(1, Math.floor(count / 8192));
  for (let index = 0; index < count; index += sampleStride) {
    const offset = 28 + index * 20;
    const z = view.getFloat32(offset + 8, true);
    const intensity = view.getFloat32(offset + 12, true);
    if (Number.isFinite(z)) { zMin = Math.min(zMin, z); zMax = Math.max(zMax, z); }
    if (Number.isFinite(intensity)) intensityMax = Math.max(intensityMax, intensity);
  }
  if (Number.isFinite(zMin) && zMax > zMin) {
    const margin = Math.max(.05, (zMax - zMin) * .03);
    state.zMin = state.zMin * .8 + (zMin - margin) * .2;
    state.zMax = state.zMax * .8 + (zMax + margin) * .2;
  }
  state.intensityMax = state.intensityMax * .8 + Math.max(.05, intensityMax) * .2;
  if (rendererReady) {
    gl.bindBuffer(gl.ARRAY_BUFFER, pointBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Uint8Array(buffer, 28), gl.DYNAMIC_DRAW);
  }
  state.points = count;
  state.activeDevice = deviceId;
  state.lastFrameAt = performance.now();
  state.frameCounter++;
  const indicator = $("socketState");
  indicator.className = "connection online";
  indicator.innerHTML = "<i></i> LIVE";
  $("pointCount").textContent = count.toLocaleString();
  $("emptyState").classList.toggle("hidden", count > 0);
}

function render(schedule = true) {
  if (!gl) return;
  const width = Math.max(1, Math.floor(canvas.clientWidth * devicePixelRatio));
  const height = Math.max(1, Math.floor(canvas.clientHeight * devicePixelRatio));
  if (canvas.width !== width || canvas.height !== height) { canvas.width = width; canvas.height = height; }
  gl.viewport(0, 0, width, height);
  gl.clearColor(0.025, 0.036, 0.045, 1);
  gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
  gl.enable(gl.DEPTH_TEST);
  gl.enable(gl.BLEND);
  gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
  const vp = viewProjection();

  gl.useProgram(lineProgram);
  gl.uniformMatrix4fv(gl.getUniformLocation(lineProgram, "uViewProjection"), false, vp);
  gl.bindBuffer(gl.ARRAY_BUFFER, gridPositionBuffer); gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 0, 0);
  gl.bindBuffer(gl.ARRAY_BUFFER, gridColorBuffer); gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 0, 0);
  gl.drawArrays(gl.LINES, 0, gridVertexCount);

  if (state.points) {
    gl.useProgram(pointProgram);
    gl.uniformMatrix4fv(gl.getUniformLocation(pointProgram, "uViewProjection"), false, vp);
    gl.uniform1f(gl.getUniformLocation(pointProgram, "uPointSize"), state.pointSize * devicePixelRatio);
    gl.uniform1f(gl.getUniformLocation(pointProgram, "uMaxDistance"), state.maxDistance);
    gl.uniform1f(gl.getUniformLocation(pointProgram, "uZMin"), state.zMin);
    gl.uniform1f(gl.getUniformLocation(pointProgram, "uZMax"), state.zMax);
    gl.uniform1f(gl.getUniformLocation(pointProgram, "uIntensityMax"), state.intensityMax);
    gl.uniform1i(gl.getUniformLocation(pointProgram, "uColorMode"), state.colorMode);
    gl.bindBuffer(gl.ARRAY_BUFFER, pointBuffer);
    gl.enableVertexAttribArray(0); gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 20, 0);
    gl.enableVertexAttribArray(1); gl.vertexAttribPointer(1, 1, gl.FLOAT, false, 20, 12);
    gl.enableVertexAttribArray(2); gl.vertexAttribPointer(2, 1, gl.UNSIGNED_SHORT, false, 20, 16);
    gl.drawArrays(gl.POINTS, 0, state.points);
  }
  const now = performance.now();
  if (now - state.fpsAt >= 1000) {
    $("fps").textContent = ((state.frameCounter - state.fpsCounter) * 1000 / (now - state.fpsAt)).toFixed(1);
    state.fpsCounter = state.frameCounter; state.fpsAt = now;
  }
  const frameAge = state.lastFrameAt ? Math.max(0, Math.round(now-state.lastFrameAt)) : 0;
  const staleAfter = state.networkReloading ? 8000 : 2000;
  if (state.points && frameAge > staleAfter) {
    state.points = 0;
    $("pointCount").textContent = "0";
    $("emptyState").classList.remove("hidden");
    const indicator = $("socketState");
    indicator.className = "connection offline";
    indicator.innerHTML = "<i></i> NO LIDAR DATA";
  }
  $("frameAge").textContent = state.lastFrameAt ? `${frameAge} ms` : "—";
  if (schedule !== false) requestAnimationFrame(render);
}

function connectWebSocket() {
  const protocol = location.protocol === "https:" ? "wss" : "ws";
  const socket = new WebSocket(`${protocol}://${location.host}/ws/points`);
  state.socket = socket; socket.binaryType = "arraybuffer";
  const indicator = $("socketState");
  socket.onopen = () => { indicator.className = "connection"; indicator.innerHTML = "<i></i> WAITING DATA"; };
  socket.onmessage = (event) => { if (event.data instanceof ArrayBuffer) uploadFrame(event.data); };
  socket.onclose = () => { indicator.className = "connection offline"; indicator.innerHTML = "<i></i> RECONNECTING"; setTimeout(connectWebSocket, 1200); };
  socket.onerror = () => socket.close();
}

function renderDevices(devices) {
  devices = devices.filter(device => device.state === "online");
  $("deviceCount").textContent = devices.length;
  const list = $("deviceList");
  if (!devices.length) { list.innerHTML = '<div class="empty-devices">等待雷达流量。自动模式会分析 IP、端口和 VLAN，无需预先填写。</div>'; return; }
  if (!devices.some(device => device.id === state.selectedDevice)) {
    state.selectedDevice = devices[0].id;
    state.activeDevice = null;
    state.points = 0;
  }
  list.innerHTML = devices.map(device => `
    <article class="device-card ${device.state}${device.id === state.selectedDevice ? " selected" : ""}">
      <span class="device-vendor">${escapeHtml(device.vendor)} · ${device.state}</span>
      <h3 title="${escapeHtml(device.model)}">${escapeHtml(device.model)}</h3>
      <div class="device-meta"><span>${escapeHtml(device.ip)}</span><span>${device.packets.toLocaleString()} pkt</span></div>
      <button type="button" class="device-view-button" data-device-id="${device.id}">${device.id === state.selectedDevice ? "正在查看点云" : "查看点云"}</button>
      <a class="device-web-link" href="http://${escapeHtml(device.ip)}/" target="_blank" rel="noopener noreferrer">打开雷达控制页 ↗</a>
      <div class="confidence"><span style="width:${Math.round(device.confidence*100)}%"></span></div>
    </article>`).join("");
  const selected = devices.find(device => device.id === state.selectedDevice);
  $("activeModel").textContent = `${selected.vendor} · ${selected.model}`;
}

$("deviceList").addEventListener("click", event => {
  const button = event.target.closest("[data-device-id]");
  if (!button) return;
  state.selectedDevice = Number(button.dataset.deviceId);
  state.activeDevice = null;
  state.points = 0;
  state.lastFrameAt = 0;
  $("pointCount").textContent = "0";
  $("emptyState").classList.remove("hidden");
  $("deviceList").querySelectorAll(".device-card").forEach(card => card.classList.toggle("selected", card.contains(button)));
  $("deviceList").querySelectorAll(".device-view-button").forEach(item => item.textContent = item === button ? "正在查看点云" : "查看点云");
  $("activeModel").textContent = button.closest(".device-card").querySelector("h3").textContent;
});

function escapeHtml(value) {
  return String(value).replace(/[&<>'"]/g, char => ({"&":"&amp;","<":"&lt;",">":"&gt;","'":"&#39;",'"':"&quot;"})[char]);
}

function syncNetworkMode() {
  const automatic = $("autoDiscovery").checked;
  $("listenIp").disabled = automatic;
  $("udpPorts").disabled = automatic;
  $("autoArpAssist").disabled = !automatic;
}

function updateGatewayStatus(gateway, ptp) {
  if (!gateway) return;
  const forwarding = gateway.forwarding, recording = gateway.recording;
  $("forwardPackets").textContent = forwarding.packets.toLocaleString();
  $("forwardBytes").textContent = formatBytes(forwarding.bytes);
  $("forwardDropped").textContent = forwarding.dropped_packets.toLocaleString();
  if (forwarding.last_error) {
    $("gatewayResult").className = "config-result error";
    $("gatewayResult").textContent = forwarding.last_error;
  }
  $("startRecording").disabled = recording.active;
  $("stopRecording").disabled = !recording.active;
  $("recordPackets").textContent = `${recording.packets.toLocaleString()} pkt`;
  $("recordBytes").textContent = formatBytes(recording.bytes);
  $("recordFree").textContent = recording.directory ? formatBytes(recording.free_bytes) : "—";
  const recordLabel = recording.active
    ? `正在录制 · ${recording.current_file} · 队列 ${recording.queued_packets}`
    : recording.error ? recording.error
      : recording.current_file ? `已停止 · ${recording.current_file}` : "尚未开始录包";
  $("recordingStatus").textContent = recordLabel;
  $("recordingStatus").className = `config-result ${recording.error ? "error" : recording.active || recording.current_file ? "ok" : ""}`;

  if (ptp) {
    const mode = ptp.effective_timestamping ? ` · ${ptp.effective_timestamping === "hardware" ? "硬件时间戳" : "软件时间戳"}` : "";
    $("ptpStatus").textContent = ptp.active
      ? `PTP 运行中 · ${ptp.config.interface} · ${ptp.config.role === "master" ? "主时钟" : "从时钟"}${mode}`
      : ptp.error || (ptp.available ? "PTP 未启动" : "系统未安装 linuxptp（ptp4l）");
    $("ptpStatus").className = `config-result ${ptp.error ? "error" : ptp.active ? "ok" : ""}`;
    $("ptpLog").textContent = (ptp.logs || []).slice(-12).join("\n");
  }
}

function loadGatewayForm(gateway, ptp) {
  const config = gateway.config;
  $("forwardEnabled").checked = config.enabled;
  $("forwardIp").value = config.target_ip;
  $("forwardPort").value = config.target_port;
  $("forwardSourcePorts").value = config.source_ports.join(", ");
  if (ptp) {
    const currentInterface = ptp.config.interface;
    $("ptpInterface").innerHTML = (ptp.interfaces || []).map(item =>
      `<option value="${escapeHtml(item.name)}">${escapeHtml(item.name)} · ${item.state}${item.hardware_timestamping ? " · PHC" : " · software"}</option>`
    ).join("") || '<option value="">未发现可用网口</option>';
    $("ptpInterface").value = currentInterface;
    $("ptpEnabled").checked = ptp.config.enabled;
    $("ptpRole").value = ptp.config.role;
    $("ptpTransport").value = ptp.config.transport;
    $("ptpTimestamping").value = ptp.config.timestamping;
    $("ptpDomain").value = ptp.config.domain;
    $("ptpPriority").value = ptp.config.priority1;
    $("ptpDelay").value = ptp.config.delay_mechanism;
  }
  state.gatewayLoaded = true;
}

async function refreshStorage(force = false) {
  if (!force && performance.now() - state.storageLoadedAt < 5000) return;
  state.storageLoadedAt = performance.now();
  try {
    const response = await fetch("/api/gateway"), gateway = await response.json();
    const select = $("recordDirectory"), selected = select.value || gateway.recording.directory;
    select.innerHTML = gateway.storage.length
      ? gateway.storage.map(item => `<option value="${escapeHtml(item.mountpoint)}">${escapeHtml(item.mountpoint)} · ${formatBytes(item.free_bytes)} 可用</option>`).join("")
      : '<option value="">未发现已挂载且可写的 USB 存储</option>';
    if ([...select.options].some(item => item.value === selected)) select.value = selected;
  } catch (_error) { /* Main status poll reports service connectivity. */ }
}

async function refreshStatus() {
  try {
    const [statusResponse, devicesResponse] = await Promise.all([fetch("/api/status"), fetch("/api/devices")]);
    const status = await statusResponse.json(), result = await devicesResponse.json();
    const networkAdapt = status.network_adapt || {};
    state.networkReloading = Boolean(
      networkAdapt.started_ns && !networkAdapt.point_flow_restored_ns &&
      networkAdapt.state !== "error" && networkAdapt.state !== "restored"
    );
    $("version").textContent = `v${status.version}`;
    $("uptime").textContent = `${status.uptime_seconds}s`;
    $("clients").textContent = status.websocket_clients;
    if (status.gateway_only) {
      $("shutdownNote").textContent = "OpenWrt 服务模式持续运行；关闭网页不会停止转发、录包或 PTP。";
      if (!state.points) $("emptyState").innerHTML = '<div class="scanner"><span></span></div><h3>雷达网关运行中</h3><p>自动发现、UDP 转发和录包请在“网络设置”中控制，授时状态请在“PTP设置”中查看。</p>';
    }
    const capturePackets = status.capture ? status.capture.accepted_packets : 0;
    const pcapPackets = status.pcap ? status.pcap.accepted_packets : 0;
    $("packetTotal").textContent = (capturePackets + pcapPackets + status.listeners.reduce((sum, item) => sum + item.packets, 0)).toLocaleString();
    const listenerItems = status.listeners.map(item => `<span class="listener ${item.state}" title="${escapeHtml(item.error || item.bind_ip)}">${escapeHtml(item.bind_ip)}:${item.port} · ${item.state}</span>`);
    if (status.capture) {
      const captureState = status.capture.active ? "listening" : "error";
      listenerItems.unshift(`<span class="listener ${captureState}" title="${escapeHtml(status.capture.error || status.capture.library || "")}">L2 ${escapeHtml(status.capture.backend)} · ${status.capture.interfaces.filter(item => item.state === "capturing").length} NIC</span>`);
      status.capture.flows.slice(0, 6).forEach(flow => {
        const vlan = flow.vlan_ids.length ? ` · VLAN ${flow.vlan_ids.join("/")}` : " · untagged";
        listenerItems.push(`<span class="listener listening" title="${escapeHtml(flow.interface)}">${escapeHtml(flow.source_ip)}:${flow.source_port} → ${escapeHtml(flow.destination_ip)}:${flow.destination_port}${vlan}</span>`);
      });
      const arp = status.capture.arp_assist;
      if (arp && arp.enabled) {
        const active = arp.candidates.find(item => item.replies > 0);
        if (active) {
          const vlan = active.vlan_ids.length ? ` · VLAN ${active.vlan_ids.join("/")}` : "";
          listenerItems.unshift(`<span class="listener listening" title="${escapeHtml(active.interface)}">ARP 已引导 · ${escapeHtml(active.source_ip)} → ${escapeHtml(active.requested_ip)}${vlan}</span>`);
        } else if (arp.candidates.some(item => item.eligible)) {
          const candidate = arp.candidates.find(item => item.eligible);
          listenerItems.push(`<span class="listener warning" title="${escapeHtml(candidate.reason)}">ARP 已发现 · ${escapeHtml(candidate.source_ip)} 请求 ${escapeHtml(candidate.requested_ip)}</span>`);
        }
      }
    }
    if (status.pcap && status.pcap.active) {
      listenerItems.unshift(`<span class="listener listening" title="离线 PCAP SDK 回放">PCAP · ${escapeHtml(status.pcap.name)} · 第 ${status.pcap.completed_loops + 1} 轮</span>`);
    }
    if (!status.gateway_only && !status.native_plugins.loaded.length) {
      const detail = status.native_plugins.errors.join("; ") || "plugins directory is empty";
      listenerItems.unshift(`<span class="listener error" title="${escapeHtml(detail)}">DECODER PLUGINS MISSING · 无法输出点云</span>`);
    } else if (status.native_plugins.errors.length) {
      status.native_plugins.errors.forEach(error => listenerItems.unshift(`<span class="listener error" title="${escapeHtml(error)}">DECODER ERROR · ${escapeHtml(error)}</span>`));
    }
    status.native_plugins.loaded.forEach(plugin => (plugin.instances || []).forEach(instance => {
      const calibration = instance.calibration || {};
      const labels = {
        factory: "雷达工厂标定",
        file: "手动标定文件",
        cache: "雷达专属标定缓存",
        nominal: "Pandar40 官方默认角度 · PTC跨网段不可达",
        sdk_default: "Hesai SDK AT128 标称修正 · PTC跨网段不可达",
        sdk_native: "厂商 SDK 原生几何解算",
        missing: "标定缺失 · 点云高度可能无效",
        pending: "等待标定",
      };
      const css = calibration.state === "missing" ? "error" : calibration.state === "nominal" ? "warning" : "listening";
      const label = labels[calibration.state] || calibration.label || "未知";
      listenerItems.unshift(`<span class="listener ${css}" title="${escapeHtml(instance.device_ip || "")}">CALIBRATION · ${escapeHtml(label)}</span>`);
    }));
    $("listenerList").innerHTML = listenerItems.join("");
    if (status.pcap && !state.pcapUploading) {
      const pcap = status.pcap;
      $("pcapProgress").style.width = `${Math.round(pcap.progress * 100)}%`;
      $("stopPcap").disabled = !pcap.active;
      const labels = {
        idle: "尚未导入 PCAP",
        playing: `${pcap.name} · ${(pcap.progress * 100).toFixed(1)}% · 第 ${pcap.completed_loops + 1} 轮 · ${pcap.accepted_packets.toLocaleString()} 个雷达包`,
        completed: `${pcap.name} · 回放完成 · ${pcap.accepted_packets.toLocaleString()} 个雷达包`,
        stopped: pcap.name ? `${pcap.name} · 已停止` : "PCAP 回放已停止",
        error: `${pcap.name} · ${pcap.error || "回放失败"}`,
      };
      $("pcapStatus").textContent = labels[pcap.state] || pcap.state;
      $("pcapStatus").className = `config-result ${pcap.state === "error" ? "error" : pcap.active || pcap.state === "completed" ? "ok" : ""}`;
    }
    if (!state.configLoaded) {
      $("autoDiscovery").checked = Boolean(status.config.auto_discovery);
      $("autoArpAssist").checked = status.config.auto_arp_assist !== false;
      $("listenIp").value = status.config.listen_ip;
      $("udpPorts").value = status.config.udp_ports.join(", ");
      $("sourceNetworks").value = status.config.source_networks.join(", ");
      syncNetworkMode();
      state.configLoaded = true;
    }
    if (!state.gatewayLoaded) loadGatewayForm(status.gateway, status.ptp);
    updateGatewayStatus(status.gateway, status.ptp);
    refreshStorage();
    const addresses = status.config.local_ipv4_addresses.length ? status.config.local_ipv4_addresses.join(", ") : "等待网卡地址";
    const latestFlow = status.capture && status.capture.flows.length ? status.capture.flows[0] : null;
    const activeArp = status.capture && status.capture.arp_assist
      ? status.capture.arp_assist.candidates.find(item => item.replies > 0)
      : null;
    const discovered = latestFlow
      ? `已发现 ${latestFlow.source_ip}:${latestFlow.source_port} → ${latestFlow.destination_ip}:${latestFlow.destination_port}${latestFlow.vlan_ids.length ? ` · VLAN ${latestFlow.vlan_ids.join("/")}` : ""}`
      : activeArp
        ? `ARP 已引导 ${activeArp.source_ip} → ${activeArp.requested_ip}${activeArp.vlan_ids.length ? ` · VLAN ${activeArp.vlan_ids.join("/")}` : ""}`
      : `扫描全部网卡（${addresses}）`;
    const adapt = status.network_adapt || status.config.network_adapt;
    const adaptText = adapt && adapt.state !== "idle"
      ? ` · 网卡自适应：${adapt.message || adapt.state}`
      : "";
    $("autoSummary").textContent = status.config.auto_discovery
      ? `${status.capture && status.capture.active ? "二层抓包" : "UDP 降级"} · ${discovered}${adaptText}`
      : `当前：${status.config.effective_listen_ip} · ${status.config.effective_udp_ports.join(", ")}`;
    renderDevices(result.devices);
  } catch (_error) { /* WebSocket indicator already exposes the connection state. */ }
}

canvas.addEventListener("pointerdown", event => { state.dragging = true; state.lastX = event.clientX; state.lastY = event.clientY; canvas.setPointerCapture(event.pointerId); });
canvas.addEventListener("pointerup", () => state.dragging = false);
canvas.addEventListener("pointermove", event => {
  if (!state.dragging) return;
  state.yaw -= (event.clientX - state.lastX) * .006;
  state.pitch = Math.max(-1.45, Math.min(1.45, state.pitch + (event.clientY - state.lastY) * .006));
  state.lastX = event.clientX; state.lastY = event.clientY;
});
canvas.addEventListener("wheel", event => { event.preventDefault(); state.distance = Math.max(3, Math.min(250, state.distance * Math.exp(event.deltaY * .001))); }, { passive: false });
canvas.addEventListener("dblclick", () => setView("perspective"));

$("safeShutdown").addEventListener("click", async () => {
  const button = $("safeShutdown");
  button.disabled = true;
  button.textContent = "正在恢复网卡并退出…";
  try {
    const response = await fetch("/api/shutdown", { method: "POST" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    $("shutdownNote").textContent = "程序正在恢复启动前的 IP、VLAN 和 DHCP配置，可以关闭此页面。";
  } catch (error) {
    button.disabled = false;
    button.textContent = "安全退出并恢复网卡";
    $("shutdownNote").textContent = `安全退出失败：${error.message}`;
  }
});

function setView(view) {
  if (view === "top") { state.yaw = 0; state.pitch = 1.48; state.distance = 48; }
  else if (view === "front") { state.yaw = 0; state.pitch = .05; state.distance = 42; }
  else if (view === "side") { state.yaw = Math.PI / 2; state.pitch = .05; state.distance = 42; }
  else { state.yaw = -.74; state.pitch = .56; state.distance = 42; }
  document.querySelectorAll("[data-view]").forEach(button => button.classList.toggle("active", button.dataset.view === view));
}

document.querySelectorAll("[data-view]").forEach(button => button.addEventListener("click", () => setView(button.dataset.view)));
const networkPanel = $("networkPanel");
const ptpPanel = $("ptpPanel");
const inputConfigPanel = document.querySelector(".input-config");
const diagnosticsPanel = document.querySelector(".diagnostics");
const ptpConfigPanel = document.querySelector(".ptp-config");
if (networkPanel && inputConfigPanel) networkPanel.insertBefore(inputConfigPanel, networkPanel.children[1] || null);
if (networkPanel && diagnosticsPanel) networkPanel.appendChild(diagnosticsPanel);
if (ptpPanel && ptpConfigPanel) ptpPanel.appendChild(ptpConfigPanel);

document.querySelectorAll("[data-control-tab]").forEach(button => button.addEventListener("click", () => {
  const selected = button.dataset.controlTab;
  document.querySelectorAll("[data-control-tab]").forEach(item => item.classList.toggle("active", item === button));
  $("renderPanel").classList.toggle("active", selected === "render");
  $("networkPanel").classList.toggle("active", selected === "network");
  $("ptpPanel").classList.toggle("active", selected === "ptp");
  $("supportPanel").classList.toggle("active", selected === "support");
}));
$("colorMode").addEventListener("change", event => state.colorMode = Number(event.target.value));
$("pointSize").addEventListener("input", event => { state.pointSize = Number(event.target.value); $("pointSizeValue").textContent = state.pointSize.toFixed(1); });
$("maxDistance").addEventListener("input", event => { state.maxDistance = Number(event.target.value); $("distanceValue").textContent = `${state.maxDistance} m`; });
$("importPcap").addEventListener("click", () => $("pcapFile").click());
$("pcapFile").addEventListener("change", async event => {
  const file = event.target.files && event.target.files[0];
  if (!file) return;
  state.pcapUploading = true;
  $("importPcap").disabled = true;
  $("stopPcap").disabled = true;
  $("pcapProgress").style.width = "0%";
  $("pcapStatus").className = "config-result";
  $("pcapStatus").textContent = `正在导入 ${file.name}（${(file.size / 1024 / 1024).toFixed(1)} MB）…`;
  try {
    const response = await fetch("/api/pcap/replay", {
      method: "POST",
      headers: {
        "Content-Type": "application/vnd.tcpdump.pcap",
        "X-PCAP-Filename": encodeURIComponent(file.name),
        "X-PCAP-Loop": $("loopPcap").checked ? "1" : "0",
      },
      body: file,
    });
    const body = await response.json();
    if (!response.ok) throw new Error(body.error || "PCAP 导入失败");
    $("pcapStatus").className = "config-result ok";
    $("pcapStatus").textContent = `${file.name} · 已开始 SDK 回放`;
  } catch (error) {
    $("pcapStatus").className = "config-result error";
    $("pcapStatus").textContent = error.message;
  } finally {
    state.pcapUploading = false;
    $("importPcap").disabled = false;
    event.target.value = "";
    refreshStatus();
  }
});
$("stopPcap").addEventListener("click", async () => {
  $("stopPcap").disabled = true;
  try {
    const response = await fetch("/api/pcap/stop", { method: "POST" });
    const body = await response.json();
    if (!response.ok) throw new Error(body.error || "停止失败");
  } catch (error) {
    $("pcapStatus").className = "config-result error";
    $("pcapStatus").textContent = error.message;
  }
  refreshStatus();
});
$("autoDiscovery").addEventListener("change", syncNetworkMode);
$("applyNetwork").addEventListener("click", async () => {
  const result = $("configResult");
  const payload = {
    auto_discovery: $("autoDiscovery").checked,
    auto_arp_assist: $("autoArpAssist").checked,
    listen_ip: $("listenIp").value.trim(),
    udp_ports: $("udpPorts").value.split(",").map(value => value.trim()).filter(Boolean).map(Number),
    source_networks: $("sourceNetworks").value.split(",").map(value => value.trim()).filter(Boolean),
  };
  result.className = "config-result";
  result.textContent = "正在重启 UDP 监听器…";
  try {
    const response = await fetch("/api/config", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload) });
    const body = await response.json();
    if (!response.ok) throw new Error(body.error || "配置失败");
    state.configLoaded = false;
    if (body.listener_errors.length) {
      result.className = "config-result error";
      result.textContent = body.listener_errors.map(item => `${item.bind_ip}:${item.port} ${item.error}`).join("；");
    } else {
      result.className = "config-result ok";
      result.textContent = "监听配置已应用并保存";
    }
    refreshStatus();
  } catch (error) {
    result.className = "config-result error";
    result.textContent = error.message;
  }
});

async function postJson(path, payload) {
  const response = await fetch(path, {
    method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(payload),
  });
  const body = await response.json();
  if (!response.ok) throw new Error(body.error || "操作失败");
  return body;
}

$("applyGateway").addEventListener("click", async () => {
  const result = $("gatewayResult");
  result.className = "config-result"; result.textContent = "正在应用转发配置…";
  try {
    await postJson("/api/gateway/config", {
      enabled: $("forwardEnabled").checked,
      target_ip: $("forwardIp").value.trim(),
      target_port: Number($("forwardPort").value),
      source_ports: $("forwardSourcePorts").value.split(",").map(item => item.trim()).filter(Boolean).map(Number),
    });
    result.className = "config-result ok"; result.textContent = "转发配置已保存并立即生效";
  } catch (error) { result.className = "config-result error"; result.textContent = error.message; }
  refreshStatus();
});

$("startRecording").addEventListener("click", async () => {
  const result = $("recordingStatus");
  result.className = "config-result"; result.textContent = "正在创建 PCAP…";
  try {
    await postJson("/api/gateway/recording/start", {
      directory: $("recordDirectory").value,
      prefix: $("recordPrefix").value.trim(),
      split_mb: Number($("recordSplitMb").value),
      min_free_mb: 256,
    });
  } catch (error) { result.className = "config-result error"; result.textContent = error.message; }
  refreshStatus();
});

$("stopRecording").addEventListener("click", async () => {
  $("stopRecording").disabled = true;
  try { await fetch("/api/gateway/recording/stop", { method: "POST" }); }
  finally { refreshStatus(); refreshStorage(true); }
});

$("applyPtp").addEventListener("click", async () => {
  const result = $("ptpStatus");
  result.className = "config-result"; result.textContent = "正在重启 PTP 服务…";
  try {
    await postJson("/api/ptp/config", {
      enabled: $("ptpEnabled").checked,
      interface: $("ptpInterface").value,
      role: $("ptpRole").value,
      transport: $("ptpTransport").value,
      timestamping: $("ptpTimestamping").value,
      domain: Number($("ptpDomain").value),
      priority1: Number($("ptpPriority").value),
      delay_mechanism: $("ptpDelay").value,
    });
    result.className = "config-result ok"; result.textContent = $("ptpEnabled").checked ? "PTP 已启动" : "PTP 已停止";
  } catch (error) { result.className = "config-result error"; result.textContent = error.message; }
  refreshStatus();
});

connectWebSocket();
if (!rendererReady) {
  const detail = rendererError || "当前浏览器或显卡不支持 WebGL2";
  $("emptyState").innerHTML = `<h3>点云渲染器启动失败</h3><p>${escapeHtml(detail)}</p><p>设备发现、IP 和抓包状态仍会继续显示。</p>`;
} else {
  buildGrid(); render();
}
refreshStatus(); setInterval(refreshStatus, 1000);
