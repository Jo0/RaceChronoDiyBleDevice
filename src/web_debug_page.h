#pragma once

// The single page served by web_debug.cpp. Polls /api/state (200 ms while
// streaming, 1 s otherwise); that polling doubles as the heartbeat that keeps
// a streaming session alive, so closing or backgrounding the tab ends it.
static const char WEB_DEBUG_PAGE[] = R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>BRZ CAN Debug</title>
<style>
  :root {
    --bg: #f6f7f9; --fg: #1b1f24; --muted: #667080; --card: #fff;
    --line: #dfe3e8; --accent: #1a6fd6; --on: #1f9d55; --off: #b42318;
    --warn: #b54708; --chg: #ffe08a;
  }
  @media (prefers-color-scheme: dark) {
    :root {
      --bg: #111418; --fg: #e6e9ee; --muted: #8b95a3; --card: #1a1f25;
      --line: #2b323b; --accent: #4c9bff; --on: #3fcf7f; --off: #ff6b5e;
      --warn: #f5a524; --chg: #6b5410;
    }
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 16px; background: var(--bg); color: var(--fg);
    font: 15px/1.4 system-ui, -apple-system, Segoe UI, Roboto, sans-serif;
  }
  h1 { font-size: 18px; margin: 0 0 12px; }
  .bar {
    display: flex; flex-wrap: wrap; gap: 8px 16px; align-items: center;
    background: var(--card); border: 1px solid var(--line);
    border-radius: 10px; padding: 12px; margin-bottom: 12px;
  }
  button {
    font: inherit; font-weight: 600; border: 0; border-radius: 8px;
    padding: 10px 18px; color: #fff; background: var(--accent); cursor: pointer;
  }
  button.stop { background: var(--off); }
  button.ghost { background: transparent; color: var(--fg); font-weight: 500;
                 border: 1px solid var(--line); padding: 9px 14px; }
  .stat { color: var(--muted); font-size: 13px; }
  .stat b { color: var(--fg); font-weight: 600; }
  .dot { display: inline-block; width: 8px; height: 8px; border-radius: 50%;
         margin-right: 4px; background: var(--muted); }
  .dot.on { background: var(--on); } .dot.off { background: var(--off); }
  .wrap { overflow-x: auto; background: var(--card);
          border: 1px solid var(--line); border-radius: 10px; }
  table { border-collapse: collapse; width: 100%; font-size: 13px; }
  th, td { padding: 6px 10px; text-align: right; white-space: nowrap;
           border-bottom: 1px solid var(--line); }
  th { color: var(--muted); font-weight: 600; position: sticky; top: 0;
       background: var(--card); }
  th:first-child, td:first-child, td.data, th.data { text-align: left; }
  td { font-family: ui-monospace, SFMono-Regular, Consolas, monospace;
       font-variant-numeric: tabular-nums; }
  td.data span { padding: 0 1px; border-radius: 3px; }
  td.data span.chg { background: var(--chg); }
  tr.stale td { color: var(--muted); }
  .empty { padding: 24px; text-align: center; color: var(--muted); }
  h2 { font-size: 12px; font-weight: 600; text-transform: uppercase;
       letter-spacing: .05em; color: var(--muted); margin: 16px 0 6px; }
  .sig { display: grid; gap: 8px;
         grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); }
  .tile { background: var(--card); border: 1px solid var(--line);
          border-radius: 10px; padding: 8px 12px; }
  .tile .k { display: flex; justify-content: space-between; gap: 6px;
             color: var(--muted); font-size: 12px; }
  .tile .v { font: 600 22px/1.25 ui-monospace, SFMono-Regular, Consolas, monospace;
             font-variant-numeric: tabular-nums; margin-top: 2px; }
  .tile .u { font: 400 12px/1 system-ui, sans-serif; color: var(--muted);
             margin-left: 4px; }
  .tile.stale .v { color: var(--muted); }
  .diag { background: var(--card); border: 1px solid var(--line);
          border-left: 4px solid var(--muted); border-radius: 10px;
          padding: 10px 12px; }
  .diag.ok { border-left-color: var(--on); }
  .diag.warn { border-left-color: var(--warn); }
  .diag.bad { border-left-color: var(--off); }
  .diag .t { font-weight: 600; }
  .diag .d { color: var(--muted); font-size: 13px; margin-top: 2px; }
  .counters { display: flex; flex-wrap: wrap; gap: 4px 16px; margin-top: 8px;
              padding-top: 8px; border-top: 1px solid var(--line); }
  .counters .stat b.nz { color: var(--warn); }
  .note { color: var(--muted); font-size: 12px; margin-top: 10px; }
</style>
</head>
<body>
<h1>BRZ Telemetry &middot; CAN debug</h1>
<div class="bar">
  <button id="toggle">Start CAN feed</button>
  <button id="units" class="ghost" title="Switch decoded values between metric and imperial">km/h · °C</button>
  <span class="stat"><span id="linkDot" class="dot"></span><b id="link">connecting&hellip;</b></span>
  <span class="stat">CAN reader <b id="can">&ndash;</b></span>
  <span class="stat">Frames <b id="total">0</b></span>
  <span class="stat">Bus <b id="busRate">0</b>/s</span>
  <span class="stat">IDs <b id="ids">0</b></span>
</div>
<div class="diag" id="diag">
  <div class="t" id="diagTitle">Connecting&hellip;</div>
  <div class="d" id="diagDetail"></div>
  <div class="counters">
    <span class="stat" title="Bit/stuff/CRC/form errors seen on the wire">Bus errors <b id="cBusErrors">0</b></span>
    <span class="stat" title="Receive Error Counter of the CAN controller right now">REC <b id="cRec">0</b></span>
    <span class="stat" title="Frames lost because the driver's receive queue was full">Queue drops <b id="cRxMissed">0</b></span>
    <span class="stat" title="Frames lost in the controller's hardware FIFO">FIFO drops <b id="cRxOverrun">0</b></span>
    <span class="stat" title="Times the bus went quiet for more than 100 ms (also sent to RaceChrono as CAN ID 0x777)">Timeouts <b id="cTimeouts">0</b></span>
    <span class="stat" title="Frames dropped in the RaceChrono (BLE) send buffer">BLE buffer drops <b id="cBufferOverflows">0</b></span>
    <span class="stat">(totals since boot, incl. RaceChrono sessions)</span>
  </div>
</div>
<h2>Engine</h2>
<div class="sig" id="sigEngine"></div>
<h2>Chassis</h2>
<div class="sig" id="sigChassis"></div>
<h2>Raw frames</h2>
<div class="wrap">
  <table>
    <thead><tr><th>ID</th><th>DLC</th><th class="data">Data</th>
      <th>Count</th><th>Hz</th><th>Age ms</th></tr></thead>
    <tbody id="rows"></tbody>
  </table>
  <div id="empty" class="empty">CAN feed is off. Press &ldquo;Start CAN feed&rdquo;.</div>
</div>
<p class="note" id="note"></p>
<script>
const $ = (id) => document.getElementById(id);
let streaming = false;
let prev = new Map();   // id -> {count, data, t, hz}
let prevTotal = null, prevT = 0, busHz = 0;

function hexId(id) { return '0x' + id.toString(16).toUpperCase().padStart(3, '0'); }

// Decoded channels for FT86 gen1 (2013-2020 BRZ / 86 / FR-S), transcribed
// from the RaceChrono equations in can_db/ft86.md. In RaceChrono terms, A is
// b[0], B is b[1], ...; bytesToIntLe() is signed little-endian. |n| is how
// many payload bytes the equation needs. Units are left blank where ft86.md
// doesn't state one.
//
// Signals with a |kind| go through the metric/imperial toggle below. Their
// |f| returns the unit the RaceChrono equation produces: m/s for speeds (the
// 0.015694 multiplier is Subaru's ~0.0565 km/h-per-count wheel-speed scaling
// divided by 3.6), deg C for temperatures. The car always sends metric over
// CAN, whatever the cluster is set to display.
const UNITS = {
  metric: {speed: ['km/h', (v) => v * 3.6], temp: ['°C', (v) => v]},
  imperial: {speed: ['mph', (v) => v * 2.2369363], temp: ['°F', (v) => v * 9 / 5 + 32]},
};
let unitSystem = 'metric';
try { if (localStorage.getItem('units') === 'imperial') unitSystem = 'imperial'; } catch (e) {}

const s8 = (b, i) => (b[i] >= 0x80 ? b[i] - 0x100 : b[i]);
const s16le = (b, i) => { const v = b[i] | (b[i + 1] << 8); return v >= 0x8000 ? v - 0x10000 : v; };
const SIGNALS = [
  {g: 'Engine', name: 'Engine RPM', id: 0x140, n: 4, unit: 'rpm',
   f: (b) => (b[2] | (b[3] << 8)) & 0x3fff},  // bitsToUIntLe(raw, 16, 14)
  {g: 'Engine', name: 'Accelerator', id: 0x140, n: 1, unit: '%', f: (b) => b[0] / 2.55},
  {g: 'Engine', name: 'Throttle valve', id: 0x140, n: 7, unit: '%', f: (b) => b[6] / 2.55},
  {g: 'Engine', name: 'Clutch', id: 0x140, n: 2, unit: '%', f: (b) => (b[1] & 0x80) / 1.28},
  {g: 'Engine', name: 'Gear', id: 0x141, n: 7, unit: '',
   // RaceChrono divides integers here, so min(g, 7) / 7 is 0 for gears 1-6
   // and 1 only for the 7 ("neutral") code.
   f: (b) => { const g = b[6] & 0xf; return g * (1 - Math.floor(Math.min(g, 7) / 7)); },
   fmt: (v) => (v === 0 ? 'N' : String(v))},
  {g: 'Engine', name: 'Coolant temp', id: 0x360, n: 4, kind: 'temp', f: (b) => b[3] - 40},
  {g: 'Engine', name: 'Oil temp', id: 0x360, n: 3, kind: 'temp', f: (b) => b[2] - 40},

  {g: 'Chassis', name: 'Speed', id: 0xD1, n: 2, kind: 'speed', dp: 1, f: (b) => s16le(b, 0) * 0.015694},
  {g: 'Chassis', name: 'Brake', id: 0xD1, n: 3, unit: '%', f: (b) => Math.min(b[2] / 0.7, 100)},
  {g: 'Chassis', name: 'Brake pressure', id: 0xD1, n: 3, unit: '', f: (b) => b[2] * 128},
  {g: 'Chassis', name: 'Steering angle', id: 0xD0, n: 2, unit: '°', dp: 1,
   f: (b) => s16le(b, 0) * -0.1},  // positive = turning left
  {g: 'Chassis', name: 'Yaw rate', id: 0xD0, n: 4, unit: '°/s', dp: 1, f: (b) => s16le(b, 2) * -0.286478897},
  {g: 'Chassis', name: 'Lateral accel', id: 0xD0, n: 7, unit: '', dp: 1, f: (b) => s8(b, 6) * 0.2},
  {g: 'Chassis', name: 'Long. accel', id: 0xD0, n: 8, unit: '', dp: 1, f: (b) => s8(b, 7) * -0.1},
  {g: 'Chassis', name: 'Wheel FL', id: 0xD4, n: 2, kind: 'speed', dp: 1, f: (b) => s16le(b, 0) * 0.015694},
  {g: 'Chassis', name: 'Wheel FR', id: 0xD4, n: 4, kind: 'speed', dp: 1, f: (b) => s16le(b, 2) * 0.015694},
  {g: 'Chassis', name: 'Wheel RL', id: 0xD4, n: 6, kind: 'speed', dp: 1, f: (b) => s16le(b, 4) * 0.015694},
  {g: 'Chassis', name: 'Wheel RR', id: 0xD4, n: 8, kind: 'speed', dp: 1, f: (b) => s16le(b, 6) * 0.015694},
];

for (const s of SIGNALS) {
  const tile = document.createElement('div');
  tile.className = 'tile stale';
  tile.innerHTML = '<div class="k"><span>' + s.name + '</span><span>' + hexId(s.id) +
      '</span></div><div class="v"><span>&ndash;</span><span class="u"></span></div>';
  $('sig' + s.g).appendChild(tile);
  s.tile = tile;
  s.value = tile.querySelector('.v span');
  s.unitEl = tile.querySelector('.u');
}

function applyUnitLabels() {
  const u = UNITS[unitSystem];
  for (const s of SIGNALS) s.unitEl.textContent = s.kind ? u[s.kind][0] : s.unit;
  $('units').textContent = u.speed[0] + ' · ' + u.temp[0];
}

$('units').onclick = () => {
  unitSystem = unitSystem === 'metric' ? 'imperial' : 'metric';
  try { localStorage.setItem('units', unitSystem); } catch (e) {}
  applyUnitLabels();
  renderSignals(lastById);
};
applyUnitLabels();

// |byId|: CAN ID -> {b: payload bytes, age: ms since last seen}.
let lastById = new Map();
function renderSignals(byId) {
  lastById = byId;
  for (const s of SIGNALS) {
    const frame = byId.get(s.id);
    let text = '–', stale = true;
    if (frame && frame.b.length >= s.n) {
      let v = s.f(frame.b);
      if (s.kind) v = UNITS[unitSystem][s.kind][1](v);
      text = s.fmt ? s.fmt(v) : v.toFixed(s.dp || 0);
      stale = frame.age > 1000;
    }
    s.value.textContent = text;
    s.tile.classList.toggle('stale', stale);
  }
}

function render(s) {
  const wasStreaming = streaming;
  streaming = s.streaming;
  if (streaming && !wasStreaming) { prev.clear(); prevTotal = null; busHz = 0; }

  $('toggle').textContent = streaming ? 'Stop CAN feed' : 'Start CAN feed';
  $('toggle').className = streaming ? 'stop' : '';
  $('can').textContent = !streaming ? 'off' : (s.can ? 'running' : 'starting');
  $('total').textContent = s.total.toLocaleString();
  $('ids').textContent = s.frames.length + (s.untracked ? ' (+untracked)' : '');
  renderDiagnosis(s);
  $('note').textContent = 'The feed stops on its own if this page stops polling for ' +
      (s.clientTimeoutMs / 1000) + ' s, or when RaceChrono connects over BLE ' +
      '(Wi-Fi is switched off for the whole RaceChrono session).';

  const t = performance.now();
  if (prevTotal !== null && t > prevT) {
    busHz = 0.7 * busHz + 0.3 * ((s.total - prevTotal) * 1000 / (t - prevT));
  }
  prevTotal = s.total; prevT = t;
  $('busRate').textContent = Math.round(busHz);

  const frames = s.frames.slice().sort((a, b) => a[0] - b[0]);
  $('empty').style.display = (streaming && frames.length) ? 'none' : '';
  $('empty').textContent = streaming
      ? 'Waiting for CAN frames…'
      : 'CAN feed is off. Press “Start CAN feed”.';

  const html = [];
  const next = new Map();
  const byId = new Map();
  for (const [id, len, data, count, age] of frames) {
    const b = [];
    for (let i = 0; i < len; i++) b.push(parseInt(data.substr(i * 2, 2), 16));
    if (streaming) byId.set(id, {b, age});

    const p = prev.get(id);
    let hz = 0;
    if (p && t > p.t) hz = 0.7 * p.hz + 0.3 * ((count - p.count) * 1000 / (t - p.t));
    next.set(id, {count, data, t, hz});

    let bytes = '';
    for (let i = 0; i < len; i++) {
      const b = data.substr(i * 2, 2);
      const changed = p && p.data.substr(i * 2, 2) !== b;
      bytes += '<span' + (changed ? ' class="chg"' : '') + '>' + b + '</span> ';
    }
    html.push('<tr' + (age > 1000 ? ' class="stale"' : '') + '><td>' + hexId(id) +
        '</td><td>' + len + '</td><td class="data">' + bytes + '</td><td>' +
        count.toLocaleString() + '</td><td>' + hz.toFixed(1) + '</td><td>' +
        age + '</td></tr>');
  }
  prev = next;
  $('rows').innerHTML = streaming ? html.join('') : '';
  renderSignals(byId);
}

// Diagnosis looks at how the counters moved over the last few seconds, not
// at their since-boot totals, so old problems don't stick around forever.
const DIAG_WINDOW_MS = 3000;
let diagHistory = [];

function diagnose(s) {
  const d = s.diag;
  const kbps = Math.round(d.bitRate / 1000) + ' kbit/s';

  if (!s.streaming) {
    return ['', 'CAN feed is off', 'Press “Start CAN feed” to listen to the bus.'];
  }
  if (d.startFailed) {
    return ['bad', 'CAN controller failed to start',
        'The ESP32 refused to start its CAN (TWAI) driver. That points at the ' +
        'firmware or GPIO setup, not the car or the wiring. Check the serial log.'];
  }
  if (d.busOff) {
    return ['bad', 'CAN controller is bus-off',
        'The controller shut itself off after too many errors. Check CANH/CANL wiring and the ' +
        kbps + ' bit rate.'];
  }

  const now = performance.now();
  diagHistory.push({t: now, total: s.total, busErrors: d.busErrors,
                    dropped: d.rxMissed + d.rxOverrun, timeouts: d.timeouts});
  while (diagHistory.length > 1 && now - diagHistory[0].t > DIAG_WINDOW_MS) diagHistory.shift();
  const first = diagHistory[0], last = diagHistory[diagHistory.length - 1];
  if (last.t - first.t < 1000) {
    return ['', 'Listening…', 'Collecting a few seconds of bus activity.'];
  }

  const frames = last.total - first.total;
  const errors = last.busErrors - first.busErrors;
  const dropped = last.dropped - first.dropped;
  const timeouts = last.timeouts - first.timeouts;
  const rate = Math.round(frames * 1000 / (last.t - first.t));

  if (frames > 0) {
    if (errors > 0) {
      return ['warn', 'Receiving, but with bus errors',
          'Frames get through, but some are corrupted. Check the connector is fully seated, ' +
          'the CANH/CANL pair is twisted, and termination (about 60 Ω between CANH and ' +
          'CANL with the car off).'];
    }
    if (timeouts > 0) {
      return ['warn', 'Bus keeps dropping out',
          'Frames arrive, then stop for more than 100 ms at a time. Usually a loose ' +
          'CANH/CANL connection, or the car going to sleep.'];
    }
    if (dropped > 0) {
      return ['warn', 'Receiving, but dropping frames',
          'The ESP32 is not reading frames off the controller fast enough. That is a ' +
          'firmware problem, not wiring.'];
    }
    return ['ok', 'CAN bus OK',
        rate + ' frames/s at ' + kbps + ', from ' + s.frames.length + ' CAN IDs.'];
  }

  if (errors > 0) {
    return ['bad', 'Bus activity, but no valid frames',
        'Something is on the wire but nothing decodes. Most likely CANH and CANL are ' +
        'swapped, or the car’s bus is not running at ' + kbps + '.'];
  }
  return ['bad', 'CAN bus is silent',
      'No frames and no errors. Check, in order: the car is in ACC or ON; CANH/CANL are ' +
      'connected; the transceiver has 3V3 and GND; CTX goes to GPIO4 and CRX to GPIO5. In ' +
      'listen-only mode some wiring faults (e.g. swapped CANH/CANL) can also look silent.'];
}

function renderDiagnosis(s) {
  if (!s.streaming) diagHistory = [];
  const [level, title, detail] = diagnose(s);
  $('diag').className = 'diag ' + level;
  $('diagTitle').textContent = title;
  $('diagDetail').textContent = detail;

  const d = s.diag;
  const counters = {cBusErrors: d.busErrors, cRec: d.rec, cRxMissed: d.rxMissed,
                    cRxOverrun: d.rxOverrun, cTimeouts: d.timeouts,
                    cBufferOverflows: d.bufferOverflows};
  for (const [id, v] of Object.entries(counters)) {
    $(id).textContent = v.toLocaleString();
    $(id).className = v ? 'nz' : '';
  }
}

function setLink(ok) {
  $('link').textContent = ok ? 'connected' : 'no response';
  $('linkDot').className = 'dot ' + (ok ? 'on' : 'off');
}

async function poll() {
  try {
    const r = await fetch('/api/state', {cache: 'no-store'});
    render(await r.json());
    setLink(true);
  } catch (e) {
    setLink(false);
  }
  setTimeout(poll, streaming ? 200 : 1000);
}

$('toggle').onclick = async () => {
  $('toggle').disabled = true;
  try {
    const r = await fetch('/api/stream?on=' + (streaming ? 0 : 1), {method: 'POST'});
    render(await r.json());
    setLink(true);
  } catch (e) {
    setLink(false);
  }
  $('toggle').disabled = false;
};

poll();
</script>
</body>
</html>
)html";
