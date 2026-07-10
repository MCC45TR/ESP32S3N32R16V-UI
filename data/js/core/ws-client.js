(function(global) {
  var root = global.MROS = global.MROS || {};
  root.core = root.core || {};

  function publishWsHealth(key, value, detail) {
    global.mrosWebHealth = global.mrosWebHealth || {};
    global.mrosWebHealth[key] = value;
    if (detail !== undefined) global.mrosWebHealth[key + 'Detail'] = detail;
    try {
      window.dispatchEvent(new CustomEvent('mros-web-health', {
        detail: { key: key, value: value, info: detail || '' }
      }));
    } catch (err) {}
  }

  function takeCachedWsTicket(channel) {
    var cache = global.wsTicketCache;
    if (!cache || !cache.tickets || Date.now() >= Number(cache.expiresAt || 0)) return '';
    var key = String(channel || 'telemetry');
    var tok = cache.tickets[key] || '';
    if (tok) {
      cache.tickets[key] = '';
      return tok;
    }
    return '';
  }

  function requestWsTicket(channel) {
    var cached = takeCachedWsTicket(channel);
    if (cached) return Promise.resolve(cached);
    return fetch('/api/ws-ticket')
      .then(function(r) {
        if (!r.ok) throw new Error('ws-ticket');
        return r.json();
      })
      .then(function(d) {
        var expiresMs = Number(d && d.expires_ms) || 12000;
        global.wsTicketCache = {
          tickets: Object.assign({}, (d && d.tickets) || {}),
          expiresAt: Date.now() + Math.max(1000, expiresMs - 1000)
        };
        if (d && d.ticket && !global.wsTicketCache.tickets.telemetry) {
          global.wsTicketCache.tickets.telemetry = d.ticket;
        }
        return takeCachedWsTicket(channel) || (d && d.ticket) || '';
      });
  }

  function shellSocketInstance() {
    return (global.shellWs && global.shellWs.readyState === WebSocket.OPEN) ? global.shellWs : null;
  }

  var SHELL_BIN_HEADER_LEN = 32;
  var shellTextDecoder = null;

  function shellReadU16(view, offset) {
    return view.getUint16(offset, true);
  }

  function shellReadU32(view, offset) {
    return view.getUint32(offset, true);
  }

  function shellBinaryStats() {
    global.mrosShellBinaryStats = global.mrosShellBinaryStats || {
      frames: 0,
      bytes: 0,
      jsonFrames: 0,
      jsonBytes: 0,
      cborFrames: 0,
      cborBytes: 0,
      cborFallbacks: 0,
      errors: 0,
      fallbacks: 0,
      ackSent: 0,
      ackSkipped: 0,
      controlBackpressure: 0,
      controlSendErrors: 0,
      maxBufferedAmount: 0,
      lastSeq: 0
    };
    return global.mrosShellBinaryStats;
  }

  var binaryDecodeWorker = null;
  var binaryDecodeSeq = 0;
  var binaryDecodeCallbacks = {};
  var binaryDecodeMetricTimer = 0;

  function binaryWorkerStats() {
    global.mrosBinaryWorkerStats = global.mrosBinaryWorkerStats || {
      enabled: false,
      frames: 0,
      errors: 0,
      fallbacks: 0,
      dropped: 0,
      latencyMs: 0,
      shellFrames: 0,
      telemetryFrames: 0
    };
    return global.mrosBinaryWorkerStats;
  }

  function publishBinaryWorkerMetrics() {
    var now = Date.now();
    if (now - binaryDecodeMetricTimer < 2000) return;
    binaryDecodeMetricTimer = now;
    var stats = binaryWorkerStats();
    publishWsHealth('workerDecode', stats.enabled ? 'worker' : 'fallback',
      'frames=' + stats.frames + ' errors=' + stats.errors + ' fallbacks=' + stats.fallbacks);
    try {
      if (global.ws && global.ws.readyState === WebSocket.OPEN) {
        global.ws.send('WORKER_DECODE_METRIC:' +
          [stats.frames, stats.errors, stats.fallbacks, Math.round(stats.latencyMs || 0), stats.dropped].join(':'));
      }
    } catch (err) {}
  }

  function ensureBinaryDecodeWorker() {
    var stats = binaryWorkerStats();
    if (binaryDecodeWorker) return binaryDecodeWorker;
    if (typeof Worker === 'undefined') {
      stats.enabled = false;
      return null;
    }
    try {
      binaryDecodeWorker = new Worker('/js/workers/binary-decoder-worker.js?v=20260516_shell_native_wifi_r1');
      stats.enabled = true;
      binaryDecodeWorker.onmessage = function(ev) {
        var msg = ev.data || {};
        var cb = binaryDecodeCallbacks[msg.id];
        if (!cb) {
          stats.dropped++;
          publishBinaryWorkerMetrics();
          return;
        }
        delete binaryDecodeCallbacks[msg.id];
        stats.latencyMs = Number(msg.latency_ms || 0);
        if (msg.ok) {
          stats.frames++;
          if (msg.channel === 'shell') stats.shellFrames++;
          if (msg.channel === 'telemetry') stats.telemetryFrames++;
          cb.resolve(msg.payload, msg);
        } else {
          stats.errors++;
          cb.reject(new Error(msg.error || 'worker decode'));
        }
        publishBinaryWorkerMetrics();
      };
      binaryDecodeWorker.onerror = function() {
        stats.errors++;
        stats.enabled = false;
        binaryDecodeWorker = null;
        publishBinaryWorkerMetrics();
      };
    } catch (err) {
      stats.enabled = false;
      binaryDecodeWorker = null;
    }
    return binaryDecodeWorker;
  }

  function decodeBinaryViaWorker(channel, buffer, fallbackDecode, onDecoded, onError) {
    var worker = ensureBinaryDecodeWorker();
    var stats = binaryWorkerStats();
    if (!worker) {
      stats.fallbacks++;
      try { onDecoded(fallbackDecode(buffer), { fallback: true, bytes: buffer.byteLength }); }
      catch (errFallback) { onError(errFallback); }
      publishBinaryWorkerMetrics();
      return;
    }
    var id = ++binaryDecodeSeq;
    binaryDecodeCallbacks[id] = {
      resolve: onDecoded,
      reject: onError
    };
    try {
      worker.postMessage({ id: id, channel: channel, buffer: buffer }, [buffer]);
    } catch (err) {
      delete binaryDecodeCallbacks[id];
      stats.fallbacks++;
      try { onDecoded(fallbackDecode(buffer), { fallback: true, bytes: buffer.byteLength }); }
      catch (errFallback2) { onError(errFallback2); }
      publishBinaryWorkerMetrics();
    }
  }

  function shellBufferedSend(socket, payload, highWater) {
    if (!socket) return false;
    var stats = shellBinaryStats();
    var buffered = Number(socket.bufferedAmount || 0);
    if (buffered > stats.maxBufferedAmount) stats.maxBufferedAmount = buffered;
    if (buffered > (highWater || 131072)) {
      stats.controlBackpressure++;
      publishWsHealth('shellWs', 'backpressure', String(buffered));
      return false;
    }
    try {
      socket.send(payload);
      return true;
    } catch (err) {
      stats.controlSendErrors++;
      return false;
    }
  }

  function shellEncodeCborUint(value) {
    value = Number(value) >>> 0;
    if (value < 24) return [value];
    if (value < 256) return [0x18, value];
    if (value < 65536) return [0x19, (value >> 8) & 0xFF, value & 0xFF];
    return [0x1A, (value >>> 24) & 0xFF, (value >>> 16) & 0xFF, (value >>> 8) & 0xFF, value & 0xFF];
  }

  function shellEncodeCborText(text) {
    var bytes = Array.prototype.slice.call(new TextEncoder().encode(String(text || '')));
    var len = bytes.length;
    var head;
    if (len < 24) head = [0x60 | len];
    else if (len < 256) head = [0x78, len];
    else head = [0x79, (len >> 8) & 0xFF, len & 0xFF];
    return head.concat(bytes);
  }

  function shellEncodeCborMap(values) {
    var keys = Object.keys(values).filter(function(key) {
      return values[key] !== undefined && values[key] !== null;
    });
    var out = keys.length < 24 ? [0xA0 | keys.length] : [0xB8, keys.length & 0xFF];
    keys.forEach(function(key) {
      out = out.concat(shellEncodeCborText(key));
      if (typeof values[key] === 'number') out = out.concat(shellEncodeCborUint(values[key]));
      else out = out.concat(shellEncodeCborText(values[key]));
    });
    return new Uint8Array(out);
  }

  function shellSendControlFrame(op, paneId, payload, commandId, highWater) {
    var socket = shellSocketInstance();
    if (!socket || !global.shellCborControlReady) return false;
    var stats = shellBinaryStats();
    var body = shellEncodeCborMap({
      op: String(op || ''),
      pane: Number(paneId || 0) >>> 0,
      payload: String(payload || ''),
      command_id: Number(commandId || 0) >>> 0
    });
    var frame = new Uint8Array(8 + body.length);
    frame[0] = 0x53; frame[1] = 0x43; frame[2] = 0x42; frame[3] = 0x31; // SCB1
    frame[4] = 1;
    frame[5] = 8;
    frame[6] = 0;
    frame[7] = 0;
    frame.set(body, 8);
    if (!shellBufferedSend(socket, frame.buffer, highWater || 65536)) {
      stats.cborFallbacks++;
      return false;
    }
    stats.cborFrames++;
    stats.cborBytes += frame.byteLength;
    return true;
  }

  function shellSendAck(seq) {
    var socket = shellSocketInstance();
    if (!socket) return;
    if (shellBufferedSend(socket, 'ACK:' + (Number(seq) >>> 0) + ':65536', 131072)) {
      shellBinaryStats().ackSent++;
    } else {
      shellBinaryStats().ackSkipped++;
    }
  }

  function requestShellJsonFallback(reason) {
    var socket = shellSocketInstance();
    var stats = shellBinaryStats();
    stats.fallbacks++;
    publishWsHealth('shellWs', 'binary-fallback', reason || 'decode');
    try {
      if (socket) {
        socket.send('SHELL_BIN_DECODE_ERROR:' + String(reason || 'decode'));
        socket.send('FORMAT:shell-json-v1');
      }
    } catch (err) {}
  }

  function decodeShellBinaryFrame(buffer) {
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < SHELL_BIN_HEADER_LEN) {
      throw new Error('short shell frame');
    }
    var view = new DataView(buffer);
    if (view.getUint8(0) !== 0x4D || view.getUint8(1) !== 0x53 ||
        view.getUint8(2) !== 0x48 || view.getUint8(3) !== 0x31) {
      throw new Error('bad shell magic');
    }
    var version = view.getUint8(4);
    var headerLen = view.getUint8(5);
    if (version !== 1 || headerLen !== SHELL_BIN_HEADER_LEN) {
      throw new Error('unsupported shell frame');
    }
    var frameType = view.getUint8(6);
    var flags = view.getUint8(7);
    var seq = shellReadU32(view, 8);
    var sessionId = shellReadU32(view, 20);
    var paneId = shellReadU16(view, 24);
    var commandId = shellReadU16(view, 26);
    var payloadLen = shellReadU32(view, 28);
    if (payloadLen !== (buffer.byteLength - headerLen)) {
      throw new Error('shell payload length');
    }
    var payload = buffer.slice(headerLen);
    var stats = shellBinaryStats();
    if (seq && seq <= stats.lastSeq && stats.lastSeq - seq < 1024) {
      return { type: 'duplicate', seq: seq };
    }
    if (seq) stats.lastSeq = seq;
    if (!shellTextDecoder) shellTextDecoder = new TextDecoder('utf-8');
    if (frameType === 2) {
      return {
        type: 'stream',
        output: shellTextDecoder.decode(payload, { stream: true }),
        pane_id: paneId,
        session_id: sessionId,
        command_id: commandId,
        binary: true,
        seq: seq,
        flags: flags
      };
    }
    if (frameType === 4) {
      var finalText = shellTextDecoder.decode(payload, { stream: false });
      var parsed = finalText ? JSON.parse(finalText) : {};
      parsed.type = parsed.type || 'exec';
      parsed.pane_id = typeof parsed.pane_id === 'number' ? parsed.pane_id : paneId;
      parsed.session_id = typeof parsed.session_id === 'number' ? parsed.session_id : sessionId;
      parsed.command_id = commandId;
      parsed.seq = seq;
      parsed.binary = true;
      return parsed;
    }
    if (frameType === 8) {
      return {
        type: 'error',
        message: shellTextDecoder.decode(payload, { stream: false }) || 'shell binary error',
        pane_id: paneId,
        session_id: sessionId,
        command_id: commandId,
        binary: true,
        seq: seq
      };
    }
    return { type: 'metric', binary: true, seq: seq, pane_id: paneId, session_id: sessionId };
  }

  function statusBus() {
    return root.core && root.core.statusBus ? root.core.statusBus : null;
  }

  function emitStatus(topic, payload) {
    var bus = statusBus();
    if (bus && typeof bus.emit === 'function') bus.emit(topic, payload);
  }

  function sendTelemetrySubscription(topic, enabled) {
    if (!global.ws || global.ws.readyState !== WebSocket.OPEN) return;
    global.ws.send('SUB:' + String(topic || '').toLowerCase() + ':' + (enabled ? '1' : '0'));
  }

  function isSceneVisible() {
    var canvas = document.getElementById('canvas-container');
    return !document.hidden && (!canvas || canvas.offsetParent !== null);
  }

  function notifySceneSubscription() {
    if (!global.ws || global.ws.readyState !== WebSocket.OPEN) return;
    sendTelemetrySubscription('scene', isSceneVisible());
  }

  function notifyTelemetryRateProfile() {
    if (!global.ws || global.ws.readyState !== WebSocket.OPEN) return;
    var background = !!document.hidden;
    sendTelemetrySubscription('fast', !background);
    sendTelemetrySubscription('medium', !background);
    sendTelemetrySubscription('slow', true);
    global.mrosTelemetryGatingStats = global.mrosTelemetryGatingStats || {
      backgroundTransitions: 0,
      lastProfile: ''
    };
    var profile = background ? 'background-slow' : 'foreground-fast';
    if (global.mrosTelemetryGatingStats.lastProfile !== profile) {
      global.mrosTelemetryGatingStats.backgroundTransitions++;
      global.mrosTelemetryGatingStats.lastProfile = profile;
      publishWsHealth('telemetryRate', profile);
    }
  }

  function notifyDebugSubscription(enabled) {
    global.debugSubscriptionWanted = !!enabled;
    if (global.debugWs && global.debugWs.readyState === WebSocket.OPEN && global.debugWsAuthReady) {
      global.debugWs.send('SUB:debug:' + (global.debugSubscriptionWanted ? '1' : '0'));
    }
    if (global.ws && global.ws.readyState === WebSocket.OPEN) {
      sendTelemetrySubscription('debug', global.debugSubscriptionWanted);
    }
  }

  function notifyBaseSubscriptions() {
    notifySceneSubscription();
    notifyTelemetryRateProfile();
    sendTelemetrySubscription('debug', false);
    sendTelemetrySubscription('shell', true);
    sendTelemetrySubscription('trajectory', false);
    sendTelemetrySubscription('logs', false);
  }

  function connectShellWS() {
    if (global.shellWs && (global.shellWs.readyState === WebSocket.OPEN || global.shellWs.readyState === WebSocket.CONNECTING)) {
      if (global.shellTerm && global.shellTerm.authReady && global.shellWs.readyState === WebSocket.OPEN && typeof global.shellSendState === 'function') {
        global.shellSendState();
      }
      return;
    }
    var wsProto = (window.location.protocol === 'https:') ? 'wss://' : 'ws://';
    global.shellWs = new WebSocket(wsProto + window.location.host + '/ws/shell');
    global.shellWs.binaryType = 'arraybuffer';
    global.shellWs.onopen = function() {
      if (typeof global.shellSetAuthReady === 'function') global.shellSetAuthReady(false);
      else if (global.shellTerm) global.shellTerm.authReady = false;
      publishWsHealth('shellWs', 'auth');
      requestWsTicket('shell')
        .then(function(tok) {
          if (tok && global.shellWs && global.shellWs.readyState === WebSocket.OPEN) global.shellWs.send('AUTH:' + tok);
          else if (global.shellWs) global.shellWs.close();
        })
        .catch(function() {
          publishWsHealth('shellWs', 'ticket-fail');
          if (global.shellWs) global.shellWs.close();
        });
    };
    global.shellWs.onmessage = function(e) {
      if (e.data instanceof ArrayBuffer) {
        decodeBinaryViaWorker('shell', e.data, decodeShellBinaryFrame, function(shellMessage, meta) {
          var stats = shellBinaryStats();
          stats.frames++;
          stats.bytes += (meta && meta.bytes) || (e.data && e.data.byteLength) || 0;
          if (shellMessage && shellMessage.seq && shellMessage.seq <= stats.lastSeq && stats.lastSeq - shellMessage.seq < 1024) {
            shellMessage = { type: 'duplicate', seq: shellMessage.seq };
          } else if (shellMessage && shellMessage.seq) {
            stats.lastSeq = shellMessage.seq;
          }
          if (shellMessage && shellMessage.seq) shellSendAck(shellMessage.seq);
          if (shellMessage && shellMessage.type !== 'duplicate') emitStatus('shell:message', shellMessage);
          publishWsHealth('shellWs', 'ok', 'shell-bin-v1');
        }, function(errBin) {
          shellBinaryStats().errors++;
          requestShellJsonFallback(errBin && errBin.message ? errBin.message : 'decode');
        });
        return;
      }
      var statsText = shellBinaryStats();
      statsText.jsonFrames++;
      statsText.jsonBytes += String(e.data || '').length;
      var incoming = null;
      try { incoming = JSON.parse(e.data); } catch (err) { return; }
      if (incoming && incoming.auth) {
        if (typeof global.shellSetAuthReady === 'function') global.shellSetAuthReady(incoming.auth === 'ok');
        else if (global.shellTerm) global.shellTerm.authReady = (incoming.auth === 'ok');
        publishWsHealth('shellWs', incoming.auth === 'ok' ? 'ok' : 'fail', incoming.channel || '');
        if (incoming.auth === 'ok') {
          if (incoming.formats && incoming.formats.indexOf('shell-bin-v1') >= 0 &&
              global.shellWs && global.shellWs.readyState === WebSocket.OPEN) {
            shellBufferedSend(global.shellWs, 'FORMAT:shell-bin-v1', 131072);
          }
          if (incoming.formats && incoming.formats.indexOf('shell-cbor-v1') >= 0 &&
              global.shellWs && global.shellWs.readyState === WebSocket.OPEN) {
            shellBufferedSend(global.shellWs, 'FORMAT:shell-cbor-v1', 131072);
          }
          if (typeof global.shellSendState === 'function') global.shellSendState();
        }
        return;
      }
      if (incoming && incoming.shell && typeof global.shellHandleMessage === 'function') {
        if (incoming.shell.type === 'protocol' && incoming.shell.format === 'shell-cbor-v1') {
          global.shellCborControlReady = true;
          publishWsHealth('shellWsControl', 'ok', 'shell-cbor-v1');
          if (typeof global.shellSendState === 'function') global.shellSendState();
        }
        emitStatus('shell:message', incoming.shell);
      }
    };
    global.shellWs.onclose = function() {
      publishWsHealth('shellWs', 'closed');
      global.shellCborControlReady = false;
      if (typeof global.shellSetAuthReady === 'function') {
        global.shellSetAuthReady(false);
      } else if (global.shellTerm) {
        global.shellTerm.authReady = false;
        global.shellTerm.busy = false;
        global.shellTerm.ready = false;
      }
      if (global.ws && global.ws.readyState === WebSocket.OPEN) {
        setTimeout(connectShellWS, 2000);
      }
    };
  }

  function connectDebugWS(enableStream) {
    global.debugSubscriptionWanted = !!enableStream;
    if (global.debugWs && (global.debugWs.readyState === WebSocket.OPEN || global.debugWs.readyState === WebSocket.CONNECTING)) {
      notifyDebugSubscription(global.debugSubscriptionWanted);
      return;
    }
    var wsProto = (window.location.protocol === 'https:') ? 'wss://' : 'ws://';
    global.debugWsAuthReady = false;
    global.debugWs = new WebSocket(wsProto + window.location.host + '/ws/debug');
    global.debugWs.onopen = function() {
      publishWsHealth('debugWs', 'auth');
      requestWsTicket('debug')
        .then(function(tok) {
          if (tok && global.debugWs && global.debugWs.readyState === WebSocket.OPEN) global.debugWs.send('AUTH:' + tok);
          else if (global.debugWs) global.debugWs.close();
        })
        .catch(function() {
          publishWsHealth('debugWs', 'ticket-fail');
          if (global.debugWs) global.debugWs.close();
        });
    };
    global.debugWs.onmessage = function(e) {
      var incoming = null;
      try { incoming = JSON.parse(e.data); } catch (err) { return; }
      if (incoming && incoming.auth) {
        global.debugWsAuthReady = (incoming.auth === 'ok');
        publishWsHealth('debugWs', global.debugWsAuthReady ? 'ok' : 'fail', incoming.channel || '');
        if (global.debugWsAuthReady) notifyDebugSubscription(global.debugSubscriptionWanted);
        return;
      }
      emitStatus('debug:update', incoming && incoming.debug ? incoming.debug : incoming);
    };
    global.debugWs.onclose = function() {
      global.debugWsAuthReady = false;
      publishWsHealth('debugWs', 'closed');
      if (global.debugSubscriptionWanted && global.ws && global.ws.readyState === WebSocket.OPEN) {
        setTimeout(function() { connectDebugWS(true); }, 2000);
      }
    };
  }

  function applyTelemetryPayload(incoming) {
    if (incoming && incoming.c3 && typeof incoming.c3 === 'object') incoming = Object.assign({}, incoming, incoming.c3);
    if (incoming && typeof incoming === 'object') global.wsTelemetryCache = Object.assign(global.wsTelemetryCache || {}, incoming);
    var d = global.wsTelemetryCache || {};
    var hasC3Update = !!(incoming && (incoming.c3_pos !== undefined || incoming.c3_spd !== undefined || incoming.c3_acc !== undefined || incoming.c3_connected !== undefined || incoming.c3_crc_err !== undefined || incoming.c3_marker_err !== undefined || incoming.c3_total_rx !== undefined || incoming.c3_quality !== undefined || incoming.c3_espnow_active !== undefined));
    if (typeof d.traj_scale !== 'undefined' && typeof global.ikSetTrajScaleUI === 'function') global.ikSetTrajScaleUI(d.traj_scale);
    if (typeof global.ikTransportState === 'object') {
      if (typeof d.spi_connected !== 'undefined') global.ikTransportState.spiConnected = !!d.spi_connected;
      if (typeof d.espnow_connected !== 'undefined') global.ikTransportState.espNowConnected = !!d.espnow_connected;
      if (typeof d.ik_backend === 'string') global.ikTransportState.serverBackend = String(d.ik_backend).toUpperCase();
    }
    if (typeof d.ik_pref === 'string' && typeof global.ikSetComputationPreference === 'function') global.ikSetComputationPreference(d.ik_pref);
    if (incoming && incoming.robot_math && typeof incoming.robot_math === 'object' && typeof global.ikSetMathState === 'function') global.ikSetMathState(incoming.robot_math, true);
    else if (d.robot_math && typeof d.robot_math === 'object' && typeof global.ikSetMathState === 'function') global.ikSetMathState(d.robot_math, true);
    if (incoming && incoming.robot_cmd && typeof global.handleRobotUiCommand === 'function') global.handleRobotUiCommand(incoming.robot_cmd);
    else if (d.robot_cmd && typeof global.handleRobotUiCommand === 'function') global.handleRobotUiCommand(d.robot_cmd);
    if (typeof global.ikRefreshModeIndicator === 'function') global.ikRefreshModeIndicator();

    var b_p4 = document.getElementById('badge-p4');
    var b_s3 = document.getElementById('badge-s3');
    var b_c3 = document.getElementById('badge-c3');
    var b_pca = document.getElementById('badge-pca');
    if (b_s3) { b_s3.style.backgroundColor = '#B5EA6A'; b_s3.style.color = '#121212'; }
    if (b_p4) {
      if (d.spi_connected) { b_p4.className = 'dev-badge ok'; b_p4.style.backgroundColor = '#B5EA6A'; b_p4.style.color = '#121212'; }
      else { b_p4.className = 'dev-badge offline'; b_p4.style.backgroundColor = '#EA6A6A'; b_p4.style.color = '#F0F2F3'; }
    }
    if (b_c3) {
      if (d.c3_connected) { b_c3.className = 'dev-badge ok'; b_c3.style.backgroundColor = '#B5EA6A'; b_c3.style.color = '#121212'; }
      else { b_c3.className = 'dev-badge offline'; b_c3.style.backgroundColor = '#EA6A6A'; b_c3.style.color = '#F0F2F3'; }
    }
    if (b_pca) {
      if (d.pca_ready) { b_pca.className = 'dev-badge ok'; b_pca.style.backgroundColor = '#B5EA6A'; b_pca.style.color = '#121212'; }
      else { b_pca.className = 'dev-badge offline'; b_pca.style.backgroundColor = '#EA6A6A'; b_pca.style.color = '#F0F2F3'; }
    }

    if (hasC3Update && d.c3_total_rx !== undefined && global.c3RxRateState) {
      var rxNow = Number(d.c3_total_rx);
      var nowMs = Date.now();
      if (isFinite(rxNow) && rxNow >= 0) {
        if (global.c3RxRateState.lastCount !== null && global.c3RxRateState.lastTsMs > 0) {
          var dtMs = nowMs - global.c3RxRateState.lastTsMs;
          if (dtMs > 0) {
            var delta = rxNow - global.c3RxRateState.lastCount;
            if (delta < 0) delta += 4294967296;
            global.c3RxRateState.accumPackets += delta;
            if (global.c3RxRateState.windowStartMs <= 0) global.c3RxRateState.windowStartMs = global.c3RxRateState.lastTsMs;
            var windowMs = nowMs - global.c3RxRateState.windowStartMs;
            if (windowMs >= 1000) {
              var windowSec = windowMs / 1000.0;
              global.c3RxRateState.rate = windowSec > 0 ? (global.c3RxRateState.accumPackets / windowSec) : 0;
              global.c3RxRateState.accumPackets = 0;
              global.c3RxRateState.windowStartMs = nowMs;
              var rxEl = document.getElementById('c3_total_rx');
              if (rxEl) rxEl.innerText = global.c3RxRateState.rate.toFixed(1) + ' pkt/s';
            }
          }
        } else {
          global.c3RxRateState.windowStartMs = nowMs;
        }
        global.c3RxRateState.lastCount = rxNow;
        global.c3RxRateState.lastTsMs = nowMs;
      }
    }

    if (hasC3Update) {
      var c3StatusEl = document.getElementById('c3_status_disp');
      if (c3StatusEl) {
        var c3EspNowConnected = !!d.espnow_connected || String(d.ik_backend || '').toUpperCase() === 'P4-ESP-NOW';
        var c3EspNowActive = c3EspNowConnected || !!d.c3_espnow_active;
        if (c3EspNowConnected) { c3StatusEl.innerText = 'BAGLI'; c3StatusEl.className = 'badge green'; }
        else if (c3EspNowActive) { c3StatusEl.innerText = 'AKTIF'; c3StatusEl.className = 'badge yellow'; }
        else { c3StatusEl.innerText = 'KAPALI'; c3StatusEl.className = 'badge red'; }
      }
      var connC3 = document.getElementById('uart_conn_disp');
      if (connC3) {
        connC3.innerText = d.c3_connected ? 'BAGLI' : 'KOPUK';
        connC3.className = d.c3_connected ? 'badge green' : 'badge red';
      }
    }

    if (hasC3Update && d.c3_pos !== undefined && global.encCalLive) {
      global.encCalLive.pos = Number(d.c3_pos);
      global.encCalLive.spd = Number(d.c3_spd);
      global.encCalLive.acc = Number(d.c3_acc);
      global.encCalLive.tsMs = Date.now();
      if (typeof global.encCalRefreshLiveReadout === 'function') global.encCalRefreshLiveReadout();
      var q = document.getElementById('c3_quality');
      var c3Pos = document.getElementById('c3_pos_disp');
      var c3Spd = document.getElementById('c3_spd_disp');
      var c3Acc = document.getElementById('c3_acc_disp');
      var uartCrc = document.getElementById('uart_crc_err');
      var c3Marker = document.getElementById('c3_marker_err');
      if (c3Pos) c3Pos.innerText = d.c3_pos.toFixed(2) + '°';
      if (c3Spd) c3Spd.innerText = d.c3_spd.toFixed(1);
      if (c3Acc) c3Acc.innerText = d.c3_acc.toFixed(1);
      if (uartCrc) uartCrc.innerText = d.c3_crc_err || 0;
      if (c3Marker) c3Marker.innerText = d.c3_marker_err || 0;
      if (q && d.c3_quality !== undefined) {
        q.innerText = d.c3_quality.toFixed(2) + '%';
        q.style.color = d.c3_quality > 95 ? '#B5EA6A' : '#EA6A6A';
      }
    }

    var spiStatus = document.getElementById('spi_conn_status');
    if (spiStatus) {
      if (d.spi_connected) { spiStatus.innerText = 'SPI OK'; spiStatus.className = 'badge green'; }
      else { spiStatus.innerText = 'SPI KOPUK'; spiStatus.className = 'badge red'; }
    }
    var loopEl = document.getElementById('loop_ms');
    if (loopEl && d.p4_loop !== undefined) loopEl.innerText = d.p4_loop;

    var powerBtn = document.getElementById('btn-pwr');
    if (d.motor !== undefined) global.lastMotorState = Number(d.motor);
    if (powerBtn) {
      if (global.lastMotorState === 1) { powerBtn.innerText = 'MOTORLARI KAPA'; powerBtn.style.backgroundColor = '#B5EA6A'; powerBtn.style.color = '#121212'; }
      else { powerBtn.innerText = 'MOTORLARI AC'; powerBtn.style.backgroundColor = '#EA6A6A'; powerBtn.style.color = '#F0F2F3'; }
    }

    var oeBtn = document.getElementById('btn-oe');
    var oeStatus = document.getElementById('oe_status');
    if (d.oe !== undefined) global.lastOeState = Number(d.oe);
    if (oeStatus) {
      if (global.lastOeState === 1) { oeStatus.innerText = 'ACTIVE'; oeStatus.className = 'badge green'; }
      else { oeStatus.innerText = 'STOPPED'; oeStatus.className = 'badge red'; }
    }
    if (oeBtn) {
      if (global.lastOeState === 1) { oeBtn.innerText = 'MOTORLARI DEVRE DIŞI BIRAK'; oeBtn.style.background = 'var(--accent-red)'; oeBtn.style.color = '#F0F2F3'; }
      else { oeBtn.innerText = 'MOTORLARI DEVREYE AL'; oeBtn.style.background = 'var(--accent-green)'; oeBtn.style.color = '#121212'; }
    }

    var targetTurret = (d.turret !== undefined) ? d.turret : 0.0;
    var actualTurret = (typeof global.rawEncoderToTurretDeg === 'function') ? global.rawEncoderToTurretDeg(d.c3_pos) : targetTurret;
    var turretDisp = document.getElementById('ang_t');
    var valT = document.getElementById('val_t');
    var pidTgt = document.getElementById('pid_target');
    var pidAct = document.getElementById('pid_actual');
    var pidErr = document.getElementById('pid_error');
    if (turretDisp) turretDisp.innerText = (isFinite(actualTurret) ? actualTurret : targetTurret).toFixed(2);
    if (valT) valT.innerText = targetTurret.toFixed(2);
    if (pidTgt) pidTgt.innerText = targetTurret.toFixed(2) + '°';
    if (pidAct) pidAct.innerText = isFinite(actualTurret) ? (actualTurret.toFixed(2) + '°') : '--';
    if (pidErr && isFinite(actualTurret)) {
      var err = targetTurret - actualTurret;
      pidErr.innerText = err.toFixed(2) + '°';
      pidErr.style.color = Math.abs(err) < 1.0 ? '#B5EA6A' : '#EAB96A';
    }
    if (typeof global.updatePIDOptInfoBar === 'function') global.updatePIDOptInfoBar(d.pid_out, targetTurret, d.c3_pos);
    if (d.c3_pos !== undefined && typeof global.addPIDOptSample === 'function') global.addPIDOptSample(targetTurret, actualTurret, Number(d.c3_pos), Number(d.c3_spd));
    var pidOut = document.getElementById('pid_out');
    if (d.pid_out !== undefined && pidOut) pidOut.innerText = Number(d.pid_out).toFixed(1);

    if (d.joints && d.joints.length === 6 && typeof global.updateRobotAngles === 'function') {
      var tVal = isFinite(actualTurret) ? actualTurret : d.turret;
      global.updateRobotAngles([tVal, d.joints[0], d.joints[1], d.joints[2], d.joints[3], d.joints[4], d.joints[5]]);
    }
    if (d.coord_x !== undefined && typeof global.ikUpdateActualPoseDisplay === 'function' && typeof global.ikPoseFromTelemetryPayload === 'function') {
      global.ikUpdateActualPoseDisplay(global.ikPoseFromTelemetryPayload(d));
    }
    if (global.ikFallbackModeEnabled && typeof global.ikGetComputationMode === 'function' && global.ikGetComputationMode() === 'WEB' && typeof global.ikPoseFromAnglesDeg === 'function' && typeof global.getSliderAngles === 'function' && typeof global.ikUpdateActualPoseDisplay === 'function') {
      var localPose = global.ikPoseFromAnglesDeg(global.getSliderAngles());
      if (localPose) global.ikUpdateActualPoseDisplay(localPose);
    }

    var spiCrcErr = document.getElementById('spi_crc_err');
    var spiMarkerErr = document.getElementById('spi_marker_err');
    if (d.spi_total !== undefined) {
      if (spiCrcErr) spiCrcErr.innerText = d.spi_crc_err;
      if (spiMarkerErr) spiMarkerErr.innerText = d.spi_marker_err;
    }
    if (d.spi_err_rev !== undefined && d.spi_err_rev !== global.lastSpiErrRev) {
      global.lastSpiErrRev = d.spi_err_rev;
      if (typeof global.fetchSpiErrors === 'function') global.fetchSpiErrors();
    }
    if (d.console_rev !== undefined && d.console_rev !== global.lastConsoleRev) {
      global.lastConsoleRev = d.console_rev;
      var nowMs = Date.now();
      if (nowMs - global.lastConsoleFetchMs > 500) {
        global.lastConsoleFetchMs = nowMs;
        if (typeof global.fetchLogs === 'function') global.fetchLogs();
      }
    }

    var wifiBtn = document.getElementById('btn-wifi-setup');
    if (wifiBtn) {
      if (d.wifi_ap) {
        wifiBtn.style.display = 'flex';
        wifiBtn.style.border = '2px solid var(--primary)';
        wifiBtn.style.boxShadow = 'none';
      } else {
        wifiBtn.style.display = 'none';
      }
    }
  }

  function queueTelemetryUpdate(incoming) {
    if (incoming && typeof incoming === 'object') {
      global.pendingTelemetryPatch = Object.assign(global.pendingTelemetryPatch || {}, incoming);
    }
    if (global.telemetryApplyQueued) return;
    global.telemetryApplyQueued = true;
    requestAnimationFrame(function() {
      global.telemetryApplyQueued = false;
      var framePatch = global.pendingTelemetryPatch || {};
      global.pendingTelemetryPatch = null;
      applyTelemetryPayload(framePatch);
    });
  }

  function queueSlowTelemetryUpdate(incoming) {
    if (incoming && typeof incoming === 'object') {
      global.pendingSlowTelemetryPatch = Object.assign(global.pendingSlowTelemetryPatch || {}, incoming);
    }
    if (global.telemetrySlowApplyTimer) return;
    global.telemetrySlowApplyTimer = setTimeout(function() {
      global.telemetrySlowApplyTimer = null;
      var patch = global.pendingSlowTelemetryPatch || {};
      global.pendingSlowTelemetryPatch = null;
      queueTelemetryUpdate(patch);
    }, 250);
  }

  function telemetryHasAny(payload, keys) {
    if (!payload || typeof payload !== 'object') return false;
    for (var i = 0; i < keys.length; i++) {
      if (payload[keys[i]] !== undefined) return true;
    }
    return false;
  }

  var BIN_HEADER_LEN = 28;
  var BIN_FIELDS = [
    ['turret', 'i32', 10],
    ['gripper', 'i32', 10],
    ['joints', 'i32x6', 10],
    ['coord_x', 'i32', 10],
    ['coord_y', 'i32', 10],
    ['coord_z', 'i32', 10],
    ['coord_roll', 'i32', 10],
    ['coord_pitch', 'i32', 10],
    ['coord_yaw', 'i32', 10],
    ['alpha', 'i32', 10],
    ['lp', 'i32', 1],
    ['fk_x', 'i32', 10],
    ['fk_y', 'i32', 10],
    ['fk_z', 'i32', 10],
    ['fk_a', 'i32', 10],
    ['p4_loop', 'u32', 1],
    ['motor', 'i32', 1],
    ['spi_total', 'u32', 1],
    ['spi_crc_err', 'u32', 1],
    ['spi_marker_err', 'u32', 1],
    ['spi_err_rev', 'u32', 1],
    ['spi_last_marker', 'i32', 1],
    ['spi_connected', 'bool', 1],
    ['s3_devstat', 'i32', 1],
    ['espnow_connected', 'bool', 1],
    ['c3_pos', 'i32', 100],
    ['c3_spd', 'i32', 10],
    ['c3_acc', 'i32', 10],
    ['c3_connected', 'bool', 1],
    ['c3_espnow_active', 'bool', 1],
    ['c3_crc_err', 'u32', 1],
    ['c3_marker_err', 'u32', 1],
    ['c3_total_rx', 'u32', 1],
    ['c3_quality', 'i32', 100],
    ['c3_hz', 'u32', 1],
    ['pid_out', 'i32', 10],
    ['traj_scale', 'i32', 100],
    ['oe', 'i32', 1],
    ['uptime', 'u32', 1],
    ['console_rev', 'u32', 1],
    ['pca_ready', 'bool', 1],
    ['wifi_ap', 'bool', 1]
  ];

  function hasBinField(mask0, mask1, id) {
    if (id < 32) return (mask0 & (1 << id)) !== 0;
    return (mask1 & (1 << (id - 32))) !== 0;
  }

  function decodeTelemetryBinaryFrame(buffer) {
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < BIN_HEADER_LEN) {
      throw new Error('bin-short');
    }
    var view = new DataView(buffer);
    if (view.getUint8(0) !== 0x4d || view.getUint8(1) !== 0x52 ||
        view.getUint8(2) !== 0x42 || view.getUint8(3) !== 0x31) {
      throw new Error('bin-magic');
    }
    if (view.getUint8(4) !== 1) throw new Error('bin-version');
    var headerLen = view.getUint8(5);
    if (headerLen < BIN_HEADER_LEN || buffer.byteLength < headerLen) {
      throw new Error('bin-header');
    }
    var payloadLen = view.getUint32(16, true);
    if ((headerLen + payloadLen) !== buffer.byteLength) {
      throw new Error('bin-length');
    }
    var mask0 = view.getUint32(20, true);
    var mask1 = view.getUint32(24, true);
    var offset = headerLen;
    var out = {
      schema_version: 1,
      frame_type: 'bin-v1',
      bin_type: view.getUint8(6),
      bin_seq: view.getUint32(8, true),
      server_ms: view.getUint32(12, true)
    };
    var c3 = null;
    for (var id = 0; id < BIN_FIELDS.length; id++) {
      if (!hasBinField(mask0, mask1, id)) continue;
      var spec = BIN_FIELDS[id];
      var name = spec[0];
      var type = spec[1];
      var scale = spec[2] || 1;
      var value;
      if (type === 'i32') {
        if ((offset + 4) > buffer.byteLength) throw new Error('bin-field');
        value = view.getInt32(offset, true);
        offset += 4;
        if (scale !== 1) value = value / scale;
      } else if (type === 'u32') {
        if ((offset + 4) > buffer.byteLength) throw new Error('bin-field');
        value = view.getUint32(offset, true);
        offset += 4;
      } else if (type === 'bool') {
        if ((offset + 1) > buffer.byteLength) throw new Error('bin-field');
        value = view.getUint8(offset) !== 0;
        offset += 1;
      } else if (type === 'i32x6') {
        if ((offset + 24) > buffer.byteLength) throw new Error('bin-field');
        value = [];
        for (var j = 0; j < 6; j++) {
          var q = view.getInt32(offset, true);
          offset += 4;
          value.push(scale !== 1 ? q / scale : q);
        }
      }
      out[name] = value;
      if (name.indexOf('c3_') === 0) {
        c3 = c3 || {};
        c3[name] = value;
      }
    }
    if (offset !== buffer.byteLength) throw new Error('bin-trailing');
    if (c3) out.c3 = c3;
    return out;
  }

  function routeTelemetryPayload(incoming) {
    if (!incoming || typeof incoming !== 'object') return;
    var routed = false;
    emitStatus('telemetry:raw', incoming);
    if (telemetryHasAny(incoming, ['turret', 'gripper', 'pid_out', 'motor', 'oe', 'traj_scale'])) {
      routed = true;
      emitStatus('telemetry:fast', incoming);
    }
    if (telemetryHasAny(incoming, ['joints', 'coord_x', 'coord_y', 'coord_z', 'coord_roll', 'coord_pitch', 'coord_yaw', 'alpha', 'fk_x', 'fk_y', 'fk_z', 'fk_a', 'lp'])) {
      routed = true;
      emitStatus('telemetry:scene', incoming);
    }
    if (telemetryHasAny(incoming, ['p4_loop', 'uptime', 'spi_total', 'spi_crc_err', 'spi_marker_err', 'spi_err_rev', 'spi_connected', 'espnow_connected', 'c3', 'c3_pos', 'console_rev', 'pca_ready', 'wifi_ap', 'ik_backend', 'ik_pref', 'robot_math', 'robot_cmd', 'telemetry_budget_mode', 'telemetry_budget_free', 'telemetry_budget_largest'])) {
      routed = true;
      emitStatus('telemetry:system', incoming);
    }
    if (!routed) emitStatus('telemetry:fast', incoming);
  }

  function queueTelemetryPayload(incoming) {
    if (!incoming || typeof incoming !== 'object') return;
    global.pendingTelemetryRoutePatch =
      Object.assign(global.pendingTelemetryRoutePatch || {}, incoming);
    global.mrosTelemetryRouteStats = global.mrosTelemetryRouteStats || {
      frames: 0,
      batches: 0
    };
    global.mrosTelemetryRouteStats.frames++;
    if (global.telemetryRouteQueued) return;
    global.telemetryRouteQueued = true;
    requestAnimationFrame(function() {
      global.telemetryRouteQueued = false;
      var patch = global.pendingTelemetryRoutePatch || {};
      global.pendingTelemetryRoutePatch = null;
      global.mrosTelemetryRouteStats.batches++;
      routeTelemetryPayload(patch);
    });
  }

  function connectWS() {
    var wsProto = (window.location.protocol === 'https:') ? 'wss://' : 'ws://';
    global.wsTelemetryCache = {};
    global.pendingTelemetryPatch = null;
    global.pendingSlowTelemetryPatch = null;
    global.pendingTelemetryRoutePatch = null;
    global.telemetryApplyQueued = false;
    global.telemetryRouteQueued = false;
    global.telemetrySlowApplyTimer = null;
    global.lastMotorState = null;
    global.lastOeState = null;
    global.ws = new WebSocket(wsProto + window.location.host + '/ws/telemetry');
    global.ws.binaryType = 'arraybuffer';
    global.ws.onopen = function() {
      publishWsHealth('telemetryWs', 'auth');
      requestWsTicket('telemetry')
        .then(function(tok) {
          if (tok) global.ws.send('AUTH:' + tok);
          else global.ws.close();
        })
        .catch(function() {
          publishWsHealth('telemetryWs', 'ticket-fail');
          global.ws.close();
        });
      if (typeof global.fetchSpiErrors === 'function') global.fetchSpiErrors();
      if (typeof global.fetchLogs === 'function') global.fetchLogs();
      if (typeof global.fetchPIDConfig === 'function') global.fetchPIDConfig();
    };
    global.ws.onmessage = function(e) {
      if (e.data instanceof ArrayBuffer) {
        decodeBinaryViaWorker('telemetry', e.data, decodeTelemetryBinaryFrame, function(binaryPatch, meta) {
          global.mrosTelemetryBinaryStats = global.mrosTelemetryBinaryStats || { frames: 0, bytes: 0, errors: 0, fallbacks: 0 };
          global.mrosTelemetryBinaryStats.frames++;
          global.mrosTelemetryBinaryStats.bytes += (meta && meta.bytes) || (e.data && e.data.byteLength) || 0;
          publishWsHealth('telemetryWs', 'ok', 'bin-v1');
          queueTelemetryPayload(binaryPatch);
        }, function(errBin) {
          global.mrosTelemetryBinaryStats = global.mrosTelemetryBinaryStats || { frames: 0, bytes: 0, errors: 0, fallbacks: 0 };
          global.mrosTelemetryBinaryStats.errors++;
          global.mrosTelemetryBinaryStats.fallbacks++;
          publishWsHealth('telemetryWs', 'binary-fallback', errBin && errBin.message ? errBin.message : 'decode');
          try { if (global.ws && global.ws.readyState === WebSocket.OPEN) global.ws.send('FORMAT:json-v1'); } catch (sendErr) {}
        });
        return;
      }
      var incoming = null;
      try { incoming = JSON.parse(e.data); } catch (err) { return; }
      if (incoming && incoming.auth) {
        if (incoming.auth === 'ok') {
          publishWsHealth('telemetryWs', 'ok', incoming.channel || '');
          if (global.ws && global.ws.readyState === WebSocket.OPEN &&
              incoming.formats && incoming.formats.indexOf('bin-v1') >= 0) {
            try { global.ws.send('FORMAT:bin-v1'); } catch (fmtErr) {}
          }
          if (typeof global.ikSendTrajScaleViaWs === 'function') global.ikSendTrajScaleViaWs();
          notifyBaseSubscriptions();
          connectShellWS();
          connectDebugWS(false);
        } else if (global.shellTerm) {
          publishWsHealth('telemetryWs', 'fail');
          global.shellTerm.authReady = false;
        }
        return;
      }
      if (incoming && incoming.shell && typeof global.shellHandleMessage === 'function') {
        emitStatus('shell:message', incoming.shell);
        return;
      }
      queueTelemetryPayload(incoming);
    };
    global.ws.onclose = function() {
      publishWsHealth('telemetryWs', 'closed');
      global.wsTelemetryCache = {};
      global.pendingTelemetryRoutePatch = null;
      global.telemetryRouteQueued = false;
      global.pendingSlowTelemetryPatch = null;
      if (global.telemetrySlowApplyTimer) {
        clearTimeout(global.telemetrySlowApplyTimer);
        global.telemetrySlowApplyTimer = null;
      }
      global.lastMotorState = null;
      global.lastOeState = null;
      if (global.shellTerm) {
        global.shellTerm.authReady = false;
        global.shellTerm.busy = false;
        global.shellTerm.ready = false;
      }
      if (global.shellWs) {
        try { global.shellWs.close(); } catch (err) {}
        global.shellWs = null;
      }
      if (global.debugWs) {
        try { global.debugWs.close(); } catch (err) {}
        global.debugWs = null;
      }
      if (typeof global.ikTransportState === 'object') {
        global.ikTransportState.spiConnected = false;
        global.ikTransportState.espNowConnected = false;
        global.ikTransportState.serverBackend = 'WEB';
      }
      if (typeof global.ikRefreshModeIndicator === 'function') global.ikRefreshModeIndicator();
      setTimeout(connectWS, 2000);
    };
  }

  if (statusBus()) {
    statusBus().on('telemetry:fast', queueTelemetryUpdate);
    statusBus().on('telemetry:scene', queueTelemetryUpdate);
    statusBus().on('telemetry:system', queueSlowTelemetryUpdate);
    statusBus().on('shell:message', function(shellMessage) {
      if (shellMessage && typeof global.shellHandleMessage === 'function') {
        global.shellHandleMessage(shellMessage);
      }
    });
  }

  root.core.wsClient = {
    requestWsTicket: requestWsTicket,
    publishWsHealth: publishWsHealth,
    shellSocketInstance: shellSocketInstance,
    notifySceneSubscription: notifySceneSubscription,
    notifyDebugSubscription: notifyDebugSubscription,
    notifyBaseSubscriptions: notifyBaseSubscriptions,
    connectShellWS: connectShellWS,
    shellSendControlFrame: shellSendControlFrame,
    connectDebugWS: connectDebugWS,
    connectWS: connectWS
  };

  global.requestWsTicket = requestWsTicket;
  global.shellSocketInstance = shellSocketInstance;
  global.notifySceneSubscription = notifySceneSubscription;
  global.notifyDebugSubscription = notifyDebugSubscription;
  global.notifyBaseSubscriptions = notifyBaseSubscriptions;
  global.connectShellWS = connectShellWS;
  global.shellSendControlFrame = shellSendControlFrame;
  global.connectDebugWS = connectDebugWS;
  global.applyTelemetryPayload = applyTelemetryPayload;
  global.queueTelemetryUpdate = queueTelemetryUpdate;
  global.queueSlowTelemetryUpdate = queueSlowTelemetryUpdate;
  global.queueTelemetryPayload = queueTelemetryPayload;
  global.routeTelemetryPayload = routeTelemetryPayload;
  global.connectWS = connectWS;

  document.addEventListener('visibilitychange', function() {
    notifySceneSubscription();
    notifyTelemetryRateProfile();
  });
})(window);
