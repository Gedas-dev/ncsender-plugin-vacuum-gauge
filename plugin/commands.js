/*
 * Vacuum Gauge - ncSender plugin (server side).
 *
 * Runs inside ncSender's embedded JS engine. Polls an ESP32 vacuum gauge
 * over HTTP, keeps the latest reading for the plugin window, and watches
 * the vacuum while a job is running.
 *
 * It only uses a background timer. It does not hook the G-code or the
 * command pipeline, so a fault here cannot block machine commands. When the
 * check is set to act, it sends a feed hold, or stops the job and raises Z.
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
  lastActionAt: 0,
  seq: null,
  lastStop: null
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
  var mode = raw.guardMode === 'off' || raw.guardMode === 'hold' || raw.guardMode === 'park' || raw.guardMode === 'stop' ? raw.guardMode : 'warn';
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
    guardDelaySec: vgNum(raw.guardDelaySec, 2, 0.5, 30),
    liftMm: vgNum(raw.liftMm, 20, 0, 200)
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
  if (cfg.guardMode === 'stop') {
    VG.lastAction = 'stop';
    vgStartStop(now, state, details);
  } else if (cfg.guardMode === 'park' && machine.indexOf('Hold') !== 0 && machine.indexOf('Door') !== 0) {
    // Safety-door command: the controller holds, stops the spindle and, when
    // its parking cycle is enabled ($41), retracts Z. ncSender marks the job
    // paused and Resume brings Z back down and restarts the spindle. This is
    // the same command ncSender's own "Park on Pause" setting sends.
    VG.lastAction = 'park';
    var p41 = null;
    try { p41 = pluginContext.getFirmwareSetting(41); } catch (e41) { p41 = null; }
    var parking = typeof p41 === 'number' ? (Math.floor(p41) % 2 === 1) : null;
    pluginContext.sendCommand('\x84', '0x84 (Park - vacuum low)');
    details['Controller parking ($41)'] = parking === null ? 'unknown' : (parking ? 'on' : 'off');
    pluginContext.showNotice('Vacuum low - job paused',
      parking === false
        ? 'The vacuum dropped below the minimum, so the job was paused and the spindle was stopped. Z was not raised, because parking is switched off in the controller ($41). Restore the vacuum, then press Resume.'
        : 'The vacuum dropped below the minimum, so the job was paused: the controller stops the spindle and parks Z. Restore the vacuum, then press Resume. The spindle restarts and Z returns before cutting continues.',
      details);
  } else if (cfg.guardMode === 'hold' && machine.indexOf('Hold') !== 0) {
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

// ---- Stop the job, switch the spindle off, raise Z ----------------------
//
// pluginContext.stopJob() ends the job, sends a feed hold, waits until the
// axes are at rest and then soft-resets the controller. The reset switches
// the spindle and coolant off. Only after the controller has been Idle for a
// while (so the reset is certainly over) is M5 sent again and Z raised.
// A jog sent before the reset would be cut off by it and raise an alarm.

var VG_STOP_MIN_MS = 2000;      // never move sooner than this after stopJob
var VG_STOP_IDLE_MS = 1000;     // controller must report Idle this long
var VG_STOP_TIMEOUT_MS = 20000; // give up waiting for Idle after this
var VG_Z_TOP_MARGIN = 1;        // never raise above machine Z = -1 mm

function vgStartStop(now, state, details) {
  if (VG.seq) return;
  VG.seq = { startedAt: now, idleSince: 0, line: state && state.line ? state.line : 0, details: details };
  pluginContext.stopJob();
}

// How far Z may go up from machine position z (mm). Machine Z is 0 at the
// top after homing and negative below it. If the position is not below the
// top margin, the machine is either at the top or not homed: do not move.
function vgLiftFor(z, liftMm) {
  if (typeof z !== 'number' || !isFinite(z) || liftMm <= 0) return 0;
  var room = -VG_Z_TOP_MARGIN - z;
  if (room <= 0) return 0;
  return Math.round(Math.min(liftMm, room) * 1000) / 1000;
}

function vgFinishStop(now, outcome, liftedMm) {
  var seq = VG.seq;
  VG.seq = null;
  VG.lastStop = { at: now, outcome: outcome, liftedMm: liftedMm, line: seq.line };
  var details = seq.details || {};
  if (seq.line) details['Stopped at line'] = String(seq.line);
  var msg;
  if (outcome === 'lifted') {
    details['Z raised'] = liftedMm + ' mm';
    msg = 'The vacuum dropped below the minimum, so the job was stopped, the spindle was switched off and Z was raised. The job is ended, not paused. Restore the vacuum, check the workpiece, then start again from the stopped line.';
  } else if (outcome === 'nolift') {
    msg = 'The vacuum dropped below the minimum, so the job was stopped and the spindle was switched off. Z was not raised: the machine is at the top of its travel, is not homed, or the raise distance is set to 0.';
  } else if (outcome === 'alarm') {
    msg = 'The vacuum dropped below the minimum and the job was stopped, but the controller is in an alarm state, so Z was not raised. Check the machine before moving it.';
  } else {
    msg = 'The vacuum dropped below the minimum and a stop was sent, but the controller did not report Idle in time, so Z was not raised. Check that the spindle has stopped.';
  }
  pluginContext.showNotice('Vacuum lost - job stopped', msg, details);
}

function vgStopStep(now) {
  var seq = VG.seq;
  if (!seq) return;
  var state = pluginContext.getServerState();
  var machine = String(state && state.machineStatus ? state.machineStatus : '');
  var elapsed = now - seq.startedAt;

  if (machine.indexOf('Alarm') === 0 && elapsed >= VG_STOP_MIN_MS) { vgFinishStop(now, 'alarm', 0); return; }
  if (machine.indexOf('Idle') === 0) { if (!seq.idleSince) seq.idleSince = now; }
  else seq.idleSince = 0;

  if (elapsed >= VG_STOP_MIN_MS && seq.idleSince && now - seq.idleSince >= VG_STOP_IDLE_MS) {
    pluginContext.sendCommand('M5', 'M5 (Spindle off - vacuum lost)');
    var z = state && state.mpos ? state.mpos.z : null;
    var lift = vgLiftFor(z, VG.cfg ? VG.cfg.liftMm : 0);
    if (lift > 0) {
      var target = Math.round((z + lift) * 1000) / 1000;
      pluginContext.sendCommand('$J=G53 G21 G90 Z' + target + ' F1500', 'Raise Z ' + lift + ' mm (vacuum lost)');
      vgFinishStop(now, 'lifted', lift);
    } else {
      vgFinishStop(now, 'nolift', 0);
    }
    return;
  }
  if (elapsed >= VG_STOP_TIMEOUT_MS) vgFinishStop(now, 'timeout', 0);
}

function vgTick() {
  var now = Date.now();
  try { vgPoll(now); } catch (e) { VG.busy = false; VG.lastError = String(e && e.message ? e.message : e); }
  try { vgGuard(now); } catch (e2) { VG.guardError = String(e2 && e2.message ? e2.message : e2); }
  try { vgStopStep(now); } catch (e3) { VG.seq = null; VG.guardError = String(e3 && e3.message ? e3.message : e3); }
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
      lastActionAgoMs: VG.lastActionAt ? now - VG.lastActionAt : null,
      stopping: !!VG.seq,
      lastStop: VG.lastStop
    },
    cfg: VG.cfg
  };
}

// Note: pluginContext.log is deliberately not used anywhere in this file.
// Calling it crashes the packaged (native) ncSender build.
pluginContext.setInterval(vgTick, 250);
