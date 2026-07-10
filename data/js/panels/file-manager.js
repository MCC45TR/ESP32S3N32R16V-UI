(function(global) {
  var state = {
    open: false,
    path: '/ESPUSER',
    mounts: [],
    view: 'list',
    scale: 1,
    sortKey: 'name',
    sortDir: 'asc',
    items: [],
    pageLimit: 200,
    nextOffset: 0,
    total: 0,
    hasMore: false,
    selected: new Set(),
    lastIndex: -1,
    clipboard: null,
    downloaderOpen: false,
    downloadTimer: null,
    downloadTransferId: null,
    transfers: [],
    editor: {
      open: false,
      item: null,
      path: '',
      language: 'text',
      mode: 'code',
      content: '',
      originalContent: '',
      csvRows: [],
      dirty: false,
      wrap: true,
      search: '',
      replace: '',
      diffOpen: false,
      activeCell: { row: 0, col: 0 },
      mtime: 0,
      readOnly: false,
      largeGuard: false
    },
    editorTabs: []
  };

  function el(id) { return document.getElementById(id); }
  function enc(v) { return encodeURIComponent(v || ''); }
  function fmtSize(bytes) {
    var n = Number(bytes || 0);
    var units = ['B', 'KB', 'MB', 'GB'];
    var i = 0;
    while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
    return (i === 0 ? String(Math.round(n)) : n.toFixed(n >= 10 ? 1 : 2)) + ' ' + units[i];
  }
  function fmtDate(value) {
    var n = Number(value || 0);
    if (!isFinite(n) || n <= 0) return '-';
    var ms = n > 100000000000 ? n : n * 1000;
    try {
      return new Date(ms).toLocaleString('tr-TR', {
        year: 'numeric', month: '2-digit', day: '2-digit',
        hour: '2-digit', minute: '2-digit'
      });
    } catch (err) {
      return String(value);
    }
  }
  function esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g, function(ch) {
      return ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[ch];
    });
  }
  function fileExt(path) {
    var name = String(path || '').toLowerCase();
    var slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    var dot = name.lastIndexOf('.');
    return dot >= 0 ? name.substring(dot + 1) : '';
  }
  function isTextExtension(ext) {
    return !!({
      c: 1, h: 1, hh: 1, hpp: 1, cpp: 1, cxx: 1, cc: 1, ino: 1,
      py: 1, json: 1, yml: 1, yaml: 1, cfg: 1, conf: 1, ini: 1,
      csv: 1, sql: 1, js: 1, css: 1, html: 1, htm: 1, xml: 1,
      md: 1, txt: 1, log: 1, sh: 1, msh: 1, bash: 1, zsh: 1,
      cmake: 1, toml: 1, env: 1, service: 1, list: 1, map: 1,
      license: 1, gitignore: 1, dockerfile: 1, makefile: 1
    })[String(ext || '').toLowerCase()];
  }
  function detectLanguage(path, item) {
    var ext = fileExt(path || (item && item.name) || '');
    var map = {
      c: 'c',
      h: 'cpp',
      hh: 'cpp',
      hpp: 'cpp',
      cpp: 'cpp',
      cxx: 'cpp',
      cc: 'cpp',
      ino: 'cpp',
      py: 'python',
      json: 'json',
      yml: 'yaml',
      yaml: 'yaml',
      cfg: 'cfg',
      conf: 'cfg',
      ini: 'cfg',
      csv: 'csv',
      db: 'sql',
      sql: 'sql',
      js: 'javascript',
      css: 'css',
      html: 'html',
      htm: 'html',
      xml: 'xml',
      md: 'markdown',
      txt: 'text',
      log: 'text',
      sh: 'shell',
      msh: 'shell',
      bash: 'shell',
      zsh: 'shell',
      cmake: 'cmake'
    };
    return map[ext] || ((item && item.kind === 'json') ? 'json' : ((item && item.kind === 'table') ? 'csv' : 'text'));
  }
  function isEditableFile(item) {
    if (!item || item.is_dir) return false;
    var ext = fileExt(item.path || item.name);
    if (isTextExtension(ext)) return true;
    var lang = detectLanguage(item.path || item.name, item);
    if (lang !== 'text') return true;
    var mime = String(item.mime || '').toLowerCase();
    var kind = String(item.kind || '').toLowerCase();
    return kind === 'text' || kind === 'code' || kind === 'json' || kind === 'table' ||
      mime.indexOf('text/') === 0 ||
      mime.indexOf('json') >= 0 ||
      mime.indexOf('xml') >= 0 ||
      mime.indexOf('javascript') >= 0 ||
      mime.indexOf('x-shellscript') >= 0;
  }
  function languageLabel(lang) {
    var labels = {
      c: 'C',
      cpp: 'C/C++',
      python: 'Python',
      json: 'JSON',
      yaml: 'YAML',
      cfg: 'CFG/INI',
      csv: 'CSV Tablo',
      sql: 'SQL/DB',
      javascript: 'JavaScript',
      css: 'CSS',
      html: 'HTML',
      xml: 'XML',
      markdown: 'Markdown',
      shell: 'Shell',
      cmake: 'CMake',
      text: 'Metin'
    };
    return labels[lang] || 'Metin';
  }
  function editorModeForLanguage(lang) {
    return lang === 'csv' ? 'csv' : 'code';
  }
  function api(url, opts) {
    return fetch(url, opts || {}).then(function(r) {
      return r.json().catch(function() { return { success: r.ok }; }).then(function(j) {
        if (!r.ok || j.success === false) throw new Error(j.error_code || j.error || ('HTTP ' + r.status));
        return j;
      });
    });
  }
  function providerForPath(path) {
    var p = String(path || state.path || '/ESPUSER');
    if (p === '/p4' || p.indexOf('/p4/') === 0) return 'p4';
    if (p === '/p4-sdcard' || p.indexOf('/p4-sdcard/') === 0) return 'p4-sdcard';
    return 'local';
  }
  function mountByProvider(provider) {
    for (var i = 0; i < state.mounts.length; i++) {
      if (state.mounts[i].provider === provider) return state.mounts[i];
    }
    return null;
  }
  function fetchMounts() {
    return api('/api/files/mounts').then(function(data) {
      state.mounts = data.mounts || [];
      return state.mounts;
    }).catch(function(err) {
      status('Mount durumu alınamadı: ' + err.message);
      return [];
    });
  }
  function mountAction(provider, action) {
    return api('/api/files/mount?target=' + enc(provider) + '&action=' + enc(action), { method: 'POST' })
      .then(function(data) {
        state.mounts = data.mounts || state.mounts;
        status((data.message || provider + ' ' + action) + '');
        renderBreadcrumb();
        return refresh();
      }).catch(function(err) {
        status('Mount hata: ' + err.message);
        return fetchMounts().then(function() { renderBreadcrumb(); });
      });
  }
  function itemByPath(path) {
    for (var i = 0; i < state.items.length; i++) if (state.items[i].path === path) return state.items[i];
    return null;
  }
  function selectedItems() {
    var out = [];
    state.selected.forEach(function(path) {
      var item = itemByPath(path);
      if (item) out.push(item);
    });
    return out;
  }
  function iconText(item) {
    if (!item) return 'FILE';
    if (item.is_dir) return 'DIR';
    var name = String(item.name || '').toUpperCase();
    var dot = name.lastIndexOf('.');
    if (dot >= 0 && dot + 1 < name.length) return name.substring(dot + 1, dot + 5);
    if (item.kind === 'image') return 'IMG';
    if (item.kind === 'text') return 'TXT';
    return 'BIN';
  }
  function icon(item) {
    var kind = item && item.kind ? item.kind : 'binary';
    return '<span class="fm-mime-icon fm-kind-' + esc(kind) + '">' + esc(iconText(item)) + '</span>';
  }
  function status(text) {
    var node = el('file-manager-status');
    if (node) node.textContent = text || 'Hazır';
  }
  function downloadUiStatus(phase, size, meta, pct) {
    var phaseNode = el('fm-download-phase');
    var sizeNode = el('fm-download-size');
    var metaNode = el('fm-download-meta');
    var fill = el('fm-download-bar-fill');
    if (phaseNode) phaseNode.textContent = phase || 'Hazır';
    if (sizeNode) sizeNode.textContent = size || '--';
    if (metaNode) metaNode.textContent = meta || '';
    if (fill) fill.style.width = Math.max(0, Math.min(100, Number(pct) || 0)).toFixed(1) + '%';
  }
  function pushTransfer(label, phase, progress, stateName) {
    var item = {
      id: 'fm-' + Date.now() + '-' + Math.floor(Math.random() * 1000),
      label: label || 'Transfer',
      phase: phase || 'Hazırlandı',
      progress: Math.max(0, Math.min(100, Number(progress) || 0)),
      state: stateName || 'active'
    };
    state.transfers.unshift(item);
    state.transfers = state.transfers.slice(0, 8);
    if (typeof global.mrosUxAddJob === 'function') item.uxJobId = global.mrosUxAddJob(item);
    renderTransferQueue();
    return item.id;
  }
  function updateTransfer(id, phase, progress, stateName) {
    var item = null;
    state.transfers.forEach(function(t) { if (t.id === id) item = t; });
    if (!item) return;
    item.phase = phase || item.phase;
    item.progress = Math.max(0, Math.min(100, Number(progress) || 0));
    item.state = stateName || item.state;
    if (typeof global.mrosUxUpdateJob === 'function' && item.uxJobId) global.mrosUxUpdateJob(item.uxJobId, item);
    renderTransferQueue();
  }
  function renderTransferQueue() {
    var node = el('file-manager-transfer-queue');
    if (!node) return;
    if (!state.transfers.length) {
      node.classList.remove('open');
      node.innerHTML = '';
      return;
    }
    node.classList.add('open');
    node.innerHTML = '<div class="fm-transfer-head"><strong>Transfer Kuyruğu</strong><span>' +
      state.transfers.length + ' işlem</span></div>' +
      state.transfers.map(function(t) {
        return '<div class="fm-transfer-row state-' + esc(t.state || 'active') + '">' +
          '<div><strong>' + esc(t.label) + '</strong><span>' + esc(t.phase) + '</span></div>' +
          '<div class="fm-transfer-meter"><i style="width:' + Math.max(0, Math.min(100, Number(t.progress) || 0)).toFixed(1) + '%"></i></div>' +
          '<code>' + Math.round(Number(t.progress) || 0) + '%</code>' +
          '</div>';
      }).join('');
  }
  function renderSelectionBar() {
    var node = el('file-manager-selectionbar');
    if (!node) return;
    var items = selectedItems();
    if (!items.length) {
      node.innerHTML = '<span>Seçim yok</span><strong>' + esc(state.path) + '</strong>';
      return;
    }
    var total = items.reduce(function(sum, item) { return sum + Number(item.size || 0); }, 0);
    node.innerHTML =
      '<span>' + items.length + ' öğe seçili</span>' +
      '<strong>' + esc(fmtSize(total)) + '</strong>' +
      (items.length === 1 && isEditableFile(items[0]) ? '<button type="button" data-fm-select-act="edit">Editörde aç</button>' : '') +
      '<button type="button" data-fm-select-act="download">İndir</button>' +
      '<button type="button" data-fm-select-act="clear">Seçimi temizle</button>' +
      '<button type="button" data-fm-select-act="delete" class="danger">Sil</button>';
    Array.prototype.forEach.call(node.querySelectorAll('[data-fm-select-act]'), function(btn) {
      btn.addEventListener('click', function() {
        var act = btn.getAttribute('data-fm-select-act');
        if (act === 'edit') openEditor(items[0]);
        if (act === 'download') downloadItems(items);
        if (act === 'clear') { state.selected.clear(); render(); }
        if (act === 'delete') deleteSelected();
      });
    });
  }
  function csvColumnName(index) {
    var n = Number(index || 0) + 1;
    var out = '';
    while (n > 0) {
      var r = (n - 1) % 26;
      out = String.fromCharCode(65 + r) + out;
      n = Math.floor((n - 1) / 26);
    }
    return out || 'A';
  }
  function parseCsv(text) {
    var rows = [];
    var row = [];
    var cell = '';
    var quoted = false;
    var src = String(text || '');
    for (var i = 0; i < src.length; i++) {
      var ch = src.charAt(i);
      if (quoted) {
        if (ch === '"' && src.charAt(i + 1) === '"') {
          cell += '"';
          i++;
        } else if (ch === '"') {
          quoted = false;
        } else {
          cell += ch;
        }
        continue;
      }
      if (ch === '"') {
        quoted = true;
      } else if (ch === ',') {
        row.push(cell);
        cell = '';
      } else if (ch === '\n') {
        row.push(cell);
        rows.push(row);
        row = [];
        cell = '';
      } else if (ch !== '\r') {
        cell += ch;
      }
    }
    row.push(cell);
    if (row.length > 1 || row[0] !== '' || rows.length === 0) rows.push(row);
    var cols = rows.reduce(function(max, r) { return Math.max(max, r.length); }, 1);
    rows.forEach(function(r) {
      while (r.length < cols) r.push('');
    });
    return rows;
  }
  function stringifyCsv(rows) {
    return (rows || []).map(function(row) {
      return (row || []).map(function(cell) {
        var text = String(cell == null ? '' : cell);
        if (/[",\r\n]/.test(text)) return '"' + text.replace(/"/g, '""') + '"';
        return text;
      }).join(',');
    }).join('\n');
  }
  function editorSetDirty(dirty) {
    state.editor.dirty = !!dirty;
    syncEditorTabFromState();
    var badge = el('fm-editor-dirty');
    if (badge) badge.textContent = state.editor.dirty ? 'Kaydedilmedi' : 'Kaydedildi';
    if (badge) badge.classList.toggle('dirty', state.editor.dirty);
  }
  function editorTabLabel(path) {
    var text = String(path || 'Yeni dosya');
    var slash = text.lastIndexOf('/');
    return slash >= 0 ? text.substring(slash + 1) : text;
  }
  function syncEditorTabFromState() {
    if (!state.editor.open || !state.editor.path) return;
    var found = false;
    state.editorTabs.forEach(function(tab) {
      if (tab.path === state.editor.path) {
        tab.dirty = !!state.editor.dirty;
        tab.content = state.editor.content;
        tab.originalContent = state.editor.originalContent;
        tab.language = state.editor.language;
        tab.mode = state.editor.mode;
        tab.mtime = state.editor.mtime || 0;
        tab.readOnly = !!state.editor.readOnly;
        found = true;
      }
    });
    if (!found) {
      state.editorTabs.push({
        path: state.editor.path,
        dirty: !!state.editor.dirty,
        content: state.editor.content || '',
        originalContent: state.editor.originalContent || '',
        language: state.editor.language || 'text',
        mode: state.editor.mode || 'code',
        mtime: state.editor.mtime || 0,
        readOnly: !!state.editor.readOnly
      });
    }
  }
  function restoreEditorTab(tab) {
    if (!tab) return;
    if (state.editor.open) {
      if (state.editor.mode === 'csv') state.editor.content = stringifyCsv(collectCsvFromDom());
      else {
        var ta = el('fm-editor-textarea');
        if (ta) state.editor.content = ta.value;
      }
      syncEditorTabFromState();
    }
    state.editor = {
      open: true,
      item: itemByPath(tab.path) || { name: editorTabLabel(tab.path), path: tab.path, is_dir: false, kind: 'text' },
      path: tab.path,
      language: tab.language || detectLanguage(tab.path),
      mode: tab.mode || editorModeForLanguage(tab.language || detectLanguage(tab.path)),
      content: tab.content || '',
      originalContent: tab.originalContent || '',
      csvRows: (tab.mode === 'csv' || tab.language === 'csv') ? parseCsv(tab.content || '') : [],
      dirty: !!tab.dirty,
      wrap: state.editor.wrap !== false,
      search: state.editor.search || '',
      replace: state.editor.replace || '',
      diffOpen: false,
      activeCell: { row: 0, col: 0 },
      mtime: tab.mtime || 0,
      readOnly: !!tab.readOnly,
      largeGuard: !!tab.readOnly
    };
    renderEditor();
  }
  function closeEditorTab(path) {
    var target = String(path || state.editor.path || '');
    var tab = state.editorTabs.filter(function(t) { return t.path === target; })[0];
    if (tab && tab.dirty && !global.confirm('Kaydedilmemiş sekme kapatılsın mı?')) return;
    state.editorTabs = state.editorTabs.filter(function(t) { return t.path !== target; });
    if (state.editor.path === target) {
      if (state.editorTabs.length) restoreEditorTab(state.editorTabs[state.editorTabs.length - 1]);
      else closeEditor(true);
    } else {
      renderEditor();
    }
  }
  function renderEditorTabs() {
    if (!state.editorTabs.length || (state.editor.path && !state.editorTabs.some(function(t) { return t.path === state.editor.path; }))) {
      syncEditorTabFromState();
    }
    if (!state.editorTabs.length) return '';
    return '<div class="fm-editor-tabs" role="tablist">' +
      state.editorTabs.map(function(tab) {
        var active = tab.path === state.editor.path;
        return '<button type="button" class="fm-editor-tab' + (active ? ' active' : '') + (tab.dirty ? ' dirty' : '') +
          '" data-editor-tab="' + esc(tab.path) + '" title="' + esc(tab.path) + '">' +
          '<span>' + esc(editorTabLabel(tab.path)) + '</span>' +
          (tab.readOnly ? '<em>RO</em>' : '') +
          (tab.dirty ? '<b>*</b>' : '') +
          '<i data-editor-tab-close="' + esc(tab.path) + '">×</i>' +
        '</button>';
      }).join('') + '</div>';
  }
  function editorStatsText(text) {
    var src = String(text || '');
    var bytes = 0;
    if (global.TextEncoder) bytes = new TextEncoder().encode(src).length;
    else bytes = src.length;
    var lines = src.length ? src.split(/\n/).length : 1;
    return lines + ' satır | ' + fmtSize(bytes);
  }
  function editorCurrentText() {
    if (state.editor.mode === 'csv') {
      state.editor.csvRows = collectCsvFromDom();
      state.editor.content = stringifyCsv(state.editor.csvRows);
    } else {
      var textarea = el('fm-editor-textarea');
      if (textarea) state.editor.content = textarea.value;
    }
    return String(state.editor.content || '');
  }
  function editorSetText(text) {
    state.editor.content = String(text == null ? '' : text);
    if (state.editor.mode === 'csv') {
      state.editor.csvRows = parseCsv(state.editor.content);
      renderCsvEditorBody();
      return;
    }
    var textarea = el('fm-editor-textarea');
    if (textarea) textarea.value = state.editor.content;
    updateCodeEditorPreview();
  }
  function editorSyncSearchInputs() {
    var s = el('fm-editor-search');
    var r = el('fm-editor-replace');
    if (s) state.editor.search = s.value || '';
    if (r) state.editor.replace = r.value || '';
  }
  function editorSetSearchStatus(text) {
    var statusNode = el('fm-editor-search-status');
    if (statusNode) statusNode.textContent = text || '';
  }
  function editorFindNext() {
    editorSyncSearchInputs();
    var needle = String(state.editor.search || '');
    if (!needle) {
      editorSetSearchStatus('Arama metni yok');
      return false;
    }
    if (state.editor.mode === 'csv') {
      editorSetSearchStatus('CSV modunda tablo içinde tarama kullanılıyor');
      return false;
    }
    var textarea = el('fm-editor-textarea');
    if (!textarea) return false;
    var haystack = textarea.value || '';
    var start = textarea.selectionEnd || 0;
    var index = haystack.toLocaleLowerCase('tr-TR')
      .indexOf(needle.toLocaleLowerCase('tr-TR'), start);
    if (index < 0 && start > 0) {
      index = haystack.toLocaleLowerCase('tr-TR')
        .indexOf(needle.toLocaleLowerCase('tr-TR'), 0);
    }
    if (index < 0) {
      editorSetSearchStatus('Bulunamadı');
      return false;
    }
    textarea.focus();
    textarea.setSelectionRange(index, index + needle.length);
    editorSetSearchStatus('Bulundu: ' + (index + 1));
    return true;
  }
  function editorReplaceOne() {
    editorSyncSearchInputs();
    var textarea = el('fm-editor-textarea');
    if (!textarea || !state.editor.search) return;
    var selected = textarea.value.substring(textarea.selectionStart || 0, textarea.selectionEnd || 0);
    if (selected.toLocaleLowerCase('tr-TR') !== state.editor.search.toLocaleLowerCase('tr-TR')) {
      if (!editorFindNext()) return;
    }
    var start = textarea.selectionStart || 0;
    var end = textarea.selectionEnd || start;
    textarea.value = textarea.value.substring(0, start) + state.editor.replace + textarea.value.substring(end);
    textarea.setSelectionRange(start, start + state.editor.replace.length);
    state.editor.content = textarea.value;
    editorSetDirty(true);
    updateCodeEditorPreview();
    editorSetSearchStatus('1 eşleşme değiştirildi');
  }
  function editorReplaceAll() {
    editorSyncSearchInputs();
    var needle = String(state.editor.search || '');
    if (!needle) return;
    var text = editorCurrentText();
    var pattern = new RegExp(needle.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'gi');
    var count = 0;
    var next = text.replace(pattern, function() {
      count++;
      return state.editor.replace || '';
    });
    editorSetText(next);
    editorSetDirty(count > 0 || state.editor.dirty);
    editorSetSearchStatus(count + ' eşleşme değiştirildi');
  }
  function editorGoToLine() {
    var textarea = el('fm-editor-textarea');
    if (!textarea) return;
    var raw = global.prompt('Satır numarası', '1');
    var line = Math.max(1, Number(raw || 1) || 1);
    var text = textarea.value || '';
    var index = 0;
    for (var i = 1; i < line && index < text.length; i++) {
      var next = text.indexOf('\n', index);
      if (next < 0) break;
      index = next + 1;
    }
    textarea.focus();
    textarea.setSelectionRange(index, index);
    editorSetSearchStatus('Satır ' + line);
  }
  function renderEditorDiff() {
    var node = el('fm-editor-diff');
    if (!node) return;
    if (!state.editor.diffOpen) {
      node.innerHTML = '';
      return;
    }
    var before = String(state.editor.originalContent || '').split(/\n/);
    var after = editorCurrentText().split(/\n/);
    var max = Math.max(before.length, after.length);
    var rows = [];
    var changed = 0;
    for (var i = 0; i < max && rows.length < 80; i++) {
      if ((before[i] || '') === (after[i] || '')) continue;
      changed++;
      rows.push('<div class="fm-editor-diff-row"><b>' + (i + 1) + '</b><span>- ' +
        esc(before[i] == null ? '' : before[i]) + '</span><span>+ ' +
        esc(after[i] == null ? '' : after[i]) + '</span></div>');
    }
    node.innerHTML = '<div class="fm-editor-diff-title">Diff önizleme: ' +
      changed + ' değişen satır</div>' + (rows.join('') || '<div class="fm-editor-diff-empty">Değişiklik yok</div>');
  }
  function syntaxKeywordPattern(lang) {
    if (lang === 'python') return /\b(False|None|True|and|as|assert|async|await|break|class|continue|def|elif|else|except|finally|for|from|global|if|import|in|is|lambda|not|or|pass|raise|return|try|while|with|yield)\b/g;
    if (lang === 'c' || lang === 'cpp') return /\b(auto|bool|break|case|catch|char|class|const|constexpr|continue|default|do|double|else|enum|extern|false|float|for|if|inline|int|long|namespace|nullptr|private|protected|public|return|short|sizeof|static|struct|switch|template|this|true|try|typedef|typename|uint8_t|uint16_t|uint32_t|uint64_t|void|volatile|while)\b/g;
    if (lang === 'javascript') return /\b(await|async|break|case|catch|class|const|continue|default|else|export|false|finally|for|function|if|import|let|new|null|return|switch|this|throw|true|try|undefined|var|while)\b/g;
    if (lang === 'shell') return /\b(alias|break|case|cd|continue|do|done|elif|else|esac|eval|exec|exit|export|fi|for|function|if|in|local|printf|read|return|set|shift|source|test|then|trap|umask|unset|until|while)\b/g;
    if (lang === 'sql') return /\b(ALTER|AND|AS|BY|CREATE|DELETE|DROP|FROM|GROUP|INSERT|INTO|JOIN|KEY|LIMIT|NOT|NULL|OR|ORDER|PRIMARY|SELECT|SET|TABLE|UPDATE|VALUES|WHERE)\b/gi;
    if (lang === 'cmake') return /\b(add_executable|add_library|cmake_minimum_required|endif|endforeach|function|if|include|list|message|option|project|set|target_include_directories|target_link_libraries)\b/g;
    return null;
  }
  function highlightCode(text, lang) {
    var working = String(text || '');
    var tokens = [];
    function stash(cls, value) {
      var id = '\uE000' + String.fromCharCode(0xE100 + tokens.length) + '\uE000';
      tokens.push({ id: id, html: '<span class="fm-syn-' + cls + '">' + esc(value) + '</span>' });
      return id;
    }
    if (lang === 'json') {
      working = working.replace(/"(?:\\.|[^"\\])*"(?=\s*:)/g, function(m) { return stash('key', m); });
      working = working.replace(/"(?:\\.|[^"\\])*"/g, function(m) { return stash('str', m); });
    } else {
      working = working.replace(/("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')/g, function(m) { return stash('str', m); });
      if (lang === 'python' || lang === 'shell' || lang === 'yaml' || lang === 'cfg') {
        working = working.replace(/(^|\s)(#.*)$/gm, function(m, p, c) { return p + stash('comment', c); });
      } else if (lang === 'c' || lang === 'cpp' || lang === 'javascript' || lang === 'css') {
        working = working.replace(/(\/\/.*$|\/\*[\s\S]*?\*\/)/gm, function(m) { return stash('comment', m); });
      } else if (lang === 'sql') {
        working = working.replace(/(--.*$)/gm, function(m) { return stash('comment', m); });
      } else if (lang === 'markdown') {
        working = working.replace(/(^#{1,6}\s.*$)/gm, function(m) { return stash('keyword', m); });
      }
    }
    var html = esc(working);
    if (lang === 'shell') {
      html = html.replace(/^(\s*)(#![^\n\r]*)/gm, '$1<span class="fm-syn-shebang">$2</span>');
      html = html.replace(/(^|[\s;|&])(\.?\/[^\s;&|<>]+|[A-Za-z_][A-Za-z0-9_-]*)(?=(?:\s|$))/g, '$1<span class="fm-syn-command">$2</span>');
      html = html.replace(/(\$\{?[A-Za-z_][A-Za-z0-9_]*\}?|\$[0-9@#?*-])/g, '<span class="fm-syn-var">$1</span>');
      html = html.replace(/(\|\||\|)/g, '<span class="fm-syn-op">$1</span>');
    }
    html = html.replace(/\b(-?\d+(?:\.\d+)?(?:e[+-]?\d+)?)\b/gi, '<span class="fm-syn-num">$1</span>');
    html = html.replace(/\b(true|false|null|None|True|False)\b/g, '<span class="fm-syn-bool">$1</span>');
    var keywordPattern = syntaxKeywordPattern(lang);
    if (keywordPattern) html = html.replace(keywordPattern, '<span class="fm-syn-keyword">$1</span>');
    tokens.forEach(function(t) {
      html = html.split(t.id).join(t.html);
    });
    return html;
  }
  function updateCodeEditorPreview() {
    var textarea = el('fm-editor-textarea');
    if (textarea) state.editor.content = textarea.value;
    var preview = el('fm-editor-preview');
    if (preview) preview.innerHTML = highlightCode(state.editor.content, state.editor.language);
    var lines = el('fm-editor-lines');
    if (lines) {
      var count = Math.max(1, String(state.editor.content || '').split(/\n/).length);
      var html = '';
      for (var i = 1; i <= count; i++) html += '<span>' + i + '</span>';
      lines.innerHTML = html;
    }
    var stats = el('fm-editor-stats');
    if (stats) stats.textContent = editorStatsText(state.editor.content);
  }
  function collectCsvFromDom() {
    var table = el('fm-csv-table');
    if (!table) return state.editor.csvRows || [];
    var rows = [];
    Array.prototype.forEach.call(table.querySelectorAll('tbody tr'), function(tr) {
      var row = [];
      Array.prototype.forEach.call(tr.querySelectorAll('[data-csv-cell]'), function(td) {
        row.push(td.textContent || '');
      });
      rows.push(row);
    });
    return rows;
  }
  function renderCsvEditorBody() {
    var body = el('fm-editor-body');
    if (!body) return;
    var rows = state.editor.csvRows || [[]];
    var cols = rows.reduce(function(max, r) { return Math.max(max, r.length); }, 1);
    rows.forEach(function(r) { while (r.length < cols) r.push(''); });
    var thead = '<tr><th class="fm-csv-corner"></th>';
    for (var c = 0; c < cols; c++) thead += '<th>' + csvColumnName(c) + '</th>';
    thead += '</tr>';
    var tbody = rows.map(function(row, r) {
      return '<tr><th>' + (r + 1) + '</th>' + row.map(function(cell, c) {
        var active = (state.editor.activeCell.row === r && state.editor.activeCell.col === c) ? ' active' : '';
        return '<td class="' + active + '" contenteditable="true" spellcheck="false" data-csv-cell="1" data-r="' + r + '" data-c="' + c + '">' + esc(cell) + '</td>';
      }).join('') + '</tr>';
    }).join('');
    body.innerHTML =
      '<div class="fm-csv-toolbar">' +
        '<button type="button" data-csv-act="add-row">Satır +</button>' +
        '<button type="button" data-csv-act="add-col">Sütun +</button>' +
        '<button type="button" data-csv-act="del-row">Satırı Sil</button>' +
        '<button type="button" data-csv-act="del-col">Sütunu Sil</button>' +
      '</div>' +
      '<div class="fm-csv-scroll"><table id="fm-csv-table" class="fm-csv-table"><thead>' + thead + '</thead><tbody>' + tbody + '</tbody></table></div>';
    Array.prototype.forEach.call(body.querySelectorAll('[data-csv-cell]'), function(cell) {
      cell.addEventListener('focus', function() {
        state.editor.activeCell = { row: Number(cell.getAttribute('data-r')) || 0, col: Number(cell.getAttribute('data-c')) || 0 };
        Array.prototype.forEach.call(body.querySelectorAll('[data-csv-cell].active'), function(n) { n.classList.remove('active'); });
        cell.classList.add('active');
      });
      cell.addEventListener('input', function() {
        state.editor.csvRows = collectCsvFromDom();
        state.editor.content = stringifyCsv(state.editor.csvRows);
        editorSetDirty(true);
        renderEditorDiff();
        var stats = el('fm-editor-stats');
        if (stats) stats.textContent = state.editor.csvRows.length + ' satır | ' + (state.editor.csvRows[0] || []).length + ' sütun | ' + fmtSize(state.editor.content.length);
      });
    });
    Array.prototype.forEach.call(body.querySelectorAll('[data-csv-act]'), function(btn) {
      btn.addEventListener('click', function() {
        var act = btn.getAttribute('data-csv-act');
        state.editor.csvRows = collectCsvFromDom();
        var current = state.editor.csvRows;
        var colCount = current.reduce(function(max, r) { return Math.max(max, r.length); }, 1);
        if (act === 'add-row') current.push(new Array(colCount).fill(''));
        if (act === 'add-col') current.forEach(function(r) { r.push(''); });
        if (act === 'del-row' && current.length > 1) current.splice(Math.min(state.editor.activeCell.row, current.length - 1), 1);
        if (act === 'del-col' && colCount > 1) current.forEach(function(r) { r.splice(Math.min(state.editor.activeCell.col, r.length - 1), 1); });
        state.editor.activeCell.row = Math.min(state.editor.activeCell.row, current.length - 1);
        state.editor.activeCell.col = Math.max(0, Math.min(state.editor.activeCell.col, (current[0] || []).length - 1));
        state.editor.content = stringifyCsv(current);
        editorSetDirty(true);
        renderCsvEditorBody();
      });
    });
    var stats = el('fm-editor-stats');
    if (stats) stats.textContent = rows.length + ' satır | ' + cols + ' sütun | ' + fmtSize(String(state.editor.content || '').length);
  }
  function renderCodeEditorBody() {
    var body = el('fm-editor-body');
    if (!body) return;
    body.innerHTML =
      '<div class="fm-code-editor' + (state.editor.wrap ? ' wrap' : '') + '">' +
        '<div id="fm-editor-lines" class="fm-editor-lines"></div>' +
        '<textarea id="fm-editor-textarea" class="fm-editor-textarea" spellcheck="false" autocomplete="off"></textarea>' +
        '<pre id="fm-editor-preview" class="fm-editor-preview" aria-label="Renklendirilmiş önizleme"></pre>' +
      '</div>';
    var textarea = el('fm-editor-textarea');
    if (textarea) {
      textarea.value = state.editor.content || '';
      textarea.addEventListener('input', function() {
        state.editor.content = textarea.value;
        editorSetDirty(true);
        updateCodeEditorPreview();
        renderEditorDiff();
      });
      textarea.addEventListener('scroll', function() {
        var preview = el('fm-editor-preview');
        var lines = el('fm-editor-lines');
        if (preview) {
          preview.scrollTop = textarea.scrollTop;
          preview.scrollLeft = textarea.scrollLeft;
        }
        if (lines) lines.scrollTop = textarea.scrollTop;
      });
    }
    updateCodeEditorPreview();
  }
  function renderEditor() {
    var panel = el('file-manager-editor');
    var content = document.querySelector('.file-manager-content');
    if (!panel) return;
    if (!state.editor.open) {
      panel.classList.remove('open');
      if (content) content.classList.remove('editor-open');
      panel.innerHTML = '';
      return;
    }
    if (content) content.classList.add('editor-open');
    panel.classList.add('open');
    var langOptions = ['text', 'c', 'cpp', 'python', 'json', 'yaml', 'cfg', 'csv', 'sql', 'javascript', 'css', 'html', 'xml', 'markdown', 'shell', 'cmake'];
    panel.innerHTML =
      renderEditorTabs() +
      '<div class="fm-editor-head">' +
        '<div><div class="fm-editor-title">' + esc(state.editor.path || 'Yeni dosya') + '</div>' +
        '<div class="fm-editor-sub"><span id="fm-editor-dirty">' + (state.editor.dirty ? 'Kaydedilmedi' : 'Kaydedildi') + '</span><span id="fm-editor-stats">--</span><span>' + esc(fmtDate(state.editor.mtime)) + '</span>' + (state.editor.readOnly ? '<span class="fm-editor-readonly">Salt okunur</span>' : '') + '</div></div>' +
        '<div class="fm-editor-actions">' +
          '<select id="fm-editor-language" aria-label="Editör dili">' + langOptions.map(function(lang) {
            return '<option value="' + lang + '"' + (state.editor.language === lang ? ' selected' : '') + '>' + languageLabel(lang) + '</option>';
          }).join('') + '</select>' +
          '<button type="button" id="fm-editor-wrap">SATIR SAR</button>' +
          '<button type="button" id="fm-editor-reload">YENİDEN YÜKLE</button>' +
          '<button type="button" id="fm-editor-save">KAYDET</button>' +
          '<button type="button" id="fm-editor-close">KAPAT</button>' +
        '</div>' +
      '</div>' +
      '<div class="fm-editor-searchbar">' +
        '<input id="fm-editor-search" type="search" placeholder="Ara" value="' + esc(state.editor.search || '') + '">' +
        '<input id="fm-editor-replace" type="text" placeholder="Değiştir" value="' + esc(state.editor.replace || '') + '">' +
        '<button type="button" id="fm-editor-find">BUL</button>' +
        '<button type="button" id="fm-editor-replace-one">DEĞİŞTİR</button>' +
        '<button type="button" id="fm-editor-replace-all">TÜMÜ</button>' +
        '<button type="button" id="fm-editor-goto">SATIR</button>' +
        '<button type="button" id="fm-editor-diff-toggle">DIFF</button>' +
        '<span id="fm-editor-search-status" class="fm-editor-search-status"></span>' +
      '</div>' +
      '<div id="fm-editor-diff" class="fm-editor-diff"></div>' +
      '<div id="fm-editor-body" class="fm-editor-body"></div>';
    var dirty = el('fm-editor-dirty');
    if (dirty) dirty.classList.toggle('dirty', state.editor.dirty);
    Array.prototype.forEach.call(panel.querySelectorAll('[data-editor-tab]'), function(tabBtn) {
      tabBtn.addEventListener('click', function(ev) {
        var close = ev.target && ev.target.getAttribute ? ev.target.getAttribute('data-editor-tab-close') : '';
        if (close) {
          ev.preventDefault();
          ev.stopPropagation();
          closeEditorTab(close);
          return;
        }
        var path = tabBtn.getAttribute('data-editor-tab');
        var tab = state.editorTabs.filter(function(t) { return t.path === path; })[0];
        restoreEditorTab(tab);
      });
    });
    var langSelect = el('fm-editor-language');
    if (langSelect) langSelect.addEventListener('change', function() {
      if (state.editor.mode === 'csv') state.editor.content = stringifyCsv(collectCsvFromDom());
      else {
        var ta = el('fm-editor-textarea');
        if (ta) state.editor.content = ta.value;
      }
      state.editor.language = langSelect.value || 'text';
      state.editor.mode = editorModeForLanguage(state.editor.language);
      if (state.editor.mode === 'csv') state.editor.csvRows = parseCsv(state.editor.content);
      renderEditor();
      editorSetDirty(true);
    });
    var wrapBtn = el('fm-editor-wrap');
    if (wrapBtn) wrapBtn.addEventListener('click', function() {
      state.editor.wrap = !state.editor.wrap;
      renderCodeEditorBody();
    });
    var saveBtn = el('fm-editor-save');
    if (saveBtn) saveBtn.disabled = !!state.editor.readOnly;
    if (saveBtn) saveBtn.addEventListener('click', saveEditor);
    var closeBtn = el('fm-editor-close');
    if (closeBtn) closeBtn.addEventListener('click', closeEditor);
    var reloadBtn = el('fm-editor-reload');
    if (reloadBtn) reloadBtn.addEventListener('click', function() {
      if (state.editor.item) openEditor(state.editor.item, true);
    });
    var findBtn = el('fm-editor-find');
    if (findBtn) findBtn.addEventListener('click', editorFindNext);
    var replaceOneBtn = el('fm-editor-replace-one');
    if (replaceOneBtn) replaceOneBtn.addEventListener('click', editorReplaceOne);
    var replaceAllBtn = el('fm-editor-replace-all');
    if (replaceAllBtn) replaceAllBtn.addEventListener('click', editorReplaceAll);
    var gotoBtn = el('fm-editor-goto');
    if (gotoBtn) gotoBtn.addEventListener('click', editorGoToLine);
    var diffBtn = el('fm-editor-diff-toggle');
    if (diffBtn) diffBtn.addEventListener('click', function() {
      state.editor.diffOpen = !state.editor.diffOpen;
      renderEditorDiff();
    });
    var searchInput = el('fm-editor-search');
    if (searchInput) searchInput.addEventListener('keydown', function(ev) {
      if (ev.key === 'Enter') {
        ev.preventDefault();
        editorFindNext();
      }
    });
    if (state.editor.mode === 'csv') renderCsvEditorBody();
    else renderCodeEditorBody();
    renderEditorDiff();
  }
  function closeEditor(force) {
    if (!force && state.editor.dirty && !global.confirm('Kaydedilmemiş değişiklikler kapatılsın mı?')) return;
    state.editorTabs = state.editorTabs.filter(function(t) { return t.path !== state.editor.path; });
    state.editor.open = false;
    state.editor.item = null;
    renderEditor();
  }
  function openEditor(item, forceReload) {
    if (!item || item.is_dir) return;
    var existing = state.editorTabs.filter(function(tab) { return tab.path === item.path; })[0];
    if (!forceReload && existing) {
      restoreEditorTab(existing);
      return;
    }
    if (!forceReload && state.editor.dirty && !global.confirm('Kaydedilmemiş değişiklikler var. Yeni dosya açılsın mı?')) return;
    if (!isEditableFile(item)) {
      status('Bu dosya metin editörü için uygun değil.');
      return;
    }
    var largeReadOnly = Number(item.size || 0) > 768 * 1024;
    if (!forceReload && Number(item.size || 0) > 512 * 1024 &&
        !global.confirm('Bu dosya büyük (' + fmtSize(item.size) + '). Açılırsa ' + (largeReadOnly ? 'salt okunur modda ' : '') + 'çalışacak. Devam edilsin mi?')) return;
    status('Editör açılıyor: ' + item.name);
    fetch('/api/files/download?path=' + enc(item.path), { cache: 'no-store' })
      .then(function(res) {
        if (!res.ok) throw new Error('HTTP ' + res.status);
        return res.text();
      })
      .then(function(text) {
        var lang = detectLanguage(item.path, item);
        state.editor = {
          open: true,
          item: item,
          path: item.path,
          language: lang,
          mode: editorModeForLanguage(lang),
          content: text,
          originalContent: text,
          csvRows: lang === 'csv' ? parseCsv(text) : [],
          dirty: false,
          wrap: state.editor.wrap !== false,
          search: state.editor.search || '',
          replace: state.editor.replace || '',
          diffOpen: false,
          activeCell: { row: 0, col: 0 },
          mtime: Number(item.mtime || item.modified || 0),
          readOnly: largeReadOnly || item.writable === false,
          largeGuard: largeReadOnly
        };
        syncEditorTabFromState();
        renderEditor();
        status('Editör hazır: ' + item.name);
      })
      .catch(function(err) {
        status('Editör hata: ' + err.message);
      });
  }
  function newTextFile() {
    var name = global.prompt('Yeni dosya adı', 'notlar.txt');
    if (!name) return;
    var path = state.path.replace(/\/$/, '') + '/' + name;
    var lang = detectLanguage(path);
    state.editor = {
      open: true,
      item: { name: name, path: path, is_dir: false, size: 0, writable: true, kind: lang === 'csv' ? 'table' : 'text' },
      path: path,
      language: lang,
      mode: editorModeForLanguage(lang),
      content: '',
      originalContent: '',
      csvRows: lang === 'csv' ? [['']] : [],
      dirty: true,
      wrap: state.editor.wrap !== false,
      search: state.editor.search || '',
      replace: state.editor.replace || '',
      diffOpen: false,
      activeCell: { row: 0, col: 0 },
      mtime: 0,
      readOnly: false,
      largeGuard: false
    };
    syncEditorTabFromState();
    renderEditor();
  }
  function saveEditor() {
    if (!state.editor.open || !state.editor.path) return;
    if (state.editor.readOnly) {
      status('Salt okunur dosya kaydedilemez: ' + state.editor.path);
      return;
    }
    if (state.editor.mode === 'csv') {
      state.editor.csvRows = collectCsvFromDom();
      state.editor.content = stringifyCsv(state.editor.csvRows);
    } else {
      var textarea = el('fm-editor-textarea');
      if (textarea) state.editor.content = textarea.value;
    }
    var saveBtn = el('fm-editor-save');
    if (saveBtn) saveBtn.disabled = true;
    status('Kaydediliyor: ' + state.editor.path);
    var expectedMtime = Number(state.editor.mtime || 0) || 0;
    fetch('/api/files/save?path=' + enc(state.editor.path) + '&expected_mtime=' + enc(expectedMtime), {
      method: 'POST',
      headers: {
        'Content-Type': 'text/plain; charset=utf-8',
        'If-Unmodified-Since-MROS': String(expectedMtime)
      },
      body: state.editor.content
    }).then(function(res) {
      return res.json().catch(function() { return { success: res.ok }; }).then(function(j) {
        if (!res.ok || j.success === false) {
          var err = new Error(j.error || ('HTTP ' + res.status));
          err.payload = j;
          err.status = res.status;
          throw err;
        }
        return j;
      });
    }).then(function(j) {
      state.editor.originalContent = state.editor.content;
      state.editor.mtime = Number(j.mtime || j.modified || state.editor.mtime || 0) || state.editor.mtime;
      editorSetDirty(false);
      renderEditorDiff();
      status('Kaydedildi: ' + state.editor.path);
      refresh();
    }).catch(function(err) {
      if (err && err.payload && err.payload.error === 'STALE_FILE') {
        state.editor.diffOpen = true;
        renderEditorDiff();
        var current = err.payload.current_modified || err.payload.current_mtime || 'bilinmiyor';
        status('Kayıt durduruldu: dosya cihazda değişmiş (' + current + '). Yeniden yükleyin veya farkları inceleyin.');
        return;
      }
      status('Kayıt hata: ' + err.message);
    }).finally(function() {
      if (saveBtn) saveBtn.disabled = false;
    });
  }
  function downloadTargetValue() {
    var target = el('fm-download-target');
    return target && target.value.trim() ? target.value.trim() : 'auto';
  }
  function downloadUrlValue() {
    var url = el('fm-download-url');
    return url ? url.value.trim() : '';
  }
  function basenameFromUrl(rawUrl) {
    var text = String(rawUrl || '').trim();
    if (!text) return 'download.bin';
    try {
      var u = new URL(text, global.location && global.location.href ? global.location.href : undefined);
      text = u.pathname || text;
    } catch (err) {
      var q = text.indexOf('?');
      if (q >= 0) text = text.substring(0, q);
    }
    text = text.replace(/\/+$/, '');
    var slash = Math.max(text.lastIndexOf('/'), text.lastIndexOf('\\'));
    var name = slash >= 0 ? text.substring(slash + 1) : text;
    name = (name || 'download.bin').replace(/[\\/:*?"<>|]/g, '_');
    return name || 'download.bin';
  }
  function fillDownloadTargetFromCwd() {
    var target = el('fm-download-target');
    if (!target) return;
    var name = basenameFromUrl(downloadUrlValue());
    target.value = state.path.replace(/\/$/, '') + '/' + name;
    downloadUiStatus('Hedef hazır', '--', 'Bu dizine indir: ' + target.value, 0);
  }
  function setActiveViewButtons() {
    ['list', 'grid', 'detail'].forEach(function(view) {
      var node = el('fm-view-' + view);
      if (node) node.classList.toggle('active', state.view === view);
    });
  }
  function clampScale(value) {
    var n = Number(value);
    if (!isFinite(n)) n = 1;
    return Math.max(0.75, Math.min(1.45, Math.round(n * 20) / 20));
  }
  function applyScale() {
    var content = document.querySelector('#fileManagerModal .file-manager-content');
    state.scale = clampScale(state.scale);
    if (content) {
      content.style.setProperty('--fm-scale', String(state.scale));
      content.style.setProperty('--fm-font', Math.round(12 * state.scale * 10) / 10 + 'px');
      content.style.setProperty('--fm-pad', Math.round(10 * state.scale) + 'px');
      content.style.setProperty('--fm-card-min', Math.round(132 * state.scale) + 'px');
    }
    var label = el('fm-scale-value');
    if (label) label.textContent = Math.round(state.scale * 100) + '%';
    try { global.localStorage.setItem('mros.fileManager.scale', String(state.scale)); } catch (err) {}
  }
  function adjustScale(delta) {
    state.scale = clampScale(state.scale + Number(delta || 0));
    applyScale();
  }
  function renderBreadcrumb() {
    var node = el('file-manager-breadcrumb');
    if (!node) return;
    var parts = state.path.split('/').filter(Boolean);
    var provider = providerForPath(state.path);
    var remote = provider !== 'local';
    var mount = remote ? mountByProvider(provider) : null;
    var html = '<span class="fm-source-switch">' +
      '<button type="button" data-source-path="/ESPUSER"' + (provider === 'local' ? ' class="active"' : '') + '>S3 /ESPUSER</button>' +
      '<button type="button" data-source-path="/p4"' + (provider === 'p4' ? ' class="active"' : '') + '>P4</button>' +
      '<button type="button" data-source-path="/p4-sdcard"' + (provider === 'p4-sdcard' ? ' class="active"' : '') + '>P4 SDCard</button>' +
      '</span>';
    if (remote) {
      var mounted = mount && mount.mounted;
      html += '<button type="button" data-mount-act="' + (mounted ? 'umount' : 'mount') +
        '" data-mount-target="' + esc(provider) + '">' + (mounted ? 'Unmount' : 'Bağlan/Mount') + '</button>';
      html += '<span>' + esc((mount && (mount.peer_status || mount.error_code)) || 'unknown') + '</span>';
    }
    html += '<button type="button" data-path="' + (remote ? esc('/' + provider) : '/ESPUSER') + '">' +
      (remote ? esc('/' + provider) : '/ESPUSER') + '</button>';
    var cur = '';
    for (var i = 0; i < parts.length; i++) {
      cur += '/' + parts[i];
      if (cur === '/ESPUSER' || cur === '/p4' || cur === '/p4-sdcard') continue;
      html += '<span>/</span><button type="button" data-path="' + esc(cur) + '">' + esc(parts[i]) + '</button>';
    }
    node.innerHTML = html;
    Array.prototype.forEach.call(node.querySelectorAll('[data-path]'), function(btn) {
      btn.addEventListener('click', function() { openPath(btn.getAttribute('data-path') || '/ESPUSER'); });
    });
    Array.prototype.forEach.call(node.querySelectorAll('[data-source-path]'), function(btn) {
      btn.addEventListener('click', function() {
        openPath(btn.getAttribute('data-source-path') || '/ESPUSER');
      });
    });
    Array.prototype.forEach.call(node.querySelectorAll('[data-mount-act]'), function(btn) {
      btn.addEventListener('click', function() {
        mountAction(btn.getAttribute('data-mount-target') || provider, btn.getAttribute('data-mount-act') || 'status');
      });
    });
    var sub = el('file-manager-subtitle');
    if (sub) sub.textContent = state.path;
  }
  function sortedItems() {
    var key = state.sortKey || 'name';
    var dir = state.sortDir === 'desc' ? -1 : 1;
    return state.items.slice().sort(function(a, b) {
      if (!!a.is_dir !== !!b.is_dir) return a.is_dir ? -1 : 1;
      var av;
      var bv;
      if (key === 'size') {
        av = a.is_dir ? -1 : Number(a.size || 0);
        bv = b.is_dir ? -1 : Number(b.size || 0);
      } else if (key === 'type') {
        av = a.is_dir ? 'folder' : String(a.mime || a.kind || '');
        bv = b.is_dir ? 'folder' : String(b.mime || b.kind || '');
      } else if (key === 'mtime') {
        av = Number(a.mtime || 0);
        bv = Number(b.mtime || 0);
      } else {
        av = String(a.name || '').toLowerCase();
        bv = String(b.name || '').toLowerCase();
      }
      if (typeof av === 'number' || typeof bv === 'number') {
        if (av !== bv) return av < bv ? -1 * dir : 1 * dir;
      } else {
        var cmp = String(av).localeCompare(String(bv), 'tr');
        if (cmp !== 0) return cmp * dir;
      }
      return String(a.name || '').localeCompare(String(b.name || ''), 'tr');
    });
  }
  function rowHtml(item, index) {
    var selected = state.selected.has(item.path) ? ' selected' : '';
    return '<tr class="fm-row' + selected + '" data-index="' + index + '" data-path="' + esc(item.path) + '">' +
      '<td><div class="fm-name">' + icon(item) + '<span>' + esc(item.name) + '</span></div></td>' +
      '<td>' + esc(item.is_dir ? 'Klasör' : item.mime || item.kind) + '</td>' +
      '<td>' + esc(item.is_dir ? '-' : fmtSize(item.size)) + '</td>' +
      '<td>' + esc(fmtDate(item.mtime || item.modified)) + '</td>' +
      '<td>' + esc(item.path) + '</td>' +
      '</tr>';
  }
  function cardHtml(item, index) {
    var selected = state.selected.has(item.path) ? ' selected' : '';
    return '<div class="fm-card' + selected + '" data-index="' + index + '" data-path="' + esc(item.path) + '">' +
      '<div>' + icon(item) + '</div>' +
      '<div class="fm-card-name">' + esc(item.name) + '</div>' +
      '<div class="fm-card-meta">' + esc(item.is_dir ? 'Klasör' : fmtSize(item.size)) + '</div>' +
      '<div class="fm-card-meta">' + esc(fmtDate(item.mtime || item.modified)) + '</div>' +
      '</div>';
  }
  function detailRowHtml(item, index) {
    var selected = state.selected.has(item.path) ? ' selected' : '';
    var type = item.is_dir ? 'Klasör' : (item.mime || item.kind || languageLabel(detectLanguage(item.path, item)));
    var actions = item.is_dir
      ? '<button type="button" data-fm-row-act="open">Aç</button>'
      : '<button type="button" data-fm-row-act="download">İndir</button>';
    if (isEditableFile(item)) actions += '<button type="button" data-fm-row-act="edit">Editörde aç</button>';
    return '<div class="fm-detail-row' + selected + '" data-index="' + index + '" data-path="' + esc(item.path) + '">' +
      '<div class="fm-detail-primary"><div class="fm-name">' + icon(item) + '<strong>' + esc(item.name) + '</strong></div>' +
      '<small>' + esc(item.path) + '</small></div>' +
      '<div class="fm-detail-meta">' +
      '<span>Tür: ' + esc(type) + '</span>' +
      '<span>Boyut: ' + esc(item.is_dir ? '-' : fmtSize(item.size)) + '</span>' +
      '<span>Değiştirilme: ' + esc(fmtDate(item.mtime || item.modified)) + '</span>' +
      '<span>Yazma: ' + esc(item.writable ? 'izinli' : 'salt-okunur') + '</span>' +
      '</div><div class="fm-detail-actions">' + actions + '</div></div>';
  }
  function renderDetail() {
    var node = el('file-manager-detail');
    if (!node) return;
    var items = selectedItems();
    if (items.length === 0) {
      node.innerHTML = '<div class="fm-detail-line">Seçim yok</div><div class="fm-detail-line">Yol: ' + esc(state.path) + '</div>';
      return;
    }
    if (items.length > 1) {
      var total = items.reduce(function(sum, it) { return sum + Number(it.size || 0); }, 0);
      node.innerHTML = '<div class="shell-fullscreen-title" style="font-size:14px;">' + items.length + ' öğe seçili</div>' +
        '<div class="fm-detail-line">Toplam boyut: ' + esc(fmtSize(total)) + '</div>' +
        '<div class="fm-detail-line">Sağ tık ile toplu işlem</div>';
      return;
    }
    var item = items[0];
    node.innerHTML = '<div style="margin-bottom:10px;">' + icon(item) + '</div>' +
      '<div class="shell-fullscreen-title" style="font-size:14px; word-break:break-word;">' + esc(item.name) + '</div>' +
      '<div class="fm-detail-line">Tür: ' + esc(item.is_dir ? 'Klasör' : item.mime || item.kind) + '</div>' +
      '<div class="fm-detail-line">Boyut: ' + esc(item.is_dir ? '-' : fmtSize(item.size)) + '</div>' +
      '<div class="fm-detail-line">Değiştirilme: ' + esc(fmtDate(item.mtime || item.modified)) + '</div>' +
      '<div class="fm-detail-line">Yol: ' + esc(item.path) + '</div>' +
      '<div class="fm-detail-line">Yazılabilir: ' + esc(item.writable ? 'evet' : 'hayır') + '</div>' +
      (isEditableFile(item) ? '<button type="button" class="fm-detail-edit" data-fm-edit-path="' + esc(item.path) + '">Editörde aç</button>' : '');
    var editBtn = node.querySelector('[data-fm-edit-path]');
    if (editBtn) editBtn.addEventListener('click', function() { openEditor(item); });
  }
  function bindItemEvents() {
    var body = el('file-manager-body');
    if (!body) return;
    Array.prototype.forEach.call(body.querySelectorAll('[data-fm-row-act]'), function(btn) {
      btn.addEventListener('click', function(ev) {
        ev.preventDefault();
        ev.stopPropagation();
        var row = btn.closest('[data-path]');
        var item = row ? itemByPath(row.getAttribute('data-path')) : null;
        if (!item) return;
        var act = btn.getAttribute('data-fm-row-act');
        if (act === 'edit') openEditor(item);
        else if (act === 'download') downloadItems([item]);
        else activateItem(item);
      });
    });
    Array.prototype.forEach.call(body.querySelectorAll('[data-path]'), function(node) {
      node.addEventListener('click', function(ev) {
        selectItem(node.getAttribute('data-path'), Number(node.getAttribute('data-index')), ev);
      });
      node.addEventListener('dblclick', function() {
        activateItem(itemByPath(node.getAttribute('data-path')));
      });
      node.addEventListener('contextmenu', function(ev) {
        ev.preventDefault();
        selectItem(node.getAttribute('data-path'), Number(node.getAttribute('data-index')), ev, true);
        showContext(ev.clientX, ev.clientY);
      });
    });
    body.oncontextmenu = function(ev) {
      if (ev.target.closest('[data-path]')) return;
      ev.preventDefault();
      showContext(ev.clientX, ev.clientY, true);
    };
  }
  function render() {
    setActiveViewButtons();
    renderBreadcrumb();
    var body = el('file-manager-body');
    if (!body) return;
    var rows = sortedItems();
    var moreHtml = state.hasMore
      ? '<div style="padding:10px 0;text-align:center;"><button type="button" class="fm-more-btn" id="file-manager-load-more">Daha fazla yükle (' +
        esc(String(rows.length)) + '/' + esc(String(state.total || rows.length)) + ')</button></div>'
      : '';
    if (state.view === 'grid') {
      body.innerHTML = '<div class="fm-grid">' + rows.map(cardHtml).join('') + '</div>' + moreHtml;
    } else if (state.view === 'detail') {
      body.innerHTML = '<div class="fm-detail-list">' + rows.map(detailRowHtml).join('') + '</div>' + moreHtml;
    } else {
      body.innerHTML = '<table class="fm-table"><thead><tr>' +
        sortHeaderHtml('name', 'Ad', '30%') +
        sortHeaderHtml('type', 'Tür', '14%') +
        sortHeaderHtml('size', 'Boyut', '12%') +
        sortHeaderHtml('mtime', 'Değiştirilme', '18%') +
        '<th>Yol</th>' +
        '</tr></thead><tbody>' + rows.map(rowHtml).join('') + '</tbody></table>' + moreHtml;
    }
    bindSortHeaders();
    bindItemEvents();
    var moreBtn = el('file-manager-load-more');
    if (moreBtn) moreBtn.addEventListener('click', loadMore);
    renderDetail();
    renderSelectionBar();
    renderTransferQueue();
    status(rows.length + (state.total ? '/' + state.total : '') + ' öğe | ' + state.selected.size + ' seçili');
  }
  function sortHeaderHtml(key, label, width) {
    var active = state.sortKey === key;
    var arrow = active ? (state.sortDir === 'desc' ? ' ↓' : ' ↑') : '';
    return '<th class="fm-sort-th' + (active ? ' active' : '') + '" style="width:' + width + ';" data-fm-sort="' + key + '">' +
      '<button type="button">' + esc(label + arrow) + '</button></th>';
  }
  function bindSortHeaders() {
    var body = el('file-manager-body');
    if (!body) return;
    Array.prototype.forEach.call(body.querySelectorAll('[data-fm-sort]'), function(th) {
      th.addEventListener('click', function() {
        var key = th.getAttribute('data-fm-sort') || 'name';
        if (state.sortKey === key) state.sortDir = state.sortDir === 'asc' ? 'desc' : 'asc';
        else {
          state.sortKey = key;
          state.sortDir = key === 'mtime' ? 'desc' : 'asc';
        }
        refresh();
      });
    });
  }
  function applyListPage(data, append) {
    var incoming = data.items || [];
    state.items = append ? state.items.concat(incoming) : incoming;
    state.nextOffset = Number(data.next_offset || state.items.length) || state.items.length;
    state.total = Number(data.total || state.items.length) || state.items.length;
    state.hasMore = !!data.has_more;
    var nextSelected = new Set();
    state.selected.forEach(function(path) {
      if (itemByPath(path)) nextSelected.add(path);
    });
    state.selected = nextSelected;
    render();
  }
  function refresh() {
    hideContext();
    status('Yükleniyor...');
    state.nextOffset = 0;
    state.total = 0;
    state.hasMore = false;
    return fetchMounts().then(function() {
      return api('/api/files/list?path=' + enc(state.path) + '&offset=0&limit=' + enc(String(state.pageLimit)) + '&sort=' + enc(state.sortKey) + '&dir=' + enc(state.sortDir));
    }).then(function(data) {
      applyListPage(data, false);
    }).catch(function(err) {
      status('Hata: ' + err.message);
      state.items = [];
      render();
    });
  }
  function loadMore() {
    if (!state.hasMore) return;
    status('Yükleniyor...');
    return api('/api/files/list?path=' + enc(state.path) + '&offset=' + enc(String(state.nextOffset)) + '&limit=' + enc(String(state.pageLimit)) + '&sort=' + enc(state.sortKey) + '&dir=' + enc(state.sortDir)).then(function(data) {
      applyListPage(data, true);
    }).catch(function(err) {
      status('Hata: ' + err.message);
    });
  }
  function toggleDownloader(force) {
    state.downloaderOpen = (typeof force === 'boolean') ? force : !state.downloaderOpen;
    var panel = el('file-manager-downloader');
    if (panel) panel.classList.toggle('open', state.downloaderOpen);
    if (state.downloaderOpen) pollDownloadStatus();
  }
  function checkDownload() {
    var url = downloadUrlValue();
    if (!url) {
      downloadUiStatus('URL gerekli', '--', 'İndirilecek bağlantıyı gir.', 0);
      return;
    }
    toggleDownloader(true);
    downloadUiStatus('Kontrol ediliyor', '--', 'Sunucu başlığı, indirilebilirlik ve boş alan kontrol ediliyor...', 8);
    api('/api/files/fetch/check?url=' + enc(url) + '&target=' + enc(downloadTargetValue()) + '&cwd=' + enc(state.path))
      .then(function(j) {
        var total = Number(j.content_length || -1);
        var free = Number(j.fs_free || 0);
        var ok = !!j.downloadable && !!j.enough_space;
        var size = total >= 0 ? fmtSize(total) + ' / boş ' + fmtSize(free) : 'Boyut bilinmiyor / boş ' + fmtSize(free);
        downloadUiStatus(ok ? 'Hazır' : 'Başlatılamaz', size,
          'HTTP ' + j.status_code + ' | hedef: ' + (j.target || '--') +
          (total < 0 ? ' | Content-Length yok' : (j.enough_space ? '' : ' | boş alan yetersiz')), ok ? 100 : 35);
        if (j.target) {
          var t = el('fm-download-target');
          if (t && (!t.value.trim() || t.value.trim() === 'auto')) t.value = j.target;
        }
      }).catch(function(err) {
        downloadUiStatus('Kontrol hatası', '--', err.message, 0);
      });
  }
  function startDownload() {
    var url = downloadUrlValue();
    if (!url) {
      downloadUiStatus('URL gerekli', '--', 'İndirilecek bağlantıyı gir.', 0);
      return;
    }
    toggleDownloader(true);
    downloadUiStatus('Başlatılıyor', '--', 'İndirme taskı hazırlanıyor...', 5);
    api('/api/files/fetch/start?url=' + enc(url) + '&target=' + enc(downloadTargetValue()) + '&cwd=' + enc(state.path), {
      method: 'POST'
    }).then(function(j) {
      state.downloadTransferId = pushTransfer('URL indir', 'Kuyrukta: ' + (j.target || '--'), 8, 'active');
      downloadUiStatus('Kuyrukta', '--', 'Hedef: ' + (j.target || '--'), 8);
      pollDownloadStatus(true);
    }).catch(function(err) {
      downloadUiStatus('Başlatma hatası', '--', err.message, 0);
    });
  }
  function pollDownloadStatus(force) {
    if (state.downloadTimer && !force) return;
    if (state.downloadTimer) {
      clearInterval(state.downloadTimer);
      state.downloadTimer = null;
    }
    function once() {
      api('/api/files/fetch/status').then(function(j) {
        var total = Number(j.total || -1);
        var written = Number(j.written || 0);
        var pct = total > 0 ? (written * 100 / total) : Number(j.progress || 0);
        var size = total > 0 ? fmtSize(written) + ' / ' + fmtSize(total) : fmtSize(written);
        var eta = Number(j.eta_ms || -1);
        var meta = (j.target || '--') + (eta > 0 ? ' | ETA ' + Math.ceil(eta / 1000) + ' sn' : '');
        if (j.error) meta += ' | ' + j.error;
        downloadUiStatus(j.phase || 'idle', size, meta, pct);
        if (j.active || j.done) {
          if (!state.downloadTransferId) state.downloadTransferId = pushTransfer('URL indir', j.phase || 'idle', pct, j.error ? 'danger' : (j.done ? 'ok' : 'active'));
          else updateTransfer(state.downloadTransferId, j.phase || 'idle', pct, j.error ? 'danger' : (j.done ? 'ok' : 'active'));
        }
        if (j.done && !j.active) {
          if (state.downloadTimer) clearInterval(state.downloadTimer);
          state.downloadTimer = null;
          if (j.ok) refresh();
        }
      }).catch(function(err) {
        downloadUiStatus('Durum alınamadı', '--', err.message, 0);
      });
    }
    once();
    state.downloadTimer = setInterval(once, 1000);
  }
  function openPath(path) {
    state.path = path || '/ESPUSER';
    state.selected.clear();
    state.lastIndex = -1;
    refresh();
  }
  function selectItem(path, index, ev, keepExisting) {
    if (!path) return;
    if (ev && ev.shiftKey && state.lastIndex >= 0) {
      var rows = sortedItems();
      var a = Math.min(state.lastIndex, index);
      var b = Math.max(state.lastIndex, index);
      if (!ev.ctrlKey && !ev.metaKey) state.selected.clear();
      for (var i = a; i <= b; i++) state.selected.add(rows[i].path);
    } else if (ev && (ev.ctrlKey || ev.metaKey)) {
      if (state.selected.has(path)) state.selected.delete(path);
      else state.selected.add(path);
      state.lastIndex = index;
    } else {
      if (!keepExisting || !state.selected.has(path)) {
        state.selected.clear();
        state.selected.add(path);
      }
      state.lastIndex = index;
    }
    render();
  }
  function activateItem(item) {
    if (!item) return;
    if (item.is_dir) openPath(item.path);
    else if (isEditableFile(item)) openEditor(item);
    else downloadItems([item]);
  }
  function downloadItems(items) {
    (items || selectedItems()).forEach(function(item) {
      if (!item.is_dir) global.open('/api/files/download?path=' + enc(item.path), '_blank');
    });
  }
  function postEach(endpoint, items, targetBuilder) {
    var chain = Promise.resolve();
    items.forEach(function(item) {
      chain = chain.then(function() {
        var target = targetBuilder ? targetBuilder(item) : '';
        return api(endpoint + '?path=' + enc(item.path) + target, { method: 'POST' });
      });
    });
    return chain.then(refresh).catch(function(err) { status('Hata: ' + err.message); });
  }
  function renameItem(item) {
    if (!item) return;
    var next = global.prompt('Yeni ad', item.name);
    if (!next) return;
    var to = state.path.replace(/\/$/, '') + '/' + next;
    api('/api/files/rename?path=' + enc(item.path) + '&to=' + enc(to), { method: 'POST' })
      .then(refresh).catch(function(err) { status('Hata: ' + err.message); });
  }
  function duplicateItem(item) {
    if (!item || item.is_dir) return;
    var dot = item.name.lastIndexOf('.');
    var base = dot > 0 ? item.name.substring(0, dot) : item.name;
    var ext = dot > 0 ? item.name.substring(dot) : '';
    var to = state.path.replace(/\/$/, '') + '/' + base + '-copy' + ext;
    api('/api/files/copy?path=' + enc(item.path) + '&to=' + enc(to), { method: 'POST' })
      .then(refresh).catch(function(err) { status('Hata: ' + err.message); });
  }
  function deleteSelected() {
    var items = selectedItems();
    if (!items.length) return;
    if (!global.confirm(items.length + ' öğe silinsin mi?')) return;
    postEach('/api/files/delete', items);
  }
  function newFolder() {
    var name = global.prompt('Klasör adı', 'new-folder');
    if (!name) return;
    var path = state.path.replace(/\/$/, '') + '/' + name;
    api('/api/files/mkdir?path=' + enc(path), { method: 'POST' })
      .then(refresh).catch(function(err) { status('Hata: ' + err.message); });
  }
  function upload() {
    var input = el('file-manager-upload-input');
    if (input) input.click();
  }
  function setupUpload() {
    var input = el('file-manager-upload-input');
    if (!input) return;
    input.addEventListener('change', function() {
      var files = Array.prototype.slice.call(input.files || []);
      var chain = Promise.resolve();
      files.forEach(function(file) {
        chain = chain.then(function() {
          status('Yükleniyor: ' + file.name);
          var transferId = pushTransfer('Upload: ' + file.name, 'Yükleniyor', 15, 'active');
          return fetch('/api/files/upload?path=' + enc(state.path.replace(/\/$/, '') + '/' + file.name), {
            method: 'POST',
            body: file
          }).then(function(r) { return r.json(); }).then(function(j) {
            if (!j.success) throw new Error(j.error || 'UPLOAD_FAILED');
            updateTransfer(transferId, 'Tamamlandı', 100, 'ok');
          });
        });
      });
      chain.then(function() {
        input.value = '';
        refresh();
      }).catch(function(err) {
        input.value = '';
        pushTransfer('Upload', err.message, 100, 'danger');
        status('Upload hata: ' + err.message);
      });
    });
  }
  function showContext(x, y, blank) {
    var menu = el('file-manager-context');
    if (!menu) return;
    var items = selectedItems();
    var one = items.length === 1 ? items[0] : null;
    var html = '';
    if (!blank && one) html += '<button data-act="open">Aç / indir</button>';
    if (!blank && one && isEditableFile(one)) html += '<button data-act="edit">Editörde aç</button>';
    if (!blank && items.length) html += '<button data-act="download">İndir</button>';
    html += '<button data-act="upload">Upload</button><button data-act="new-text">Yeni metin dosyası</button><button data-act="mkdir">Yeni klasör</button>';
    if (!blank && one) html += '<button data-act="rename">Yeniden adlandır</button>';
    if (!blank && one && !one.is_dir) html += '<button data-act="duplicate">Kopyasını oluştur</button>';
    if (!blank && items.length) html += '<button class="danger" data-act="delete">Sil</button>';
    menu.innerHTML = html;
    menu.style.display = 'block';
    menu.style.left = Math.min(x, global.innerWidth - 210) + 'px';
    menu.style.top = Math.min(y, global.innerHeight - 260) + 'px';
    Array.prototype.forEach.call(menu.querySelectorAll('button'), function(btn) {
      btn.addEventListener('click', function() {
        var act = btn.getAttribute('data-act');
        hideContext();
        if (act === 'open' && one) activateItem(one);
        if (act === 'edit' && one) openEditor(one);
        if (act === 'download') downloadItems(items);
        if (act === 'upload') upload();
        if (act === 'new-text') newTextFile();
        if (act === 'mkdir') newFolder();
        if (act === 'rename' && one) renameItem(one);
        if (act === 'duplicate' && one) duplicateItem(one);
        if (act === 'delete') deleteSelected();
      });
    });
  }
  function hideContext() {
    var menu = el('file-manager-context');
    if (menu) menu.style.display = 'none';
  }
  function open() {
    var modal = el('fileManagerModal');
    if (!modal) return;
    state.open = true;
    try {
      var cfg = {};
      if (typeof global.popupSettingsGetSection === 'function') cfg = Object.assign(cfg, global.popupSettingsGetSection('files') || {});
      if (typeof global.deviceSettingsGetSection === 'function') cfg = Object.assign(cfg, global.deviceSettingsGetSection('files') || {});
      if (cfg.defaultScale) state.scale = Number(cfg.defaultScale) / 100;
      if (cfg.defaultSort) state.sortKey = String(cfg.defaultSort || 'name');
      if (cfg.defaultSortDir) state.sortDir = String(cfg.defaultSortDir || 'asc') === 'desc' ? 'desc' : 'asc';
      if (cfg.defaultView) state.view = String(cfg.defaultView) === 'details' ? 'detail' : String(cfg.defaultView);
      var savedScale = parseFloat(global.localStorage.getItem('mros.fileManager.scale') || '1');
      if (isFinite(savedScale)) state.scale = savedScale;
      var savedView = global.localStorage.getItem('mros.fileManager.view');
      if (savedView === 'grid' || savedView === 'list' || savedView === 'detail') state.view = savedView;
    } catch (err) {}
    applyScale();
    modal.style.display = 'block';
    modal.setAttribute('aria-hidden', 'false');
    refresh();
    if (state.downloaderOpen) pollDownloadStatus(true);
  }
  function close() {
    var modal = el('fileManagerModal');
    if (!modal) return;
    if (state.editor.open && state.editor.dirty && !global.confirm('Kaydedilmemiş editör değişiklikleri var. Dosya yöneticisi kapatılsın mı?')) return;
    state.open = false;
    modal.style.display = 'none';
    modal.setAttribute('aria-hidden', 'true');
    hideContext();
    state.editor.open = false;
    state.editor.dirty = false;
    renderEditor();
    if (state.downloadTimer) {
      clearInterval(state.downloadTimer);
      state.downloadTimer = null;
    }
  }
  function setView(view) {
    state.view = (view === 'details') ? 'detail' : (view || 'list');
    try { global.localStorage.setItem('mros.fileManager.view', state.view); } catch (err) {}
    render();
  }

  document.addEventListener('click', hideContext);
  document.addEventListener('keydown', function(ev) {
    if (!state.open) return;
    var inEditor = ev.target && ev.target.closest && ev.target.closest('#file-manager-editor');
    if ((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === 's' && state.editor.open) {
      ev.preventDefault();
      saveEditor();
      return;
    }
    if (inEditor && ev.key !== 'Escape') return;
    if (ev.key === 'Escape') {
      if (state.editor.open) closeEditor();
      else close();
      return;
    }
    if (ev.key === 'F5') { ev.preventDefault(); refresh(); }
    if (ev.key === 'Enter') {
      var items = selectedItems();
      if (items.length === 1) activateItem(items[0]);
    }
    if ((ev.ctrlKey || ev.metaKey) && ev.key.toLowerCase() === 'a') {
      ev.preventDefault();
      state.items.forEach(function(item) { state.selected.add(item.path); });
      render();
    }
  });
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', setupUpload);
  } else {
    setupUpload();
  }

  global.openFileManager = open;
  global.closeFileManager = close;
  global.fileManagerRefresh = refresh;
  global.fileManagerSetView = setView;
  global.fileManagerAdjustScale = adjustScale;
  global.fileManagerUpload = upload;
  global.fileManagerNewFolder = newFolder;
  global.fileManagerToggleDownloader = toggleDownloader;
  global.fileManagerCheckDownload = checkDownload;
  global.fileManagerStartDownload = startDownload;
  global.fileManagerDownloadToCurrentDir = fillDownloadTargetFromCwd;
  global.fileManagerNewTextFile = newTextFile;
})(window);
