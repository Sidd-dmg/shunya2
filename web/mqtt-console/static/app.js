const MODES = ["Manual", "Automation", "Queue", "Hybrid"];
const state = {
  devices: new Map(),
  prefix: "",
  status: null,
  steps: [
    { type: "relay", relays: [1], duration: 2 },
    { type: "delay", duration: 2 },
    { type: "relay", relays: [2, 3], duration: 2 },
  ],
  rules: [{ node: 1, relay: 1, tempMin: 20, tempMax: 30, humMin: "", humMax: "", logic: "OR" }],
};

const $ = (id) => document.getElementById(id);

function log(message) {
  const item = document.createElement("li");
  item.textContent = `${new Date().toLocaleTimeString()}  ${message}`;
  $("activity").prepend(item);
}

async function command(payload) {
  if (!state.prefix) throw new Error("no gateway selected");
  const response = await fetch("/api/command", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ prefix: state.prefix, payload }),
  });
  const body = await response.json();
  if (!response.ok) throw new Error(body.error || "command failed");
  log(`sent ${JSON.stringify(payload)}`);
}

function selectedStatus() {
  return state.prefix ? state.devices.get(state.prefix)?.status || null : null;
}

function statusAgeSeconds(status) {
  const parsed = Date.parse(String(status?.timestamp || "").replace(" ", "T"));
  return Number.isFinite(parsed) ? (Date.now() - parsed) / 1000 : Infinity;
}

function render() {
  const select = $("device");
  const previous = state.prefix;
  const entries = [...state.devices.entries()].sort(
    (a, b) => statusAgeSeconds(a[1].status) - statusAgeSeconds(b[1].status)
  );
  select.replaceChildren();
  const placeholder = document.createElement("option");
  placeholder.value = "";
  placeholder.textContent = entries.length ? "Choose a gateway" : "Waiting…";
  select.append(placeholder);
  for (const [prefix, device] of entries) {
    const status = device.status || {};
    const stale = statusAgeSeconds(status) > 180;
    const option = document.createElement("option");
    option.value = prefix;
    option.textContent = `${prefix} · ${status.mode_name || "?"} · ${status.ip_address || "no IP"}${stale ? " · stale" : ""}`;
    option.selected = prefix === previous;
    select.append(option);
  }
  const status = selectedStatus();
  if (!status) {
    $("relays").replaceChildren();
    $("modes").replaceChildren();
    $("nodes").replaceChildren();
    $("stats").replaceChildren();
    $("device-title").textContent = entries.length ? "Choose the gateway to control" : "No gateway seen yet";
    $("device-meta").textContent = entries.length
      ? "Several devices share this broker. Nothing is commanded until you select one."
      : "The console listens for retained status on the configured broker.";
    $("raw").textContent = "{}";
    return;
  }
  state.status = status;
  $("mode-name").textContent = status.mode_name || "unknown";
  $("device-title").textContent = state.prefix;
  $("device-meta").textContent =
    `${status.firmware_version || "?"} · ${status.ip_address || "no IP"} · ${status.wifi_ssid || "Wi-Fi offline"}`;
  const uptime = Math.round((status.uptime_ms || 0) / 1000);
  const specs = [
    ["Wi-Fi", status.wifi_connected ? `${status.wifi_rssi} dBm` : "offline"],
    ["MQTT", status.mqtt_connected ? "connected" : "offline"],
    ["Nodes", String(status.paired_nodes ?? 0)],
    ["Uptime", `${uptime} s`],
    ["Sleep", `${status.sensor_sleep_sec ?? "?"} s`],
    ["Log", String(status.event_log?.pending_count ?? 0)],
  ];
  $("stats").replaceChildren(...specs.map(([label, value]) => {
    const node = document.createElement("div");
    node.className = "stat";
    node.innerHTML = `<span>${label}</span><b>${value}</b>`;
    return node;
  }));
  const relays = Array.isArray(status.relays) ? status.relays : Array(7).fill(false);
  const manual = status.mode === 0;
  $("relay-hint").textContent = manual
    ? "Manual commands change all seven relay outputs immediately."
    : "Relay buttons are locked until Manual mode is active.";
  $("relays").replaceChildren(...relays.map((on, index) => {
    const button = document.createElement("button");
    button.className = `relay${on ? " on" : ""}`;
    button.disabled = !manual || index >= 7;
    button.innerHTML = `<span>Relay ${index + 1}</span><b>${on ? "ON" : "OFF"}</b>`;
    button.onclick = () => command({ [`relay${index + 1}`]: !on }).catch(showError);
    return button;
  }));
  $("modes").replaceChildren(...MODES.map((name, index) => {
    const button = document.createElement("button");
    button.className = `mode${status.mode === index ? " active" : ""}`;
    button.textContent = name;
    button.onclick = () => applyMode(index);
    return button;
  }));
  const pairing = Boolean(status.pairing_mode);
  $("pair").textContent = pairing ? "Close pairing" : "Open pairing";
  $("pair-state").textContent = pairing ? "Pairing open" : "Pairing closed";
  if (document.activeElement !== $("sleep") && status.sensor_sleep_sec) {
    $("sleep").value = status.sensor_sleep_sec;
    $("sleep-out").textContent = `${status.sensor_sleep_sec} s`;
  }
  renderNodes(status.nodes || []);
  const queue = status.queue;
  $("queue-state").textContent = queue
    ? `Step ${queue.current_step}/${queue.total_steps} · ${queue.running ? "running" : "stopped"}${queue.paused ? " · paused" : ""}`
    : "Queue status appears in Queue or Hybrid mode.";
  $("log-state").textContent = status.ota_available
    ? "An OTA update is reported available."
    : "No OTA update is currently reported.";
  $("raw").textContent = JSON.stringify(status, null, 2);
}

function renderNodes(nodes) {
  $("nodes").replaceChildren(...(nodes.length ? nodes : [{ empty: true }]).map((node) => {
    const row = document.createElement("div");
    row.className = "node";
    if (node.empty) {
      row.innerHTML = "<b>No nodes</b><small>Hub pairing is required</small>";
      return row;
    }
    const identity = node.ieee || node.mac || "";
    const battery = node.battery == null ? identity : `${node.battery}%`;
    row.innerHTML = `<div><b>Node ${node.id}</b><small>${node.active ? "online" : "offline"} · LQI ${node.lqi ?? "?"}</small></div>` +
      `<div><b>${Number(node.temperature).toFixed(1)}°</b><small>${Number(node.humidity).toFixed(0)}% · ${battery}</small></div>`;
    return row;
  }));
}

function renderSteps() {
  $("queue-steps").replaceChildren(...state.steps.map((step, index) => {
    const row = document.createElement("div");
    row.className = "step";
    const kind = document.createElement("select");
    ["relay", "delay"].forEach((type) => {
      const option = document.createElement("option");
      option.value = type;
      option.textContent = type;
      option.selected = step.type === type;
      kind.append(option);
    });
    kind.onchange = () => { step.type = kind.value; renderSteps(); };
    const detail = document.createElement("input");
    detail.placeholder = step.type === "delay" ? "seconds" : "relays, e.g. 1,3";
    detail.value = step.type === "delay" ? step.duration : step.relays.join(",");
    detail.onchange = () => {
      if (step.type === "delay") step.duration = Number(detail.value);
      else step.relays = detail.value.split(",").map(Number).filter((n) => n >= 1 && n <= 7);
    };
    const seconds = document.createElement("input");
    seconds.type = "number";
    seconds.min = "1";
    seconds.value = step.duration;
    seconds.hidden = step.type === "delay";
    seconds.onchange = () => { step.duration = Number(seconds.value); };
    const remove = document.createElement("button");
    remove.className = "ghost";
    remove.textContent = "Remove";
    remove.onclick = () => { state.steps.splice(index, 1); renderSteps(); };
    row.append(kind, detail, seconds, remove);
    return row;
  }));
}

function renderRules() {
  $("rules").replaceChildren(...state.rules.map((rule, index) => {
    const row = document.createElement("div");
    row.className = "rule";
    const fields = [
      ["node", "Node", 1, 16],
      ["relay", "Relay", 1, 7],
      ["tempMin", "Temp min", -40, 80],
      ["tempMax", "Temp max", -40, 80],
    ];
    fields.forEach(([key, label, min, max]) => {
      const input = document.createElement("input");
      input.type = "number";
      input.min = min;
      input.max = max;
      input.placeholder = label;
      input.value = rule[key];
      input.title = label;
      input.onchange = () => { rule[key] = Number(input.value); };
      row.append(input);
    });
    const remove = document.createElement("button");
    remove.className = "ghost";
    remove.textContent = "Remove";
    remove.onclick = () => { state.rules.splice(index, 1); renderRules(); };
    row.append(remove);
    return row;
  }));
}

async function applyMode(mode) {
  const payload = { mode };
  if ($("graceful").checked) payload.transition = "graceful";
  if ($("autosave").checked) payload.auto_save = true;
  await command(payload);
}

function queuePayload() {
  return {
    queue: {
      loop: $("queue-loop").checked,
      auto_start: $("queue-auto").checked,
      steps: state.steps.map((step) => step.type === "delay"
        ? { delay: Number(step.duration) }
        : { relays: step.relays, duration: Number(step.duration) }),
    },
  };
}

function showError(error) {
  log(`error: ${error.message}`);
}

function onEvent(event) {
  if (event.type === "snapshot") {
    $("broker").textContent = event.broker ? "Broker connected" : "Broker offline";
    $("broker").className = `pill${event.broker ? "" : " offline"}`;
    event.devices.forEach((device) => state.devices.set(device.prefix, device));
  } else if (event.type === "broker") {
    $("broker").textContent = event.connected ? "Broker connected" : "Broker offline";
    $("broker").className = `pill${event.connected ? "" : " offline"}`;
  } else if (event.type === "status") {
    const device = state.devices.get(event.prefix) || { prefix: event.prefix };
    device.status = event.payload;
    state.devices.set(event.prefix, device);
  } else if (event.type === "link_test") {
    log(`link ${event.payload.link_test}: ${event.payload.pass ? "PASS" : "FAIL"} ${event.payload.detail || ""}`);
  } else if (event.type === "event_log") {
    log(`event log ${JSON.stringify(event.payload)}`);
  } else if (event.type === "node") {
    log(`node ${event.payload.node_id}: ${event.payload.temperature}° / ${event.payload.humidity}%`);
  }
  render();
}

function connect() {
  const source = new EventSource("/api/stream");
  source.onmessage = (message) => onEvent(JSON.parse(message.data));
  source.onerror = () => log("live stream interrupted; the browser will retry");
}

$("device").onchange = (event) => { state.prefix = event.target.value; render(); };
$("all-off").onclick = () => command({ all: false }).catch(showError);
$("pair").onclick = () => command({ pair: !selectedStatus()?.pairing_mode }).catch(showError);
$("sleep").oninput = () => { $("sleep-out").textContent = `${$("sleep").value} s`; };
$("sleep-apply").onclick = () => command({ sensor_sleep: Number($("sleep").value) }).catch(showError);
$("queue-add").onclick = () => { state.steps.push({ type: "relay", relays: [1], duration: 2 }); renderSteps(); };
$("queue-apply").onclick = async () => {
  try {
    await command({ mode: 2, auto_save: $("autosave").checked });
    await command(queuePayload());
  } catch (error) { showError(error); }
};
["pause", "resume", "restart", "stop"].forEach((action) => {
  $(`queue-${action}`).onclick = () => command({ queue_control: action }).catch(showError);
});
$("rule-add").onclick = () => {
  state.rules.push({ node: 1, relay: 1, tempMin: 20, tempMax: 30, humMin: "", humMax: "", logic: "OR" });
  renderRules();
};
$("rules-apply").onclick = async () => {
  try {
    await command({ mode: 1, auto_save: $("autosave").checked });
    await command({
      automation: {
        rules: state.rules.map((rule) => ({
          node: rule.node,
          relay: rule.relay,
          temp: { min: rule.tempMin, max: rule.tempMax },
          logic: rule.logic,
        })),
      },
    });
  } catch (error) { showError(error); }
};
$("rules-clear").onclick = async () => {
  try {
    await command({ mode: 1, auto_save: $("autosave").checked });
    await command({ automation: { clear: true } });
  } catch (error) { showError(error); }
};
$("hybrid-apply").onclick = () => command({
  mode: 3,
  auto_save: $("autosave").checked,
}).then(() => command({ hybrid: { logic: $("hybrid-logic").value } })).catch(showError);
["display", "display-tx", "stats", "hub"].forEach((name) => {
  const button = document.createElement("button");
  button.className = "ghost";
  button.textContent = name;
  button.onclick = () => command({ link_test: name }).catch(showError);
  $("tests").append(button);
});
$("log-count").onclick = () => command({ event_log: "count" }).catch(showError);
$("log-send").onclick = () => command({ event_log: "send" }).catch(showError);
$("log-clear").onclick = () => command({ event_log: "clear" }).catch(showError);
$("ota-check").onclick = () => command({ ota_check: true }).catch(showError);
$("clear-activity").onclick = () => { $("activity").replaceChildren(); };

renderSteps();
renderRules();
connect();
