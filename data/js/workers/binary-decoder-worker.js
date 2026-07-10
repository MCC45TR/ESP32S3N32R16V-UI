(function() {
  'use strict';

  var SHELL_BIN_HEADER_LEN = 32;
  var BIN_HEADER_LEN = 28;
  var TELEMETRY_MAGIC = 'MRB1';
  var SHELL_MAGIC = 'MSH1';
  var shellTextDecoder = typeof TextDecoder !== 'undefined' ? new TextDecoder('utf-8') : null;

  var BIN_FIELDS = [
    ['turret', 'i32', 10], ['gripper', 'i32', 10], ['joints', 'i32x6', 10],
    ['coord_x', 'i32', 10], ['coord_y', 'i32', 10], ['coord_z', 'i32', 10],
    ['coord_roll', 'i32', 10], ['coord_pitch', 'i32', 10], ['coord_yaw', 'i32', 10],
    ['alpha', 'i32', 10], ['lp', 'i32', 1], ['fk_x', 'i32', 10],
    ['fk_y', 'i32', 10], ['fk_z', 'i32', 10], ['fk_a', 'i32', 10],
    ['p4_loop', 'u32', 1], ['motor', 'i32', 1], ['spi_total', 'u32', 1],
    ['spi_crc_err', 'u32', 1], ['spi_marker_err', 'u32', 1], ['spi_err_rev', 'u32', 1],
    ['spi_last_marker', 'i32', 1], ['spi_connected', 'bool', 1], ['s3_devstat', 'i32', 1],
    ['espnow_connected', 'bool', 1], ['c3_pos', 'i32', 100], ['c3_spd', 'i32', 10],
    ['c3_acc', 'i32', 10], ['c3_connected', 'bool', 1], ['c3_espnow_active', 'bool', 1],
    ['c3_crc_err', 'u32', 1], ['c3_marker_err', 'u32', 1], ['c3_total_rx', 'u32', 1],
    ['c3_quality', 'i32', 100], ['c3_hz', 'u32', 1], ['pid_out', 'i32', 10],
    ['traj_scale', 'i32', 100], ['oe', 'i32', 1], ['uptime', 'u32', 1],
    ['console_rev', 'u32', 1], ['pca_ready', 'bool', 1], ['wifi_ap', 'bool', 1]
  ];

  function hasBinField(mask0, mask1, id) {
    if (id < 32) return (mask0 & (1 << id)) !== 0;
    return (mask1 & (1 << (id - 32))) !== 0;
  }

  function decodeTelemetryBinaryFrame(buffer) {
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < BIN_HEADER_LEN) throw new Error('bin-short');
    var view = new DataView(buffer);
    if (view.getUint8(0) !== 0x4d || view.getUint8(1) !== 0x52 ||
        view.getUint8(2) !== 0x42 || view.getUint8(3) !== 0x31) throw new Error('bin-magic');
    if (view.getUint8(4) !== 1) throw new Error('bin-version');
    var headerLen = view.getUint8(5);
    if (headerLen < BIN_HEADER_LEN || buffer.byteLength < headerLen) throw new Error('bin-header');
    var payloadLen = view.getUint32(16, true);
    if ((headerLen + payloadLen) !== buffer.byteLength) throw new Error('bin-length');
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

  function decodeShellBinaryFrame(buffer) {
    if (!(buffer instanceof ArrayBuffer) || buffer.byteLength < SHELL_BIN_HEADER_LEN) throw new Error('short shell frame');
    var view = new DataView(buffer);
    if (view.getUint8(0) !== 0x4D || view.getUint8(1) !== 0x53 ||
        view.getUint8(2) !== 0x48 || view.getUint8(3) !== 0x31) throw new Error('bad shell magic');
    if (view.getUint8(4) !== 1 || view.getUint8(5) !== SHELL_BIN_HEADER_LEN) throw new Error('unsupported shell frame');
    var frameType = view.getUint8(6);
    var flags = view.getUint8(7);
    var seq = view.getUint32(8, true);
    var sessionId = view.getUint32(20, true);
    var paneId = view.getUint16(24, true);
    var commandId = view.getUint16(26, true);
    var payloadLen = view.getUint32(28, true);
    if (payloadLen !== (buffer.byteLength - SHELL_BIN_HEADER_LEN)) throw new Error('shell payload length');
    var payload = buffer.slice(SHELL_BIN_HEADER_LEN);
    if (!shellTextDecoder && typeof TextDecoder !== 'undefined') shellTextDecoder = new TextDecoder('utf-8');
    var decoder = shellTextDecoder;
    if (frameType === 2) {
      return {
        type: 'stream',
        output: decoder ? decoder.decode(payload, { stream: true }) : '',
        pane_id: paneId,
        session_id: sessionId,
        command_id: commandId,
        binary: true,
        seq: seq,
        flags: flags
      };
    }
    if (frameType === 4) {
      var finalText = decoder ? decoder.decode(payload, { stream: false }) : '{}';
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
        message: decoder ? decoder.decode(payload, { stream: false }) : 'shell binary error',
        pane_id: paneId,
        session_id: sessionId,
        command_id: commandId,
        binary: true,
        seq: seq
      };
    }
    return { type: 'metric', binary: true, seq: seq, pane_id: paneId, session_id: sessionId };
  }

  self.onmessage = function(ev) {
    var msg = ev.data || {};
    var started = (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();
    try {
      var payload = msg.channel === 'shell' ? decodeShellBinaryFrame(msg.buffer)
                                            : decodeTelemetryBinaryFrame(msg.buffer);
      var ended = (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();
      self.postMessage({
        id: msg.id,
        ok: true,
        channel: msg.channel,
        bytes: msg.buffer ? msg.buffer.byteLength : 0,
        latency_ms: Math.max(0, ended - started),
        payload: payload
      });
    } catch (err) {
      var failed = (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();
      self.postMessage({
        id: msg.id,
        ok: false,
        channel: msg.channel,
        bytes: msg.buffer ? msg.buffer.byteLength : 0,
        latency_ms: Math.max(0, failed - started),
        error: err && err.message ? err.message : String(err || 'decode')
      });
    }
  };
})();
