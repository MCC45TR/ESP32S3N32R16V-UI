(function(global) {
  var root = global.MROS = global.MROS || {};
  root.panels = root.panels || {};

  var consolePanel = {
    mode: 'log',
    logText: ''
  };

  function createShellTerm(paneId) {
    return {
      paneId: paneId || 0,
      label: 'S3',
      target: 's3',
    transcript: '',
    segments: [],
    prompt: '',
    input: '',
    cursor: 0,
    history: [],
    historyIndex: -1,
    pendingLogs: [],
    busy: false,
    ready: false,
    authReady: false,
    attached: false,
    maxChars: 24000,
    lastResizeCols: 0,
    lastResizeRows: 0,
    resizeTimer: null,
    resizeObserver: null,
    fullscreenOpen: false,
    renderQueued: false,
    inputComposing: false,
    inputRevision: 0,
    inputRenderQueued: false,
    inputLastUserMs: 0,
    inputLocalFirst: true,
    completionRequestRevision: -1,
    completionRequestInput: '',
    suggestRequestRevision: -1,
    suggestRequestInput: '',
      commandSeq: 0,
      sawStreamForCommand: false,
      suggestion: '',
      suggestions: [],
      suggestionsMeta: [],
      completionMenuOpen: false,
      completionIndex: 0,
      historySearchOpen: false,
      historySearchQuery: '',
      syntaxSpans: [],
      smartQuoteHint: '',
      suggestTimer: null,
      historyMeta: [],
      activeHistoryRecord: null,
      pasteModeUntil: 0,
      virtualChars: 18000,
      clearCount: 0
    };
  }

  var shellTerm = createShellTerm(0);
  var shellPanes = [shellTerm];
  var shellActivePane = 0;
  var SHELL_WS_INPUT_HIGH_WATER = 64 * 1024;
  var SHELL_WS_CONTROL_HIGH_WATER = 128 * 1024;

  function activePaneId() { return shellTerm && typeof shellTerm.paneId === 'number' ? shellTerm.paneId : 0; }
  function shellIoStats() {
    global.mrosShellBinaryStats = global.mrosShellBinaryStats || {};
    var stats = global.mrosShellBinaryStats;
    stats.inputFrames = stats.inputFrames || 0;
    stats.inputBytes = stats.inputBytes || 0;
    stats.inputBackpressure = stats.inputBackpressure || 0;
    stats.inputSendErrors = stats.inputSendErrors || 0;
    stats.maxBufferedAmount = stats.maxBufferedAmount || 0;
    return stats;
  }
  function shellSendTextFrame(socket, payload, highWater) {
    if (!socket) return false;
    var stats = shellIoStats();
    var buffered = Number(socket.bufferedAmount || 0);
    if (buffered > stats.maxBufferedAmount) stats.maxBufferedAmount = buffered;
    if (buffered > (highWater || SHELL_WS_CONTROL_HIGH_WATER)) {
      stats.inputBackpressure++;
      try {
        window.dispatchEvent(new CustomEvent('mros-web-health', {
          detail: { key: 'shellWs', value: 'backpressure', info: String(buffered) }
        }));
      } catch (err) {}
      return false;
    }
    try {
      socket.send(payload);
      stats.inputFrames++;
      stats.inputBytes += String(payload || '').length;
      return true;
    } catch (errSend) {
      stats.inputSendErrors++;
      return false;
    }
  }
  function shellSendControlMessage(op, payload, highWater, commandId) {
    var socket = typeof global.shellSocketInstance === 'function' ? global.shellSocketInstance() : null;
    if (!socket || !shellTerm.authReady) return false;
    if (typeof global.shellSendControlFrame === 'function' &&
        global.shellSendControlFrame(op, activePaneId(), payload || '', commandId || 0, highWater || SHELL_WS_CONTROL_HIGH_WATER)) {
      return true;
    }
    var textOp = String(op || '').toUpperCase();
    return shellSendTextFrame(socket,
      'SHELL2:' + textOp + ':' + activePaneId() + ':' + String(payload || ''),
      highWater || SHELL_WS_CONTROL_HIGH_WATER);
  }
  function shellPaneById(paneId, create) {
    var id = Math.max(0, Math.min(3, Number(paneId) || 0));
    for (var i = 0; i < shellPanes.length; i++) {
      if (shellPanes[i].paneId === id) return shellPanes[i];
    }
    if (!create || shellPanes.length >= 4) return null;
    var pane = createShellTerm(id);
    pane.authReady = !!(shellTerm && shellTerm.authReady);
    shellPanes.push(pane);
    return pane;
  }
  function shellSetActivePane(paneId) {
    var pane = shellPaneById(paneId, false);
    if (!pane) return;
    var wasFullscreen = !!(shellTerm && shellTerm.fullscreenOpen);
    shellTerm = pane;
    shellTerm.fullscreenOpen = wasFullscreen;
    shellActivePane = pane.paneId;
    global.shellTerm = shellTerm;
    syncShellInputUi();
    shellRender();
    shellSendState();
  }
  function shellNextPaneId() {
    for (var id = 0; id < 4; id++) {
      if (!shellPaneById(id, false)) return id;
    }
    return -1;
  }
  function shellAddPane() {
    var next = shellNextPaneId();
    if (next < 0) return;
    shellPaneById(next, true);
    shellSetActivePane(next);
  }
  function shellCloseActivePane() {
    shellClosePane(activePaneId());
  }
  function shellClosePane(paneId) {
    if (shellPanes.length <= 1) return;
    var id = Math.max(0, Math.min(3, Number(paneId) || 0));
    shellPanes = shellPanes.filter(function(pane) { return pane.paneId !== id; });
    if (!shellPanes.length) {
      shellPaneById(0, true);
    }
    if (shellTerm && shellTerm.paneId === id) shellSetActivePane(shellPanes[0].paneId);
    else shellRender();
  }
  function shellConnectTarget(target) {
    target = String(target || 's3').toLowerCase();
    shellTerm.target = target;
    shellTerm.label = target.toUpperCase();
    shellTerm.input = target === 's3' ? 'exit' : ('mshell connect ' + target);
    shellTerm.cursor = shellTerm.input.length;
    shellRender();
    shellSendExec();
  }
  function shellSetAuthReady(ready) {
    shellPanes.forEach(function(pane) {
      pane.authReady = !!ready;
      if (!ready) {
        pane.busy = false;
        pane.ready = false;
      }
    });
    shellTerm.authReady = !!ready;
    if (!ready) {
      shellTerm.busy = false;
      shellTerm.ready = false;
    }
  }

  function shellPrimaryTerminalEl() { return document.getElementById('log-text'); }
  function shellFullscreenTerminalEl() { return document.getElementById('shell-fullscreen-text'); }
  function shellSetFullscreenStatus(message) {
    var statusEl = document.getElementById('shell-fullscreen-status');
    if (statusEl && message) statusEl.textContent = message;
  }
  function shellTerminalEl() {
    var modalEl = shellFullscreenTerminalEl();
    if (shellTerm.fullscreenOpen && modalEl) return modalEl;
    return shellPrimaryTerminalEl();
  }
  function shellTerminalSurfaces() {
    var surfaces = [];
    var main = shellPrimaryTerminalEl();
    var modal = shellFullscreenTerminalEl();
    if (main) surfaces.push({ el: main, includeInput: false });
    if (modal) surfaces.push({ el: modal, includeInput: true });
    return surfaces;
  }
  function shellInputEl() { return document.getElementById('console-command-input'); }
  function shellVisibleText() { return consolePanel.mode === 'shell' ? shellRenderText() : consolePanel.logText; }

  function shellInputStats() {
    global.mrosShellInputStats = global.mrosShellInputStats || {
      localFirst: true,
      userEdits: 0,
      forcedSyncs: 0,
      skippedRenderSyncs: 0,
      serverInputRewriteBlocked: 0,
      staleSuggestionDrops: 0,
      staleCompletionDrops: 0,
      renderBatches: 0,
      compositionEvents: 0
    };
    return global.mrosShellInputStats;
  }

  function shellSyncInputElement(force) {
    var inputEl = shellInputEl();
    if (!inputEl) return;
    var stats = shellInputStats();
    var focused = document.activeElement === inputEl;
    inputEl.disabled = consolePanel.mode !== 'shell';
    inputEl.placeholder = consolePanel.mode === 'shell' ? 'Komut yaz, Enter ile gonder...' : 'Konsol pasif';
    if (!force && (focused || shellTerm.inputComposing)) {
      stats.skippedRenderSyncs++;
      return;
    }
    if (inputEl.value !== shellTerm.input) {
      inputEl.value = shellTerm.input;
      stats.forcedSyncs++;
    }
    if ((force || focused) && !shellTerm.inputComposing) {
      try { inputEl.setSelectionRange(shellTerm.cursor, shellTerm.cursor); } catch (err) {}
    }
  }

  function shellScheduleInputRender() {
    if (shellTerm.inputRenderQueued) return;
    shellTerm.inputRenderQueued = true;
    requestAnimationFrame(function() {
      shellTerm.inputRenderQueued = false;
      shellInputStats().renderBatches++;
      if (shellTerm.fullscreenOpen) shellRender();
      else syncShellInputUi();
    });
  }

  function applyShellFullscreenUiSettings() {
    var s = (typeof global.terminalGetSettings === 'function') ? global.terminalGetSettings() : {};
    var refreshBtn = document.getElementById('terminal-refresh-btn');
    var killBtn = document.getElementById('terminal-kill-btn');
    var closePaneBtn = document.getElementById('terminal-close-pane-btn');
    if (refreshBtn) refreshBtn.style.display = (s.showRefreshButton === false) ? 'none' : '';
    if (killBtn) killBtn.style.display = (s.showKillButton === false) ? 'none' : '';
    if (closePaneBtn) closePaneBtn.disabled = shellPanes.length <= 1;
  }

  function ensureShellPaneToolbar() {
    var terminal = shellPrimaryTerminalEl();
    if (!terminal || document.getElementById('shell-pane-toolbar')) return;
    var toolbar = document.createElement('div');
    toolbar.id = 'shell-pane-toolbar';
    toolbar.style.cssText = 'display:none;gap:6px;align-items:center;margin:6px 0;flex-wrap:wrap;';
    toolbar.innerHTML =
      '<button type="button" data-shell-action="split">Konsolu Bol</button>' +
      '<button type="button" data-shell-action="close">Pane Kapat</button>' +
      '<select data-shell-action="target" title="Shell hedefi">' +
      '<option value="s3">S3 local</option><option value="p4">P4 UART</option><option value="c3">C3 via P4</option>' +
      '</select>' +
      '<input id="shell-api-quick" type="text" placeholder="api: system.status" style="min-width:180px;max-width:260px;flex:1;background:#121821;color:#e8edf5;border:1px solid #303744;border-radius:6px;padding:5px 7px;">' +
      '<button type="button" data-shell-action="api-run">API Call</button>' +
      '<button type="button" data-shell-action="jobs">Jobs</button>' +
      '<span id="shell-pane-status" style="color:#9fb4d6;font-size:12px;"></span>';
    terminal.parentNode.insertBefore(toolbar, terminal);
    toolbar.addEventListener('click', function(ev) {
      var action = ev.target && ev.target.getAttribute ? ev.target.getAttribute('data-shell-action') : '';
      if (action === 'split') shellAddPane();
      if (action === 'close') shellCloseActivePane();
      if (action === 'api-run') shellRunApiQuick();
      if (action === 'jobs') shellShowJobs();
    });
    toolbar.addEventListener('change', function(ev) {
      var action = ev.target && ev.target.getAttribute ? ev.target.getAttribute('data-shell-action') : '';
      if (action === 'target') shellConnectTarget(ev.target.value);
    });
  }

  function shellAppendSystemBlock(title, text) {
    shellRemovePromptTail();
    shellTerm.transcript += '\n[' + title + ']\n' + String(text || '') + '\n';
    shellResetSegmentsFromTranscript();
    shellEnsurePromptTail();
    shellClampTranscript();
    shellRender();
  }

  function shellRunApiQuick() {
    var input = document.getElementById('shell-api-quick');
    var api = input ? String(input.value || '').trim() : '';
    if (!api) api = 'system.status';
    var body = new URLSearchParams();
    if (/^(GET|POST|PUT|DELETE)\s+/i.test(api)) {
      var parts = api.split(/\s+/);
      body.set('method', parts[0].toUpperCase());
      body.set('path', parts[1] || '/system/status');
    } else {
      body.set('api', api);
    }
    fetch('/api/mshell/call', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: body.toString(),
      cache: 'no-store'
    }).then(function(resp) { return resp.text(); })
      .then(function(text) { shellAppendSystemBlock('mshell api', text); })
      .catch(function(err) { shellAppendSystemBlock('mshell api error', err && err.message ? err.message : String(err)); });
  }

  function shellShowJobs() {
    fetch('/api/mshell/jobs', { cache: 'no-store' })
      .then(function(resp) { return resp.text(); })
      .then(function(text) { shellAppendSystemBlock('mshell jobs', text); })
      .catch(function(err) { shellAppendSystemBlock('mshell jobs error', err && err.message ? err.message : String(err)); });
  }

  function syncShellPaneToolbar() {
    var toolbar = document.getElementById('shell-pane-toolbar');
    if (!toolbar) return;
    toolbar.style.display = consolePanel.mode === 'shell' ? 'flex' : 'none';
    var status = document.getElementById('shell-pane-status');
    if (status) status.textContent = shellPanes.length + '/4 pane · aktif pane ' + (activePaneId() + 1);
    var select = toolbar.querySelector('[data-shell-action="target"]');
    if (select && select.value !== shellTerm.target) select.value = shellTerm.target || 's3';
  }

  function syncShellInputUi() {
    var inputEl = shellInputEl();
    var prefixEl = document.getElementById('console-input-prefix');
    var sendBtn = document.getElementById('console-command-send');
    var panelRoot = document.getElementById('panel-logs-root');
    var shellMode = consolePanel.mode === 'shell';
    if (panelRoot) panelRoot.classList.toggle('console-shell-disabled', !shellMode);
    if (prefixEl) prefixEl.textContent = '';
    if (inputEl) {
      shellSyncInputElement(false);
    }
    var suggestEl = document.getElementById('shell-suggest-ghost');
    if (suggestEl) {
      var ghost = shellComputeGhostSuggestion();
      suggestEl.textContent = ghost;
      suggestEl.style.display = (shellMode && ghost) ? '' : 'none';
    }
    var aux = shellEnsureInputAux();
    if (aux.preview) {
      aux.preview.style.display = shellMode ? '' : 'none';
      aux.preview.innerHTML = shellBuildSyntaxPreview(shellTerm.input);
      aux.preview.classList.toggle('is-history-search', !!shellTerm.historySearchOpen);
    }
    shellRenderCompletionMenu(false);
    if (sendBtn) sendBtn.disabled = !shellMode || shellTerm.busy || !shellTerm.ready;
    syncShellPaneToolbar();
  }

  function shellRenderText() { return shellTerm.transcript; }
  function shellVisibleTranscript(text, pane) {
    var max = Math.max(2000, Number((pane || shellTerm).virtualChars || 18000));
    var value = String(text || '');
    if (value.length <= max) return value;
    return '[virtual terminal: onceki ' + (value.length - max) + ' karakter scrollback ring icinde tutuldu]\n' +
      value.slice(value.length - max);
  }
  function shellResetSegmentsFromTranscript() {
    shellTerm.segments = shellTerm.transcript ? [shellTerm.transcript] : [];
  }
  function shellRebuildTranscriptFromSegments() {
    shellTerm.transcript = (shellTerm.segments || []).join('');
  }
  function shellAppendTranscript(text) {
    var value = String(text || '');
    if (!value) return;
    if (/\u001bc|\u001b\[(2|3)?J/.test(value)) {
      var tail = value.replace(/^[\s\S]*?(?:\u001bc|\u001b\[(?:2|3)?J)/, '');
      shellTerm.transcript = '';
      shellTerm.segments = [];
      shellTerm.clearCount++;
      value = tail;
      if (!value) return;
    }
    if (value.indexOf('\r') >= 0) {
      value = value.replace(/\r(?!\n)/g, '\n');
    }
    if (!shellTerm.segments || !shellTerm.segments.length) {
      shellResetSegmentsFromTranscript();
    }
    shellTerm.segments.push(value);
    shellTerm.transcript += value;
    shellClampTranscript();
  }
  function shellSetTranscript(text) {
    shellTerm.transcript = String(text || '');
    shellResetSegmentsFromTranscript();
    shellClampTranscript();
  }
  function shellClampTranscript() {
    if (shellTerm.transcript.length <= shellTerm.maxChars) return;
    var excess = shellTerm.transcript.length - shellTerm.maxChars;
    if (shellTerm.segments && shellTerm.segments.length) {
      while (excess > 0 && shellTerm.segments.length) {
        var first = shellTerm.segments[0] || '';
        if (first.length <= excess) {
          excess -= first.length;
          shellTerm.segments.shift();
        } else {
          shellTerm.segments[0] = first.slice(excess);
          excess = 0;
        }
      }
      shellRebuildTranscriptFromSegments();
    } else {
      shellTerm.transcript = shellTerm.transcript.slice(shellTerm.transcript.length - shellTerm.maxChars);
      shellResetSegmentsFromTranscript();
    }
  }
  function shellEnsurePromptTail() {
    if (!shellTerm.prompt) return;
    if (!shellTerm.transcript.endsWith(shellTerm.prompt)) {
      shellTerm.transcript += shellTerm.prompt;
      shellResetSegmentsFromTranscript();
    }
  }
  function shellRemovePromptTail() {
    if (!shellTerm.prompt) return;
    if (shellTerm.transcript.endsWith(shellTerm.prompt)) {
      shellTerm.transcript = shellTerm.transcript.slice(0, -shellTerm.prompt.length);
      shellResetSegmentsFromTranscript();
    }
  }

  function shellEscapeHtml(text) {
    return String(text || '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function shellTokenizeInput(line) {
    var value = String(line || '');
    var tokens = [];
    var re = /(\"(?:\\.|[^\"])*\"|'(?:\\.|[^'])*'|[|&;<>]+|\S+)/g;
    var match;
    while ((match = re.exec(value))) {
      tokens.push({ value: match[0], start: match.index, end: match.index + match[0].length });
    }
    return tokens;
  }

  function shellClassifyToken(token, index) {
    var value = String(token && token.value || '');
    if (/^[|&;<>]+$/.test(value)) return 'op';
    if (/^['"]/.test(value) && !(/(['"])$/.test(value))) return 'error';
    if (/^['"]/.test(value)) return 'quote';
    if (value.charAt(0) === '-') return 'option';
    if (value.indexOf('/') >= 0 || value === '.' || value === '..' || value.indexOf('~') === 0) return 'path';
    if (index === 0 || (token && token.prevOp)) return 'command';
    if (/^\$[A-Za-z_][A-Za-z0-9_]*$/.test(value)) return 'env';
    return 'arg';
  }

  function shellBuildSyntaxPreview(line) {
    var tokens = shellTokenizeInput(line);
    var prevWasOp = true;
    var hint = '';
    var html = tokens.map(function(token, index) {
      token.prevOp = prevWasOp;
      var kind = shellClassifyToken(token, prevWasOp ? 0 : index);
      prevWasOp = kind === 'op';
      var value = String(token.value || '');
      if (!hint && !/^['"]/.test(value) && /[\s()[\]{};|&<>]/.test(value)) {
        hint = 'Smart quote: bosluk veya ozel karakter iceren path icin tirnak kullan.';
      }
      return '<span class="shell-syntax-token shell-syntax-' + kind + '">' + shellEscapeHtml(value) + '</span>';
    }).join('<span class="shell-syntax-space"> </span>');
    shellTerm.syntaxSpans = tokens;
    shellTerm.smartQuoteHint = hint;
    return html || '<span class="shell-syntax-muted">Komut, path, option ve pipe tokenlari burada renklenecek.</span>';
  }

  function shellFuzzyScore(query, text) {
    query = String(query || '').toLowerCase();
    text = String(text || '').toLowerCase();
    if (!query) return text ? 1 : 0;
    var qi = 0;
    var score = 0;
    for (var i = 0; i < text.length && qi < query.length; i++) {
      if (text[i] === query[qi]) {
        qi++;
        score += (i === 0 || text[i - 1] === ' ' || text[i - 1] === '-' || text[i - 1] === '/') ? 4 : 2;
      }
    }
    return qi === query.length ? score - Math.min(20, text.length / 8) : -1;
  }

  function shellBuildCompletionItems() {
    var input = String(shellTerm.input || '');
    var items = [];
    var seen = {};
    function add(value, kind, rank, meta) {
      value = String(value || '');
      if (!value || seen[value]) return;
      seen[value] = true;
      items.push(Object.assign({ value: value, completion_kind: kind || 'command', rank: rank || (items.length + 1) }, meta || {}));
    }
    if (Array.isArray(shellTerm.suggestionsMeta) && shellTerm.suggestionsMeta.length) {
      shellTerm.suggestionsMeta.forEach(function(item, idx) {
        add(item.value || item, item.completion_kind || 'server', item.rank || (idx + 1), item);
      });
    }
    if (Array.isArray(shellTerm.suggestions)) {
      shellTerm.suggestions.forEach(function(value, idx) { add(value, 'server', idx + 1); });
    }
    var historyQuery = shellTerm.historySearchOpen ? shellTerm.historySearchQuery : input;
    var historyMatches = [];
    for (var i = shellTerm.history.length - 1; i >= 0; i--) {
      var line = shellTerm.history[i];
      var score = shellFuzzyScore(historyQuery, line);
      if (score >= 0 || (input && String(line || '').indexOf(input) === 0)) {
        var meta = (shellTerm.historyMeta || []).filter(function(record) { return record && record.line === line; }).pop() || {};
        historyMatches.push({
          value: line,
          completion_kind: 'history',
          rank: 100 - score,
          history_meta: {
            ts: meta.ts || 0,
            rc: meta.rc,
            duration_ms: meta.duration_ms,
            pane_id: meta.pane_id !== undefined ? meta.pane_id : shellTerm.paneId
          }
        });
      }
    }
    historyMatches.sort(function(a, b) { return a.rank - b.rank; }).slice(0, 8).forEach(function(item) {
      add(item.value, 'history', item.rank, item);
    });
    items.sort(function(a, b) {
      if (Number(a.rank || 0) !== Number(b.rank || 0)) return Number(a.rank || 0) - Number(b.rank || 0);
      return String(a.value).localeCompare(String(b.value));
    });
    return items.slice(0, 12);
  }

  function shellEnsureInputAux() {
    var row = document.getElementById('console-input-row');
    if (!row || !row.parentElement) return {};
    var preview = document.getElementById('shell-syntax-preview');
    if (!preview) {
      preview = document.createElement('div');
      preview.id = 'shell-syntax-preview';
      preview.className = 'shell-syntax-preview';
      row.parentElement.insertBefore(preview, row.nextSibling);
    }
    var menu = document.getElementById('shell-completion-menu');
    if (!menu) {
      menu = document.createElement('div');
      menu.id = 'shell-completion-menu';
      menu.className = 'shell-completion-menu';
      row.parentElement.insertBefore(menu, preview.nextSibling);
    }
    return { preview: preview, menu: menu };
  }

  function shellRenderCompletionMenu(forceOpen) {
    var aux = shellEnsureInputAux();
    if (!aux.menu) return;
    var items = shellBuildCompletionItems();
    var open = !!(consolePanel.mode === 'shell' && (forceOpen || shellTerm.completionMenuOpen || shellTerm.historySearchOpen) && items.length);
    shellTerm.completionMenuOpen = open;
    if (!open) {
      aux.menu.style.display = 'none';
      aux.menu.innerHTML = '';
      return;
    }
    shellTerm.completionIndex = Math.max(0, Math.min(shellTerm.completionIndex || 0, items.length - 1));
    aux.menu.style.display = '';
    var title = shellTerm.historySearchOpen ? ('Reverse search: ' + shellEscapeHtml(shellTerm.historySearchQuery || shellTerm.input || '')) : 'Completion';
    aux.menu.innerHTML = '<div class="shell-completion-head">' + title + '</div>' +
      items.map(function(item, idx) {
        var meta = item.history_meta ? (' rc=' + (item.history_meta.rc == null ? '?' : item.history_meta.rc) + ' ' + (item.history_meta.duration_ms || 0) + 'ms pane=' + ((item.history_meta.pane_id || 0) + 1)) : ('rank ' + (item.rank || idx + 1));
        return '<button type="button" class="shell-completion-item' + (idx === shellTerm.completionIndex ? ' active' : '') + '" data-shell-completion="' + idx + '">' +
          '<span class="shell-completion-value">' + shellEscapeHtml(item.value) + '</span>' +
          '<span class="shell-completion-meta">' + shellEscapeHtml(item.completion_kind || 'item') + ' · ' + shellEscapeHtml(meta) + '</span></button>';
      }).join('') +
      (shellTerm.smartQuoteHint ? '<div class="shell-smart-quote-hint">' + shellEscapeHtml(shellTerm.smartQuoteHint) + '</div>' : '');
    Array.prototype.forEach.call(aux.menu.querySelectorAll('[data-shell-completion]'), function(btn) {
      btn.addEventListener('mousedown', function(ev) {
        ev.preventDefault();
        var idx = Number(btn.getAttribute('data-shell-completion')) || 0;
        shellAcceptCompletionIndex(idx);
      });
    });
  }

  function shellAcceptCompletionIndex(index) {
    var items = shellBuildCompletionItems();
    if (!items.length) return false;
    var item = items[Math.max(0, Math.min(Number(index) || 0, items.length - 1))];
    if (!item || !item.value) return false;
    shellTerm.historySearchOpen = false;
    shellTerm.completionMenuOpen = false;
    shellSetInput(item.value, String(item.value).length);
    return true;
  }

  function shellMoveCompletion(delta) {
    var items = shellBuildCompletionItems();
    if (!items.length) return false;
    shellTerm.completionMenuOpen = true;
    shellTerm.completionIndex = (items.length + (shellTerm.completionIndex || 0) + delta) % items.length;
    syncShellInputUi();
    return true;
  }

  function shellAnsiColor(code) {
    var map = {
      30: '#121212', 31: '#EA6A6A', 32: '#9BEB5D', 33: '#EAB96A',
      34: '#6A97EA', 35: '#C48BFF', 36: '#4DB8FF', 37: '#F0F2F3',
      90: '#7F8A93', 91: '#FF8A80', 92: '#B5EA6A', 93: '#FFD277',
      94: '#8AB4FF', 95: '#E39CFF', 96: '#82E9FF', 97: '#FFFFFF'
    };
    return map[code] || '';
  }

  function shellAnsiIndexedColor(index) {
    var base = {
      0: '#121212', 1: '#EA6A6A', 2: '#9BEB5D', 3: '#EAB96A',
      4: '#6A97EA', 5: '#C48BFF', 6: '#4DB8FF', 7: '#F0F2F3',
      8: '#7F8A93', 9: '#FF8A80', 10: '#B5EA6A', 11: '#FFD277',
      12: '#8AB4FF', 13: '#E39CFF', 14: '#82E9FF', 15: '#FFFFFF'
    };
    if (index <= 15) return base[index] || '#F0F2F3';
    if (index >= 16 && index <= 231) {
      var n = index - 16;
      var levels = [0, 95, 135, 175, 215, 255];
      var r = levels[Math.floor(n / 36) % 6];
      var g = levels[Math.floor(n / 6) % 6];
      var b = levels[n % 6];
      return 'rgb(' + r + ',' + g + ',' + b + ')';
    }
    if (index >= 232 && index <= 255) {
      var v = 8 + ((index - 232) * 10);
      return 'rgb(' + v + ',' + v + ',' + v + ')';
    }
    return '#F0F2F3';
  }

  function shellAnsiStateCss(state) {
    if (!state) return '';
    var css = '';
    if (state.color) css += 'color:' + state.color + ';';
    if (state.background) css += 'background:' + state.background + ';';
    if (state.bold) css += 'font-weight:700;';
    if (state.dim) css += 'opacity:0.72;';
    if (state.italic) css += 'font-style:italic;';
    if (state.underline) css += 'text-decoration:underline;';
    return css;
  }

  function shellApplyAnsiCodes(state, codes) {
    var i = 0;
    while (i < codes.length) {
      var code = codes[i++];
      if (code === 0) {
        state.color = '';
        state.background = '';
        state.bold = false;
        state.dim = false;
        state.italic = false;
        state.underline = false;
        continue;
      }
      if (code === 1) { state.bold = true; continue; }
      if (code === 2) { state.dim = true; continue; }
      if (code === 3) { state.italic = true; continue; }
      if (code === 4) { state.underline = true; continue; }
      if (code === 22) { state.bold = false; state.dim = false; continue; }
      if (code === 23) { state.italic = false; continue; }
      if (code === 24) { state.underline = false; continue; }
      if (code === 39) { state.color = ''; continue; }
      if (code === 49) { state.background = ''; continue; }
      if ((code >= 30 && code <= 37) || (code >= 90 && code <= 97)) {
        state.color = shellAnsiColor(code);
        continue;
      }
      if ((code >= 40 && code <= 47) || (code >= 100 && code <= 107)) {
        state.background = shellAnsiColor(code - 10);
        continue;
      }
      if (code === 38 && i + 1 < codes.length && codes[i] === 5) {
        var indexed = codes[i + 1];
        i += 2;
        if (indexed >= 0 && indexed <= 255) state.color = shellAnsiIndexedColor(indexed);
      }
    }
  }

  function shellAnsiToHtml(text) {
    var input = String(text || '').replace(/\u001bc/g, '').replace(/\u001b\[[0-9;?]*[HfJK]/g, '');
    var state = { color: '', background: '', bold: false, dim: false, italic: false, underline: false };
    var html = '';
    var buffer = '';

    function flush() {
      if (!buffer) return;
      var escaped = shellEscapeHtml(buffer);
      var css = shellAnsiStateCss(state);
      html += css ? '<span style="' + css + '">' + escaped + '</span>' : escaped;
      buffer = '';
    }

    for (var i = 0; i < input.length; i++) {
      var ch = input[i];
      if (ch === '\u001b' && input[i + 1] === '[') {
        flush();
        var match = input.slice(i).match(/^\u001b\[([0-9;?]*)([A-Za-z])/);
        if (!match) continue;
        if (match[2] === 'm') {
          var codes = match[1].length ? match[1].split(';').map(function(part) {
            var parsed = parseInt(part, 10);
            return Number.isFinite(parsed) ? parsed : 0;
          }) : [0];
          shellApplyAnsiCodes(state, codes);
        } else if (match[2] === 'J' && (match[1] === '2' || match[1] === '')) {
          html = '';
          buffer = '';
        }
        i += match[0].length - 1;
        continue;
      }
      buffer += ch;
    }
    flush();
    return html;
  }

  function shellStripAnsi(text) {
    return String(text || '').replace(/\u001b\[[0-9;?]*[A-Za-z]/g, '').replace(/\u001bc/g, '');
  }

  function shellMeasureCell() {
    var terminalEl = shellTerminalEl();
    if (!terminalEl) return null;
    var probe = document.getElementById('shell-char-probe');
    if (!probe) {
      probe = document.createElement('span');
      probe.id = 'shell-char-probe';
      probe.textContent = 'MMMMMMMMMM';
      probe.style.position = 'absolute';
      probe.style.visibility = 'hidden';
      probe.style.whiteSpace = 'pre';
      probe.style.pointerEvents = 'none';
      document.body.appendChild(probe);
    }
    var style = window.getComputedStyle(terminalEl);
    probe.style.fontFamily = style.fontFamily;
    probe.style.fontSize = style.fontSize;
    probe.style.fontWeight = style.fontWeight;
    probe.style.letterSpacing = style.letterSpacing;
    probe.style.lineHeight = style.lineHeight;
    var rect = probe.getBoundingClientRect();
    var fontSize = parseFloat(style.fontSize) || 12;
    var lineHeight = parseFloat(style.lineHeight) || (fontSize * 1.45);
    return {
      charWidth: rect.width > 0 ? (rect.width / 10) : Math.max(7, fontSize * 0.62),
      lineHeight: lineHeight
    };
  }

  function shellSendResize(force) {
    var socket = typeof global.shellSocketInstance === 'function' ? global.shellSocketInstance() : null;
    if (!socket || !shellTerm.authReady || consolePanel.mode !== 'shell') return;
    var terminalEl = shellTerminalEl();
    if (!terminalEl) return;
    var style = window.getComputedStyle(terminalEl);
    var metrics = shellMeasureCell();
    if (!metrics || !metrics.charWidth || !metrics.lineHeight) return;
    var width = terminalEl.clientWidth - (parseFloat(style.paddingLeft) || 0) - (parseFloat(style.paddingRight) || 0);
    var height = terminalEl.clientHeight - (parseFloat(style.paddingTop) || 0) - (parseFloat(style.paddingBottom) || 0);
    var cols = Math.max(40, Math.floor(width / metrics.charWidth));
    var rows = Math.max(10, Math.floor(height / metrics.lineHeight));
    if (!force && cols === shellTerm.lastResizeCols && rows === shellTerm.lastResizeRows) return;
    shellTerm.lastResizeCols = cols;
    shellTerm.lastResizeRows = rows;
    shellSendControlMessage('resize', cols + ':' + rows, SHELL_WS_CONTROL_HIGH_WATER);
  }

  function shellScheduleResize(force) {
    if (shellTerm.resizeTimer) clearTimeout(shellTerm.resizeTimer);
    shellTerm.resizeTimer = setTimeout(function() {
      shellTerm.resizeTimer = null;
      shellSendResize(!!force);
    }, force ? 30 : 120);
  }

  function shellRenderSurface(el, text, shellMode, includeInput) {
    if (!el) return;
    if (shellMode) {
      var html = '';
      if (shellPanes.length > 1) {
        html = '<div class="shell-pane-grid" style="grid-template-columns:repeat(' + Math.min(shellPanes.length, 2) + ',minmax(0,1fr));">';
        shellPanes.forEach(function(pane) {
          var active = pane.paneId === shellTerm.paneId;
          var body = shellAnsiToHtml(shellVisibleTranscript(pane.transcript || pane.prompt || '', pane));
          if (includeInput && active) {
            var beforeMulti = shellEscapeHtml(pane.input.slice(0, pane.cursor));
            var afterMulti = shellEscapeHtml(pane.input.slice(pane.cursor));
            body += beforeMulti + '<span class="shell-cursor"></span>' + afterMulti;
          }
          html += '<div class="shell-pane-card' + (active ? ' active' : '') + '" data-shell-pane="' + pane.paneId + '">' +
            '<div class="shell-pane-head"><span>pane ' + (pane.paneId + 1) + ' · ' + shellEscapeHtml(pane.target || 's3') + '</span>' +
            '<button type="button" class="shell-pane-close" data-shell-pane-close="' + pane.paneId + '"' + (shellPanes.length <= 1 ? ' disabled' : '') + '>KAPAT</button></div>' +
            '<div class="shell-pane-body">' + body + '</div></div>';
        });
        html += '</div>';
      } else {
        html = shellAnsiToHtml(shellVisibleTranscript(text, shellTerm));
        if (includeInput) {
          var before = shellEscapeHtml(shellTerm.input.slice(0, shellTerm.cursor));
          var after = shellEscapeHtml(shellTerm.input.slice(shellTerm.cursor));
          html += before + '<span class="shell-cursor"></span>' + after;
        }
      }
      if (el.innerHTML !== html) el.innerHTML = html;
      el.classList.add('is-shell');
    } else {
      if (el.textContent !== text) el.textContent = text;
      el.classList.remove('is-shell');
    }
    el.scrollTop = el.scrollHeight;
  }

  function shellRender() {
    shellClampTranscript();
    var text = shellVisibleText();
    var shellMode = consolePanel.mode === 'shell';
    shellTerminalSurfaces().forEach(function(surface) {
      shellRenderSurface(surface.el, text, shellMode, surface.includeInput && shellTerm.fullscreenOpen);
    });
    syncShellInputUi();
    syncMobileLogMirror();
    var statusEl = document.getElementById('shell-fullscreen-status');
    if (statusEl) {
      statusEl.textContent = (shellTerm.busy ? 'Komut çalışıyor...' : (shellTerm.ready ? 'Hazır' : 'Bağlantı bekleniyor')) +
        (shellPanes.length > 1 ? (' | ' + shellPanes.length + ' pane') : '');
    }
    applyShellFullscreenUiSettings();
  }
  function shellScheduleRender() {
    if (shellTerm.renderQueued) return;
    shellTerm.renderQueued = true;
    requestAnimationFrame(function() {
      shellTerm.renderQueued = false;
      shellRender();
    });
  }

  function shellReplacePrompt(newPrompt) {
    var oldPrompt = shellTerm.prompt;
    shellTerm.prompt = newPrompt || shellTerm.prompt || '';
    syncShellInputUi();
    if (!shellTerm.prompt) return;
    if (!shellTerm.transcript) {
      shellTerm.transcript = shellTerm.prompt;
      shellResetSegmentsFromTranscript();
      return;
    }
    if (oldPrompt && shellTerm.transcript.endsWith(oldPrompt)) {
      shellTerm.transcript = shellTerm.transcript.slice(0, -oldPrompt.length) + shellTerm.prompt;
      shellResetSegmentsFromTranscript();
      return;
    }
    shellEnsurePromptTail();
  }

  function shellCommitPendingLogs() {
    if (!shellTerm.pendingLogs.length) return;
    var pending = shellTerm.pendingLogs.join('');
    shellTerm.pendingLogs = [];
    if (shellTerm.prompt && shellTerm.transcript.endsWith(shellTerm.prompt)) {
      shellTerm.transcript = shellTerm.transcript.slice(0, -shellTerm.prompt.length) + pending + shellTerm.prompt;
    } else {
      shellTerm.transcript += pending;
      shellEnsurePromptTail();
    }
    shellResetSegmentsFromTranscript();
    shellClampTranscript();
  }

  function shellAppendPassiveLogs(text) {
    if (!text) return;
    if (consolePanel.mode === 'shell') {
      shellTerm.pendingLogs.push(text);
      if (shellTerm.pendingLogs.length > 256) {
        shellTerm.pendingLogs = shellTerm.pendingLogs.slice(shellTerm.pendingLogs.length - 256);
      }
      return;
    }
    if (consolePanel.mode === 'log') shellRender();
  }

  function shellCommitLocalError(message) {
    if (!shellTerm.prompt) shellTerm.prompt = 'mshell$ ';
    shellEnsurePromptTail();
    shellTerm.transcript += shellTerm.input + '\n' + message + '\n' + shellTerm.prompt;
    shellResetSegmentsFromTranscript();
    shellTerm.input = '';
    shellTerm.cursor = 0;
    shellTerm.busy = false;
    shellTerm.ready = true;
    shellClampTranscript();
    shellRender();
  }

  function shellReportLocalStatus(message) {
    if (!shellTerm.prompt) shellTerm.prompt = 'mshell$ ';
    shellEnsurePromptTail();
    shellRemovePromptTail();
    shellAppendTranscript('[client] ' + String(message || 'shell send failed') + '\n');
    shellEnsurePromptTail();
    shellScheduleRender();
  }

  function shellComputeGhostSuggestion() {
    var input = String(shellTerm.input || '');
    if (!input || shellTerm.busy) return '';
    var candidates = [];
    if (Array.isArray(shellTerm.suggestions)) candidates = candidates.concat(shellTerm.suggestions);
    for (var i = shellTerm.history.length - 1; i >= 0; i--) candidates.push(shellTerm.history[i]);
    for (var j = 0; j < candidates.length; j++) {
      var value = String(candidates[j] || '');
      if (value.length > input.length && value.indexOf(input) === 0) {
        shellTerm.suggestion = value;
        return value.slice(input.length);
      }
    }
    shellTerm.suggestion = '';
    return '';
  }

  function shellAcceptGhostSuggestion() {
    var ghost = shellComputeGhostSuggestion();
    if (!ghost) return false;
    shellSetInput(shellTerm.input + ghost, shellTerm.input.length + ghost.length);
    return true;
  }

  function shellScheduleSuggestRequest() {
    if (shellTerm.suggestTimer) clearTimeout(shellTerm.suggestTimer);
    if (!shellTerm.input || shellTerm.input.length < 2 || shellTerm.busy) return;
    shellTerm.suggestTimer = setTimeout(function() {
      shellTerm.suggestTimer = null;
      shellRequestSuggest();
    }, 180);
  }

  function shellInsertText(text) {
    if (!text) return;
    shellSetInput(
      shellTerm.input.slice(0, shellTerm.cursor) + text + shellTerm.input.slice(shellTerm.cursor),
      shellTerm.cursor + text.length
    );
  }

  function shellSendState() {
    var socket = typeof global.shellSocketInstance === 'function' ? global.shellSocketInstance() : null;
    if (!socket || !shellTerm.authReady) return;
    shellSendControlMessage('state', '', SHELL_WS_CONTROL_HIGH_WATER);
    shellScheduleResize(true);
  }

  function shellSendExec() {
    if (consolePanel.mode !== 'shell' || shellTerm.busy) return;
    var socket = typeof global.shellSocketInstance === 'function' ? global.shellSocketInstance() : null;
    if (!socket || !shellTerm.authReady) {
      shellCommitLocalError('shell websocket not ready');
      return;
    }
    var line = shellTerm.input;
    if (line.trim().length > 0) {
      if (!shellTerm.history.length || shellTerm.history[shellTerm.history.length - 1] !== line) {
        shellTerm.history.push(line);
        if (shellTerm.history.length > 64) shellTerm.history.shift();
      }
    }
    shellTerm.historyIndex = -1;
    shellTerm.commandSeq = (Number(shellTerm.commandSeq || 0) + 1) & 0xFFFF;
    if (shellTerm.commandSeq === 0) shellTerm.commandSeq = 1;
    if (line.trim().length > 0) {
      shellTerm.activeHistoryRecord = {
        line: line,
        ts: Date.now(),
        rc: null,
        duration_ms: null,
        command_id: shellTerm.commandSeq
      };
      shellTerm.historyMeta.push(shellTerm.activeHistoryRecord);
      if (shellTerm.historyMeta.length > 64) shellTerm.historyMeta.shift();
    }
    var execPayload = line;
    if (!shellSendControlMessage('exec', execPayload, SHELL_WS_INPUT_HIGH_WATER, shellTerm.commandSeq)) {
      shellReportLocalStatus('shell websocket backpressure: komut gonderilmedi');
      return;
    }
    shellTerm.busy = true;
  }

  function shellRequestCompletion() {
    if (consolePanel.mode !== 'shell' || shellTerm.busy) return;
    var socket = typeof global.shellSocketInstance === 'function' ? global.shellSocketInstance() : null;
    if (!socket || !shellTerm.authReady) return;
    shellTerm.completionMenuOpen = true;
    shellTerm.historySearchOpen = false;
    shellTerm.completionIndex = 0;
    shellTerm.completionRequestRevision = shellTerm.inputRevision;
    shellTerm.completionRequestInput = shellTerm.input;
    syncShellInputUi();
    shellSendControlMessage('complete', shellTerm.input, SHELL_WS_CONTROL_HIGH_WATER);
  }

  function shellRequestSuggest() {
    if (consolePanel.mode !== 'shell' || shellTerm.busy) return;
    var socket = typeof global.shellSocketInstance === 'function' ? global.shellSocketInstance() : null;
    if (!socket || !shellTerm.authReady) return;
    shellTerm.suggestRequestRevision = shellTerm.inputRevision;
    shellTerm.suggestRequestInput = shellTerm.input;
    shellSendControlMessage('suggest', shellTerm.input, SHELL_WS_CONTROL_HIGH_WATER);
  }

  function shellHandleHistory(direction) {
    if (!shellTerm.history.length) return;
    if (direction < 0) {
      if (shellTerm.historyIndex < 0) shellTerm.historyIndex = shellTerm.history.length - 1;
      else if (shellTerm.historyIndex > 0) shellTerm.historyIndex--;
    } else {
      if (shellTerm.historyIndex < 0) return;
      if (shellTerm.historyIndex < shellTerm.history.length - 1) shellTerm.historyIndex++;
      else {
        shellTerm.historyIndex = -1;
        shellSetInput('', 0);
        shellRender();
        return;
      }
    }
    var nextHistoryInput = shellTerm.historyIndex >= 0 ? shellTerm.history[shellTerm.historyIndex] : '';
    shellSetInput(nextHistoryInput, nextHistoryInput.length);
    shellRender();
  }

  function shellOpenHistorySearch() {
    shellTerm.historySearchOpen = true;
    shellTerm.completionMenuOpen = true;
    shellTerm.completionIndex = 0;
    shellTerm.historySearchQuery = shellTerm.input || '';
    syncShellInputUi();
  }

  function shellHandleCompletionNavigation(key) {
    if (!(shellTerm.completionMenuOpen || shellTerm.historySearchOpen)) return false;
    if (key === 'ArrowUp') return shellMoveCompletion(-1);
    if (key === 'ArrowDown') return shellMoveCompletion(1);
    if (key === 'Enter' || key === 'Tab') return shellAcceptCompletionIndex(shellTerm.completionIndex || 0);
    if (key === 'Escape') {
      shellTerm.completionMenuOpen = false;
      shellTerm.historySearchOpen = false;
      syncShellInputUi();
      return true;
    }
    return false;
  }

  function shellSetInput(nextInput, nextCursor) {
    shellTerm.input = String(nextInput || '');
    shellTerm.cursor = Math.max(0, Math.min(typeof nextCursor === 'number' ? nextCursor : shellTerm.input.length, shellTerm.input.length));
    shellTerm.inputRevision++;
    if (shellTerm.historySearchOpen) shellTerm.historySearchQuery = shellTerm.input;
    shellSyncInputElement(true);
    shellScheduleSuggestRequest();
    shellScheduleInputRender();
  }

  function shellDeleteInputRange(start, end) {
    var safeStart = Math.max(0, Math.min(start, shellTerm.input.length));
    var safeEnd = Math.max(safeStart, Math.min(end, shellTerm.input.length));
    shellSetInput(shellTerm.input.slice(0, safeStart) + shellTerm.input.slice(safeEnd), safeStart);
  }

  function shellHandleDirectPaste(text) {
    if (shellTerm.busy || consolePanel.mode !== 'shell') return;
    var raw = String(text || '').replace(/\r/g, '');
    var lines = raw.split('\n').filter(function(line) { return line.length > 0; });
    var clean = lines.length > 1 ? lines.join(' ; ') : raw.replace(/\n/g, ' ');
    if (!clean) return;
    if (lines.length > 1) {
      shellTerm.pasteModeUntil = Date.now() + 5000;
      shellReportLocalStatus('paste mode: ' + lines.length + ' satir tek komut olarak yapistirildi, otomatik calistirma yok');
    }
    shellSetInput(shellTerm.input.slice(0, shellTerm.cursor) + clean + shellTerm.input.slice(shellTerm.cursor), shellTerm.cursor + clean.length);
  }

  function shellHandleCtrlC() {
    var selection = window.getSelection ? String(window.getSelection()) : '';
    if (selection) return false;
    shellRemovePromptTail();
    shellTerm.transcript += (shellTerm.input || shellTerm.busy) ? (shellTerm.input + '^C\n') : '^C\n';
    shellResetSegmentsFromTranscript();
    shellTerm.input = '';
    shellTerm.cursor = 0;
    shellTerm.inputRevision++;
    shellTerm.busy = false;
    shellEnsurePromptTail();
    shellSyncInputElement(true);
    shellScheduleRender();
    return true;
  }

  function shellHandleDirectKeydown(ev) {
    if (consolePanel.mode !== 'shell' || !shellTerm.ready) return;
    var key = ev.key;
    if (ev.ctrlKey || ev.metaKey) {
      var lower = String(key || '').toLowerCase();
      if (lower === 'r') {
        ev.preventDefault();
        shellOpenHistorySearch();
        return;
      }
      if (lower === 'c') {
        if (shellHandleCtrlC()) ev.preventDefault();
        return;
      }
      if (lower === 'v' || lower === 'a') return;
      if (lower === 'l') {
        ev.preventDefault();
        clearConsoleLocal();
      }
      return;
    }
    if (shellTerm.busy) {
      ev.preventDefault();
      return;
    }
    if (shellHandleCompletionNavigation(key)) {
      ev.preventDefault();
      return;
    }
    if (key === 'Enter') { ev.preventDefault(); shellSendExec(); return; }
    if (key === 'Tab') { ev.preventDefault(); shellRequestCompletion(); return; }
    if (key === 'ArrowUp') { ev.preventDefault(); shellHandleHistory(-1); return; }
    if (key === 'ArrowDown') { ev.preventDefault(); shellHandleHistory(1); return; }
    if (key === 'ArrowLeft') { ev.preventDefault(); shellSetInput(shellTerm.input, shellTerm.cursor - 1); return; }
    if (key === 'ArrowRight') {
      ev.preventDefault();
      if (shellTerm.cursor >= shellTerm.input.length && shellAcceptGhostSuggestion()) return;
      shellSetInput(shellTerm.input, shellTerm.cursor + 1);
      return;
    }
    if (key === 'Home') { ev.preventDefault(); shellSetInput(shellTerm.input, 0); return; }
    if (key === 'End') { ev.preventDefault(); shellSetInput(shellTerm.input, shellTerm.input.length); return; }
    if (key === 'Backspace') { ev.preventDefault(); if (shellTerm.cursor > 0) shellDeleteInputRange(shellTerm.cursor - 1, shellTerm.cursor); return; }
    if (key === 'Delete') { ev.preventDefault(); if (shellTerm.cursor < shellTerm.input.length) shellDeleteInputRange(shellTerm.cursor, shellTerm.cursor + 1); return; }
    if (key === 'Escape') { ev.preventDefault(); closeShellFullscreen(); return; }
    if (key && key.length === 1 && !ev.altKey) {
      ev.preventDefault();
      shellSetInput(shellTerm.input.slice(0, shellTerm.cursor) + key + shellTerm.input.slice(shellTerm.cursor), shellTerm.cursor + key.length);
    }
  }

  function shellHandleMessage(shellMsg) {
    if (!shellMsg || typeof shellMsg !== 'object') return;
    if (typeof shellMsg.pane_id === 'number') {
      var pane = shellPaneById(shellMsg.pane_id, true);
      if (pane) {
        shellTerm = pane;
        shellActivePane = pane.paneId;
        global.shellTerm = shellTerm;
      }
    }
    if (shellMsg.type === 'state') {
      shellTerm.ready = true;
      shellTerm.busy = false;
      shellReplacePrompt(shellMsg.prompt || shellTerm.prompt);
      shellRender();
      shellScheduleResize(true);
      return;
    }
    if (shellMsg.type === 'start') {
      shellTerm.ready = true;
      shellTerm.busy = true;
      shellTerm.sawStreamForCommand = false;
      shellTerm.input = '';
      shellTerm.cursor = 0;
      shellTerm.inputRevision++;
      shellTerm.prompt = shellMsg.prompt || shellTerm.prompt;
      shellRemovePromptTail();
      shellAppendTranscript(shellMsg.output || '');
      shellClampTranscript();
      shellRender();
      return;
    }
    if (shellMsg.type === 'stream') {
      shellTerm.ready = true;
      shellTerm.sawStreamForCommand = true;
      shellRemovePromptTail();
      shellAppendTranscript(shellMsg.output || '');
      shellClampTranscript();
      shellScheduleRender();
      return;
    }
    if (shellMsg.type === 'exec') {
      shellTerm.ready = true;
      shellTerm.busy = false;
      shellTerm.input = '';
      shellTerm.cursor = 0;
      shellTerm.inputRevision++;
      shellTerm.prompt = shellMsg.prompt || shellTerm.prompt;
      var finalOutput = shellMsg.output || '';
      if (shellTerm.activeHistoryRecord) {
        if (typeof shellMsg.rc === 'number') shellTerm.activeHistoryRecord.rc = shellMsg.rc;
        if (typeof shellMsg.duration_ms === 'number') shellTerm.activeHistoryRecord.duration_ms = shellMsg.duration_ms;
        shellTerm.activeHistoryRecord = null;
      }
      if (shellTerm.sawStreamForCommand && finalOutput && finalOutput.indexOf('stream queue overflow') === -1) {
        finalOutput = '';
      }
      shellTerm.sawStreamForCommand = false;
      if (shellMsg.cleared) {
        shellSetTranscript(finalOutput || shellTerm.prompt || '');
      } else {
        shellRemovePromptTail();
        shellAppendTranscript(finalOutput);
        shellEnsurePromptTail();
      }
      shellCommitPendingLogs();
      shellClampTranscript();
      shellRender();
      return;
    }
    if (shellMsg.type === 'complete') {
      var completionStillCurrent =
        shellTerm.completionRequestRevision === shellTerm.inputRevision &&
        shellTerm.completionRequestInput === shellTerm.input;
      if (!completionStillCurrent) {
        shellInputStats().staleCompletionDrops++;
        shellInputStats().serverInputRewriteBlocked++;
        return;
      }
      if (typeof shellMsg.line === 'string') {
        shellTerm.input = shellMsg.line;
        shellTerm.cursor = shellTerm.input.length;
        shellTerm.inputRevision++;
        shellTerm.completionRequestRevision = shellTerm.inputRevision;
        shellTerm.completionRequestInput = shellTerm.input;
        shellSyncInputElement(true);
      }
      shellTerm.suggestionsMeta = Array.isArray(shellMsg.suggestions_meta) ? shellMsg.suggestions_meta.slice(0, 12) : [];
      if (Array.isArray(shellMsg.suggestions)) {
        shellTerm.suggestions = shellMsg.suggestions.slice(0, 12);
      }
      shellTerm.completionMenuOpen = true;
      shellTerm.completionIndex = 0;
      shellRender();
      return;
    }
    if (shellMsg.type === 'suggest') {
      var suggestStillCurrent =
        shellTerm.suggestRequestRevision === shellTerm.inputRevision &&
        shellTerm.suggestRequestInput === shellTerm.input;
      if (!suggestStillCurrent) {
        shellInputStats().staleSuggestionDrops++;
        shellInputStats().serverInputRewriteBlocked++;
        return;
      }
      shellTerm.suggestionsMeta = Array.isArray(shellMsg.suggestions_meta) ? shellMsg.suggestions_meta.slice(0, 12) : [];
      if (Array.isArray(shellMsg.suggestions)) {
        shellTerm.suggestions = shellMsg.suggestions.slice(0, 12);
        syncShellInputUi();
      }
      return;
    }
    if (shellMsg.type === 'error') {
      if (shellMsg.prompt) shellTerm.prompt = shellMsg.prompt;
      shellCommitLocalError(shellMsg.message || 'shell error');
    }
  }

  function openShellFullscreen() {
    var modal = document.getElementById('shellFullscreenModal');
    if (!modal) return;
    shellTerm.fullscreenOpen = true;
    bindShellFullscreenButtons();
    modal.style.display = 'block';
    modal.setAttribute('aria-hidden', 'false');
    applyShellFullscreenUiSettings();
    setConsolePanelMode('shell');
    shellRender();
    shellScheduleResize(true);
    var term = shellFullscreenTerminalEl();
    if (term) setTimeout(function() { try { term.focus(); } catch (err) {} }, 20);
  }
  function shellFullscreenSplit() {
    shellAddPane();
    applyShellFullscreenUiSettings();
    shellSetFullscreenStatus('Pane eklendi · aktif pane ' + (activePaneId() + 1) + ' / ' + shellPanes.length);
    shellRender();
  }
  function shellFullscreenClosePane() {
    if (shellPanes.length <= 1) {
      shellSetFullscreenStatus('Tek pane kaldı; kapatılacak ayrı pane yok.');
      return;
    }
    shellCloseActivePane();
    applyShellFullscreenUiSettings();
    shellSetFullscreenStatus('Pane kapatıldı · aktif pane ' + (activePaneId() + 1) + ' / ' + shellPanes.length);
    shellRender();
  }

  function shellRefreshState() {
    if (typeof global.shellSendState === 'function') global.shellSendState();
    if (typeof global.shellScheduleResize === 'function') global.shellScheduleResize(true);
    if (typeof global.shellSocketInstance === 'function' && !global.shellSocketInstance() && typeof global.connectWS === 'function') {
      try { global.connectWS(); } catch (e) {}
      setTimeout(function() {
        if (typeof global.shellSendState === 'function') global.shellSendState();
      }, 220);
    }
  }

  function shellKillProcess() {
    var socket = typeof global.shellSocketInstance === 'function' ? global.shellSocketInstance() : null;
    if (socket && shellTerm.busy) {
      shellSetFullscreenStatus('Aktif komuta interrupt gönderiliyor...');
      shellSendControlMessage('exec', '\u0003', SHELL_WS_CONTROL_HIGH_WATER);
      shellHandleCtrlC();
      shellRender();
      return;
    }
    shellSetFullscreenStatus('Aktif süreç yok; shell bağlantısı tazeleniyor...');
    if (global.shellWs) {
      try { global.shellWs.close(); } catch (err) {}
    }
    if (typeof global.connectWS === 'function') {
      setTimeout(function() {
        try { global.connectWS(); } catch (e) {}
        setTimeout(function() {
          if (typeof global.shellSendState === 'function') global.shellSendState();
        }, 220);
      }, 120);
    }
  }

  function closeShellFullscreen() {
    var modal = document.getElementById('shellFullscreenModal');
    if (modal) {
      modal.style.display = 'none';
      modal.setAttribute('aria-hidden', 'true');
    }
    shellTerm.fullscreenOpen = false;
    shellRender();
    shellScheduleResize(true);
  }

  function bindShellFullscreenButtons() {
    [
      ['terminal-split-btn', shellFullscreenSplit],
      ['terminal-close-pane-btn', shellFullscreenClosePane],
      ['terminal-kill-btn', shellKillProcess],
      ['terminal-refresh-btn', shellRefreshState]
    ].forEach(function(pair) {
      var btn = document.getElementById(pair[0]);
      if (!btn || btn.dataset.shellFullscreenBound === '1') return;
      btn.dataset.shellFullscreenBound = '1';
      btn.onclick = null;
      btn.removeAttribute('onclick');
      btn.addEventListener('click', function(ev) {
        ev.preventDefault();
        ev.stopPropagation();
        pair[1]();
      });
    });
  }

  function initShellTerminal() {
    var el = shellTerminalEl();
    var inputEl = shellInputEl();
    if (!el || !inputEl || shellTerm.attached) return;
    shellTerm.attached = true;
    var fullscreenEl = shellFullscreenTerminalEl();
    ensureShellPaneToolbar();
    bindShellFullscreenButtons();

    el.addEventListener('mousedown', function(ev) {
      var closeButton = ev.target && ev.target.closest ? ev.target.closest('[data-shell-pane-close]') : null;
      if (closeButton) {
        ev.preventDefault();
        ev.stopPropagation();
        shellClosePane(Number(closeButton.getAttribute('data-shell-pane-close')) || 0);
        return;
      }
      var paneButton = ev.target && ev.target.closest ? ev.target.closest('[data-shell-pane]') : null;
      if (paneButton) shellSetActivePane(Number(paneButton.getAttribute('data-shell-pane')) || 0);
      if (consolePanel.mode === 'shell') inputEl.focus();
    });
    el.addEventListener('focus', function() { if (consolePanel.mode === 'shell') inputEl.focus(); });
    inputEl.addEventListener('paste', function(ev) {
      ev.preventDefault();
      if (shellTerm.busy) return;
      var pasted = (ev.clipboardData || window.clipboardData).getData('text');
      shellHandleDirectPaste(pasted || '');
    });
    inputEl.addEventListener('compositionstart', function() {
      shellTerm.inputComposing = true;
      shellInputStats().compositionEvents++;
    });
    inputEl.addEventListener('compositionend', function(ev) {
      shellTerm.inputComposing = false;
      shellTerm.input = ev.target.value || '';
      shellTerm.cursor = typeof ev.target.selectionStart === 'number' ? ev.target.selectionStart : shellTerm.input.length;
      shellTerm.inputRevision++;
      shellInputStats().compositionEvents++;
      shellScheduleSuggestRequest();
      shellScheduleInputRender();
    });
    inputEl.addEventListener('input', function(ev) {
      shellTerm.input = ev.target.value || '';
      shellTerm.cursor = typeof ev.target.selectionStart === 'number' ? ev.target.selectionStart : shellTerm.input.length;
      shellTerm.inputRevision++;
      shellTerm.inputLastUserMs = Date.now();
      shellInputStats().userEdits++;
      if (shellTerm.historySearchOpen) shellTerm.historySearchQuery = shellTerm.input;
      shellScheduleSuggestRequest();
      shellScheduleInputRender();
    });
    inputEl.addEventListener('click', function(ev) {
      shellTerm.cursor = typeof ev.target.selectionStart === 'number' ? ev.target.selectionStart : shellTerm.input.length;
    });
    inputEl.addEventListener('keyup', function(ev) {
      shellTerm.cursor = typeof ev.target.selectionStart === 'number' ? ev.target.selectionStart : shellTerm.input.length;
    });
    inputEl.addEventListener('keydown', function(ev) {
      if (!shellTerm.ready || consolePanel.mode !== 'shell') return;
      if (ev.ctrlKey || ev.metaKey) {
        if (String(ev.key || '').toLowerCase() === 'r') {
          ev.preventDefault();
          shellOpenHistorySearch();
        }
        return;
      }
      if (shellTerm.busy && ev.key !== 'c') return;
      if (shellHandleCompletionNavigation(ev.key)) {
        ev.preventDefault();
        return;
      }
      if (ev.key === 'Enter') { ev.preventDefault(); shellSendExec(); return; }
      if (ev.key === 'Tab') { ev.preventDefault(); shellRequestCompletion(); return; }
      if (ev.key === 'ArrowUp') { ev.preventDefault(); shellHandleHistory(-1); return; }
      if (ev.key === 'ArrowDown') { ev.preventDefault(); shellHandleHistory(1); }
      if (ev.key === 'ArrowRight' && shellTerm.cursor >= shellTerm.input.length && shellAcceptGhostSuggestion()) {
        ev.preventDefault();
        return;
      }
    });
    if (fullscreenEl) {
      fullscreenEl.addEventListener('keydown', shellHandleDirectKeydown);
      fullscreenEl.addEventListener('paste', function(ev) {
        ev.preventDefault();
        shellHandleDirectPaste((ev.clipboardData || window.clipboardData).getData('text'));
      });
      fullscreenEl.addEventListener('mousedown', function() {
        var ev = arguments[0];
        var closeButton = ev && ev.target && ev.target.closest ? ev.target.closest('[data-shell-pane-close]') : null;
        if (closeButton) {
          ev.preventDefault();
          ev.stopPropagation();
          shellClosePane(Number(closeButton.getAttribute('data-shell-pane-close')) || 0);
          return;
        }
        var paneButton = ev && ev.target && ev.target.closest ? ev.target.closest('[data-shell-pane]') : null;
        if (paneButton) shellSetActivePane(Number(paneButton.getAttribute('data-shell-pane')) || 0);
        if (consolePanel.mode === 'shell') fullscreenEl.focus();
      });
    }
    if (window.ResizeObserver) {
      shellTerm.resizeObserver = new ResizeObserver(function() {
        shellScheduleResize(false);
      });
      shellTerm.resizeObserver.observe(el);
      if (fullscreenEl) shellTerm.resizeObserver.observe(fullscreenEl);
    }
    window.addEventListener('resize', function() {
      shellScheduleResize(false);
    });
    syncShellInputUi();
    applyShellFullscreenUiSettings();
    shellRender();
    shellScheduleResize(true);
  }

  function sanitizeConsoleLine(rawLine) {
    if (!rawLine) return '';
    var s = rawLine.replace(/\r/g, '');
    s = s.replace(/^([A-Z0-9]+:\s*)?(\[[ ]*\d+\]\s*|[IWEV]\s*\(\d+\)\s*)/, '$1');
    s = s.replace(/[^\x09\x20-\x7E]/g, '?');
    if (s.length > 220) s = s.substring(0, 220) + ' ...';
    return s.trim();
  }

  function isLikelyConsoleNoise(line) {
    if (!line) return true;
    if (/(.)\1{24,}/.test(line)) return true;
    var changes = 0;
    for (var i = 1; i < line.length; i++) {
      if (line[i] !== line[i - 1]) changes++;
    }
    if (line.length >= 40 && changes < Math.floor(line.length / 12)) return true;
    return false;
  }

  function fetchLogs() {
    var reqOffset = global.consoleLogOffset || 0;
    fetch('/api/console/delta?offset=' + encodeURIComponent(reqOffset) + '&max=4096')
      .then(function(r) { return r.json(); })
      .then(function(data) {
        var el = document.getElementById('log-text');
        if (!el || !data || data.success === false) return;
        var nextOffset = Number(data.next_offset || reqOffset);
        var truncated = !!data.truncated;
        var text = typeof data.text === 'string' ? data.text : '';
        if (truncated && consolePanel.logText.length > 0) {
          var note = '[log stream resynced]\n';
          consolePanel.logText += note;
          shellAppendPassiveLogs(note);
        }
        global.consoleLogBaseOffset = Number(data.base_offset || 0);
        global.consoleLogOffset = nextOffset;
        if (typeof data.rev !== 'undefined') global.lastConsoleRev = Number(data.rev);
        if (!text) {
          shellRender();
          return;
        }
        var lines = text.split('\n').filter(function(x) { return x.trim().length > 0; });
        var d = new Date();
        var ts = '[' + String(d.getHours()).padStart(2, '0') + ':' +
          String(d.getMinutes()).padStart(2, '0') + ':' +
          String(d.getSeconds()).padStart(2, '0') + '.' +
          String(d.getMilliseconds()).padStart(3, '0') + '] ';
        for (var i = 0; i < lines.length; i++) {
          var cleanLine = sanitizeConsoleLine(lines[i]);
          if (!cleanLine || isLikelyConsoleNoise(cleanLine)) continue;
          var rendered = ts + cleanLine + '\n';
          consolePanel.logText += rendered;
          if (consolePanel.logText.length > shellTerm.maxChars) {
            consolePanel.logText = consolePanel.logText.slice(consolePanel.logText.length - shellTerm.maxChars);
          }
          shellAppendPassiveLogs(rendered);
        }
        shellRender();
      });
  }

  function clearConsoleLocal() {
    if (consolePanel.mode === 'shell') {
      shellSetTranscript(shellTerm.prompt || '');
      shellTerm.input = '';
      shellTerm.cursor = 0;
      shellTerm.inputRevision++;
      shellTerm.pendingLogs = [];
    } else {
      consolePanel.logText = '';
    }
    shellSyncInputElement(true);
    syncShellInputUi();
    shellRender();
    syncMobileLogMirror();
  }

  function syncMobileLogMirror() {
    var mobileEl = document.getElementById('mobile-log-text');
    if (!mobileEl) return;
    mobileEl.value = shellStripAnsi(shellVisibleText());
    mobileEl.scrollTop = mobileEl.scrollHeight;
  }

  function setConsolePanelMode(mode) {
    var nextMode = mode === 'shell' ? 'shell' : 'log';
    consolePanel.mode = nextMode;
    var logBtn = document.getElementById('console-mode-log');
    var shellBtn = document.getElementById('console-mode-shell');
    if (logBtn) logBtn.classList.toggle('active', nextMode === 'log');
    if (shellBtn) shellBtn.classList.toggle('active', nextMode === 'shell');
    syncShellInputUi();
    shellRender();
    if (nextMode === 'shell') {
      shellSendState();
      var inputEl = shellInputEl();
      if (inputEl) inputEl.focus();
    }
  }

  function openMobileLogDrawer() {
    var drawer = document.getElementById('mobile-log-drawer');
    if (!drawer) return;
    drawer.classList.add('open');
    drawer.setAttribute('aria-hidden', 'false');
    syncMobileLogMirror();
  }

  function closeMobileLogDrawer() {
    var drawer = document.getElementById('mobile-log-drawer');
    if (!drawer) return;
    drawer.classList.remove('open');
    drawer.setAttribute('aria-hidden', 'true');
  }

  function openLogPanelFromTab(btnEl) {
    if (document.body.classList.contains('mobile-mode')) {
      document.querySelectorAll('.tab-btn').forEach(function(b) { b.classList.remove('active'); });
      if (btnEl) btnEl.classList.add('active');
      openMobileLogDrawer();
      return;
    }
    if (typeof global.switchTab === 'function') global.switchTab('tab-log', btnEl);
  }

  root.panels.shell = {
    consolePanel: consolePanel,
    shellTerm: shellTerm,
    initShellTerminal: initShellTerminal,
    shellHandleMessage: shellHandleMessage,
    fetchLogs: fetchLogs,
    setConsolePanelMode: setConsolePanelMode
  };

  global.consolePanel = consolePanel;
  global.shellTerm = shellTerm;
  global.shellScheduleResize = shellScheduleResize;
  global.shellSocketInstance = global.shellSocketInstance;
  global.shellSendState = shellSendState;
  global.shellSendExec = shellSendExec;
  global.shellRequestCompletion = shellRequestCompletion;
  global.shellRequestSuggest = shellRequestSuggest;
  global.shellHandleMessage = shellHandleMessage;
  global.fetchLogs = fetchLogs;
  global.initShellTerminal = initShellTerminal;
  global.clearConsoleLocal = clearConsoleLocal;
  global.setConsolePanelMode = setConsolePanelMode;
  global.openShellFullscreen = openShellFullscreen;
  global.closeShellFullscreen = closeShellFullscreen;
  global.openMobileLogDrawer = openMobileLogDrawer;
  global.closeMobileLogDrawer = closeMobileLogDrawer;
  global.openLogPanelFromTab = openLogPanelFromTab;
  global.syncMobileLogMirror = syncMobileLogMirror;
  global.shellAppendPassiveLogs = shellAppendPassiveLogs;
  global.shellRender = shellRender;
  global.shellStripAnsi = shellStripAnsi;
  global.shellRefreshState = shellRefreshState;
  global.shellKillProcess = shellKillProcess;
  global.shellFullscreenSplit = shellFullscreenSplit;
  global.shellFullscreenClosePane = shellFullscreenClosePane;
  global.shellSetAuthReady = shellSetAuthReady;
  global.shellInputStats = shellInputStats;
  global.applyShellFullscreenUiSettings = applyShellFullscreenUiSettings;
})(window);
