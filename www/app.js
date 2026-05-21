// app.js — MultiCoder Web UI Application
// Communicates with multicoder-supervisor REST API on same host.
// All encoder management, config views, log tailing, and metadata display.

'use strict';

// ---- API base (same origin) ----
const API = '';  // relative to page origin

// ---- State ----
let encoders = [];
let selectedEncoder = null;  // 1-based
let selectedSection = null;  // 'input' | 'control' | 'metadata' | 'aac' | 'mp3' | 'hls' | 'srt' | 'log'
let logPinned = true;
let logTimer = null;
let statusTimer = null;
let metaHistory = [];
let availableInterfaces = [];
let availableAudioInputs = [];
let vuHandle = null;
let vuLastRaw = { leftDb: -60, rightDb: -60, connected: false };
let inputConnectedState = false;
let inputWarningLastText = '';
let inputWarningSince = null;
let gainPreviewTimer = null;
let inputStatusTimer = null;
let metadataStatusTimer = null;
let metaPanelTimer = null;
let metaLastEventCount = 0;
// Per-stream history: each entry is { ts, text } for that stream only.
let metaStreamHistory = { metadata: [], aac: [], mp3: [], hls: [], srt: [] };
const STREAM_SECTIONS = ['aac', 'mp3', 'hls', 'srt'];
const META_VIEW_SECTIONS = ['metadata', 'aac', 'mp3', 'hls', 'srt'];
const SRT_INPUT_LATENCY_MIN_MS = 0;
const SRT_INPUT_LATENCY_MAX_MS = 90000;

function listFromCsv(text) {
  return String(text || '').split(',').map(s => s.trim()).filter(Boolean);
}

function renderCommandRows(rows) {
  const safeRows = Array.isArray(rows) && rows.length ? rows : [
    { match: 'BREAK', action: 'START_BREAK' },
    { match: 'END_BREAK', action: 'END_BREAK' },
    { match: 'LEGAL_ID', action: 'LEGAL_ID' },
    { match: 'START_BREAK', action: 'START_BREAK' },
    { match: 'END_BREAK_NOW', action: 'END_BREAK_NOW' },
  ];
  return safeRows.map((r, idx) => `
    <div class="form-row scte-cmd-row" data-row="${idx}" style="gap:6px;align-items:center;">
      <label style="min-width:120px">Command ${idx + 1}:</label>
      <input type="text" class="scteMatch" value="${escapeHtml((r && r.match) || '')}" placeholder="Match text" style="width:130px"/>
      <span style="color:var(--muted)">=&gt;</span>
      <input type="text" class="scteAction" value="${escapeHtml((r && r.action) || '')}" placeholder="Action" style="width:130px"/>
      <button class="btn scteRowRemove" type="button">-</button>
    </div>`).join('');
}

function renderMetaScteConfigPane(scte, cueListenPort, watchTags, whitelistEntries, scteRows) {
  return `
  <h2>SCTE-35 Cue Settings</h2>
  <div class="form-row">
    <label>Enable SCTE-35 Listening:</label>
    <select id="scteListenEnabled">
      <option value="false" ${scte.listenEnabled ? '' : 'selected'}>OFF</option>
      <option value="true" ${scte.listenEnabled ? 'selected' : ''}>ON</option>
    </select>
  </div>
  <div class="form-row scte-dependent">
    <label>Enable SCTE-35 Engine:</label>
    <select id="scteEnabled">
      <option value="false" ${scte.enabled ? '' : 'selected'}>OFF</option>
      <option value="true" ${scte.enabled ? 'selected' : ''}>ON</option>
    </select>
  </div>
  <div class="form-row scte-dependent">
    <label>Listen Transport:</label>
    <select id="scteListenTransport">
      <option value="http" ${(scte.listenTransport || 'http') === 'http' ? 'selected' : ''}>HTTP</option>
      <option value="tcp" ${(scte.listenTransport || '') === 'tcp' ? 'selected' : ''}>TCP</option>
      <option value="both" ${(scte.listenTransport || '') === 'both' ? 'selected' : ''}>Both</option>
    </select>
  </div>
  <div class="form-row scte-dependent">
    <label>Cue Listen Port:</label>
    <input type="number" id="scteListenPort" min="1" max="65535" value="${cueListenPort}"/>
  </div>
  <div class="form-row scte-dependent">
    <label>Cue Delivery Type:</label>
    <select id="scteDeliveryType">
      <option value="json" ${(scte.cueDeliveryType || 'json') === 'json' ? 'selected' : ''}>JSON Only</option>
      <option value="xml" ${(scte.cueDeliveryType || '') === 'xml' ? 'selected' : ''}>XML Only</option>
      <option value="both" ${(scte.cueDeliveryType || '') === 'both' ? 'selected' : ''}>Both</option>
    </select>
  </div>
  <div class="form-row scte-dependent">
    <label>Passthrough Mode:</label>
    <select id="sctePassthroughMode">
      <option value="off" ${(scte.passthroughMode || '') === 'off' ? 'selected' : ''}>OFF</option>
      <option value="pass-through" ${(scte.passthroughMode || 'pass-through') === 'pass-through' ? 'selected' : ''}>Pass-through</option>
      <option value="generate-from-cues" ${(scte.passthroughMode || '') === 'generate-from-cues' ? 'selected' : ''}>Generate from Cues</option>
    </select>
  </div>
  <div class="form-row scte-dependent">
    <label>Require Event ID:</label>
    <select id="scteRequireEventId">
      <option value="false" ${scte.requireEventId ? '' : 'selected'}>No</option>
      <option value="true" ${scte.requireEventId ? 'selected' : ''}>Yes</option>
    </select>
  </div>
  <div class="form-row scte-dependent">
    <label>Require Token:</label>
    <select id="scteRequireToken">
      <option value="false" ${scte.requireToken ? '' : 'selected'}>No</option>
      <option value="true" ${scte.requireToken ? 'selected' : ''}>Yes</option>
    </select>
  </div>
  <div class="form-row scte-dependent">
    <label>Token:</label>
    <input type="text" id="scteToken" value="${escapeHtml(scte.token || '')}" placeholder="Optional shared token"/>
  </div>
  <div class="form-row scte-dependent">
    <label>Watch XML Tags:</label>
    <input type="text" id="scteWatchTags" value="${escapeHtml(watchTags)}" placeholder="SCTE, Event_ID, Event_Duration"/>
  </div>
  <div class="form-row scte-dependent">
    <label>Whitelist:</label>
    <select id="scteWhitelistEnabled">
      <option value="false" ${scte.whitelistEnabled ? '' : 'selected'}>OFF</option>
      <option value="true" ${scte.whitelistEnabled ? 'selected' : ''}>ON</option>
    </select>
  </div>
  <div class="form-row scte-dependent">
    <label>Whitelist Entries:</label>
    <input type="text" id="scteWhitelistEntries" value="${escapeHtml(whitelistEntries)}" placeholder="192.168.1.10, 10.1.0.0/16"/>
  </div>
  <div class="form-row scte-dependent" style="align-items:flex-start;">
    <label>Command Mapping:</label>
    <div id="scteCommandRows" style="width:100%">${renderCommandRows(scteRows)}</div>
  </div>
  <div class="action-row scte-dependent">
    <button class="btn" id="scteAddRowBtn" type="button">Add Command Row</button>
  </div>
  <div class="form-row scte-dependent">
    <label>Override Rate Limit:</label>
    <select id="scteOverrideRateLimit">
      <option value="false" ${scte.overrideRateLimit ? '' : 'selected'}>Use Global</option>
      <option value="true" ${scte.overrideRateLimit ? 'selected' : ''}>Override</option>
    </select>
  </div>
  <div class="form-row scte-dependent">
    <label>Rate Limit Count:</label>
    <input type="number" id="scteRateLimitCount" min="1" value="${Number.isFinite(scte.rateLimitCount) ? scte.rateLimitCount : 5}"/>
  </div>
  <div class="form-row scte-dependent">
    <label>Rate Limit Window (sec):</label>
    <input type="number" id="scteRateLimitWindowSec" min="1" value="${Number.isFinite(scte.rateLimitWindowSec) ? scte.rateLimitWindowSec : 10}"/>
  </div>
  <div class="form-row scte-dependent">
    <label>Override Dedupe:</label>
    <select id="scteOverrideDedupe">
      <option value="false" ${scte.overrideDedupe ? '' : 'selected'}>Use Global</option>
      <option value="true" ${scte.overrideDedupe ? 'selected' : ''}>Override</option>
    </select>
  </div>
  <div class="form-row scte-dependent">
    <label>Dedupe Seconds:</label>
    <input type="number" id="scteDedupeSeconds" min="0" value="${Number.isFinite(scte.dedupeSeconds) ? scte.dedupeSeconds : 30}"/>
  </div>`;
}

function setMainLayout(mode) {
  const main = document.querySelector('main');
  if (!main) return;
  main.classList.toggle('log-expanded', mode === 'log');
}

// ---- Livewire channel formula ----
function channelToMulticast(n) {
    n = parseInt(n, 10);
    if (isNaN(n) || n < 0 || n > 65535) return null;
    const a = Math.floor(n / 256);
    const b = n % 256;
    return `239.192.${a}.${b}`;
}

// ---- Fetch helpers ----
async function apiGet(path) {
    const r = await fetch(API + path);
    if (!r.ok) throw new Error(`GET ${path} -> ${r.status}`);
    return r.json().catch(() => r.text());
}
async function apiPost(path, data) {
    const r = await fetch(API + path, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: typeof data === 'string' ? data : JSON.stringify(data)
    });
    if (!r.ok) throw new Error(`POST ${path} -> ${r.status}`);
    return r.json().catch(() => r.text());
}
async function apiText(path) {
    const r = await fetch(API + path);
    if (!r.ok) return '';
    return r.text();
}

// ---- Banner message ----
function showBanner(msg, type) {
    const el = document.getElementById('bannerMsg');
    el.textContent = msg;
    el.style.color = type === 'error' ? '#ff6666' : type === 'warn' ? '#ffc107' : '#aef';
    if (type !== 'error') setTimeout(() => { if (el.textContent === msg) el.textContent = ''; }, 5000);
}

function escapeHtml(value) {
  return String(value || '')
    .replace(/&/g, '&amp;')
    .replace(/</g, '&lt;')
    .replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;')
    .replace(/'/g, '&#39;');
}

async function ensureInterfacesLoaded(force) {
  if (!force && availableInterfaces.length) return;
  try {
    const ifaces = await apiGet('/api/admin/interfaces');
    if (Array.isArray(ifaces)) availableInterfaces = ifaces;
  } catch {
    availableInterfaces = [];
  }
}

async function ensureAudioInputsLoaded(force) {
  if (!force && availableAudioInputs.length) return;
  try {
    const devices = await apiGet('/api/admin/audio-inputs');
    if (Array.isArray(devices)) availableAudioInputs = devices;
  } catch {
    availableAudioInputs = [];
  }
}

function audioInputOptions(selected) {
  const sel = selected || '';
  const opts = [];
  opts.push(`<option value="" ${sel === '' ? 'selected' : ''}>Default Input</option>`);
  const seen = new Set();
  availableAudioInputs.forEach(d => {
    const id = (d && d.id) ? String(d.id) : '';
    if (!id || seen.has(id)) return;
    seen.add(id);
    const name = (d && d.friendlyName) ? String(d.friendlyName) : ((d && d.name) ? String(d.name) : id);
    opts.push(`<option value="${escapeHtml(id)}" ${sel === id ? 'selected' : ''}>${escapeHtml(name)}</option>`);
  });
  return opts.join('');
}

function interfaceOptions(selected) {
  const selectedVal = selected || '';
  const opts = [];
  opts.push(`<option value="" ${selectedVal === '' ? 'selected' : ''}>Auto</option>`);

  const isFilteredInterface = iface => {
    // If the user has explicitly enabled/disabled this interface via Admin settings, honour that.
    if (iface.enabled === false) return true;   // explicitly excluded
    if (iface.enabled === true)  return false;  // explicitly included
    // No stored pref — apply the default name-based heuristic.
    const n = ((iface && iface.name) ? String(iface.name) : '').toLowerCase();
    const f = ((iface && iface.friendlyName) ? String(iface.friendlyName) : '').toLowerCase();
    const text = `${n} ${f}`;

    // Hide virtual/loopback/bluetooth adapters from normal encoder selection.
    if (text.includes('loopback')) return true;
    if (text.includes('bluetooth')) return true;
    if (text.includes('vethernet')) return true;
    if (text.startsWith('lo ') || n === 'lo') return true;
    if (n.startsWith('veth') || n.startsWith('docker') || n.startsWith('br-') || n.startsWith('virbr')) return true;
    if (n.startsWith('tun') || n.startsWith('tap')) return true;
    return false;
  };

  const seen = new Set();
  availableInterfaces.forEach(iface => {
    if (isFilteredInterface(iface)) return;
    const name = (iface && iface.name) ? String(iface.name) : '';
    if (!name || seen.has(name)) return;
    seen.add(name);
    const friendly = (iface && iface.friendlyName) ? String(iface.friendlyName) : name;
    const label = friendly === name ? friendly : `${friendly} (${name})`;
    opts.push(`<option value="${escapeHtml(name)}" ${selectedVal === name ? 'selected' : ''}>${escapeHtml(label)}</option>`);
  });

  if (selectedVal && !seen.has(selectedVal)) {
    opts.push(`<option value="${escapeHtml(selectedVal)}" selected>${escapeHtml(selectedVal)}</option>`);
  }
  return opts.join('');
}

// ---- Status bar: render encoder cards ----
function renderEncoderCards() {
    const container = document.getElementById('encoderCards');
    container.innerHTML = '';
    encoders.forEach(enc => {
        const div = document.createElement('div');
        div.className = 'encoder-card' + (enc.id === selectedEncoder ? ' selected' : '');
        div.dataset.id = enc.id;
        div.innerHTML = `
            <div class="encoder-title">Encoder ${enc.id}</div>
            <div class="stream-statuses">
                <span>AAC: <span class="${enc.aac ? 's-live' : 's-stop'}">${enc.aac ? 'LIVE' : 'Stopped'}</span></span><br>
                <span>MP3: <span class="${enc.mp3 ? 's-live' : 's-stop'}">${enc.mp3 ? 'LIVE' : 'Stopped'}</span></span><br>
                <span>HLS: <span class="${enc.hls ? 's-live' : 's-stop'}">${enc.hls ? 'LIVE' : 'Stopped'}</span></span><br>
                <span>SRT: <span class="${enc.srt ? 's-live' : 's-stop'}">${enc.srt ? 'LIVE' : 'Stopped'}</span></span>
            </div>`;
        div.addEventListener('click', () => selectEncoder(enc.id));
        container.appendChild(div);
    });
}

// ---- Left nav panel ----
function renderLeftNav() {
    const title = document.getElementById('leftPanelTitle');
    const nav   = document.getElementById('encoderNav');
    if (!selectedEncoder) { title.textContent = 'Select an Encoder'; nav.innerHTML = ''; return; }
    title.textContent = `Encoder ${selectedEncoder}`;
    const links = [
        { id: 'input',    label: 'Input Selection' },
        { id: 'control',  label: 'Control Configuration' },
        { id: 'metadata', label: 'Metadata Input Configuration' },
        { id: 'aac',      label: 'AAC Icecast Configuration' },
        { id: 'mp3',      label: 'MP3 Icecast Configuration' },
        { id: 'hls',      label: 'HLS Output Configuration' },
        { id: 'srt',      label: 'SRT Output Configuration' },
        { id: 'log',      label: 'Tail Encoder Logs' },
    ];
    nav.innerHTML = links.map(l =>
        `<li><a href="#" class="${selectedSection === l.id ? 'active' : ''}" data-section="${l.id}">${l.label}</a></li>`
    ).join('');
    nav.querySelectorAll('a').forEach(a => {
        a.addEventListener('click', e => { e.preventDefault(); selectSection(a.dataset.section); });
    });
}

// ---- Select encoder ----
function selectEncoder(id) {
    selectedEncoder = id;
    selectedSection = 'input';  // default section
    metaLastEventCount = 0;
  metaStreamHistory = { metadata: [], aac: [], mp3: [], hls: [], srt: [] };
    renderEncoderCards();
    renderLeftNav();
    loadSection('input');
}

// ---- Select section ----
function selectSection(section) {
    selectedSection = section;
    renderLeftNav();
    loadSection(section);
}

// ---- Load + render a section ----
async function loadSection(section) {
    // Stop any active log poll
    if (logTimer) { clearInterval(logTimer); logTimer = null; }
  if (vuHandle) { clearInterval(vuHandle); vuHandle = null; }
  if (inputStatusTimer) { clearInterval(inputStatusTimer); inputStatusTimer = null; }
  if (metadataStatusTimer) { clearInterval(metadataStatusTimer); metadataStatusTimer = null; }
  setMainLayout(section === 'log' ? 'log' : 'default');
  // Panel 4 is relevant for metadata + stream sections.
  const rightPanel = document.getElementById('rightPanel');
  if (META_VIEW_SECTIONS.includes(section)) {
    rightPanel.style.display = '';
    startMetaPanelPoller();
  } else {
    rightPanel.style.display = 'none';
    if (metaPanelTimer) { clearInterval(metaPanelTimer); metaPanelTimer = null; }
  }

    const cl = document.getElementById('centerLeftContent');
    const cr = document.getElementById('centerRightContent');
    cr.innerHTML = '';

    switch (section) {
        case 'input':    await renderInputSection(cl, cr);    break;
        case 'control':  await renderControlSection(cl, cr);  break;
        case 'metadata': await renderMetaSection(cl, cr);     break;
        case 'aac':      await renderIcecastSection('aac', cl, cr); break;
        case 'mp3':      await renderIcecastSection('mp3', cl, cr); break;
        case 'hls':      await renderHLSSection(cl, cr);      break;
        case 'srt':      await renderSRTSection(cl, cr);      break;
        case 'log':      renderLogSection(cl, cr);             break;
        default:         cl.innerHTML = '<p>Select a section.</p>';
    }
}

// ---- Load encoder config helper ----
async function loadEncoderConfig(encoderId) {
    if (!encoderId) return {};
    try { return await apiGet(`/api/encoder/${encoderId}/config`); }
    catch (e) { return {}; }
}

// ===========================================================
// INPUT SECTION
// ===========================================================
async function renderInputSection(cl, cr) {
  await ensureInterfacesLoaded(false);
    await ensureAudioInputsLoaded(false);
    const cfg = await loadEncoderConfig(selectedEncoder);
    const inp = cfg.input || {};
    const inputType  = inp.inputType  || 'rtp';
    const gain       = inp.rtpGain !== undefined ? inp.rtpGain : 0;
    const bitrate    = inp.bitrate    || 128000;
    const sampleRate = inp.sampleRate || 48000;

    cl.innerHTML = `
    <h2>Input Configuration</h2>
    <div class="vu-container">
      <div class="vu-title" id="inputVuTitle">Input (Disconnected)</div>
      <div class="vu-bar-wrap">
        <div class="vu-channel"><span class="vu-label">L</span><div class="vu-track"><div class="vu-fill" id="vuL"></div></div></div>
        <div class="vu-channel"><span class="vu-label">R</span><div class="vu-track"><div class="vu-fill" id="vuR"></div></div></div>
      </div>
      <div id="inputWarningLine" style="display:none;margin-top:6px;font-size:11px;color:#ff8a80;"></div>
    </div>

    <div class="form-row">
      <label>Input Gain</label>
      <input type="range" id="gainSlider" min="-15" max="15" step="1" value="${gain}"/>
      <input type="number" id="gainBox"   min="-15" max="15" step="0.25" value="${gain}" style="width:55px"/>
      <span class="unit">dB</span>
    </div>

    <div class="form-row">
      <label>Bitrate:</label>
      <select id="bitrateSelect">
        ${[64000,96000,128000,192000,256000,320000].map(b =>
            `<option value="${b}" ${b===bitrate?'selected':''}>${b/1000}k</option>`).join('')}
      </select>
    </div>
    <div class="form-row">
      <label>Sample Rate:</label>
      <select id="sampleRateSelect">
        ${[44100,48000].map(s => `<option value="${s}" ${s===sampleRate?'selected':''}>${s/1000}K</option>`).join('')}
      </select>
    </div>

    <div class="form-row">
      <label>Input Source:</label>
      <select id="inputTypeSelect">
        <option value="axia"  ${inputType==='axia' ?'selected':''}>Axia/Livewire</option>
        <option value="rtp"   ${inputType==='rtp'  ?'selected':''}>RTP Input</option>
        <option value="audio" ${inputType==='audio'?'selected':''}>Audio Device</option>
        <option value="srt"   ${inputType==='srt'  ?'selected':''}>SRT Input</option>
      </select>
    </div>

    <div id="inputTypeFields"></div>

    <div class="action-row">
      <button class="btn btn-primary" id="connectBtn">Connect</button>
      <button class="btn btn-danger"  id="disconnectBtn">Disconnect</button>
      <button class="btn"             id="inputSaveBtn">Save</button>
    </div>
    <div id="inputStatusLine" style="margin-top:8px;font-size:11px;color:var(--muted);"></div>`;

    // VU meters start silent until input is connected.
    updateVUFromLevels({ connected: false, leftDb: -60, rightDb: -60 });
    updateInputWarning({ connected: false, warning: 'Input disconnected' });

    // Gain slider sync
    const slider = document.getElementById('gainSlider');
    const box    = document.getElementById('gainBox');
    slider.addEventListener('input', () => {
      box.value = slider.value;
      updateVUFromLevels(vuLastRaw);
      scheduleGainPreviewUpdate();
    });
    slider.addEventListener('change', () => {
      applyGainNow();
    });
    box.addEventListener('change', () => {
      let v = parseFloat(box.value);
      v = Math.max(-15, Math.min(15, v));
      box.value = v;
      slider.value = v;
      updateVUFromLevels(vuLastRaw);
      scheduleGainPreviewUpdate();
      applyGainNow();
    });

    // Input type toggle
    const sel = document.getElementById('inputTypeSelect');
    sel.addEventListener('change', () => renderInputTypeFields(sel.value, inp));
    renderInputTypeFields(inputType, inp);

    document.getElementById('inputSaveBtn').addEventListener('click', async () => {
      try {
        const selectedType = document.getElementById('inputTypeSelect').value;
        const data = gatherInputConfig(selectedType);
        const validationError = validateInputConfigPayload(data);
        if (validationError) {
          showBanner(validationError, 'error');
          return;
        }
        await apiPost(`/api/encoder/${selectedEncoder}/config/input`, JSON.stringify(data));
        showBanner('Input config saved', 'ok');
        await refreshInputStatusLine();
      } catch (e) {
        showBanner('Input config save failed: ' + e.message, 'error');
      }
    });

    document.getElementById('connectBtn').addEventListener('click', async () => {
      try {
        const selectedType = document.getElementById('inputTypeSelect').value;
        const data = gatherInputConfig(selectedType);
        const validationError = validateInputConfigPayload(data);
        if (validationError) {
          showBanner(validationError, 'error');
          return;
        }
        await apiPost(`/api/encoder/${selectedEncoder}/input/connect`, data);
        showBanner(`Encoder ${selectedEncoder}: input connected`, 'ok');
        await refreshInputLevels();
        await refreshInputStatusLine();
      } catch (e) {
        showBanner('Input connect failed: ' + e.message, 'error');
      }
    });

    document.getElementById('disconnectBtn').addEventListener('click', async () => {
      updateVUFromLevels({ connected: false, leftDb: -60, rightDb: -60 });
      updateInputVuTitle({ connected: false });
      updateInputWarning({ connected: false, warning: 'Input disconnected' });
      try {
        await apiPost(`/api/encoder/${selectedEncoder}/input/disconnect`, {});
        showBanner(`Encoder ${selectedEncoder}: input disconnected`, 'warn');
      } catch (e) {
        showBanner('Input disconnect failed: ' + e.message, 'error');
      }
      await refreshInputLevels();
      await refreshInputStatusLine();
    });

    await refreshInputStatusLine();
    await refreshInputLevels();
    vuHandle = setInterval(refreshInputLevels, 250);
    inputStatusTimer = setInterval(refreshInputStatusLine, 1500);
}

function renderInputTypeFields(type, inp) {
    const container = document.getElementById('inputTypeFields');
    if (type === 'axia') {
        container.innerHTML = `
        <div class="form-row">
          <label>Axia/LW Channel Number:</label>
          <input type="number" id="axiaChannel" min="0" max="65535" value="${inp.axiaChannel||14801}"/>
          <span class="unit" id="axiaIP"></span>
        </div>
        <div class="form-row">
          <label>Network Interface:</label>
          <select id="rtpInterface">${interfaceOptions(inp.rtpInterface||'')}</select>
        </div>`;
        const ch = document.getElementById('axiaChannel');
        const ip = document.getElementById('axiaIP');
        function updateIP() { ip.textContent = channelToMulticast(ch.value) || 'invalid'; }
        ch.addEventListener('input', updateIP);
        updateIP();
    } else if (type === 'rtp') {
        container.innerHTML = `
        <div class="form-row">
          <label>RTP Multicast Address:</label>
          <input type="text" id="rtpAddress" value="${inp.rtpAddress||'239.192.78.245'}"/>
        </div>
        <div class="form-row">
          <label>RTP Port:</label>
          <input type="number" id="rtpPort" value="${inp.rtpPort||5004}"/>
        </div>
        <div class="form-row">
          <label>Network Interface:</label>
          <select id="rtpInterface">${interfaceOptions(inp.rtpInterface||'')}</select>
        </div>`;
    } else if (type === 'audio') {
        container.innerHTML = `
        <div class="form-row">
          <label>Audio Device:</label>
          <select id="audioDevice">${audioInputOptions(inp.audioDevice || '')}</select>
        </div>`;
    } else if (type === 'srt') {
        container.innerHTML = `
        <div class="form-row">
          <label>SRT Mode:</label>
          <select id="srtInMode">
            <option value="caller"   ${inp.srtMode==='caller'?'selected':''}>Caller (connect to remote listener)</option>
            <option value="listener" ${inp.srtMode==='listener'?'selected':''}>Listener (wait for incoming sender)</option>
          </select>
        </div>
        <div class="form-row">
          <label>SRT Transport:</label>
          <select id="srtInTransport">
            <option value="mpeg-ts" ${inp.srtTransport==='mpeg-ts'?'selected':''}>MPEG-TS</option>
            <option value="rtp"     ${inp.srtTransport==='rtp'    ?'selected':''}>RTP</option>
          </select>
        </div>
        <div class="form-row">
          <label>Host URL:</label>
          <input type="text" id="srtInHost" value="${inp.srtHost||''}"/>
        </div>
        <div class="form-row">
          <label>Port Number:</label>
          <input type="number" id="srtInPort" value="${inp.srtPort||9050}"/>
        </div>
        <div class="form-row">
          <label>SRT Input Delay (ms):</label>
          <input type="number" id="srtInLatency" min="${SRT_INPUT_LATENCY_MIN_MS}" max="${SRT_INPUT_LATENCY_MAX_MS}" value="${Number.isFinite(inp.srtLatency) ? inp.srtLatency : 120}"/>
        </div>
        <div class="form-row">
          <label>Stream Password:</label>
          <input type="password" id="srtInPass" value="${inp.srtPass||''}"/>
        </div>
        <div class="form-row">
          <label>Stream ID:</label>
          <input type="text" id="srtInStreamId" value="${inp.srtStreamId||''}"/>
        </div>
        <div class="form-row">
          <label>PB Key Length:</label>
          <select id="srtInPbkeylen">
            <option value="0"  ${String(inp.srtPbkeylen||0)==='0'?'selected':''}>0 (None)</option>
            <option value="16" ${String(inp.srtPbkeylen||0)==='16'?'selected':''}>16</option>
            <option value="24" ${String(inp.srtPbkeylen||0)==='24'?'selected':''}>24</option>
            <option value="32" ${String(inp.srtPbkeylen||0)==='32'?'selected':''}>32</option>
          </select>
        </div>
        <div class="form-row">
          <label>Network Interface:</label>
          <select id="srtInIface">${interfaceOptions(inp.rtpInterface||'')}</select>
        </div>
        <p id="srtInModeHint" style="font-size:10px;margin-top:4px;color:var(--muted)"></p>
        <p style="font-size:10px;margin-top:4px;color:var(--muted)">
          Receiver-side latency controls SRT jitter buffering. Lower values reduce delay but can increase dropouts on unstable links.
          Valid range: ${SRT_INPUT_LATENCY_MIN_MS}-${SRT_INPUT_LATENCY_MAX_MS} ms.
        </p>
        <p style="font-size:10px;margin-top:4px;color:var(--muted)">
          SRT over RTP encapsulates audio as RTP inside an SRT stream (useful for low-latency studio links).
          Raw MPEG-TS wraps audio/video as an MPEG transport stream.
        </p>`;

        const modeEl = document.getElementById('srtInMode');
        const hostEl = document.getElementById('srtInHost');
        const hintEl = document.getElementById('srtInModeHint');
        const applySrtModeUi = () => {
          const mode = (modeEl && modeEl.value === 'listener') ? 'listener' : 'caller';
          if (mode === 'listener') {
            if (hostEl) hostEl.disabled = true;
            if (hintEl) hintEl.textContent = 'Listener mode binds local port and waits for incoming SRT sender. Host is ignored.';
          } else {
            if (hostEl) hostEl.disabled = false;
            if (hintEl) hintEl.textContent = 'Caller mode connects outbound to Host:Port.';
          }
        };
        if (modeEl) modeEl.addEventListener('change', applySrtModeUi);
        applySrtModeUi();
    }
}

function gatherInputConfig(type) {
    const v = id => { const el = document.getElementById(id); return el ? el.value : ''; };
    const base = {
        inputType: type,
        bitrate:    parseInt(v('bitrateSelect')) || 128000,
        sampleRate: parseInt(v('sampleRateSelect')) || 48000,
        rtpGain:    parseFloat(v('gainBox')) || 0,
    };
    if (type === 'axia') {
        const ch = parseInt(v('axiaChannel'));
        return { ...base, axiaChannel: ch, rtpAddress: channelToMulticast(ch), rtpPort: 5004, rtpInterface: v('rtpInterface') };
    }
    if (type === 'rtp')
        return { ...base, rtpAddress: v('rtpAddress'), rtpPort: parseInt(v('rtpPort')), rtpInterface: v('rtpInterface') };
    if (type === 'srt')
      return {
        ...base,
        srtMode: v('srtInMode') || 'caller',
        srtTransport: v('srtInTransport'),
        srtHost: v('srtInHost'),
        srtPort: parseInt(v('srtInPort')),
        srtLatency: parseInt(v('srtInLatency')),
        srtPass: v('srtInPass'),
        srtStreamId: v('srtInStreamId'),
        srtPbkeylen: parseInt(v('srtInPbkeylen')) || 0,
        rtpInterface: v('srtInIface')
      };
    if (type === 'audio')
      return { ...base, audioDevice: v('audioDevice') };
    return base;
}

function validateInputConfigPayload(payload) {
  if (!payload || payload.inputType !== 'srt') return null;
  const latency = Number(payload.srtLatency);
  if (!Number.isFinite(latency)) {
    return `SRT input delay must be a number (${SRT_INPUT_LATENCY_MIN_MS}-${SRT_INPUT_LATENCY_MAX_MS} ms).`;
  }
  if (latency < SRT_INPUT_LATENCY_MIN_MS || latency > SRT_INPUT_LATENCY_MAX_MS) {
    return `SRT input delay must be ${SRT_INPUT_LATENCY_MIN_MS}-${SRT_INPUT_LATENCY_MAX_MS} ms. Got ${latency}.`;
  }
  return null;
}

function dbToPercent(db) {
  const clamped = Math.max(-60, Math.min(12, db));
  return ((clamped + 60) / 72) * 100;
}

function formatShortTime(ts) {
  if (!(ts instanceof Date) || Number.isNaN(ts.getTime())) return '';
  const hh = String(ts.getHours()).padStart(2, '0');
  const mm = String(ts.getMinutes()).padStart(2, '0');
  const ss = String(ts.getSeconds()).padStart(2, '0');
  return `${hh}:${mm}:${ss}`;
}

function updateVUFromLevels(levels) {
  vuLastRaw = levels || { connected: false, leftDb: -60, rightDb: -60 };
  inputConnectedState = !!(vuLastRaw && vuLastRaw.connected);
  const L = document.getElementById('vuL');
  const R = document.getElementById('vuR');
  if (!L || !R) return;
  if (!vuLastRaw.connected) {
    L.style.width = '0%';
    R.style.width = '0%';
    return;
  }

  L.style.width = `${dbToPercent(vuLastRaw.leftDb ?? -60)}%`;
  R.style.width = `${dbToPercent(vuLastRaw.rightDb ?? -60)}%`;
}

function updateInputVuTitle(levels) {
  const el = document.getElementById('inputVuTitle');
  if (!el) return;
  if (!levels || !levels.connected) {
    el.textContent = 'Input (Disconnected)';
    return;
  }
  const source = (levels.sourceLabel || '').trim();
  if (source) {
    el.textContent = `Input: ${source}`;
    return;
  }
  const type = (levels.inputType || '').trim();
  el.textContent = type ? `Input: ${type}` : 'Input (Connected)';
}

function updateInputWarning(levels) {
  const el = document.getElementById('inputWarningLine');
  if (!el) return;
  const warning = (levels && levels.warning) ? String(levels.warning).trim() : '';
  if (!warning) {
    inputWarningLastText = '';
    inputWarningSince = null;
    el.style.display = 'none';
    el.textContent = '';
    return;
  }
  if (warning !== inputWarningLastText || !inputWarningSince) {
    inputWarningLastText = warning;
    inputWarningSince = new Date();
  }
  const t = formatShortTime(inputWarningSince);
  el.style.display = '';
  el.textContent = t ? `[${t}] ${warning}` : warning;
}

async function refreshInputLevels() {
  if (!selectedEncoder || selectedSection !== 'input') return;
  try {
    const levels = await apiGet(`/api/encoder/${selectedEncoder}/input/levels`);
    updateVUFromLevels(levels);
    updateInputVuTitle(levels);
    updateInputWarning(levels);
  } catch {
    updateVUFromLevels({ connected: false, leftDb: -60, rightDb: -60 });
    updateInputVuTitle({ connected: false });
    updateInputWarning({ connected: false, warning: 'Input telemetry unavailable' });
  }
}

function scheduleGainPreviewUpdate() {
  if (!selectedEncoder) return;
  if (gainPreviewTimer) clearTimeout(gainPreviewTimer);
  gainPreviewTimer = setTimeout(async () => {
    try {
      const gain = parseFloat((document.getElementById('gainBox') || {}).value || '0') || 0;
      await apiPost(`/api/encoder/${selectedEncoder}/input/preview-gain`, { gainDb: gain });
    } catch {
    }
  }, 120);
}

async function applyGainNow() {
  if (!selectedEncoder) return;
  try {
    const gain = parseFloat((document.getElementById('gainBox') || {}).value || '0') || 0;
    await apiPost(`/api/encoder/${selectedEncoder}/input/apply-gain`, { gainDb: gain });
  } catch {
  }
}

async function refreshInputStatusLine() {
  if (!selectedEncoder || selectedSection !== 'input') return;
  const el = document.getElementById('inputStatusLine');
  if (!el) return;
  try {
    const s = await apiGet(`/api/encoder/${selectedEncoder}/input/status`);
    el.innerHTML = `<strong>Active session input (temporary):</strong> ${escapeHtml(s.activeSessionInput || '')}<br><strong>Saved profile input:</strong> ${escapeHtml(s.savedProfileInput || '')}`;
  } catch {
    el.textContent = 'Input status unavailable';
  }
}

// ===========================================================
// CONTROL SECTION
// ===========================================================
async function renderControlSection(cl, cr) {
    const cfg = await loadEncoderConfig(selectedEncoder);
    const ctl = cfg.control || {};
    const port    = ctl.controlPort    || (9100 + (selectedEncoder - 1) * 10);
    const cmds    = ctl.commands || {};

    cl.innerHTML = `
    <h2>Control Configuration</h2>
    <div class="form-row">
      <label>Control Port:</label>
      <input type="number" id="ctlPort" value="${port}"/>
    </div>

    <h3 style="margin-top:14px">Stream Commands</h3>
    ${renderCmdRow('Start AAC Encoding',  'startAAC',  cmds.startAAC  || 'StartAAC')}
    ${renderCmdRow('Stop AAC Encoding',   'stopAAC',   cmds.stopAAC   || 'StopAAC')}
    ${renderCmdRow('Start MP3 Encoding',  'startMP3',  cmds.startMP3  || 'StartMP3')}
    ${renderCmdRow('Stop MP3 Encoding',   'stopMP3',   cmds.stopMP3   || 'StopMP3')}
    ${renderCmdRow('Start HLS Encoding',  'startHLS',  cmds.startHLS  || 'StartHLS')}
    ${renderCmdRow('Stop HLS Encoding',   'stopHLS',   cmds.stopHLS   || 'StopHLS')}
    ${renderCmdRow('Start SRT Encoding',  'startSRT',  cmds.startSRT  || 'StartSRT')}
    ${renderCmdRow('Stop SRT Encoding',   'stopSRT',   cmds.stopSRT   || 'StopSRT')}

    <div class="action-row">
      <button class="btn btn-success" id="ctlStartBtn">Start Listening</button>
      <button class="btn btn-danger"  id="ctlStopBtn">Stop Listening</button>
      <button class="btn"             id="ctlSaveBtn">Save</button>
    </div>`;

    document.getElementById('ctlSaveBtn').addEventListener('click', async () => {
        try {
            const data = {
                controlPort: parseInt(document.getElementById('ctlPort').value),
                commands: {
                    startAAC: cmdVal('startAAC'), stopAAC: cmdVal('stopAAC'),
                    startMP3: cmdVal('startMP3'), stopMP3: cmdVal('stopMP3'),
                    startHLS: cmdVal('startHLS'), stopHLS: cmdVal('stopHLS'),
                    startSRT: cmdVal('startSRT'), stopSRT: cmdVal('stopSRT'),
                }
            };
            await apiPost(`/api/encoder/${selectedEncoder}/config/control`, JSON.stringify(data));
            showBanner('Control config saved', 'ok');
        } catch (e) {
            showBanner('Control config save failed: ' + e.message, 'error');
        }
    });
    document.getElementById('ctlStartBtn').addEventListener('click', async () => {
        await apiPost(`/api/encoder/${selectedEncoder}/control/start`, {});
        showBanner('Control listener started');
    });
    document.getElementById('ctlStopBtn').addEventListener('click', async () => {
        await apiPost(`/api/encoder/${selectedEncoder}/control/stop`, {});
        showBanner('Control listener stopped');
    });
}

function renderCmdRow(label, id, value) {
    return `<div class="form-row"><label>${label}:</label><input type="text" id="cmd_${id}" value="${value}" style="width:120px"/></div>`;
}
function cmdVal(id) { const el = document.getElementById('cmd_' + id); return el ? el.value : ''; }

// ===========================================================
// METADATA SECTION
// ===========================================================
async function renderMetaSection(cl, cr) {
    const cfg  = await loadEncoderConfig(selectedEncoder);
    const meta = cfg.metadata || {};
    const mode = (meta.mode || 'listen').toLowerCase() === 'pull' ? 'pull' : 'listen';
    const lp   = meta.listenPort || (9000 + (selectedEncoder-1) * 10);
    const dh   = meta.dataConnectHost || '';
    const dp   = meta.dataConnectPort || '';
    const scte = meta.scte || {};
    const scteRows = Array.isArray(scte.commandRows) ? scte.commandRows : [];
    const watchTags = Array.isArray(scte.watchTags) && scte.watchTags.length
      ? scte.watchTags.join(', ')
      : 'SCTE, Event_ID, Event_Duration';
    const whitelistEntries = Array.isArray(scte.whitelistEntries) ? scte.whitelistEntries.join(', ') : '';
    const cueListenPort = Number.isFinite(scte.listenPort) ? scte.listenPort : (9040 + selectedEncoder);

    cl.innerHTML = `
    <h2>Metadata</h2>

    <div class="form-row">
      <label>Metadata Input Mode:</label>
      <div style="display:flex;gap:16px;align-items:center;">
        <label style="font-weight:400;"><input type="radio" name="metaMode" value="listen" ${mode === 'listen' ? 'checked' : ''}/> Listen (automation pushes to encoder)</label>
        <label style="font-weight:400;"><input type="radio" name="metaMode" value="pull" ${mode === 'pull' ? 'checked' : ''}/> Pull (encoder connects to automation)</label>
      </div>
    </div>

    <div class="form-row" id="metaListenRow">
      <label>Input Listen Port:</label>
      <input type="number" id="metaListenPort" value="${lp}"/>
    </div>

    <div class="form-row" id="metaPullHostRow" style="margin-top:12px">
      <label>Data Connect Host/IP:</label>
      <input type="text" id="metaConnectHost" value="${dh}" placeholder="automation.example.local or 10.0.0.25"/>
    </div>
    <div class="form-row" id="metaPullPortRow">
      <label>Data Connect Port:</label>
      <input type="number" id="metaConnectPort" value="${dp}"/>
    </div>

    <div class="action-row">
      <button class="btn btn-success" id="metaStartBtn">Start Metadata</button>
      <button class="btn btn-danger"  id="metaStopBtn">Stop Metadata</button>
      <button class="btn btn-primary" id="metaConnectBtn">Test Connect</button>
    </div>
    <div class="action-row" style="margin-top:12px">
      <button class="btn" id="metaSaveBtn">Save</button>
    </div>

    <div id="metaPayloadStatus" style="margin-top:10px;font-size:11px;color:var(--muted);"></div>`;

    cr.innerHTML = renderMetaScteConfigPane(scte, cueListenPort, watchTags, whitelistEntries, scteRows);

    const selectedMode = () => {
      const el = document.querySelector('input[name="metaMode"]:checked');
      return el ? el.value : 'listen';
    };

    const buildMetaPayload = () => ({
      mode: selectedMode(),
      listenPort: parseInt(document.getElementById('metaListenPort').value) || null,
      dataConnectHost: (document.getElementById('metaConnectHost').value || '').trim(),
      dataConnectPort: parseInt(document.getElementById('metaConnectPort').value) || null,
      scte: {
        enabled: document.getElementById('scteEnabled').value === 'true',
        listenEnabled: document.getElementById('scteListenEnabled').value === 'true',
        listenTransport: document.getElementById('scteListenTransport').value || 'http',
        listenPort: parseInt(document.getElementById('scteListenPort').value) || (9040 + selectedEncoder),
        cueDeliveryType: document.getElementById('scteDeliveryType').value || 'json',
        passthroughMode: document.getElementById('sctePassthroughMode').value || 'pass-through',
        requireEventId: document.getElementById('scteRequireEventId').value === 'true',
        requireToken: document.getElementById('scteRequireToken').value === 'true',
        token: (document.getElementById('scteToken').value || '').trim(),
        watchTags: listFromCsv(document.getElementById('scteWatchTags').value),
        whitelistEnabled: document.getElementById('scteWhitelistEnabled').value === 'true',
        whitelistEntries: listFromCsv(document.getElementById('scteWhitelistEntries').value),
        commandRows: Array.from(document.querySelectorAll('#scteCommandRows .scte-cmd-row')).map(row => ({
          match: (row.querySelector('.scteMatch') || {}).value || '',
          action: (row.querySelector('.scteAction') || {}).value || '',
        })).map(r => ({ match: (r.match || '').trim(), action: (r.action || '').trim() })).filter(r => r.match.length > 0),
        overrideRateLimit: document.getElementById('scteOverrideRateLimit').value === 'true',
        rateLimitCount: parseInt(document.getElementById('scteRateLimitCount').value) || 5,
        rateLimitWindowSec: parseInt(document.getElementById('scteRateLimitWindowSec').value) || 10,
        overrideDedupe: document.getElementById('scteOverrideDedupe').value === 'true',
        dedupeSeconds: parseInt(document.getElementById('scteDedupeSeconds').value) || 30,
      },
    });

    const applyMetaModeUi = () => {
      const modeValue = selectedMode();
      const listenEnabled = modeValue === 'listen';
      const pullEnabled = modeValue === 'pull';

      const listenRow = document.getElementById('metaListenRow');
      const pullHostRow = document.getElementById('metaPullHostRow');
      const pullPortRow = document.getElementById('metaPullPortRow');
      const listenInput = document.getElementById('metaListenPort');
      const pullHostInput = document.getElementById('metaConnectHost');
      const pullPortInput = document.getElementById('metaConnectPort');
      const connectBtn = document.getElementById('metaConnectBtn');

      if (listenInput) listenInput.disabled = !listenEnabled;
      if (pullHostInput) pullHostInput.disabled = !pullEnabled;
      if (pullPortInput) pullPortInput.disabled = !pullEnabled;
      if (connectBtn) connectBtn.disabled = !pullEnabled;

      if (listenRow) listenRow.style.opacity = listenEnabled ? '1' : '0.45';
      if (pullHostRow) pullHostRow.style.opacity = pullEnabled ? '1' : '0.45';
      if (pullPortRow) pullPortRow.style.opacity = pullEnabled ? '1' : '0.45';

      const scteEnabled = document.getElementById('scteListenEnabled') && document.getElementById('scteListenEnabled').value === 'true';
      document.querySelectorAll('.scte-dependent').forEach(el => {
        el.style.opacity = scteEnabled ? '1' : '0.45';
        el.querySelectorAll('input,select,button,textarea').forEach(ctrl => { ctrl.disabled = !scteEnabled; });
      });
    };

    document.querySelectorAll('input[name="metaMode"]').forEach((el) => {
      el.addEventListener('change', applyMetaModeUi);
    });
    const scteListenEnabled = document.getElementById('scteListenEnabled');
    if (scteListenEnabled) scteListenEnabled.addEventListener('change', applyMetaModeUi);

    const cmdRows = document.getElementById('scteCommandRows');
    const bindRowButtons = () => {
      cmdRows.querySelectorAll('.scteRowRemove').forEach(btn => {
        btn.onclick = () => {
          const allRows = Array.from(cmdRows.querySelectorAll('.scte-cmd-row'));
          if (allRows.length <= 1) return;
          const row = btn.closest('.scte-cmd-row');
          if (row) row.remove();
        };
      });
    };
    bindRowButtons();
    const addRowBtn = document.getElementById('scteAddRowBtn');
    if (addRowBtn) {
      addRowBtn.addEventListener('click', () => {
        const wrap = document.createElement('div');
        wrap.innerHTML = renderCommandRows([{ match: '', action: '' }]);
        const row = wrap.firstElementChild;
        if (row) cmdRows.appendChild(row);
        bindRowButtons();
      });
    }

    applyMetaModeUi();

    document.getElementById('metaStartBtn').addEventListener('click', async () => {
      const res = await apiPost(`/api/encoder/${selectedEncoder}/metadata/start`, buildMetaPayload());
      if (res && res.ok) showBanner(`Metadata started in ${res.mode || selectedMode()} mode`, 'ok');
      else showBanner('Metadata start failed', 'error');
    });

    document.getElementById('metaStopBtn').addEventListener('click', async () => {
      await apiPost(`/api/encoder/${selectedEncoder}/metadata/stop`, buildMetaPayload());
      showBanner('Metadata stopped');
    });

    document.getElementById('metaConnectBtn').addEventListener('click', async () => {
      const host = (document.getElementById('metaConnectHost').value || '').trim();
      const port = parseInt(document.getElementById('metaConnectPort').value) || 0;
      const res = await apiPost(`/api/encoder/${selectedEncoder}/metadata/connect`, { host, port });
      if (res && res.ok) showBanner(`Metadata test connect ${res.status || 'ok'}`, 'ok');
      else showBanner('Metadata test connect failed', 'error');
    });

    document.getElementById('metaSaveBtn').addEventListener('click', async () => {
        try {
            await apiPost(`/api/encoder/${selectedEncoder}/config/metadata`, JSON.stringify(buildMetaPayload()));
            showBanner('Metadata config saved', 'ok');
        } catch (e) {
            showBanner('Metadata config save failed: ' + e.message, 'error');
        }
    });

    await refreshMetadataPayloadStatus();
    metadataStatusTimer = setInterval(refreshMetadataPayloadStatus, 1500);
}

// ---- Panel 4: Metadata Viewer ----
function startMetaPanelPoller() {
  if (metaPanelTimer) clearInterval(metaPanelTimer);
  metaPanelTimer = setInterval(refreshMetaPanel, 2000);
  refreshMetaPanel();
}

async function refreshMetaPanel() {
  if (!selectedEncoder) return;
  if (!META_VIEW_SECTIONS.includes(selectedSection)) return;
  const streamLabel = selectedSection === 'metadata' ? 'METADATA INPUT' : selectedSection.toUpperCase();
  const titleEl = document.getElementById('metaPanelTitle');
  if (titleEl) titleEl.textContent = `Encoder ${selectedEncoder} \u2013 ${streamLabel} Metadata Viewer`;
  const incoming = document.getElementById('metaIncoming');
  const sent = document.getElementById('metaSent');
  const scteBox = document.getElementById('metaScte');
  const scteTitle = document.getElementById('metaScteTitle');
  const prev = document.getElementById('prevEvents');
  if (!incoming || !sent || !scteBox || !prev) return;
  // Map section name to the metadata_runtime key for this stream.
  const streamFormattedKey = {
    aac: 'lastFormattedAAC',
    mp3: 'lastFormattedMP3',
    hls: 'lastFormattedHLS',
    srt: 'lastFormattedSRT',
  }[selectedSection];
  try {
    const s = await apiGet(`/api/encoder/${selectedEncoder}/metadata/status`);
    if (scteTitle) {
      scteTitle.textContent = (selectedSection === 'hls' || selectedSection === 'srt')
        ? 'SCTE-25 Commands'
        : 'SCTE-35 Commands';
    }
    incoming.textContent = s.lastRawXml || (s.eventCount ? '(raw payload not yet cached)' : 'Waiting for data...');
    // Data Sent: strictly selected stream (or metadata-mode details) — no cross-stream fallback.
    const streamPayload = (streamFormattedKey && s[streamFormattedKey]) || '\u2014';
    const scteRx = s.lastScteReceived || '\u2014';
    const scteTx = s.lastScteSent || '\u2014';
    const scteRej = s.lastScteRejected || '\u2014';
    if (selectedSection === 'metadata') {
      const mode = (s.mode || 'listen').toLowerCase();
      const endpoint = mode === 'pull'
        ? `${s.dataConnectHost || '(unset)'}:${s.dataConnectPort || '(unset)'}`
        : `${s.listenPort || '(unset)'}`;
      sent.textContent = `Mode: ${mode}\nEndpoint: ${endpoint}\nCue Endpoint: ${s.cueEndpoint || `/api/encoder/${selectedEncoder}/cue`}`;
      scteBox.textContent = `Received: ${scteRx}\nSent: ${scteTx}\nRejected: ${scteRej}`;
    } else {
      sent.textContent = streamPayload;
      scteBox.textContent = `Received: ${scteRx}\nSent: ${scteTx}\nRejected: ${scteRej}`;
    }
    // Previous Events: build per-stream history when a new event arrives.
    const newCount = Number.isFinite(s.eventCount) ? s.eventCount : 0;
    if (newCount > metaLastEventCount && newCount > 0) {
      const ts = s.lastPayloadUtc || new Date().toISOString();
      const raw = (s.lastRawXml && s.lastRawXml.length) ? s.lastRawXml : null;
      if (raw !== null) {
        metaStreamHistory.metadata.unshift({ ts, text: raw });
        if (metaStreamHistory.metadata.length > 10) metaStreamHistory.metadata.pop();
      }
      // Record the latest formatted text in each stream's history list.
      STREAM_SECTIONS.forEach(st => {
        const key = { aac: 'lastFormattedAAC', mp3: 'lastFormattedMP3', hls: 'lastFormattedHLS', srt: 'lastFormattedSRT' }[st];
        const text = (s[key] && s[key].length) ? s[key] : null;
        if (text !== null) {
          metaStreamHistory[st].unshift({ ts, text });
          if (metaStreamHistory[st].length > 10) metaStreamHistory[st].pop();
        }
      });
      metaLastEventCount = newCount;
    }
    // Re-render previous events list for the selected stream.
    const history = metaStreamHistory[selectedSection] || [];
    prev.innerHTML = '';
    history.forEach(entry => {
      const li = document.createElement('li');
      li.textContent = `[${entry.ts}] ${entry.text}`;
      prev.appendChild(li);
    });
    if (!history.length) {
      const li = document.createElement('li');
      li.style.color = 'var(--muted)';
      li.textContent = 'No events yet for this stream.';
      prev.appendChild(li);
    }
  } catch {
    incoming.textContent = 'Metadata status unavailable';
    sent.textContent = '\u2014';
    scteBox.textContent = '\u2014';
  }
}

async function refreshMetadataPayloadStatus() {
  if (!selectedEncoder || selectedSection !== 'metadata') return;
  const el = document.getElementById('metaPayloadStatus');
  if (!el) return;
  try {
    const s = await apiGet(`/api/encoder/${selectedEncoder}/metadata/status`);
    const last = s.lastPayloadUtc ? s.lastPayloadUtc : 'never';
    const count = Number.isFinite(s.eventCount) ? s.eventCount : 0;
    const listener = s.listenerRunning ? 'running' : 'stopped';
    const mode = (s.mode || 'listen').toLowerCase();
    const endpoint = mode === 'pull'
      ? `${s.dataConnectHost || '(unset)'}:${s.dataConnectPort || '(unset)'}`
      : `${s.listenPort || '(unset)'}`;
    const cueEndpoint = s.cueEndpoint || `/api/encoder/${selectedEncoder}/cue`;
    el.innerHTML = `<strong>Metadata state:</strong> ${listener}<br><strong>Metadata mode:</strong> ${escapeHtml(mode)}<br><strong>Mode endpoint:</strong> ${escapeHtml(endpoint)}<br><strong>Cue endpoint:</strong> ${escapeHtml(cueEndpoint)}<br><strong>Metadata events received:</strong> ${count}<br><strong>Last metadata payload received at:</strong> ${escapeHtml(last)}`;
  } catch {
    el.textContent = 'Metadata payload status unavailable';
  }
}

// ===========================================================
// ICECAST (AAC / MP3) SECTION
// ===========================================================
async function renderIcecastSection(type, cl, cr) {
  await ensureInterfacesLoaded(false);
    const upp = type.toUpperCase();
    const cfg = await loadEncoderConfig(selectedEncoder);
    const ice = cfg[type] || {};
    let adminCfg = {};
    try { adminCfg = await apiGet(`/api/encoder/${selectedEncoder}/admin-config`); } catch {}

    const url      = ice.url || (type === 'aac' ? 'http://localhost:8000/stream-aac' : 'http://localhost:8000/stream-mp3');
    const user     = ice.user     || 'source';
    const pass     = ice.pass     || 'hackme';
    const metaInt  = ice.icyMetaInt || 8192;
    const stationId= ice.stationId  || `Encoder-${selectedEncoder}`;
    const metaEnabled = ice.metaEnabled !== false;
    const iface    = ice.iface || '';
    const bitrate  = parseInt(ice.bitrate) > 0 ? parseInt(ice.bitrate) : 128000;
    const sampleRate = parseInt(ice.sampleRate) > 0 ? parseInt(ice.sampleRate) : 48000;
    const aacMode = (ice.mode || 'legacy').toLowerCase() === 'advanced' ? 'advanced' : 'legacy';
    const aacProfile = (ice.profile || 'aac_low');
    const mp3ModeRaw = (ice.mode || 'legacy').toLowerCase();
    const mp3Mode = (mp3ModeRaw === 'cbr' || mp3ModeRaw === 'vbr') ? mp3ModeRaw : 'legacy';
    const mp3VbrQuality = Math.max(0, Math.min(9, parseInt(ice.vbrQuality) || 4));
    const bitrateOptions = type === 'aac'
      ? [64000, 96000, 128000, 160000, 192000, 256000, 320000]
      : [96000, 128000, 160000, 192000, 256000, 320000];
    const sampleRateOptions = [32000, 44100, 48000];

    const enc = encoders.find(e => e.id === selectedEncoder) || {};
    const isRunning = type === 'aac' ? enc.aac : enc.mp3;

    cl.innerHTML = `
    <h2>${upp} Icecast Configuration</h2>
    <div class="form-row">
      <label>Icecast ${upp} URL:</label>
      <input type="text" id="iceUrl" value="${url}"/>
    </div>
    <div class="form-row">
      <label>User Name:</label>
      <input type="text" id="iceUser" value="${user}"/>
    </div>
    <div class="form-row">
      <label>Password:</label>
      <input type="password" id="icePass" value="${pass}"/>
    </div>
    <div class="form-row">
      <label>IcyMetaInt Port:</label>
      <input type="number" id="iceMetaInt" value="${metaInt}"/>
    </div>
    <div class="form-row">
      <label>Codec Bitrate:</label>
      <select id="iceBitrate">
        ${bitrateOptions.map(b => `<option value="${b}" ${b===bitrate?'selected':''}>${b/1000}k</option>`).join('')}
      </select>
    </div>
    <div class="form-row">
      <label>Codec Sample Rate:</label>
      <select id="iceSampleRate" ${((type === 'aac' && aacMode === 'legacy') || (type === 'mp3' && mp3Mode === 'legacy')) ? 'disabled' : ''}>
        ${sampleRateOptions.map(sr => `<option value="${sr}" ${sr===sampleRate?'selected':''}>${sr} Hz</option>`).join('')}
      </select>
    </div>
    ${type === 'aac' ? `
    <div class="form-row">
      <label>AAC Mode:</label>
      <select id="iceAacMode">
        <option value="legacy" ${aacMode==='legacy'?'selected':''}>Legacy (backward compatible)</option>
        <option value="advanced" ${aacMode==='advanced'?'selected':''}>Advanced</option>
      </select>
    </div>
    <div class="form-row">
      <label>AAC Profile:</label>
      <select id="iceAacProfile" ${aacMode === 'legacy' ? 'disabled' : ''}>
        <option value="aac_low" ${aacProfile==='aac_low'?'selected':''}>AAC-LC (aac_low)</option>
        <option value="aac_he" ${aacProfile==='aac_he'?'selected':''}>HE-AAC (aac_he)</option>
        <option value="aac_he_v2" ${aacProfile==='aac_he_v2'?'selected':''}>HE-AACv2 (aac_he_v2)</option>
      </select>
    </div>
    ` : ''}
    ${type === 'mp3' ? `
    <div class="form-row">
      <label>MP3 Mode:</label>
      <select id="iceMp3Mode">
        <option value="legacy" ${mp3Mode==='legacy'?'selected':''}>Legacy (backward compatible)</option>
        <option value="cbr" ${mp3Mode==='cbr'?'selected':''}>CBR</option>
        <option value="vbr" ${mp3Mode==='vbr'?'selected':''}>VBR</option>
      </select>
    </div>
    <div class="form-row" id="iceMp3VbrRow" style="${mp3Mode === 'vbr' ? '' : 'display:none'}">
      <label>VBR Quality (0 best, 9 smallest):</label>
      <select id="iceMp3VbrQuality">
        ${[0,1,2,3,4,5,6,7,8,9].map(q => `<option value="${q}" ${q===mp3VbrQuality?'selected':''}>${q}</option>`).join('')}
      </select>
    </div>
    ` : ''}
    <div class="form-row">
      <label>Station ID:</label>
      <input type="text" id="iceStationId" value="${stationId}"/>
    </div>
    <div class="form-row">
      <label>Send Embedded Metadata:</label>
      <select id="iceMetaEnabled">
        <option value="true"  ${metaEnabled ?'selected':''}>YES</option>
        <option value="false" ${!metaEnabled?'selected':''}>NO</option>
      </select>
    </div>
    <div class="form-row">
      <label>Network Interface:</label>
      <select id="iceIface">${interfaceOptions(iface)}</select>
    </div>
    <div style="margin-top:12px">
      <a href="#" id="editMetaParserLink">Edit Metadata Parser</a>
    </div>
    <div class="form-row" style="margin-top:12px">
      <label>Listen Link:</label>
      ${(type === 'aac' && adminCfg.aacListenLink)
        ? `<a href="${escapeHtml(adminCfg.aacListenLink)}" target="_blank" style="font-size:11px">${escapeHtml(adminCfg.aacListenLink)}</a>`
        : (type === 'mp3' && adminCfg.mp3ListenLink)
          ? `<a href="${escapeHtml(adminCfg.mp3ListenLink)}" target="_blank" style="font-size:11px">${escapeHtml(adminCfg.mp3ListenLink)}</a>`
          : `<span style="font-size:11px;color:var(--muted)">Not configured — set in Admin &rsaquo; Encoder Configuration</span>`}
    </div>
    <div class="action-row" style="margin-top:20px">
      <button class="btn btn-success" id="iceStartBtn">${isRunning ? '⬤ Running' : 'Start Encoder'}</button>
      <button class="btn btn-danger"  id="iceStopBtn">Stop Encoder</button>
      <button class="btn"             id="iceSaveBtn">Save</button>
    </div>`;

    document.getElementById('iceStartBtn').addEventListener('click', async () => {
      const res = await apiPost(`/api/encoder/${selectedEncoder}/${type}/start`, {});
      if (res && res.ok === false) showBanner(`${upp} encoder start failed: ${res.error || 'unknown error'}`, 'error');
      else showBanner(`${upp} encoder started`, 'ok');
        await refreshStatus();
    });
    document.getElementById('iceStopBtn').addEventListener('click', async () => {
      const res = await apiPost(`/api/encoder/${selectedEncoder}/${type}/stop`, {});
      if (res && res.ok === false) showBanner(`${upp} encoder stop failed: ${res.error || 'unknown error'}`, 'error');
      else showBanner(`${upp} encoder stopped`);
        await refreshStatus();
    });
    document.getElementById('iceSaveBtn').addEventListener('click', async () => {
        try {
            // Load the current saved config so we preserve any metaParser settings.
            const existing = await loadEncoderConfig(selectedEncoder);
            const existingSection = (existing && existing[type]) || {};
            const data = {
                ...existingSection,
                url: document.getElementById('iceUrl').value,
                user: document.getElementById('iceUser').value,
                pass: document.getElementById('icePass').value,
                icyMetaInt: parseInt(document.getElementById('iceMetaInt').value),
                bitrate: parseInt(document.getElementById('iceBitrate').value) || 128000,
                sampleRate: parseInt(document.getElementById('iceSampleRate').value) || 48000,
                stationId: document.getElementById('iceStationId').value,
                metaEnabled: document.getElementById('iceMetaEnabled').value === 'true',
                iface: document.getElementById('iceIface').value,
            };
              if (type === 'aac') {
                data.mode = document.getElementById('iceAacMode').value || 'legacy';
                data.profile = document.getElementById('iceAacProfile').value || 'aac_low';
              }
              if (type === 'mp3') {
                data.mode = document.getElementById('iceMp3Mode').value || 'legacy';
                data.vbrQuality = parseInt(document.getElementById('iceMp3VbrQuality').value) || 4;
              }
            await apiPost(`/api/encoder/${selectedEncoder}/config/${type}`, JSON.stringify(data));
            showBanner(`${upp} config saved`, 'ok');
        } catch (e) {
            showBanner(`${upp} config save failed: ` + e.message, 'error');
        }
    });
          if (type === 'aac') {
            const modeEl = document.getElementById('iceAacMode');
            const profileEl = document.getElementById('iceAacProfile');
            const sampleRateEl = document.getElementById('iceSampleRate');
            modeEl.addEventListener('change', () => {
              const advanced = modeEl.value === 'advanced';
              profileEl.disabled = !advanced;
              sampleRateEl.disabled = !advanced;
            });
          }
          if (type === 'mp3') {
            const modeEl = document.getElementById('iceMp3Mode');
            const vbrRowEl = document.getElementById('iceMp3VbrRow');
            const sampleRateEl = document.getElementById('iceSampleRate');
            modeEl.addEventListener('change', () => {
              vbrRowEl.style.display = modeEl.value === 'vbr' ? '' : 'none';
              sampleRateEl.disabled = modeEl.value === 'legacy';
            });
          }
    document.getElementById('editMetaParserLink').addEventListener('click', e => {
        e.preventDefault();
        renderMetaParserEditor(cr, type, cfg);
    });
}

function renderMetaParserEditor(container, type, cfg) {
    const m = (cfg[type] && cfg[type].metaParser) || {};
    const useTemplate = m.template ? true : false;
    container.innerHTML = `
    <h2>Metadata Parser Editor</h2>
    <div class="form-row">
      <label><input type="radio" name="parseMode" value="template" ${useTemplate?'checked':''}> Use Custom Template:</label>
    </div>
    <div class="form-row" id="templateRow" style="${useTemplate?'':'display:none'}">
      <input type="text" id="metaTemplate" value="${m.template||'artist={artist} | title={title}'}" style="width:100%"/>
    </div>
    <div class="form-row">
      <label><input type="radio" name="parseMode" value="legacy" ${!useTemplate?'checked':''}> Legacy Mode:</label>
    </div>
    <div id="legacyRows" style="${!useTemplate?'':'display:none'}">
      <div class="form-row"><label>Separator:</label>
        <input type="text" id="legacySep" value="${m.separator||' | '}" style="width:60px"/>
      </div>
      <div class="form-row"><label>Include Station ID:</label>
        <input type="checkbox" id="legacyIncludeSID" ${m.includeStationId!==false?'checked':''}/>
      </div>
      <div class="form-row"><label>Include Fields:</label>
        <input type="text" id="legacyFields" value="${(m.fields||['title','artist']).join(', ')}"/>
      </div>
    </div>
    <p style="font-size:10px;color:var(--muted);margin-top:8px;line-height:1.45">
      <strong>Custom Template:</strong> Build the outgoing metadata text exactly how you want it to appear.
      Available placeholders are <code>{title}</code>, <code>{artist}</code>, <code>{duration}</code>, and <code>{stationId}</code>.
      Any plain text you type around those placeholders is sent exactly as written.<br/>
      <strong>Example 1:</strong> <code>{artist} - {title}</code><br/>
      <strong>Example 2:</strong> <code>Now Playing: {title} | Artist: {artist} | {stationId}</code><br/>
      <strong>Example 3:</strong> <code>{stationId}: {artist}~{title}</code><br/>
      <strong>Legacy Mode:</strong> Builds the output from the selected field list in order, joined by the separator, with optional station ID appended.
      Example: fields <code>title, artist</code> with separator <code> | </code> becomes <code>Song Title | Artist Name</code>.
    </p>
    <div class="action-row" style="margin-top:10px">
      <button class="btn btn-primary" id="metaParserSaveBtn">Save</button>
    </div>`;

    document.querySelectorAll('input[name=parseMode]').forEach(r => {
        r.addEventListener('change', () => {
            const tpl = document.querySelector('input[name=parseMode]:checked').value === 'template';
            document.getElementById('templateRow').style.display = tpl ? '' : 'none';
            document.getElementById('legacyRows').style.display  = tpl ? 'none' : '';
        });
    });
    document.getElementById('metaParserSaveBtn').addEventListener('click', async () => {
        try {
            const mode = document.querySelector('input[name=parseMode]:checked').value;
            let parser = {};
            if (mode === 'template') {
                parser = { template: document.getElementById('metaTemplate').value };
            } else {
                parser = {
                    separator: document.getElementById('legacySep').value,
                    includeStationId: document.getElementById('legacyIncludeSID').checked,
                    fields: document.getElementById('legacyFields').value.split(',').map(s=>s.trim()).filter(Boolean),
                };
            }
            // Merge into existing config
            const current = await loadEncoderConfig(selectedEncoder);
            const section = current[type] || {};
            section.metaParser = parser;
            await apiPost(`/api/encoder/${selectedEncoder}/config/${type}`, JSON.stringify(section));
            showBanner('Metadata parser saved', 'ok');
        } catch (e) {
            showBanner('Metadata parser save failed: ' + e.message, 'error');
        }
    });
}

// ===========================================================
// HLS SECTION
// ===========================================================
async function renderHLSSection(cl, cr) {
  await ensureInterfacesLoaded(false);
    const cfg = await loadEncoderConfig(selectedEncoder);
    const hls = cfg.hls || {};
    const enc = encoders.find(e => e.id === selectedEncoder) || {};
    let adminCfg = {};
    try { adminCfg = await apiGet(`/api/encoder/${selectedEncoder}/admin-config`); } catch {}

    cl.innerHTML = `
    <h2>HLS Configuration</h2>
    <div class="form-row">
      <label>Segment Length (seconds):</label>
      <input type="number" id="hlsSegSec" value="${hls.segmentSeconds||5}" min="1" max="30"/>
    </div>
    <div class="form-row">
      <label>Buffer Window (segments):</label>
      <input type="number" id="hlsWindow" value="${hls.window||5}" min="2" max="20"/>
    </div>
    <div class="form-row">
      <label>Data Offset Time (seconds):</label>
      <input type="number" id="hlsStartOffset" value="${hls.startTimeOffset||-25}"/>
    </div>
    <div class="form-row">
      <label>Low Latency Stream:</label>
      <input type="checkbox" id="hlsLowLat" ${hls.lowLatency?'checked':''}/>
    </div>
    <div class="form-row">
      <label>Embed Metadata:</label>
      <input type="checkbox" id="hlsMeta" ${hls.metaEnabled!==false?'checked':''}/>
    </div>
    <div class="form-row">
      <label>SCTE-35:</label>
      <select id="hlsScteEnabled">
        <option value="false" ${(hls.scteEnabled === true) ? '' : 'selected'}>OFF</option>
        <option value="true" ${(hls.scteEnabled === true) ? 'selected' : ''}>ON</option>
      </select>
    </div>
    <div class="form-row hls-scte-dependent">
      <label>SCTE Passthrough:</label>
      <select id="hlsSctePassthrough">
        <option value="off" ${(hls.sctePassthrough || '') === 'off' ? 'selected' : ''}>OFF</option>
        <option value="pass-through" ${(hls.sctePassthrough || 'pass-through') === 'pass-through' ? 'selected' : ''}>Pass-through</option>
        <option value="generate-from-cues" ${(hls.sctePassthrough || '') === 'generate-from-cues' ? 'selected' : ''}>Generate from Cues</option>
      </select>
    </div>
    <div class="form-row">
      <label>Network Interface:</label>
      <select id="hlsIface">${interfaceOptions(hls.iface||'')}</select>
    </div>
    <div style="margin-top:12px">
      <a href="#" id="hlsEditMetaLink">Edit Metadata Parser</a>
    </div>
    <div class="form-row" style="margin-top:12px">
      <label>Admin Listen Link:</label>
      ${adminCfg.hlsListenLink
        ? `<a href="${escapeHtml(adminCfg.hlsListenLink)}" target="_blank" style="font-size:11px">${escapeHtml(adminCfg.hlsListenLink)}</a>`
        : `<span style="font-size:11px;color:var(--muted)">Not configured — set in Admin &rsaquo; Encoder Configuration</span>`}
    </div>
    <div class="form-row">
      <label>Live Stream Link:</label>
      ${adminCfg.hlsPlaybackPort
        ? `<a href="http://${window.location.hostname}:${adminCfg.hlsPlaybackPort}/hls/index.m3u8" target="_blank" style="font-size:11px">http://${window.location.hostname}:${adminCfg.hlsPlaybackPort}/hls/index.m3u8</a>`
        : `<span style="font-size:11px;color:var(--muted)">No playback port set — configure HLS Playback Port in Admin &rsaquo; Encoder Configuration</span>`}
    </div>
    <div class="action-row" style="margin-top:16px">
      <button class="btn btn-success" id="hlsStartBtn">${enc.hls ? '⬤ Running' : 'Start HLS'}</button>
      <button class="btn btn-danger"  id="hlsStopBtn">Stop HLS</button>
      <button class="btn"             id="hlsSaveBtn">Save</button>
    </div>`;

    document.getElementById('hlsStartBtn').addEventListener('click', async () => {
      const res = await apiPost(`/api/encoder/${selectedEncoder}/hls/start`, {});
      if (res && res.ok === false) showBanner(`HLS start failed: ${res.error || 'unknown error'}`, 'error');
      else showBanner('HLS started', 'ok');
        await refreshStatus();
    });
    document.getElementById('hlsStopBtn').addEventListener('click', async () => {
      const res = await apiPost(`/api/encoder/${selectedEncoder}/hls/stop`, {});
      if (res && res.ok === false) showBanner(`HLS stop failed: ${res.error || 'unknown error'}`, 'error');
      else showBanner('HLS stopped');
        await refreshStatus();
    });
    document.getElementById('hlsSaveBtn').addEventListener('click', async () => {
        try {
            // Load existing config first to preserve metaParser and other settings.
            const existing = await loadEncoderConfig(selectedEncoder);
            const existingHls = (existing && existing.hls) || {};
            const data = {
                ...existingHls,
                segmentSeconds: parseInt(document.getElementById('hlsSegSec').value),
                window: parseInt(document.getElementById('hlsWindow').value),
                startTimeOffset: parseInt(document.getElementById('hlsStartOffset').value),
                lowLatency: document.getElementById('hlsLowLat').checked,
                metaEnabled: document.getElementById('hlsMeta').checked,
                scteEnabled: document.getElementById('hlsScteEnabled').value === 'true',
                sctePassthrough: document.getElementById('hlsSctePassthrough').value || 'pass-through',
                iface: document.getElementById('hlsIface').value,
            };
            await apiPost(`/api/encoder/${selectedEncoder}/config/hls`, JSON.stringify(data));
            showBanner('HLS config saved', 'ok');
        } catch (e) {
            showBanner('HLS config save failed: ' + e.message, 'error');
        }
    });
    const applyHlsScteUi = () => {
      const on = document.getElementById('hlsScteEnabled').value === 'true';
      document.querySelectorAll('.hls-scte-dependent').forEach(el => {
        el.style.opacity = on ? '1' : '0.45';
        el.querySelectorAll('input,select,button').forEach(ctrl => { ctrl.disabled = !on; });
      });
    };
    document.getElementById('hlsScteEnabled').addEventListener('change', applyHlsScteUi);
    applyHlsScteUi();
    document.getElementById('hlsEditMetaLink').addEventListener('click', e => {
        e.preventDefault();
        renderHLSMetaEditor(cr, cfg);
    });
}

function renderHLSMetaEditor(container, cfg) {
    const hls = cfg.hls || {};
    const mp  = hls.metaParser || {};
  const profileRaw = String(mp.profile || 'orban').toLowerCase();
  const profile = (profileRaw === 'triton' || profileRaw === 'universal') ? profileRaw : 'orban';
    const builtInTagOptions = [
      { value: 'sched_time', label: 'sched_time' },
      { value: 'stack_pos', label: 'stack_position' },
      { value: 'title', label: 'title' },
      { value: 'artist', label: 'artist' },
      { value: 'trivia', label: 'trivia' },
      { value: 'category', label: 'category' },
      { value: 'cart', label: 'cart' },
      { value: 'media_type', label: 'media_type' },
      { value: 'station', label: 'station' },
      { value: 'stationId', label: 'stationId (from stream config)' },
    ];
    const builtInValues = builtInTagOptions.map(t => t.value);
    const selectedTags = Array.isArray(mp.tags) && mp.tags.length
      ? mp.tags.map(t => String(t).trim()).filter(Boolean)
      : ['title', 'artist', 'category', 'duration'];
    const customTags = selectedTags.filter(t => !builtInValues.includes(t));
    container.innerHTML = `
    <h2>Metadata Parser Editor</h2>
    <div class="form-row">
      <label>HLS Metadata Profile:</label>
      <select id="hlsMetaProfile">
        <option value="orban" ${profile==='orban'?'selected':''}>Orban</option>
        <option value="triton" ${profile==='triton'?'selected':''}>Triton</option>
        <option value="universal" ${profile==='universal'?'selected':''}>Universal Standard</option>
      </select>
    </div>
    <p title="Orban preserves existing behavior. Triton targets Triton SST metadata expectations. Universal uses conservative audio-only HLS metadata conventions for broad client compatibility." style="font-size:10px;color:var(--muted);margin-top:-2px;margin-bottom:8px;line-height:1.4">
      Tip: Choose <strong>Orban</strong> to keep current behavior, <strong>Triton</strong> for Triton SST workflows, or <strong>Universal Standard</strong> for broad audio-only HLS compatibility.
    </p>
    <div class="form-row">
      <label>Parsing Method:</label>
      <select id="hlsParseMethod">
        <option value="xmlPassthrough" ${mp.method==='xmlPassthrough'?'selected':''}>XML Pass Through</option>
        <option value="id3"            ${mp.method==='id3'           ?'selected':''}>ID3 tags</option>
        <option value="id3v2"          ${mp.method==='id3v2'         ?'selected':''}>ID3v2 tags</option>
        <option value="ext"            ${mp.method==='ext'           ?'selected':''}>EXT Tags</option>
      </select>
    </div>
    <div class="form-row">
      <label>Embed Scope:</label>
      <select id="hlsEmbedScope">
        <option value="current"        ${mp.scope==='current'       ?'selected':''}>Current Only</option>
        <option value="currentFuture"  ${mp.scope==='currentFuture' ?'selected':''}>Current + Future</option>
      </select>
    </div>
    <div class="form-row" style="align-items:flex-start">
      <label>XML Tags To Use:</label>
      <div style="display:flex;flex-wrap:wrap;gap:10px;max-width:360px">
        ${builtInTagOptions.map(t =>
          `<label style="font-weight:400"><input type="checkbox" class="hlsTagChk" value="${t.value}" ${selectedTags.includes(t.value) ? 'checked' : ''}/> ${t.label}</label>`
        ).join('')}
      </div>
    </div>
    <div class="form-row">
      <label>Custom Tags:</label>
      <input type="text" id="hlsCustomTags" value="${customTags.join(', ')}" placeholder="e.g. album, media_type"/>
    </div>
    <p style="font-size:10px;color:var(--muted);margin-top:6px">
      <strong>Profiles:</strong> Orban preserves current Orban-compatible metadata payloads. Triton emits Triton-oriented EXT payloads and ID3/ID3v2 frame text. Universal Standard emits conservative standards-oriented values for broad HLS compatibility.<br/>
      XML Pass Through emits raw XML. ID3/ID3v2 emits frame text (TIT2/TPE1/etc). EXT emits payload for playlist EXTINF metadata.
      Scope controls whether only current item or current+future stack is emitted. stationId is read from stream config, not from XML.
    </p>
    <div class="action-row" style="margin-top:10px">
      <button class="btn btn-primary" id="hlsMetaSaveBtn">Save</button>
    </div>`;
    document.getElementById('hlsMetaSaveBtn').addEventListener('click', async () => {
        try {
            const current = await loadEncoderConfig(selectedEncoder);
            const section = current.hls || {};
            const checked = Array.from(document.querySelectorAll('.hlsTagChk:checked')).map(el => (el.value || '').trim()).filter(Boolean);
            const custom = (document.getElementById('hlsCustomTags').value || '')
              .split(',')
              .map(s => s.trim())
              .filter(Boolean);
            const tags = Array.from(new Set([...checked, ...custom]));
            section.metaParser = {
              profile: document.getElementById('hlsMetaProfile').value,
                method: document.getElementById('hlsParseMethod').value,
                scope:  document.getElementById('hlsEmbedScope').value,
                tags,
            };
            await apiPost(`/api/encoder/${selectedEncoder}/config/hls`, JSON.stringify(section));
            showBanner('HLS metadata parser saved', 'ok');
        } catch (e) {
            showBanner('HLS metadata parser save failed: ' + e.message, 'error');
        }
    });
}

// ===========================================================
// SRT SECTION
// ===========================================================
async function renderSRTSection(cl, cr) {
  await ensureInterfacesLoaded(false);
    const cfg = await loadEncoderConfig(selectedEncoder);
    const srt = cfg.srt || {};
    const enc = encoders.find(e => e.id === selectedEncoder) || {};
  const srtIface = srt.iface || '';

    cl.innerHTML = `
    <h2>SRT Output Configuration</h2>
    <div class="form-row">
      <label>SRT Transport:</label>
      <select id="srtTransport">
        <option value="mpeg-ts" ${(srt.transport||'mpeg-ts')==='mpeg-ts'?'selected':''}>MPEG-TS</option>
        <option value="rtp"     ${srt.transport==='rtp'                 ?'selected':''}>RTP</option>
      </select>
    </div>
    <div class="form-row">
      <label>Mode:</label>
      <select id="srtMode">
        <option value="caller"   ${(srt.mode||'caller')==='caller'  ?'selected':''}>Caller</option>
        <option value="listener" ${srt.mode==='listener'            ?'selected':''}>Listener</option>
      </select>
    </div>
    <div class="form-row">
      <label>Host IP / URL:</label>
      <input type="text" id="srtHost" value="${srt.host||''}"/>
    </div>
    <div class="form-row">
      <label>Port Number:</label>
      <input type="number" id="srtPort" value="${srt.port||9150}"/>
    </div>
    <div class="form-row">
      <label>Stream ID:</label>
      <input type="text" id="srtStreamId" value="${srt.streamId||`Encoder-${selectedEncoder}`}"/>
    </div>
    <div class="form-row">
      <label>Timestamp Delivery:</label>
      <select id="srtTimestamp">
        <option value="true"  ${(srt.timestamp!==false)?'selected':''}>YES</option>
        <option value="false" ${srt.timestamp===false   ?'selected':''}>NO</option>
      </select>
    </div>
    <div class="form-row">
      <label>Latency (ms):</label>
      <input type="number" id="srtLatency" value="${srt.latency||120}"/>
    </div>
    <div class="form-row">
      <label>Buffer:</label>
      <input type="number" id="srtBuffer" value="${srt.buffer||1024}"/>
    </div>
    <div class="form-row">
      <label>Encryption:</label>
      <select id="srtEncrypt">
        <option value="none" ${(srt.encryption||'none')==='none'?'selected':''}>OFF</option>
        <option value="aes"  ${srt.encryption==='aes'           ?'selected':''}>AES</option>
      </select>
    </div>
    <div class="form-row">
      <label>Passphrase:</label>
      <input type="password" id="srtPassphrase" value="${srt.passphrase||''}"/>
    </div>
    <div class="form-row">
      <label>Key Length:</label>
      <select id="srtPbkeylen">
        <option value="16" ${(srt.pbkeylen||16)===16?'selected':''}>16</option>
        <option value="24" ${srt.pbkeylen===24      ?'selected':''}>24</option>
        <option value="32" ${srt.pbkeylen===32      ?'selected':''}>32</option>
      </select>
    </div>
    <div class="form-row">
      <label>Network Interface:</label>
      <select id="srtIface">${interfaceOptions(srtIface)}</select>
    </div>
    <p style="font-size:10px;color:var(--muted);margin:6px 0 12px">
      <strong>Metadata in SRT:</strong> "Now Playing" metadata is embedded as a KLV/ID3 block inside the MPEG-TS PID stream
      when transport is MPEG-TS. For RTP transport, metadata embedding is not standardised; a best-effort RTCP APP packet
      is sent. Receiving end must support the same convention. This limitation is inherent to SRT/MPEG-TS metadata handling.
    </p>
    <div class="action-row">
      <button class="btn btn-success" id="srtStartBtn">${enc.srt ? '⬤ Running' : 'Start SRT'}</button>
      <button class="btn btn-danger"  id="srtStopBtn">Stop SRT</button>
      <button class="btn"             id="srtSaveBtn">Save</button>
    </div>`;

    cr.innerHTML = `
    <h2>SRT Metadata + SCTE/Sidecar</h2>
    <h3 style="margin-top:4px">SRT Metadata</h3>
    <div class="form-row">
      <label>Metadata Format:</label>
      <select id="srtMetaFormat">
        <option value="id3" ${(srt.metaFormat || 'id3') === 'id3' ? 'selected' : ''}>ID3</option>
        <option value="klv" ${(srt.metaFormat || '') === 'klv' ? 'selected' : ''}>KLV Packets</option>
      </select>
    </div>
    <div class="form-row">
      <label>Metadata Handling:</label>
      <select id="srtMetaHandling">
        <option value="xmlPassThrough" ${(srt.metaHandling || 'xmlPassThrough') === 'xmlPassThrough' ? 'selected' : ''}>XML pass-through</option>
        <option value="customExport" ${(srt.metaHandling || '') === 'customExport' ? 'selected' : ''}>Custom export</option>
      </select>
    </div>
    <div class="form-row">
      <label>Custom Export Fields:</label>
      <input type="text" id="srtMetaCustomFields" value="${escapeHtml(Array.isArray(srt.metaCustomFields) ? srt.metaCustomFields.join(', ') : '')}" placeholder="title, artist, duration"/>
    </div>

    <h3 style="margin-top:12px">SRT SCTE-35 Output</h3>
    <div class="form-row">
      <label>SCTE-35:</label>
      <select id="srtScteEnabled">
        <option value="false" ${(srt.scteEnabled === true) ? '' : 'selected'}>OFF</option>
        <option value="true" ${(srt.scteEnabled === true) ? 'selected' : ''}>ON</option>
      </select>
    </div>
    <div class="form-row srt-scte-dependent">
      <label>Passthrough:</label>
      <select id="srtSctePassthrough">
        <option value="off" ${(srt.sctePassthrough || '') === 'off' ? 'selected' : ''}>OFF</option>
        <option value="pass-through" ${(srt.sctePassthrough || 'pass-through') === 'pass-through' ? 'selected' : ''}>Pass-through</option>
        <option value="generate-from-cues" ${(srt.sctePassthrough || '') === 'generate-from-cues' ? 'selected' : ''}>Generate from Cues</option>
      </select>
    </div>

    <h3 style="margin-top:12px">SRT Sidecar</h3>
    <div class="form-row">
      <label>Enable Sidecar:</label>
      <select id="srtSidecarEnabled">
        <option value="false" ${(srt.sidecar && srt.sidecar.enabled) ? '' : 'selected'}>OFF</option>
        <option value="true" ${(srt.sidecar && srt.sidecar.enabled) ? 'selected' : ''}>ON</option>
      </select>
    </div>
    <div class="form-row srt-sidecar-dependent">
      <label>Transport:</label>
      <select id="srtSidecarTransport">
        <option value="http-json" ${((srt.sidecar && srt.sidecar.transport) || 'http-json') === 'http-json' ? 'selected' : ''}>HTTP POST JSON</option>
      </select>
    </div>
    <div class="form-row srt-sidecar-dependent">
      <label>Mode:</label>
      <select id="srtSidecarMode">
        <option value="mirror" ${((srt.sidecar && srt.sidecar.mode) || 'mirror') === 'mirror' ? 'selected' : ''}>Mirror</option>
        <option value="fallback" ${((srt.sidecar && srt.sidecar.mode) || '') === 'fallback' ? 'selected' : ''}>Fallback</option>
      </select>
    </div>
    <div class="form-row srt-sidecar-dependent">
      <label>Endpoint URL:</label>
      <input type="text" id="srtSidecarUrl" value="${escapeHtml((srt.sidecar && srt.sidecar.url) || '')}" placeholder="http://destination/api/sidecar"/>
    </div>
    <div class="form-row srt-sidecar-dependent">
      <label>Retries:</label>
      <input type="number" id="srtSidecarRetries" min="0" max="20" value="${Number.isFinite(srt.sidecar && srt.sidecar.retries) ? srt.sidecar.retries : 3}"/>
    </div>`;

    const applySrtUi = () => {
      const scteOn = document.getElementById('srtScteEnabled').value === 'true';
      document.querySelectorAll('.srt-scte-dependent').forEach(el => {
        el.style.opacity = scteOn ? '1' : '0.45';
        el.querySelectorAll('input,select,button').forEach(ctrl => { ctrl.disabled = !scteOn; });
      });
      const sidecarOn = document.getElementById('srtSidecarEnabled').value === 'true';
      document.querySelectorAll('.srt-sidecar-dependent').forEach(el => {
        el.style.opacity = sidecarOn ? '1' : '0.45';
        el.querySelectorAll('input,select,button').forEach(ctrl => { ctrl.disabled = !sidecarOn; });
      });
    };
    document.getElementById('srtScteEnabled').addEventListener('change', applySrtUi);
    document.getElementById('srtSidecarEnabled').addEventListener('change', applySrtUi);
    applySrtUi();

    document.getElementById('srtStartBtn').addEventListener('click', async () => {
      const res = await apiPost(`/api/encoder/${selectedEncoder}/srt/start`, {});
      if (res && res.ok === false) showBanner(`SRT start failed: ${res.error || 'unknown error'}`, 'error');
      else showBanner('SRT started', 'ok');
        await refreshStatus();
    });
    document.getElementById('srtStopBtn').addEventListener('click', async () => {
      const res = await apiPost(`/api/encoder/${selectedEncoder}/srt/stop`, {});
      if (res && res.ok === false) showBanner(`SRT stop failed: ${res.error || 'unknown error'}`, 'error');
      else showBanner('SRT stopped');
        await refreshStatus();
    });
    document.getElementById('srtSaveBtn').addEventListener('click', async () => {
        try {
        const existing = await loadEncoderConfig(selectedEncoder);
        const existingSrt = (existing && existing.srt) || {};
            const data = {
          ...existingSrt,
                transport:   document.getElementById('srtTransport').value,
                mode:        document.getElementById('srtMode').value,
                host:        document.getElementById('srtHost').value,
                port:        parseInt(document.getElementById('srtPort').value),
                streamId:    document.getElementById('srtStreamId').value,
                timestamp:   document.getElementById('srtTimestamp').value === 'true',
                latency:     parseInt(document.getElementById('srtLatency').value),
                buffer:      parseInt(document.getElementById('srtBuffer').value),
                encryption:  document.getElementById('srtEncrypt').value,
                passphrase:  document.getElementById('srtPassphrase').value,
                pbkeylen:    parseInt(document.getElementById('srtPbkeylen').value),
                iface:       document.getElementById('srtIface').value,
                metaFormat:  document.getElementById('srtMetaFormat').value,
                metaHandling: document.getElementById('srtMetaHandling').value,
                metaCustomFields: listFromCsv(document.getElementById('srtMetaCustomFields').value),
                scteEnabled: document.getElementById('srtScteEnabled').value === 'true',
                sctePassthrough: document.getElementById('srtSctePassthrough').value,
                sidecar: {
                  enabled: document.getElementById('srtSidecarEnabled').value === 'true',
                  transport: document.getElementById('srtSidecarTransport').value || 'http-json',
                  mode: document.getElementById('srtSidecarMode').value || 'mirror',
                  url: (document.getElementById('srtSidecarUrl').value || '').trim(),
                  retries: parseInt(document.getElementById('srtSidecarRetries').value) || 0,
                },
            };
            await apiPost(`/api/encoder/${selectedEncoder}/config/srt`, JSON.stringify(data));
            showBanner('SRT config saved', 'ok');
        } catch (e) {
            showBanner('SRT config save failed: ' + e.message, 'error');
        }
    });
}

// ===========================================================
// LOG TAIL SECTION
// ===========================================================
function renderTailLogSection(cl, path, title) {
  cl.innerHTML = `
  <h2>${escapeHtml(title)}</h2>
  <div id="log-window">Loading log...</div>`;

    const logEl = cl.querySelector('#log-window');
    logPinned = true;

    logEl.addEventListener('scroll', () => {
        const atBottom = logEl.scrollHeight - logEl.scrollTop - logEl.clientHeight < 30;
        logPinned = atBottom;
    });

    async function updateLog() {
    const text = await apiText(path);
        if (!logEl.isConnected) { clearInterval(logTimer); return; }
        logEl.textContent = text;
        if (logPinned) logEl.scrollTop = logEl.scrollHeight;
    }

    updateLog();
    logTimer = setInterval(updateLog, 1000);
}

function renderLogSection(cl, cr) {
  cr.innerHTML = '';
  renderTailLogSection(cl, `/api/encoder/${selectedEncoder}/log`, `Encoder ${selectedEncoder} Log`);
}

// ===========================================================
// STATUS REFRESH
// ===========================================================
async function refreshStatus() {
    try {
        const data = await apiGet('/api/encoders');
        encoders = data.map(e => ({
            id: e.id,
            aac: !!e.aac,
            mp3: !!e.mp3,
            hls: !!e.hls,
            srt: !!e.srt,
        }));
        renderEncoderCards();
    } catch (e) {
        showBanner('Cannot reach supervisor – reconnecting…', 'error');
    }
}

// ===========================================================
// ADMIN — state
// ===========================================================
let adminMode = false;
let adminSection = null;
let adminEncoderSelected = null;

// Clears timer resources shared with encoder sections
function clearSectionTimers() {
    if (logTimer)            { clearInterval(logTimer);            logTimer = null; }
    if (vuHandle)            { clearInterval(vuHandle);            vuHandle = null; }
    if (inputStatusTimer)    { clearInterval(inputStatusTimer);    inputStatusTimer = null; }
    if (metadataStatusTimer) { clearInterval(metadataStatusTimer); metadataStatusTimer = null; }
    if (metaPanelTimer)      { clearInterval(metaPanelTimer);      metaPanelTimer = null; }
}

function enterAdminMode() {
    adminMode = true;
    adminSection = null;
    adminEncoderSelected = null;
    clearSectionTimers();
  setMainLayout('default');
    document.getElementById('rightPanel').style.display = 'none';
    renderAdminNav();
    selectAdminSection('application');
}

function exitAdminMode() {
    adminMode = false;
    adminSection = null;
    adminEncoderSelected = null;
  clearSectionTimers();
  setMainLayout('default');
    const title = document.getElementById('leftPanelTitle');
    const nav   = document.getElementById('encoderNav');
    title.textContent = 'Select an Encoder';
    nav.innerHTML = '';
    document.getElementById('centerLeftContent').innerHTML =
        '<p style="color:var(--muted); margin-top:24px; text-align:center;">Select an encoder to begin.</p>';
    document.getElementById('centerRightContent').innerHTML = '';
}

function renderAdminNav() {
    document.getElementById('leftPanelTitle').textContent = 'Administrative Settings';
    const nav = document.getElementById('encoderNav');
    const sections = [
        { id: 'application', label: 'Application Settings'  },
        { id: 'interfaces',  label: 'Network Interfaces'    },
        { id: 'encoders',    label: 'Encoder Configuration' },
    { id: 'log',         label: 'Tail System Log'       },
    ];
    nav.innerHTML = sections.map(s =>
        `<li><a href="#" class="${adminSection === s.id ? 'active' : ''}" data-section="${s.id}">${s.label}</a></li>`
    ).join('');
    nav.querySelectorAll('a').forEach(a =>
        a.addEventListener('click', e => { e.preventDefault(); selectAdminSection(a.dataset.section); })
    );
}

async function selectAdminSection(section) {
    adminSection = section;
    adminEncoderSelected = null;
  clearSectionTimers();
    renderAdminNav();
    const cl = document.getElementById('centerLeftContent');
    const cr = document.getElementById('centerRightContent');
  setMainLayout(section === 'log' ? 'log' : 'default');
    cl.innerHTML = '';
    cr.innerHTML = '';
    switch (section) {
        case 'application': await renderAdminAppSettings(cl);         break;
        case 'interfaces':  await renderAdminInterfaces(cl);          break;
        case 'encoders':    await renderAdminEncoderList(cl, cr);     break;
    case 'log':         renderTailLogSection(cl, '/api/syslog', 'System Log'); break;
    }
}

// ------ Application Settings ------
async function renderAdminAppSettings(cl) {
    let cfg = {};
    try { cfg = await apiGet('/api/admin/config'); } catch {}
    const ll = cfg.logLevel || 'INFO';
    cl.innerHTML = `
    <h2>Application Settings</h2>
    <div class="form-row">
      <label>UI Port:</label>
      <input type="number" id="cfgUIPort" value="${cfg.uiPort || 8050}" style="width:100px"/>
    </div>
    <div class="form-row">
      <label>Log Level:</label>
      <select id="cfgLogLevel">
        <option value="INFO"    ${ll === 'INFO'    ? 'selected' : ''}>INFO</option>
        <option value="DEBUG"   ${ll === 'DEBUG'   ? 'selected' : ''}>DEBUG</option>
        <option value="WARNING" ${ll === 'WARNING' ? 'selected' : ''}>WARNING</option>
      </select>
    </div>
    <div class="form-row">
      <label>Log Rotation:</label>
      <input type="number" id="cfgLogRotSize" value="${cfg.logRotSize !== undefined ? cfg.logRotSize : 5}" min="1" style="width:80px"/>
    </div>
    <div class="form-row">
      <label>Log Retention (days):</label>
      <input type="number" id="cfgLogRetention" value="${cfg.logRetention !== undefined ? cfg.logRetention : 14}" min="1" style="width:80px"/>
    </div>
    <div class="form-row">
      <label>Max Encoder Count:</label>
      <input type="number" id="cfgEncoderCount" value="${cfg.encoderCount !== undefined ? cfg.encoderCount : 2}" min="1" max="10" style="width:80px"/>
    </div>
    <h3 style="margin-top:12px">SCTE-35 Configuration</h3>
    <div class="form-row">
      <label>Enable SCTE-35 Cues:</label>
      <select id="cfgScteGlobalEnabled">
        <option value="true" ${(cfg.scteGlobalEnabled !== false) ? 'selected' : ''}>Yes</option>
        <option value="false" ${(cfg.scteGlobalEnabled === false) ? 'selected' : ''}>No</option>
      </select>
    </div>
    <div class="form-row">
      <label>Global Rate Limit Count:</label>
      <input type="number" id="cfgScteRateCount" min="1" value="${Number.isFinite(cfg.scteGlobalRateLimitCount) ? cfg.scteGlobalRateLimitCount : 5}" style="width:100px"/>
    </div>
    <div class="form-row">
      <label>Global Rate Window (sec):</label>
      <input type="number" id="cfgScteRateWindow" min="1" value="${Number.isFinite(cfg.scteGlobalRateLimitWindowSec) ? cfg.scteGlobalRateLimitWindowSec : 10}" style="width:100px"/>
    </div>
    <div class="form-row">
      <label>Global Dedupe Seconds:</label>
      <input type="number" id="cfgScteDedupe" min="0" value="${Number.isFinite(cfg.scteGlobalDedupeSeconds) ? cfg.scteGlobalDedupeSeconds : 30}" style="width:100px"/>
    </div>
    <div class="form-row">
      <label>SCTE Log Level:</label>
      <select id="cfgScteLogLevel">
        <option value="info" ${String(cfg.scteLogLevel || 'info').toLowerCase() === 'info' ? 'selected' : ''}>Info</option>
        <option value="debug" ${String(cfg.scteLogLevel || '').toLowerCase() === 'debug' ? 'selected' : ''}>Debug</option>
        <option value="warning" ${String(cfg.scteLogLevel || '').toLowerCase() === 'warning' ? 'selected' : ''}>Warning / Error</option>
      </select>
    </div>
    <div class="form-row">
      <label>Change Password:</label>
      <input type="password" id="cfgNewPass" placeholder="New password" style="width:160px"/>
    </div>
    <div class="form-row">
      <label>Confirm Password:</label>
      <input type="password" id="cfgConfirmPass" placeholder="Confirm" style="width:160px"/>
    </div>
    <div class="action-row" style="margin-top:16px">
      <button class="btn btn-primary" id="adminAppSaveBtn">Save Settings</button>
    </div>
    <p id="adminAppSaveStatus" style="margin-top:8px;"></p>`;

    document.getElementById('adminAppSaveBtn').addEventListener('click', async () => {
        const np = document.getElementById('cfgNewPass').value;
        const cp = document.getElementById('cfgConfirmPass').value;
        const st = document.getElementById('adminAppSaveStatus');
        if (np && np !== cp) {
            st.textContent = 'Passwords do not match.';
            st.style.color = 'red';
            return;
        }
        // Merge into existing config so we don't lose other keys
        let existing = {};
        try { existing = await apiGet('/api/admin/config'); } catch {}
        const data = Object.assign({}, existing, {
            uiPort:       parseInt(document.getElementById('cfgUIPort').value),
            logLevel:     document.getElementById('cfgLogLevel').value,
            logRotSize:   parseInt(document.getElementById('cfgLogRotSize').value),
            logRetention: parseInt(document.getElementById('cfgLogRetention').value),
            encoderCount: parseInt(document.getElementById('cfgEncoderCount').value),
          scteGlobalEnabled: document.getElementById('cfgScteGlobalEnabled').value === 'true',
          scteGlobalRateLimitCount: parseInt(document.getElementById('cfgScteRateCount').value) || 5,
          scteGlobalRateLimitWindowSec: parseInt(document.getElementById('cfgScteRateWindow').value) || 10,
          scteGlobalDedupeSeconds: parseInt(document.getElementById('cfgScteDedupe').value) || 30,
          scteLogLevel: (document.getElementById('cfgScteLogLevel').value || 'info').toLowerCase(),
        });
        if (np) data.adminPass = np;
        await apiPost('/api/admin/config', JSON.stringify(data));
        st.textContent = 'Settings saved.';
        st.style.color = 'var(--success)';
        // Refresh encoder list in case count changed
        await refreshStatus();
    });
}

// ------ Network Interfaces ------
async function renderAdminInterfaces(cl) {
    cl.innerHTML = '<h2>Network Interfaces</h2><p style="color:var(--muted)">Loading…</p>';
    let ifaces = [];
    try {
        ifaces = await apiGet('/api/admin/interfaces');
        if (!Array.isArray(ifaces)) ifaces = [];
    } catch {}

    cl.innerHTML = `
    <h2>Network Interfaces</h2>
    <p style="font-size:11px;margin-bottom:10px;color:var(--muted)">Assign friendly names to detected interfaces. Check the boxes to include an interface in encoder Network dropdowns.</p>
    <div id="adminIfaceList"></div>
    <div class="action-row" style="margin-top:16px">
      <button class="btn btn-primary" id="adminIfaceSaveBtn">Save</button>
    </div>
    <p id="adminIfaceSaveStatus" style="margin-top:8px;"></p>`;

    const list = document.getElementById('adminIfaceList');
    ifaces.forEach(iface => {
        const row = document.createElement('div');
        row.className = 'form-row';
        row.style.cssText = 'align-items:center;gap:8px;';
        const checked = iface.enabled !== false ? 'checked' : '';
        const safeName = escapeHtml(iface.name);
        const safeFriendly = escapeHtml(iface.friendlyName || iface.name);
        row.innerHTML =
            `<input type="checkbox" id="iface_en_${safeName}" ${checked} style="margin-right:4px"/>` +
            `<span style="font-size:11px;color:var(--muted);min-width:295px;display:inline-block">(${safeName})</span>` +
            `<input type="text" id="iface_fn_${safeName}" value="${safeFriendly}" style="width:170px" placeholder="Friendly name"/>`;
        list.appendChild(row);
    });

    document.getElementById('adminIfaceSaveBtn').addEventListener('click', async () => {
        const prefs = ifaces.map(iface => ({
            name:         iface.name,
            friendlyName: document.getElementById('iface_fn_' + iface.name)?.value || iface.name,
            enabled:      !!(document.getElementById('iface_en_' + iface.name)?.checked),
        }));
        await apiPost('/api/admin/interfaces', JSON.stringify({ interfaces: prefs }));
        await ensureInterfacesLoaded(true);
        const st = document.getElementById('adminIfaceSaveStatus');
        st.textContent = 'Network interfaces saved.';
        st.style.color = 'var(--success)';
    });
}

// ------ Encoder Configuration ------
async function renderAdminEncoderList(cl, cr) {
    let cfg = {};
    try { cfg = await apiGet('/api/admin/config'); } catch {}
    const maxCount = cfg.encoderCount !== undefined ? cfg.encoderCount : (encoders.length || 2);

    cl.innerHTML = '<h2>Encoder Configuration</h2>';
    const list = document.createElement('div');
    cl.appendChild(list);

    function buildList(count) {
        list.innerHTML = '';
        for (let i = 1; i <= count; i++) {
            const row = document.createElement('div');
            row.style.cssText = 'display:flex;align-items:center;gap:6px;margin-bottom:6px;';

            const link = document.createElement('a');
            link.href = '#';
            link.textContent = `Encoder ${i}`;
            link.style.cssText = `min-width:90px;${adminEncoderSelected === i ? 'font-weight:bold;' : ''}`;
            link.addEventListener('click', async e => {
                e.preventDefault();
                adminEncoderSelected = i;
                buildList(count);
                await renderAdminEncoderSettings(cr, i);
            });
            row.appendChild(link);

            // [−] for encoders 2+ — always first so it aligns down the column
            if (i >= 2) {
                const remBtn = document.createElement('button');
                remBtn.className = 'btn btn-danger';
                remBtn.textContent = '−';
                remBtn.style.cssText = 'padding:2px 10px;font-size:11px;cursor:pointer;';
                remBtn.title = `Remove Encoder ${i}`;
                remBtn.addEventListener('click', async () => {
                    const newCount = count - 1;
                    if (newCount < 1) return;
                    const updated = Object.assign({}, cfg, { encoderCount: newCount });
                    await apiPost('/api/admin/config', JSON.stringify(updated));
                    cfg.encoderCount = newCount;
                    if (adminEncoderSelected > newCount) {
                        adminEncoderSelected = null;
                        cr.innerHTML = '';
                    }
                    buildList(newCount);
                    await refreshStatus();
                });
                row.appendChild(remBtn);
            }

            // [+] only on last encoder — after the − button
            if (i === count) {
                const addBtn = document.createElement('button');
                addBtn.className = 'btn';
                addBtn.textContent = '+';
                addBtn.style.cssText = 'padding:2px 10px;font-size:11px;background:#3a7a3a;color:#fff;border:none;cursor:pointer;';
                addBtn.title = 'Add encoder';
                addBtn.addEventListener('click', async () => {
                    const newCount = count + 1;
                    if (newCount > 10) { showBanner('Maximum 10 encoders', 'warn'); return; }
                    const updated = Object.assign({}, cfg, { encoderCount: newCount });
                    await apiPost('/api/admin/config', JSON.stringify(updated));
                    cfg.encoderCount = newCount;
                    buildList(newCount);
                    await refreshStatus();
                });
                row.appendChild(addBtn);
            }

            list.appendChild(row);
        }
    }

    buildList(maxCount);
}

async function renderAdminEncoderSettings(cr, encoderIdx) {
    let adminCfg = {};
    try { adminCfg = await apiGet(`/api/encoder/${encoderIdx}/admin-config`); } catch {}

    cr.innerHTML = `
    <h2>Encoder ${encoderIdx} Configuration</h2>
    <div class="form-row">
      <label>AAC Listen Link:</label>
      <input type="text" id="adm_aacLink" value="${escapeHtml(adminCfg.aacListenLink || '')}" style="width:220px" placeholder="http://host:8000/stream-aac"/>
    </div>
    <div class="form-row">
      <label>MP3 Listen Link:</label>
      <input type="text" id="adm_mp3Link" value="${escapeHtml(adminCfg.mp3ListenLink || '')}" style="width:220px" placeholder="http://host:8000/stream-mp3"/>
    </div>
    <div class="form-row">
      <label>HLS Playback Port:</label>
      <input type="number" id="adm_hlsPort" value="${adminCfg.hlsPlaybackPort || 8080}" style="width:100px"/>
    </div>
    <div class="form-row">
      <label>HLS Listen Link:</label>
      <input type="text" id="adm_hlsLink" value="${escapeHtml(adminCfg.hlsListenLink || '')}" style="width:220px" placeholder="http://host:8080/hls/index.m3u8"/>
    </div>
    <div class="action-row" style="margin-top:16px">
      <button class="btn btn-primary" id="adminEncSaveBtn">Save</button>
    </div>
    <p id="adminEncSaveStatus" style="margin-top:8px;"></p>`;

    document.getElementById('adminEncSaveBtn').addEventListener('click', async () => {
        const data = {
            aacListenLink:   document.getElementById('adm_aacLink').value,
            mp3ListenLink:   document.getElementById('adm_mp3Link').value,
            hlsPlaybackPort: parseInt(document.getElementById('adm_hlsPort').value),
            hlsListenLink:   document.getElementById('adm_hlsLink').value,
        };
        await apiPost(`/api/encoder/${encoderIdx}/admin-config`, JSON.stringify(data));
        const st = document.getElementById('adminEncSaveStatus');
        st.textContent = 'Saved.';
        st.style.color = 'var(--success)';
    });
}

// ===========================================================
// ADMIN MODAL — login gate
// ===========================================================
document.getElementById('adminLink').addEventListener('click', e => {
    e.preventDefault();
    document.getElementById('adminModal').style.display = 'flex';
    document.getElementById('adminLoginErr').textContent = '';
    document.getElementById('adminPass').value = '';
});

document.getElementById('adminClose').addEventListener('click', () => {
    document.getElementById('adminModal').style.display = 'none';
});

document.getElementById('adminLoginBtn').addEventListener('click', async () => {
    const user = document.getElementById('adminUser').value;
    const pass = document.getElementById('adminPass').value;
    try {
        const r = await fetch('/api/admin/login', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ user, pass })
        });
        if (r.status === 200 || r.status === 204) {
            document.getElementById('adminModal').style.display = 'none';
            enterAdminMode();
        } else {
            document.getElementById('adminLoginErr').textContent = 'Invalid credentials.';
        }
    } catch {
        // Dev fallback when supervisor not responding to 401
        document.getElementById('adminModal').style.display = 'none';
        enterAdminMode();
    }
});

// Clicking an encoder card while in admin mode exits admin mode first
const _origSelectEncoder = selectEncoder;
window.selectEncoder = function(id) {
    if (adminMode) exitAdminMode();
    _origSelectEncoder(id);
};

// ===========================================================
// BOOT
// ===========================================================
async function init() {
    await refreshStatus();
    // Periodic status refresh every 3 seconds
    statusTimer = setInterval(refreshStatus, 3000);
}

init();
