(function(global) {
  var root = global.MROS = global.MROS || {};
  var ux = root.ux = root.ux || {};

  var SETTINGS_KEYS = [
    'ui', 'terminal', 'net', 'perf', 'prefs', 'devices', 'services', 'general',
    'security', 'files', 'robot', 'update', 'storage', 'ai', 'devtools',
    'kin3d', 'pickplace'
  ];
  var settingsSnapshot = null;
  var settingsFavorites = [];
  var jobs = [];
  var jobSeq = 1;

  function $(id) { return document.getElementById(id); }
  function clone(obj) {
    try { return JSON.parse(JSON.stringify(obj || {})); }
    catch (e) { return {}; }
  }
  function esc(s) {
    return String(s == null ? '' : s).replace(/[&<>"']/g, function(ch) {
      return ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[ch];
    });
  }
  function getSettingsSection(key) {
    if (typeof global.popupSettingsGetSection !== 'function') return {};
    return clone(global.popupSettingsGetSection(key) || {});
  }
  function captureSettings() {
    var out = {};
    SETTINGS_KEYS.forEach(function(key) { out[key] = getSettingsSection(key); });
    return out;
  }
  function countSettingChanges(snapshot) {
    if (!snapshot) return 0;
    var now = captureSettings();
    var count = 0;
    SETTINGS_KEYS.forEach(function(key) {
      if (JSON.stringify(now[key] || {}) !== JSON.stringify(snapshot[key] || {})) count++;
    });
    return count;
  }
  function currentSettingsTabId() {
    var active = document.querySelector('.settings-tab-btn.active[data-settings-tab]');
    return active ? active.getAttribute('data-settings-tab') : 'settings-tab-ui';
  }

  function installBottomPanelModes() {
    var rootEl = $('panel-logs-root');
    var header = $('logs-header-row');
    if (!rootEl || !header || $('bottom-mode-bar')) return;
    var bar = document.createElement('div');
    bar.id = 'bottom-mode-bar';
    bar.className = 'bottom-mode-bar';
    bar.setAttribute('role', 'tablist');
    bar.setAttribute('aria-label', 'Alt panel modu');
    [
      ['operation', 'Operasyon'],
      ['console', 'Konsol'],
      ['telemetry', 'Telemetri'],
      ['errors', 'Hata'],
      ['jobs', 'Jobs']
    ].forEach(function(item) {
      var btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'bottom-mode-btn';
      btn.dataset.bottomMode = item[0];
      btn.textContent = item[1];
      btn.addEventListener('click', function() { setBottomPanelMode(item[0]); });
      bar.appendChild(btn);
    });
    header.insertBefore(bar, header.children[1] || null);

    var op = document.createElement('div');
    op.id = 'bottom-operation-panel';
    op.className = 'bottom-virtual-panel bottom-operation-panel';
    op.innerHTML =
      '<div class="bottom-op-grid">' +
      '<div class="bottom-op-card"><span>IK Kanalı</span><strong data-op-from="ik_fallback_state">--</strong></div>' +
      '<div class="bottom-op-card"><span>Hedef</span><strong data-op-from="ik_target_status">--</strong></div>' +
      '<div class="bottom-op-card"><span>Döngü</span><strong><i data-op-from="loop_ms">--</i> ms</strong></div>' +
      '<div class="bottom-op-card"><span>SPI</span><strong data-op-from="spi_conn_status">--</strong></div>' +
      '<div class="bottom-op-card"><span>C3 Kalite</span><strong data-op-from="c3_quality">--</strong></div>' +
      '<div class="bottom-op-card"><span>Yol</span><strong data-op-from="traj_summary">Kuyruk boş</strong></div>' +
      '</div>';
    rootEl.insertBefore(op, $('logs-container'));

    var jobPanel = document.createElement('div');
    jobPanel.id = 'bottom-jobs-panel';
    jobPanel.className = 'bottom-virtual-panel bottom-jobs-panel';
    jobPanel.innerHTML = '<div class="bottom-jobs-head"><strong>Operasyon Kuyruğu</strong><span>Upload, download, update ve uzun görevler</span></div><div id="bottom-jobs-list" class="bottom-jobs-list"></div>';
    rootEl.insertBefore(jobPanel, $('logs-container'));

    setBottomPanelMode(localStorage.getItem('mros.bottomMode') || 'operation');
    setInterval(syncOperationPanel, 1000);
  }

  function syncOperationPanel() {
    var panel = $('bottom-operation-panel');
    if (!panel) return;
    Array.prototype.forEach.call(panel.querySelectorAll('[data-op-from]'), function(node) {
      var src = $(node.getAttribute('data-op-from'));
      if (!src) return;
      node.textContent = (src.innerText || src.textContent || '--').trim() || '--';
    });
  }

  function setDisplay(el, display) {
    if (el) el.style.setProperty('display', display, 'important');
  }
  function setBottomPanelMode(mode) {
    var next = ['operation', 'console', 'telemetry', 'errors', 'jobs'].indexOf(mode) >= 0 ? mode : 'operation';
    localStorage.setItem('mros.bottomMode', next);
    var rootEl = $('panel-logs-root');
    if (rootEl) rootEl.dataset.bottomMode = next;
    Array.prototype.forEach.call(document.querySelectorAll('.bottom-mode-btn'), function(btn) {
      btn.classList.toggle('active', btn.dataset.bottomMode === next);
    });
    var logs = $('logs-container');
    var op = $('bottom-operation-panel');
    var job = $('bottom-jobs-panel');
    var consoleCol = $('console-panel-column');
    var errRow = $('error-logs-row');
    var consoleResizer = $('console-resizer');
    var internalResizer = $('internal-table-resizer');
    var p4 = $('p4-spi-panel');
    var c3 = $('c3-spi-panel');

    setDisplay(op, next === 'operation' ? 'block' : 'none');
    setDisplay(job, next === 'jobs' ? 'block' : 'none');
    setDisplay(logs, (next === 'console' || next === 'telemetry' || next === 'errors') ? 'flex' : 'none');

    if (next === 'console') {
      setDisplay(consoleCol, 'flex');
      if (consoleCol) consoleCol.style.setProperty('--console-width', '100%');
      setDisplay(errRow, 'none');
      setDisplay(consoleResizer, 'none');
      if (typeof global.setConsolePanelMode === 'function') global.setConsolePanelMode('shell');
    } else if (next === 'telemetry' || next === 'errors') {
      setDisplay(consoleCol, next === 'telemetry' ? 'none' : 'flex');
      if (consoleCol) consoleCol.style.setProperty('--console-width', next === 'errors' ? '32%' : '0%');
      setDisplay(errRow, 'flex');
      setDisplay(consoleResizer, next === 'errors' ? 'block' : 'none');
      setDisplay(internalResizer, 'block');
      setDisplay(p4, 'flex');
      setDisplay(c3, 'flex');
      if (next === 'errors' && typeof global.setConsolePanelMode === 'function') global.setConsolePanelMode('log');
    }
    syncOperationPanel();
    renderJobs();
    if (typeof global.updateFloatingToolsDockLayout === 'function') global.updateFloatingToolsDockLayout();
    global.dispatchEvent(new Event('resize'));
  }

  function addJob(job) {
    var next = job || {};
    next.id = next.id || ('job-' + (jobSeq++));
    next.label = next.label || 'Operasyon';
    next.phase = next.phase || 'Başladı';
    next.progress = Math.max(0, Math.min(100, Number(next.progress) || 0));
    next.updatedAt = Date.now();
    jobs.unshift(next);
    jobs = jobs.slice(0, 12);
    renderJobs();
    return next.id;
  }
  function updateJob(id, patch) {
    var item = null;
    jobs.forEach(function(job) { if (job.id === id) item = job; });
    if (!item) return addJob(patch);
    Object.keys(patch || {}).forEach(function(key) { item[key] = patch[key]; });
    item.progress = Math.max(0, Math.min(100, Number(item.progress) || 0));
    item.updatedAt = Date.now();
    renderJobs();
    return item.id;
  }
  function renderJobs() {
    var list = $('bottom-jobs-list');
    if (!list) return;
    if (!jobs.length) {
      list.innerHTML = '<div class="bottom-job-empty">Aktif operasyon yok. Dosya transferleri ve uzun görevler burada izlenecek.</div>';
      return;
    }
    list.innerHTML = jobs.map(function(job) {
      return '<div class="bottom-job-row state-' + esc(job.state || 'active') + '">' +
        '<div><strong>' + esc(job.label) + '</strong><span>' + esc(job.phase || '') + '</span></div>' +
        '<div class="bottom-job-meter"><i style="width:' + Math.max(0, Math.min(100, Number(job.progress) || 0)).toFixed(1) + '%"></i></div>' +
        '<code>' + Math.round(Number(job.progress) || 0) + '%</code>' +
        '</div>';
    }).join('');
  }

  function installSettingsUx() {
    var modal = $('settingsModal');
    var shell = modal ? modal.querySelector('.settings-content-shell') : null;
    if (!modal || !shell || $('settings-ux-toolbar')) return;
    settingsFavorites = loadFavorites();
    settingsSnapshot = captureSettings();

    var toolbar = document.createElement('div');
    toolbar.id = 'settings-ux-toolbar';
    toolbar.className = 'settings-ux-toolbar';
    toolbar.innerHTML =
      '<div class="settings-search-wrap"><input id="settings-ux-search" type="search" placeholder="Ayar, kategori veya komut ara..." autocomplete="off"><button id="settings-ux-fav-toggle" type="button" title="Bu sekmeyi favorilere ekle">Yıldız</button></div>' +
      '<div id="settings-favorite-strip" class="settings-favorite-strip"></div>' +
      '<div class="settings-change-basket"><span id="settings-change-count">0 değişiklik</span><button id="settings-apply-snapshot" type="button">Uygula</button><button id="settings-revert-snapshot" type="button">Geri al</button></div>';
    shell.parentNode.insertBefore(toolbar, shell);

    $('settings-ux-search').addEventListener('input', applySettingsSearch);
    $('settings-ux-fav-toggle').addEventListener('click', toggleCurrentFavorite);
    $('settings-apply-snapshot').addEventListener('click', function() {
      settingsSnapshot = captureSettings();
      updateSettingsChangeBasket();
    });
    $('settings-revert-snapshot').addEventListener('click', revertSettingsSnapshot);
    modal.addEventListener('input', scheduleSettingsDirtyCheck, true);
    modal.addEventListener('change', scheduleSettingsDirtyCheck, true);
    renderFavoriteStrip();
    updateSettingsFavoriteButton();
    updateSettingsChangeBasket();
  }

  var dirtyTimer = null;
  function scheduleSettingsDirtyCheck() {
    if (dirtyTimer) clearTimeout(dirtyTimer);
    dirtyTimer = setTimeout(updateSettingsChangeBasket, 120);
  }
  function updateSettingsChangeBasket() {
    var count = countSettingChanges(settingsSnapshot);
    var node = $('settings-change-count');
    if (node) node.textContent = count + ' değişiklik';
    var modal = $('settingsModal');
    if (modal) modal.classList.toggle('settings-dirty', count > 0);
  }
  function revertSettingsSnapshot() {
    if (!settingsSnapshot || typeof global.popupSettingsSetSection !== 'function') return;
    SETTINGS_KEYS.forEach(function(key) { global.popupSettingsSetSection(key, clone(settingsSnapshot[key] || {})); });
    if (typeof global.applyUiPanelSettings === 'function') global.applyUiPanelSettings(global.uiPanelGetSettings ? global.uiPanelGetSettings() : null);
    var activeBtn = document.querySelector('.settings-tab-btn.active[data-settings-tab]');
    if (typeof global.switchSettingsTab === 'function') global.switchSettingsTab(currentSettingsTabId(), activeBtn);
    updateSettingsChangeBasket();
  }
  function loadFavorites() {
    try {
      var arr = JSON.parse(localStorage.getItem('mros.settingsFavorites') || '[]');
      return Array.isArray(arr) ? arr : [];
    } catch (e) { return []; }
  }
  function saveFavorites() {
    localStorage.setItem('mros.settingsFavorites', JSON.stringify(settingsFavorites.slice(0, 8)));
  }
  function tabLabel(tabId) {
    var btn = document.querySelector('.settings-tab-btn[data-settings-tab="' + tabId + '"]');
    return btn ? (btn.innerText || btn.textContent || tabId).trim() : tabId.replace('settings-tab-', '');
  }
  function renderFavoriteStrip() {
    var strip = $('settings-favorite-strip');
    if (!strip) return;
    if (!settingsFavorites.length) {
      strip.innerHTML = '<span>Favori sekme yok</span>';
      return;
    }
    strip.innerHTML = settingsFavorites.map(function(tabId) {
      return '<button type="button" data-fav-tab="' + esc(tabId) + '">' + esc(tabLabel(tabId)) + '</button>';
    }).join('');
    Array.prototype.forEach.call(strip.querySelectorAll('[data-fav-tab]'), function(btn) {
      btn.addEventListener('click', function() {
        var tab = btn.getAttribute('data-fav-tab');
        var tabBtn = document.querySelector('.settings-tab-btn[data-settings-tab="' + tab + '"]');
        if (typeof global.switchSettingsTab === 'function') global.switchSettingsTab(tab, tabBtn);
        setTimeout(function() { updateSettingsFavoriteButton(); applySettingsSearch(); }, 0);
      });
    });
  }
  function toggleCurrentFavorite() {
    var tab = currentSettingsTabId();
    var idx = settingsFavorites.indexOf(tab);
    if (idx >= 0) settingsFavorites.splice(idx, 1);
    else settingsFavorites.unshift(tab);
    settingsFavorites = settingsFavorites.slice(0, 8);
    saveFavorites();
    renderFavoriteStrip();
    updateSettingsFavoriteButton();
  }
  function updateSettingsFavoriteButton() {
    var btn = $('settings-ux-fav-toggle');
    if (!btn) return;
    var active = settingsFavorites.indexOf(currentSettingsTabId()) >= 0;
    btn.classList.toggle('active', active);
    btn.textContent = active ? 'Favori' : 'Yıldız';
  }
  function applySettingsSearch() {
    var qNode = $('settings-ux-search');
    var q = qNode ? qNode.value.trim().toLowerCase() : '';
    Array.prototype.forEach.call(document.querySelectorAll('.settings-tab-btn[data-settings-tab]'), function(btn) {
      var hay = (btn.innerText || btn.textContent || '').toLowerCase();
      btn.classList.toggle('settings-search-hidden', !!q && hay.indexOf(q) < 0);
    });
    var active = document.querySelector('.settings-tab-content.active');
    if (active) {
      Array.prototype.forEach.call(active.querySelectorAll('.settings-section, .settings-card, .kin3d-settings-group'), function(section) {
        var hay = (section.innerText || section.textContent || '').toLowerCase();
        section.classList.toggle('settings-search-hidden', !!q && hay.indexOf(q) < 0);
      });
    }
  }

  function hookGlobalFunctions() {
    if (global.__mrosUxHooksInstalled) return;
    global.__mrosUxHooksInstalled = true;
    var openSettings = global.openSettingsModal;
    if (typeof openSettings === 'function') {
      global.openSettingsModal = function() {
        openSettings.apply(this, arguments);
        setTimeout(function() {
          installSettingsUx();
          settingsSnapshot = captureSettings();
          updateSettingsFavoriteButton();
          updateSettingsChangeBasket();
        }, 0);
      };
    }
    var switchSettings = global.switchSettingsTab;
    if (typeof switchSettings === 'function') {
      global.switchSettingsTab = function() {
        var ret = switchSettings.apply(this, arguments);
        setTimeout(function() {
          updateSettingsFavoriteButton();
          applySettingsSearch();
          updateSettingsChangeBasket();
        }, 0);
        return ret;
      };
    }
  }

  function init() {
    installBottomPanelModes();
    installSettingsUx();
    hookGlobalFunctions();
    renderJobs();
    if (typeof global.updateFloatingToolsDockLayout === 'function') global.updateFloatingToolsDockLayout();
    global.addEventListener('resize', function() {
      if (typeof global.updateFloatingToolsDockLayout === 'function') global.updateFloatingToolsDockLayout();
    });
  }

  ux.setBottomPanelMode = setBottomPanelMode;
  ux.addJob = addJob;
  ux.updateJob = updateJob;
  global.setBottomPanelMode = setBottomPanelMode;
  global.mrosUxAddJob = addJob;
  global.mrosUxUpdateJob = updateJob;

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', init);
  else init();
})(window);
