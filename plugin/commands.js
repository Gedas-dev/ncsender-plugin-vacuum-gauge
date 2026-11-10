/*
 * Vacuum Gauge - ncSender plugin (server side).
 *
 * Runs inside ncSender's embedded JS engine. Polls an ESP32 vacuum gauge
 * over HTTP, keeps the latest reading for the plugin window, and watches
 * the vacuum while a job is running.
 *
 * It only uses a background timer. It does not touch the G-code or the
 * command pipeline, so a fault here cannot block machine commands.
 */

var VG_INHG_PER_KPA = 0.2953;
var VG_STALE_MS = 3000;

var VG = {
  cfg: null,
  inHg: null,
  volts: null,
  lastOkAt: 0,
  lastError: 'No reading yet',
  lastPollAt: 0,
  busy: false,
  busySince: 0,
  prevJob: null,
  weakSince: 0,
  actedThisEpisode: false,
  offlineNoticed: false,
  lastAction: null,
  lastActionAt: 0
};

function vgNum(v, fallback, min, max) {
  var n = typeof v === 'number' ? v : parseFloat(v);
  if (!isFinite(n)) return fallback;
  if (n < min) return min;
  if (n > max) return max;
  return n;
}

// Vacuum is entered either way round (15 or -15); stored as negative inHg.
function vgVac(v, fallback) {
  var n = vgNum(v, fallback, -30, 30);
  return -Math.abs(n);
}

function buildInitialConfig(raw) {
  raw = raw || {};
  var device = typeof raw.device === 'string' ? raw.device.replace(/^\s+|\s+$/g, '') : '';
  var mode = raw.guardMode === 'off' || raw.guardMode === 'hold' ? raw.guardMode : 'warn';
  var green = vgVac(raw.greenAt, -15);
  var yellow = vgVac(raw.yellowAt, -8);
  if (yellow < green) { var t = yellow; yellow = green; green = t; }
  var cfg = {
    device: device || 'vac-gauge.local',
    pollMs: vgNum(raw.pollMs, 500, 250, 5000),
    greenAt: green,
    yellowAt: yellow,
    guardMode: mode,
    minVac: vgVac(raw.minVac, -10),
    guardDelaySec: vgNum(raw.guardDelaySec, 2, 0.5, 30)
  };
  VG.cfg = cfg;
  return cfg;
}

function vgBaseUrl(device) {
  var d = String(device || '');
  if (!/^https?:\/\//i.test(d)) d = 'http://' + d;
  var m = /^(https?:\/\/[^\/]+)(\/.*)?$/i.exec(d);
  if (!m) return null;
  return { origin: m[1], path: m[2] && m[2] !== '/' ? m[2] : '/api/vacuum' };
}

// Accepts this project's firmware ({ inHg, kPa }) and an ESPHome sensor
// endpoint ({ value, state: "-18.4 inHg" }).
function vgParse(body) {
  var j = JSON.parse(body);
  var v = null;
  if (typeof j.inHg === 'number') v = j.inHg;
  else if (typeof j.kPa === 'number') v = j.kPa * VG_INHG_PER_KPA;
  else if (typeof j.value === 'number') {
    v = j.value;
    var unit = String(j.state || '').toLowerCase();
    if (unit.indexOf('kpa') >= 0) v = v * VG_INHG_PER_KPA;
    else if (unit.indexOf('mbar') >= 0 || unit.indexOf('hpa') >= 0) v = v * 0.02953;
    else if (unit.indexOf('psi') >= 0) v = v * 2.036;
  }
  if (typeof v !== 'number' || !isFinite(v)) throw new Error('No pressure value in the reply');
  return { inHg: v, volts: typeof j.volts === 'number' ? j.volts : null };
}

function vgPoll(now) {
  var cfg = VG.cfg;
  if (!cfg) return;
  if (!pluginContext.http) {
    VG.lastError = 'This ncSender version cannot poll the gauge in the background';
    return;
  }
  if (VG.busy && now - VG.busySince < 5000) return;
  if (now - VG.lastPollAt < cfg.pollMs) return;
  var target = vgBaseUrl(cfg.device);
  if (!target) { VG.lastError = 'Gauge address is not valid'; return; }
  VG.lastPollAt = now;
  VG.busy = true;
  VG.busySince = now;
  var started = pluginContext.http.request(
    { url: target.origin + target.path, timeoutMs: 2000 },
    function (res) {
      VG.busy = false;
      if (!res || !res.ok) {
        VG.lastError = res && res.error ? String(res.error)
          : 'Gauge answered HTTP ' + (res ? res.status : '?');
        return;
      }
      try {
        var r = vgParse(res.body);
        VG.inHg = r.inHg;
        VG.volts = r.volts;
        VG.lastOkAt = Date.now();
        VG.lastError = null;
      } catch (e) {
        VG.lastError = 'Unexpected reply from the gauge';
      }
    });
  if (!started) { VG.busy = false; VG.lastError = 'Gauge address is not valid'; }
}

function vgFmt(v) {
  return (Math.round(v * 10) / 10).toFixed(1) + ' inHg';
}

function vgGuard(now) {
  var cfg = VG.cfg;
  if (!cfg || cfg.guardMode === 'off' || !pluginContext.http) return;
  var state = pluginContext.getServerState();
  var job = state && state.jobStatus ? String(state.jobStatus) : '';
  var running = job === 'running';

  if (!running) {
    // A paused job keeps the episode, so resuming into weak vacuum acts again
    // only after the vacuum has recovered once or the delay has run anew.
    VG.weakSince = 0;
    if (job !== 'paused') { VG.actedThisEpisode = false; VG.offlineNoticed = false; }
    VG.prevJob = job;
    return;
  }
  if (VG.prevJob === 'paused') VG.actedThisEpisode = false;
  VG.prevJob = job;

  var fresh = VG.lastOkAt > 0 && now - VG.lastOkAt <= VG_STALE_MS;
  if (!fresh) {
    // A lost WiFi link is not lost vacuum: say so, never hold for it.
    VG.weakSince = 0;
    if (!VG.offlineNoticed && now - VG.lastOkAt > 8000) {
      VG.offlineNoticed = true;
      pluginContext.showNotice('Vacuum gauge offline',
        'No reading from the vacuum gauge. The job is still running and the vacuum is not being watched.',
        { 'Gauge': cfg.device, 'Problem': VG.lastError || 'No reply' });
    }
    return;
  }
  VG.offlineNoticed = false;

  var weak = VG.inHg > cfg.minVac;
  if (!weak) { VG.weakSince = 0; VG.actedThisEpisode = false; return; }
  if (!VG.weakSince) VG.weakSince = now;
  if (VG.actedThisEpisode || now - VG.weakSince < cfg.guardDelaySec * 1000) return;

  VG.actedThisEpisode = true;
  VG.lastActionAt = now;
  var details = { 'Reading': vgFmt(VG.inHg), 'Minimum': vgFmt(cfg.minVac) };
  var machine = String(state.machineStatus || '');
  if (cfg.guardMode === 'hold' && machine.indexOf('Hold') !== 0) {
    VG.lastAction = 'hold';
    pluginContext.sendCommand('!', '! (Feed Hold - vacuum low)');
    pluginContext.showNotice('Vacuum low - feed hold sent',
      'The vacuum dropped below the minimum while the job was running, so the machine was put on feed hold. The spindle is still turning. Restore the vacuum, then resume.',
      details);
  } else {
    VG.lastAction = 'warn';
    pluginContext.showNotice('Vacuum low',
      'The vacuum is below the minimum while the job is running. The job has not been paused.',
      details);
  }
}

function vgTick() {
  var now = Date.now();
  try { vgPoll(now); } catch (e) { VG.busy = false; VG.lastError = String(e && e.message ? e.message : e); }
  try { vgGuard(now); } catch (e2) { VG.guardError = String(e2 && e2.message ? e2.message : e2); }
}

// Called by ncSender for POST /api/plugins/community.vacuum-gauge/request
function onRequest(payload) {
  var action = payload && payload.action;
  var now = Date.now();
  if (action === 'zero') {
    if (!pluginContext.http) return { ok: false, error: 'Not available on this ncSender version' };
    var target = vgBaseUrl(VG.cfg ? VG.cfg.device : '');
    if (!target) return { ok: false, error: 'Gauge address is not valid' };
    pluginContext.http.request({ url: target.origin + '/api/zero', method: 'POST', timeoutMs: 3000 }, function (res) {
      VG.lastZero = res && res.ok ? 'ok' : 'failed';
    });
    return { ok: true };
  }
  var fresh = VG.lastOkAt > 0 && now - VG.lastOkAt <= VG_STALE_MS;
  return {
    ok: fresh,
    serverPolling: !!pluginContext.http,
    inHg: fresh ? VG.inHg : null,
    kPa: fresh ? VG.inHg / VG_INHG_PER_KPA : null,
    volts: fresh ? VG.volts : null,
    ageMs: VG.lastOkAt ? now - VG.lastOkAt : null,
    error: fresh ? null : (VG.lastError || 'No reply from the gauge'),
    guard: {
      mode: VG.cfg ? VG.cfg.guardMode : 'off',
      weak: VG.weakSince > 0,
      lastAction: VG.lastAction,
      lastActionAgoMs: VG.lastActionAt ? now - VG.lastActionAt : null
    },
    cfg: VG.cfg
  };
}

// Note: pluginContext.log is deliberately not used anywhere in this file.
// Calling it crashes the packaged (native) ncSender build.
pluginContext.setInterval(vgTick, 250);
