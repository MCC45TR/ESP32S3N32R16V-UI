(function(global) {
  var root = global.MROS = global.MROS || {};
  root.panels = root.panels || {};

  var settingsAboutCache = null;
  var settingsAboutLoading = false;
  var settingsAboutLiveTimer = null;

  var UI_PANEL_SETTINGS_KEY = 'ui';
  var TERMINAL_SETTINGS_KEY = 'terminal';
  var NET_SETTINGS_KEY = 'net';
  var PERF_SETTINGS_KEY = 'perf';
  var PREFS_SETTINGS_KEY = 'prefs';
  var DEVICES_SETTINGS_KEY = 'devices';
  var SERVICES_SETTINGS_KEY = 'services';
  var GENERAL_SETTINGS_KEY = 'general';
  var SECURITY_SETTINGS_KEY = 'security';
  var FILES_SETTINGS_KEY = 'files';
  var ROBOT_SETTINGS_KEY = 'robot';
  var UPDATE_SETTINGS_KEY = 'update';
  var STORAGE_SETTINGS_KEY = 'storage';
  var AI_SETTINGS_KEY = 'ai';
  var DEVTOOLS_SETTINGS_KEY = 'devtools';
  var UI_PANEL_DEFAULTS = {
    interfaceMode: 'comprehensive',
    densityMode: 'normal',
    followCadTheme: true,
    themeMode: 'dark',
    colorPalette: 'mros',
    hideLogsPanel: false,
    hideP4SpiPanel: false,
    hideC3SpiPanel: false,
    hideLoopIndicator: false,
    hideLogDownload: false,
    showQuickSettings: true,
    showKeyboardHints: true,
    autoCollapsePanels: false
  };
  var UI_COLOR_PALETTE_DEFAULT = 'mros';
  var UI_THEME_MODE_DEFAULT = 'dark';
  var UI_THEME_MODE_VALUES = ['system', 'dark', 'light'];
  var UI_COLOR_PALETTES = {
    mros: {
      label: 'MROS Lime',
      dark: ['#302c2c', '#4e4848', '#3d3838', '#f0f2f3', '#b5ea6a', '#9beb5d', '#7ea8f2', '#625b5b', '#ea7b7b', '#eac27c'],
      light: ['#f6f8f3', '#ffffff', '#e7ecdf', '#172016', '#5e8f2f', '#79ad44', '#416fb7', '#cbd5c4', '#c84f5a', '#b47a20']
    },
    graphite: {
      label: 'Graphite',
      dark: ['#15171b', '#23262d', '#1c1f25', '#edf2f7', '#a8b3c7', '#c0cad8', '#6f9de8', '#39404b', '#ef767a', '#e6b86f'],
      light: ['#f4f6f8', '#ffffff', '#e4e8ee', '#1b2028', '#5c6678', '#798395', '#426fb5', '#cdd3dd', '#c75056', '#a8732a']
    },
    ocean: {
      label: 'Ocean',
      dark: ['#081820', '#12303d', '#0d2632', '#e9f8ff', '#58d5c9', '#3bbdad', '#72a7ff', '#1f4b5d', '#ff7373', '#f5c86f'],
      light: ['#effcff', '#ffffff', '#d9f0f4', '#0c2630', '#1b948f', '#157c78', '#386fc6', '#bad8df', '#c84f54', '#aa7a21']
    },
    ember: {
      label: 'Ember',
      dark: ['#211916', '#3b2b25', '#2c211d', '#fff2e8', '#f2b36d', '#d8944f', '#7fa6ff', '#5a4036', '#ef6f6c', '#ffd166'],
      light: ['#fff8f1', '#ffffff', '#f2e3d6', '#251711', '#c97832', '#a95f25', '#456fc3', '#dcc8b9', '#c9504b', '#a87616']
    },
    violet: {
      label: 'Violet',
      dark: ['#191622', '#2b2638', '#231e2f', '#f4efff', '#c79cff', '#a87df0', '#71d0ff', '#443a55', '#ff7c9d', '#f0c76e'],
      light: ['#fbf8ff', '#ffffff', '#ece4f7', '#21182e', '#8155c7', '#6742a6', '#3274aa', '#d6cbe4', '#c85674', '#a5751b']
    },
    slate: {
      label: 'Slate Mint',
      dark: ['#111b1c', '#203032', '#192729', '#eaf8f5', '#8fd8c3', '#67bda6', '#7aa7ff', '#33494c', '#ef7878', '#e7c06f'],
      light: ['#f3fbf9', '#ffffff', '#dfebe8', '#132423', '#358e7a', '#2d7565', '#426fc7', '#c7d7d4', '#c75353', '#9e7422']
    }
  };
  var TERMINAL_THEME_DEFAULT = 'mros';
  var TERMINAL_DEFAULTS = {
    theme: TERMINAL_THEME_DEFAULT,
    followUiTheme: true,
    showRefreshButton: true,
    showKillButton: true,
    fontSize: 12,
    fullscreenFontSize: 13,
    shellProfile: 'operator',
    historyLimit: 80,
    commandTimeoutMs: 5000,
    showManHints: true,
    wrapLongOutput: true,
    dynamicFetchLayout: true,
    fullscreenOpacity: 25,
    fullscreenBlurPx: 10
  };
  var TERMINAL_THEMES = {
    mros: { label: 'MROS Lime', bg: '#221f1f', surface: '#2b2626', fg: '#d8f7b2', accent: '#B5EA6A', muted: '#8ea36f', border: '#5E5656', cursor: '#B5EA6A' },
    amber: { label: 'Amber CRT', bg: '#17120b', surface: '#241a0d', fg: '#ffd98a', accent: '#ffb84d', muted: '#a77b45', border: '#5d4320', cursor: '#ffb84d' },
    ocean: { label: 'Ocean Blue', bg: '#071821', surface: '#0d2431', fg: '#bcecff', accent: '#4DB8FF', muted: '#7ca7bb', border: '#1c4d66', cursor: '#82E9FF' },
    graphite: { label: 'Graphite', bg: '#111317', surface: '#1a1d24', fg: '#d9e1ea', accent: '#9fb0bf', muted: '#788491', border: '#343b45', cursor: '#f0f2f3' },
    matrix: { label: 'Matrix', bg: '#020b05', surface: '#07150b', fg: '#a9ffbd', accent: '#38ff78', muted: '#4e9a67', border: '#164525', cursor: '#38ff78' }
  };

  var NET_DEFAULTS = {
    autoScan: true,
    rememberLastSsid: true,
    showSignalDetails: true,
    mdnsName: 'mros-bridge'
  };
  var PERF_DEFAULTS = {
    telemetryProfile: 'balanced',
    logTailBytes: 8192,
    preferPsramBuffers: true,
    reduceMotion: false,
    dpmPolicy: 'observe',
    powerMode: 'balanced'
  };
  var PREFS_DEFAULTS = {
    defaultPanel: '3d',
    confirmDestructive: true,
    reopenLastPopup: false,
    compactToolDock: false,
    keyboardShortcuts: true,
    showTooltips: true,
    restoreLastView: true,
    confirmUpdate: true,
    confirmPower: true,
    dateFormat: 'locale',
    unitPreset: 'metric'
  };
  var DEVICES_DEFAULTS = {
    p4AutoReconnect: true,
    c3StatusVisible: true,
    espnowPreferBridge: false,
    recoveryWarnings: true,
    passiveDiagVisible: true,
    uartShellBridge: 'off'
  };
  var SERVICES_DEFAULTS = {
    sshEnabled: false,
    mcpEnabled: false,
    mcpAllowShell: true
  };
  var GENERAL_DEFAULTS = {
    locale: 'tr_TR.utf8',
    startupPanel: '3d',
    deviceAlias: 'mros-s3',
    showBootSummary: true
  };
  var SECURITY_DEFAULTS = {
    sessionTimeoutMin: 30,
    requireRootConfirm: true,
    requireDangerConfirm: true,
    allowRememberSession: false
  };
  var FILES_DEFAULTS = {
    defaultView: 'details',
    showHidden: false,
    multiSelect: true,
    uploadLimitKb: 1024,
    openTextEditor: true,
    defaultScale: 100,
    defaultSort: 'name',
    defaultSortDir: 'asc'
  };
  var ROBOT_DEFAULTS = {
    mathProfile: 'default',
    mathBackend: 'auto',
    onboardMathEnabled: false,
    previewRequired: true,
    singularityWarnings: true,
    cartesianSpeedMmS: 80,
    trajectoryMode: 'quintic',
    solver: 'dls',
    jacobian: 'numerical',
    nullspace: 'joint-center',
    seedPolicy: 'current',
    limitsProfile: 'soft',
    frame: 'base',
    units: 'mm-deg',
    posTolMm: 1.0,
    oriTolDeg: 2.0,
    singularityThreshold: 0.05,
    alphaStep: 0.8,
    nullGain: 0.15,
    lambdaMax: 0.5,
    maxStepDeg: 10.0,
    maxIter: 120,
    pathHeightMode: 'auto',
    groundZMm: 0,
    turretMode: 'nearest',
    cartStepMm: 12,
    yawStepDeg: 5,
    jumpRevoluteDeg: 35
  };
  var UPDATE_DEFAULTS = {
    firmwarePath: '/ESPUSER/firmware',
    autoCheckRecovery: true,
    requireBatterySafe: true,
    rollbackGuard: true,
    recoveryReadsDeviceSettings: true,
    autoOtaScan: false,
    otaScanHour: '03:00',
    scheduledReboot: false,
    scheduledRebootHour: '04:00',
    updateWindow: 'night',
    lastErrorCode: 'OK'
  };
  var STORAGE_DEFAULTS = {
    logTailBytes: 8192,
    keepLogDays: 7,
    cacheStaticAssets: true,
    warnFreeKb: 1024
  };
  var AI_DEFAULTS = {
    mode: 'proposal',
    showPlanPanel: true,
    requireApproval: true,
    allowShellTools: false
  };
  var DEVTOOLS_DEFAULTS = {
    debugEndpoints: true,
    websocketInspect: false,
    rawJsonExport: true,
    experimentalFlags: false,
    binaryTelemetry: true,
    shellBinary: true,
    cborControl: true,
    telemetryFieldGating: true,
    nativeHttpPilotVisible: false,
    verboseLogs: false,
    perfOverlay: false
  };

  function getSectionSettings(key, defaults) {
    var deviceKeys = {
      net: true, perf: true, devices: true, services: true, general: true,
      security: true, files: true, robot: true, update: true, storage: true,
      terminal: true, devtools: true
    };
    var popupSrc = (typeof global.popupSettingsGetSection === 'function')
      ? (global.popupSettingsGetSection(key) || {})
      : {};
    var deviceSrc = (deviceKeys[key] && typeof global.deviceSettingsGetSection === 'function')
      ? (global.deviceSettingsGetSection(key) || {})
      : {};
    var src = Object.assign({}, popupSrc, deviceSrc);
    var out = {};
    Object.keys(defaults).forEach(function(name) {
      out[name] = Object.prototype.hasOwnProperty.call(src, name) ? src[name] : defaults[name];
    });
    return out;
  }

  function saveSectionSettings(key, defaults, nextState) {
    if (typeof global.popupSettingsSetSection !== 'function') return;
    var clean = {};
    Object.keys(defaults).forEach(function(name) {
      var value = Object.prototype.hasOwnProperty.call(nextState, name) ? nextState[name] : defaults[name];
      if (typeof defaults[name] === 'boolean') value = !!value;
      if (typeof defaults[name] === 'number') value = Number(value) || defaults[name];
      clean[name] = value;
    });
    global.popupSettingsSetSection(key, clean);
    if (typeof global.deviceSettingsSetSection === 'function' &&
        ['net', 'perf', 'devices', 'services', 'general', 'security', 'files',
         'robot', 'update', 'storage', 'terminal', 'devtools'].indexOf(key) >= 0) {
      global.deviceSettingsSetSection(key, clean);
    }
  }

  var SETTINGS_NAV = {
    'settings-tab-general': {
      title: 'Genel',
      desc: 'Cihaz kimliği, kullanıcı profili, dil ve açılış davranışı.',
      sections: [
        ['quick', 'Hızlı'],
        ['identity', 'Kimlik'],
        ['locale', 'Dil ve Bölge'],
        ['startup', 'Başlangıç']
      ]
    },
    'settings-tab-ui': {
      title: 'Arayüz',
      desc: 'Web arayüzünün kapsamı, panelleri, yoğunluğu ve kullanıcı deneyimi.',
      sections: [
        ['theme', 'Tema'],
        ['scope', 'Kapsam'],
        ['layout', 'Yerleşim'],
        ['visibility', 'Paneller'],
        ['shell-ui', 'MShell']
      ]
    },
    'settings-tab-security': {
      title: 'Güvenlik',
      desc: 'Oturum, root onayı ve riskli işlem davranışları.',
      sections: [
        ['session', 'Oturum'],
        ['shell-sessions', 'Shell Oturumları'],
        ['users', 'Kullanıcılar'],
        ['passwords', 'Şifreler'],
        ['root-password', 'Kök Şifresi'],
        ['root', 'Root'],
        ['policy', 'Politika']
      ]
    },
    'settings-tab-profile': {
      title: 'Profil Yönetimi',
      desc: 'Kullanıcı, şifre, kök yetkisi ve aktif oturumları tek panelden yönet.',
      sections: [
        ['overview', 'Özet'],
        ['users', 'Kullanıcılar'],
        ['passwords', 'Şifreler'],
        ['sessions', 'Oturumlar']
      ]
    },
    'settings-tab-services': {
      title: 'Sistem Servisleri',
      desc: 'Uyku modunda bekleyen SSH, MCP ve web servis izinleri.',
      sections: [
        ['core', 'Çekirdek'],
        ['mcp', 'MCP'],
        ['status', 'Durum']
      ]
    },
    'settings-tab-net': {
      title: 'Ağ',
      desc: 'Shell wifi komutlarıyla aynı işlevleri web ayarlarından yönet.',
      sections: [
        ['state', 'Durum'],
        ['connect', 'Bağlan'],
        ['scan', 'Tarama']
      ]
    },
    'settings-tab-terminal': {
      title: 'Terminal ve Shell',
      desc: 'Web shell görünümü, font, tema, geçmiş ve komut davranışları.',
      sections: [
        ['theme', 'Tema'],
        ['font', 'Yazı'],
        ['shell', 'Shell'],
        ['fullscreen', 'Fullscreen']
      ]
    },
    'settings-tab-files': {
      title: 'Dosya Yönetimi',
      desc: '/ESPUSER dosya yöneticisinin görünüm, seçim ve yükleme davranışı.',
      sections: [
        ['view', 'Görünüm'],
        ['transfer', 'Transfer'],
        ['editor', 'Editör']
      ]
    },
    'settings-tab-robot': {
      title: 'Robot Kontrol',
      desc: 'Kinematik, güvenlik, trajectory ve preview politikaları.',
      sections: [
        ['math', 'Matematik'],
        ['onboard', 'ESP32-S3 Local'],
        ['advanced', 'Gelişmiş IK'],
        ['motion', 'Hareket'],
        ['safety', 'Güvenlik']
      ]
    },
    'settings-tab-3d': {
      title: '3D Görünüm',
      desc: 'CAD sahnesi, pick-place simülasyonu ve kamera davranışı.',
      sections: [
        ['scene', 'Sahne'],
        ['pickplace', 'Pick & Place'],
        ['camera', 'Kamera']
      ]
    },
    'settings-tab-comms': {
      title: 'İletişim',
      desc: 'SPI, UART, ESP-NOW ve alt cihaz köprüleme tanıları.',
      sections: [
        ['spi', 'SPI'],
        ['uart', 'UART'],
        ['espnow', 'ESP-NOW']
      ]
    },
    'settings-tab-perf': {
      title: 'Telemetri ve Performans',
      desc: 'WebSocket, PSRAM, log preview ve animasyon yükü.',
      sections: [
        ['telemetry', 'Telemetri'],
        ['metrics', 'Metrikler'],
        ['power', 'Power'],
        ['dpm', 'DPM'],
        ['memory', 'Bellek'],
        ['ui', 'UI Yükü']
      ]
    },
    'settings-tab-update': {
      title: 'Güncelleme ve Recovery',
      desc: 'Firmware yolu, recovery kontrolü, rollback ve güvenli update.',
      sections: [
        ['firmware', 'Firmware'],
        ['recovery', 'Recovery'],
        ['guard', 'Koruma'],
        ['schedule', 'Zamanlama'],
        ['errors', 'Hata Kodları']
      ]
    },
    'settings-tab-storage': {
      title: 'Depolama ve Loglar',
      desc: 'LittleFS, log saklama, cache ve alan uyarıları.',
      sections: [
        ['littlefs', 'LittleFS'],
        ['usage', 'Kullanım'],
        ['logs', 'Loglar'],
        ['cache', 'Cache']
      ]
    },
    'settings-tab-devs': {
      title: 'Bağlı Cihazlar',
      desc: 'P4, C3, PCA9685 ve recovery cihaz profilleri.',
      sections: [
        ['p4', 'ESP32-P4'],
        ['c3', 'ESP32-C3'],
        ['tests', 'Testler'],
        ['drivers', 'Sürücüler']
      ]
    },
    'settings-tab-ai': {
      title: 'AI Yönetimi',
      desc: 'Plan gösterimi, izin/onay mantığı ve MCP araç politikaları.',
      sections: [
        ['mode', 'Mod'],
        ['approval', 'Onay'],
        ['tools', 'Araçlar']
      ]
    },
    'settings-tab-devtools': {
      title: 'Geliştirici',
      desc: 'Debug endpointleri, ham JSON ve deneysel bayraklar.',
      sections: [
        ['debug', 'Debug'],
        ['api', 'API'],
        ['flags', 'Bayraklar'],
        ['performance-flags', 'Performans']
      ]
    },
    'settings-tab-prefs': {
      title: 'Tercihler',
      desc: 'Kullanıcı alışkanlıkları ve varsayılan çalışma davranışları.',
      sections: [
        ['defaults', 'Varsayılanlar'],
        ['confirm', 'Onaylar'],
        ['dock', 'Dock'],
        ['operator', 'Operatör']
      ]
    },
    'settings-tab-about': {
      title: 'Hakkında ve Durum',
      desc: 'Sürüm, build, runtime ve sağlık özetleri.',
      sections: [
        ['summary', 'Özet'],
        ['runtime', 'Runtime'],
        ['health', 'Sağlık']
      ]
    }
  };

  function openSettingsModal() {
    var modal = document.getElementById('settingsModal');
    if (!modal) return;
    modal.style.display = 'block';
    var defaultBtn = modal.querySelector('.settings-tab-btn[data-settings-tab="settings-tab-ui"]');
    switchSettingsTab('settings-tab-ui', defaultBtn || null);
  }

  function closeSettingsModal() {
    var modal = document.getElementById('settingsModal');
    if (modal) modal.style.display = 'none';
    stopSettingsAboutLive();
  }

  function settingsHeaderHtml(tabId, status) {
    var meta = SETTINGS_NAV[tabId] || { title: 'Ayarlar', desc: '' };
    return '<div class="settings-page-head"><div><h2 class="settings-page-title">' +
      meta.title + '</h2><div class="settings-page-desc">' + meta.desc +
      '</div></div>' + (status ? '<div class="settings-pill">' + status + '</div>' : '') + '</div>';
  }

  function settingsEsc(value) {
    return String(value == null ? '' : value).replace(/[&<>"']/g, function(ch) {
      return ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[ch];
    });
  }

  function settingsSectionHtml(title, inner) {
    return '<div class="settings-section"><div class="settings-section-title">' + title +
      '</div>' + inner + '</div>';
  }

  function renderSettingsSubnav(tabId, activeSection) {
    var nav = document.getElementById('settings-subnav');
    var meta = SETTINGS_NAV[tabId];
    if (!nav || !meta) return;
    var html = '<div class="settings-subnav-title">' + meta.title + '</div>';
    (meta.sections || []).forEach(function(section) {
      html += '<button type="button" class="settings-subnav-btn' +
        (section[0] === activeSection ? ' active' : '') +
        '" data-settings-section="' + section[0] + '">' + section[1] + '</button>';
    });
    nav.innerHTML = html;
    nav.querySelectorAll('.settings-subnav-btn').forEach(function(btn) {
      btn.addEventListener('click', function() {
        var section = btn.getAttribute('data-settings-section');
        renderSettingsSubnav(tabId, section);
        var target = document.querySelector('#' + tabId + ' [data-settings-anchor="' + section + '"]');
        if (target && typeof target.scrollIntoView === 'function') {
          target.scrollIntoView({ behavior: 'smooth', block: 'start' });
        }
      });
    });
    var subtitle = document.getElementById('settings-subtitle');
    if (subtitle) subtitle.textContent = meta.desc || 'Kontrol merkezi';
  }

  function uiPanelGetSettings() {
    var src = (typeof global.popupSettingsGetSection === 'function')
      ? (global.popupSettingsGetSection(UI_PANEL_SETTINGS_KEY) || {})
      : {};
    return {
      interfaceMode: src.interfaceMode || UI_PANEL_DEFAULTS.interfaceMode,
      densityMode: ['comfortable', 'normal', 'compact', 'diagnostic'].indexOf(src.densityMode) >= 0 ? src.densityMode : UI_PANEL_DEFAULTS.densityMode,
      followCadTheme: Object.prototype.hasOwnProperty.call(src, 'followCadTheme') ? !!src.followCadTheme : UI_PANEL_DEFAULTS.followCadTheme,
      themeMode: UI_THEME_MODE_VALUES.indexOf(src.themeMode) >= 0 ? src.themeMode : UI_PANEL_DEFAULTS.themeMode,
      colorPalette: UI_COLOR_PALETTES[src.colorPalette] ? src.colorPalette : UI_PANEL_DEFAULTS.colorPalette,
      hideLogsPanel: !!src.hideLogsPanel,
      hideP4SpiPanel: !!src.hideP4SpiPanel,
      hideC3SpiPanel: !!src.hideC3SpiPanel,
      hideLoopIndicator: !!src.hideLoopIndicator,
      hideLogDownload: !!src.hideLogDownload
    };
  }

  function uiPanelSaveSettings(nextState) {
    if (typeof global.popupSettingsSetSection !== 'function') return;
    global.popupSettingsSetSection(UI_PANEL_SETTINGS_KEY, {
      interfaceMode: nextState.interfaceMode || UI_PANEL_DEFAULTS.interfaceMode,
      densityMode: ['comfortable', 'normal', 'compact', 'diagnostic'].indexOf(nextState.densityMode) >= 0 ? nextState.densityMode : UI_PANEL_DEFAULTS.densityMode,
      followCadTheme: Object.prototype.hasOwnProperty.call(nextState, 'followCadTheme') ? !!nextState.followCadTheme : UI_PANEL_DEFAULTS.followCadTheme,
      themeMode: UI_THEME_MODE_VALUES.indexOf(nextState.themeMode) >= 0 ? nextState.themeMode : UI_PANEL_DEFAULTS.themeMode,
      colorPalette: UI_COLOR_PALETTES[nextState.colorPalette] ? nextState.colorPalette : UI_PANEL_DEFAULTS.colorPalette,
      hideLogsPanel: !!nextState.hideLogsPanel,
      hideP4SpiPanel: !!nextState.hideP4SpiPanel,
      hideC3SpiPanel: !!nextState.hideC3SpiPanel,
      hideLoopIndicator: !!nextState.hideLoopIndicator,
      hideLogDownload: !!nextState.hideLogDownload
    });
  }

  function uiGetResolvedPalette(settings) {
    var s = settings || uiPanelGetSettings();
    var mode = UI_THEME_MODE_DEFAULT;
    if (s.themeMode === 'system') {
      var prefersDark = true;
      try {
        prefersDark = !!(window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches);
      } catch (e) {}
      mode = prefersDark ? 'dark' : 'light';
    } else if (s.themeMode === 'light') {
      mode = 'light';
    }
    var palette = UI_COLOR_PALETTES[s.colorPalette] || UI_COLOR_PALETTES[UI_COLOR_PALETTE_DEFAULT];
    return {
      id: s.colorPalette || UI_COLOR_PALETTE_DEFAULT,
      mode: mode,
      label: palette.label,
      colors: palette[mode] || palette.dark
    };
  }

  function applyUiColorTheme(settings) {
    var resolved = uiGetResolvedPalette(settings);
    var c = resolved.colors;
    var rootEl = document.documentElement;
    if (!rootEl || !c) return;
    var onPrimary = resolved.mode === 'light' ? '#ffffff' : '#121212';
    rootEl.dataset.themeMode = resolved.mode;
    rootEl.dataset.colorPalette = resolved.id;
    rootEl.style.setProperty('--bg', c[0]);
    rootEl.style.setProperty('--surface', c[1]);
    rootEl.style.setProperty('--surface-var', c[2]);
    rootEl.style.setProperty('--text', c[3]);
    rootEl.style.setProperty('--primary', c[4]);
    rootEl.style.setProperty('--primary-variant', c[5]);
    rootEl.style.setProperty('--secondary', c[6]);
    rootEl.style.setProperty('--border', c[7]);
    rootEl.style.setProperty('--on-primary', onPrimary);
    rootEl.style.setProperty('--muted', 'color-mix(in srgb, ' + c[3] + ' 62%, ' + c[0] + ')');
    rootEl.style.setProperty('--muted-soft', 'color-mix(in srgb, ' + c[3] + ' 44%, ' + c[0] + ')');
    rootEl.style.setProperty('--input-bg', 'color-mix(in srgb, ' + c[1] + ' 72%, ' + c[0] + ')');
    rootEl.style.setProperty('--input-border', c[7]);
    rootEl.style.setProperty('--table-border', 'color-mix(in srgb, ' + c[7] + ' 76%, transparent)');
    rootEl.style.setProperty('--contrast-ink', resolved.mode === 'light' ? '#111214' : '#f8fafc');
    rootEl.style.setProperty('--dock-bg', c[1]);
    rootEl.style.setProperty('--dock-surface', c[2]);
    rootEl.style.setProperty('--dock-surface-2', c[0]);
    rootEl.style.setProperty('--dock-border', c[7]);
    rootEl.style.setProperty('--accent-green', c[4]);
    rootEl.style.setProperty('--accent-blue', c[6]);
    rootEl.style.setProperty('--accent-amber', c[9]);
    rootEl.style.setProperty('--accent-red', c[8]);
    rootEl.style.setProperty('--accent-purple', c[5]);
    rootEl.style.setProperty('--accent-mint', c[6]);
    rootEl.style.setProperty('--state-ok', c[4]);
    rootEl.style.setProperty('--state-active', c[6]);
    rootEl.style.setProperty('--state-info', c[6]);
    rootEl.style.setProperty('--state-warn', c[9]);
    rootEl.style.setProperty('--state-danger', c[8]);
    rootEl.style.setProperty('--state-disabled', c[7]);
    var paletteDef = UI_COLOR_PALETTES[resolved.id] || UI_COLOR_PALETTES[UI_COLOR_PALETTE_DEFAULT];
    var cadDark = (paletteDef.dark && paletteDef.dark[0]) || c[0];
    var cadLight = (paletteDef.light && paletteDef.light[2]) || c[2] || c[0];
    rootEl.style.setProperty('--cad-bg-dark', cadDark);
    rootEl.style.setProperty('--cad-bg-light', cadLight);
    rootEl.style.setProperty('--cad-bg-active', resolved.mode === 'light' ? cadLight : cadDark);
    rootEl.style.setProperty('--cad-grid', c[3]);
    rootEl.style.setProperty('--cad-axis-x', c[8]);
    rootEl.style.setProperty('--cad-axis-y', c[4]);
    rootEl.style.setProperty('--cad-axis-z', c[6]);
    if (typeof global.kin3d_applySystemThemeBackground === 'function') {
      var ui = uiPanelGetSettings();
      global.kin3d_applySystemThemeBackground({
        enabled: ui.followCadTheme,
        color: resolved.mode === 'light' ? cadLight : cadDark,
        save: true
      });
    }
  }

  function terminalGetSettings() {
    var src = (typeof global.popupSettingsGetSection === 'function')
      ? (global.popupSettingsGetSection(TERMINAL_SETTINGS_KEY) || {})
      : {};
    var theme = TERMINAL_THEMES[src.theme] ? src.theme : TERMINAL_THEME_DEFAULT;
    return {
      theme: theme,
      followUiTheme: Object.prototype.hasOwnProperty.call(src, 'followUiTheme') ? !!src.followUiTheme : TERMINAL_DEFAULTS.followUiTheme,
      showRefreshButton: Object.prototype.hasOwnProperty.call(src, 'showRefreshButton') ? !!src.showRefreshButton : TERMINAL_DEFAULTS.showRefreshButton,
      showKillButton: Object.prototype.hasOwnProperty.call(src, 'showKillButton') ? !!src.showKillButton : TERMINAL_DEFAULTS.showKillButton,
      fontSize: Math.max(10, Math.min(18, Number(src.fontSize) || TERMINAL_DEFAULTS.fontSize)),
      fullscreenFontSize: Math.max(11, Math.min(22, Number(src.fullscreenFontSize) || TERMINAL_DEFAULTS.fullscreenFontSize)),
      shellProfile: ['operator', 'safe', 'developer'].indexOf(src.shellProfile) >= 0 ? src.shellProfile : TERMINAL_DEFAULTS.shellProfile,
      historyLimit: Math.max(10, Math.min(300, Number(src.historyLimit) || TERMINAL_DEFAULTS.historyLimit)),
      commandTimeoutMs: Math.max(500, Math.min(60000, Number(src.commandTimeoutMs) || TERMINAL_DEFAULTS.commandTimeoutMs)),
      showManHints: Object.prototype.hasOwnProperty.call(src, 'showManHints') ? !!src.showManHints : TERMINAL_DEFAULTS.showManHints,
      wrapLongOutput: Object.prototype.hasOwnProperty.call(src, 'wrapLongOutput') ? !!src.wrapLongOutput : TERMINAL_DEFAULTS.wrapLongOutput,
      dynamicFetchLayout: Object.prototype.hasOwnProperty.call(src, 'dynamicFetchLayout') ? !!src.dynamicFetchLayout : TERMINAL_DEFAULTS.dynamicFetchLayout,
      fullscreenOpacity: Math.max(10, Math.min(92, Number(src.fullscreenOpacity) || TERMINAL_DEFAULTS.fullscreenOpacity)),
      fullscreenBlurPx: Math.max(0, Math.min(28, Number(src.fullscreenBlurPx) || TERMINAL_DEFAULTS.fullscreenBlurPx))
    };
  }

  function terminalSaveSettings(nextState) {
    if (typeof global.popupSettingsSetSection !== 'function') return;
    global.popupSettingsSetSection(TERMINAL_SETTINGS_KEY, {
      theme: TERMINAL_THEMES[nextState.theme] ? nextState.theme : TERMINAL_THEME_DEFAULT,
      followUiTheme: !!nextState.followUiTheme,
      showRefreshButton: !!nextState.showRefreshButton,
      showKillButton: !!nextState.showKillButton,
      fontSize: Math.max(10, Math.min(18, Number(nextState.fontSize) || TERMINAL_DEFAULTS.fontSize)),
      fullscreenFontSize: Math.max(11, Math.min(22, Number(nextState.fullscreenFontSize) || TERMINAL_DEFAULTS.fullscreenFontSize)),
      shellProfile: ['operator', 'safe', 'developer'].indexOf(nextState.shellProfile) >= 0 ? nextState.shellProfile : TERMINAL_DEFAULTS.shellProfile,
      historyLimit: Math.max(10, Math.min(300, Number(nextState.historyLimit) || TERMINAL_DEFAULTS.historyLimit)),
      commandTimeoutMs: Math.max(500, Math.min(60000, Number(nextState.commandTimeoutMs) || TERMINAL_DEFAULTS.commandTimeoutMs)),
      showManHints: !!nextState.showManHints,
      wrapLongOutput: !!nextState.wrapLongOutput,
      dynamicFetchLayout: !!nextState.dynamicFetchLayout,
      fullscreenOpacity: Math.max(10, Math.min(92, Number(nextState.fullscreenOpacity) || TERMINAL_DEFAULTS.fullscreenOpacity)),
      fullscreenBlurPx: Math.max(0, Math.min(28, Number(nextState.fullscreenBlurPx) || TERMINAL_DEFAULTS.fullscreenBlurPx))
    });
  }

  function applyTerminalTheme(settings) {
    var s = settings || terminalGetSettings();
    var theme = TERMINAL_THEMES[s.theme] || TERMINAL_THEMES[TERMINAL_THEME_DEFAULT];
    var docRoot = document.documentElement;
    if (!docRoot || !theme) return;
    if (s.followUiTheme) {
      var uiBg = getComputedStyle(docRoot).getPropertyValue('--bg').trim() || '#221f1f';
      var uiSurface = getComputedStyle(docRoot).getPropertyValue('--surface').trim() || '#2b2626';
      var uiText = getComputedStyle(docRoot).getPropertyValue('--text').trim() || '#d8f7b2';
      var uiPrimary = getComputedStyle(docRoot).getPropertyValue('--primary').trim() || '#B5EA6A';
      var uiBorder = getComputedStyle(docRoot).getPropertyValue('--border').trim() || '#5E5656';
      docRoot.style.setProperty('--terminal-bg', 'color-mix(in srgb, ' + uiBg + ' 86%, #000)');
      docRoot.style.setProperty('--terminal-surface', 'color-mix(in srgb, ' + uiSurface + ' 82%, ' + uiBg + ')');
      docRoot.style.setProperty('--terminal-fg', uiText);
      docRoot.style.setProperty('--terminal-accent', uiPrimary);
      docRoot.style.setProperty('--terminal-muted', 'color-mix(in srgb, ' + uiText + ' 58%, ' + uiBg + ')');
      docRoot.style.setProperty('--terminal-border', uiBorder);
      docRoot.style.setProperty('--terminal-cursor', uiPrimary);
    } else {
      docRoot.style.setProperty('--terminal-bg', theme.bg);
      docRoot.style.setProperty('--terminal-surface', theme.surface);
      docRoot.style.setProperty('--terminal-fg', theme.fg);
      docRoot.style.setProperty('--terminal-accent', theme.accent);
      docRoot.style.setProperty('--terminal-muted', theme.muted);
      docRoot.style.setProperty('--terminal-border', theme.border);
      docRoot.style.setProperty('--terminal-cursor', theme.cursor);
    }
    docRoot.style.setProperty('--terminal-font-size', s.fontSize + 'px');
    docRoot.style.setProperty('--terminal-full-font-size', s.fullscreenFontSize + 'px');
    docRoot.style.setProperty('--terminal-full-opacity', s.fullscreenOpacity + '%');
    docRoot.style.setProperty('--terminal-full-bg-opacity', Math.min(70, Math.max(18, s.fullscreenOpacity + 9)) + '%');
    docRoot.style.setProperty('--terminal-full-blur', s.fullscreenBlurPx + 'px');
    if (typeof global.applyShellFullscreenUiSettings === 'function') global.applyShellFullscreenUiSettings();
    if (typeof global.shellScheduleResize === 'function') global.shellScheduleResize(false);
  }

  function uiPanelSetVisible(id, visible, displayValue) {
    var el = document.getElementById(id);
    if (!el) return;
    if (visible) {
      el.style.removeProperty('display');
      if (displayValue) el.style.setProperty('display', displayValue);
    } else {
      el.style.setProperty('display', 'none', 'important');
    }
  }

  function uiIsElementVisible(el) {
    if (!el) return false;
    var style = window.getComputedStyle(el);
    if (!style) return false;
    if (style.display === 'none' || style.visibility === 'hidden') return false;
    var rect = el.getBoundingClientRect();
    return rect.width > 0 && rect.height > 0;
  }

  function updateFloatingToolsDockLayout() {
    var dock = document.getElementById('floating-tools-dock');
    var body = document.body;
    var isMobile = !!(body && body.classList.contains('mobile-mode'));
    var sidebar = document.querySelector('.right-sidebar');
    var logsPanel = document.getElementById('panel-logs-root');
    var rootEl = document.documentElement;
    var computed = rootEl ? getComputedStyle(rootEl) : null;
    var cssInset = computed ? parseFloat(computed.getPropertyValue('--panel-inset')) : NaN;
    var edgeGap = isMobile ? 10 : (Number.isFinite(cssInset) ? cssInset : 16);
    var rightOffset = edgeGap;
    if (!isMobile && uiIsElementVisible(sidebar)) {
      var sidebarRect = sidebar.getBoundingClientRect();
      rightOffset = Math.max(edgeGap, Math.round(window.innerWidth - sidebarRect.left + edgeGap));
    }
    var bottomOffset = isMobile ? 56 : edgeGap;
    var logsCollapsed = !!(body && body.classList.contains('logs-collapsed'));
    if (!isMobile && !logsCollapsed && uiIsElementVisible(logsPanel)) {
      var logsRect = logsPanel.getBoundingClientRect();
      bottomOffset = Math.max(bottomOffset, Math.round(window.innerHeight - logsRect.top + edgeGap));
    }
    if (rootEl) {
      rootEl.style.setProperty('--overlay-right', rightOffset + 'px');
      rootEl.style.setProperty('--overlay-bottom', bottomOffset + 'px');
      rootEl.style.setProperty('--overlay-top', edgeGap + 'px');
      rootEl.style.setProperty('--overlay-left', edgeGap + 'px');
    }
    if (dock) {
      dock.style.right = rightOffset + 'px';
      dock.style.bottom = bottomOffset + 'px';
    }
  }

  function uiApplyLogsPanelLayout(s) {
    var hideLogs = !!(s && s.hideLogsPanel);
    if (document && document.body) document.body.classList.toggle('logs-collapsed', hideLogs);
    var panelResizer = document.getElementById('panel-resizer');
    if (!panelResizer) return;
    if (hideLogs) panelResizer.style.setProperty('display', 'none', 'important');
    else panelResizer.style.removeProperty('display');
  }

  function uiApplyErrorPanelLayout(s) {
    var p4Panel = document.getElementById('p4-spi-panel');
    var c3Panel = document.getElementById('c3-spi-panel');
    var internalResizer = document.getElementById('internal-table-resizer');
    var consoleResizer = document.getElementById('console-resizer');
    var errorLogsRow = document.getElementById('error-logs-row');
    var consoleColumn = document.getElementById('console-panel-column');
    var showP4 = !!(s && !s.hideP4SpiPanel);
    var showC3 = !!(s && !s.hideC3SpiPanel);
    var anyErrorPanel = showP4 || showC3;

    if (errorLogsRow) errorLogsRow.style.setProperty('display', anyErrorPanel ? 'flex' : 'none');
    if (consoleResizer) {
      if (anyErrorPanel) consoleResizer.style.removeProperty('display');
      else consoleResizer.style.setProperty('display', 'none', 'important');
    }
    if (consoleColumn && !anyErrorPanel) consoleColumn.style.setProperty('--console-width', '100%');
    if (internalResizer) {
      if (showP4 && showC3) internalResizer.style.removeProperty('display');
      else internalResizer.style.setProperty('display', 'none', 'important');
    }
    if (p4Panel) {
      if (showP4 && showC3) {
        p4Panel.style.setProperty('flex', 'none');
        p4Panel.style.setProperty('width', 'var(--p4-spi-width, 50%)');
      } else {
        p4Panel.style.setProperty('flex', '1 1 0');
        p4Panel.style.setProperty('width', 'auto');
      }
    }
    if (c3Panel) {
      if (showP4 && showC3) c3Panel.style.setProperty('flex', '1');
      else c3Panel.style.setProperty('flex', '1 1 0');
    }
  }

  function applyUiPanelSettings(state) {
    var s = state || uiPanelGetSettings();
    applyUiColorTheme(s);
    uiPanelSetVisible('panel-logs-root', !s.hideLogsPanel, 'flex');
    uiPanelSetVisible('p4-spi-panel', !s.hideP4SpiPanel, 'flex');
    uiPanelSetVisible('c3-spi-panel', !s.hideC3SpiPanel, 'flex');
    uiPanelSetVisible('loop-indicator-wrap', !s.hideLoopIndicator, '');
    uiPanelSetVisible('logs-download-btn', !s.hideLogDownload, 'inline-flex');
    uiApplyErrorPanelLayout(s);
    uiApplyLogsPanelLayout(s);
    if (document && document.documentElement) document.documentElement.dataset.uiDensity = s.densityMode || UI_PANEL_DEFAULTS.densityMode;
    updateFloatingToolsDockLayout();
    window.dispatchEvent(new Event('resize'));
  }

  function bindSystemThemeWatcher() {
    if (!window.matchMedia) return;
    var mq = window.matchMedia('(prefers-color-scheme: dark)');
    var handler = function() {
      var s = uiPanelGetSettings();
      if (s.themeMode === 'system') applyUiPanelSettings(s);
    };
    if (typeof mq.addEventListener === 'function') mq.addEventListener('change', handler);
    else if (typeof mq.addListener === 'function') mq.addListener(handler);
  }

  function uiToggleRowHtml(id, title, checked) {
    return '<div class="kin3d-setting-tile">' +
      '<span class="kin3d-setting-label">' + title + '</span>' +
      '<label class="kin3d-setting-switch">' +
      '<input type="checkbox" id="' + id + '"' + (checked ? ' checked' : '') + ' style="display:none;">' +
      '<span class="kin3d-setting-switch-track"></span>' +
      '<span class="kin3d-setting-switch-knob"></span>' +
      '</label>' +
      '</div>';
  }

  function uiPaletteCardHtml(id, palette, active, mode) {
    var colors = (palette && palette[mode]) || (palette && palette.dark) || [];
    var swatches = colors.map(function(color) {
      return '<i style="background:' + color + ';"></i>';
    }).join('');
    return '<button type="button" class="ui-palette-card' + (active ? ' active' : '') + '" data-ui-palette="' + id + '">' +
      '<span class="ui-palette-name">' + palette.label + '</span>' +
      '<span class="ui-palette-swatches">' + swatches + '</span>' +
      '</button>';
  }

  function mountSettingsUiTabContent() {
    var body = document.getElementById('settings-tab-ui-body');
    if (!body) return;
    var s = uiPanelGetSettings();
    var paletteCards = Object.keys(UI_COLOR_PALETTES).map(function(id) {
      return uiPaletteCardHtml(id, UI_COLOR_PALETTES[id], id === s.colorPalette, s.themeMode);
    }).join('');
    body.innerHTML =
      settingsHeaderHtml('settings-tab-ui', 'Kapsam: ' + s.interfaceMode) +
      '<div data-settings-anchor="theme">' +
      settingsSectionHtml('Tema ve Renk',
      '<div class="settings-two-col">' +
      '<div class="settings-card"><div class="kin3d-setting-label" style="margin-bottom:8px;">Tema modu</div>' +
      '<select id="ui_theme_mode" class="coord-input" style="width:220px !important; text-align:left !important;">' +
      '<option value="system"' + (s.themeMode === 'system' ? ' selected' : '') + '>Sistem (Otomatik)</option>' +
      '<option value="dark"' + (s.themeMode === 'dark' ? ' selected' : '') + '>Karanlık</option>' +
      '<option value="light"' + (s.themeMode === 'light' ? ' selected' : '') + '>Aydınlık</option>' +
      '</select></div>' +
      '<div class="settings-card muted">Her palet 10 temel renkten oluşur ve web arayüzündeki ana yüzey, metin, vurgu, uyarı ve buton renklerine uygulanır.</div>' +
      '</div><div class="ui-palette-grid">' + paletteCards + '</div>') + '</div>' +
      '<div data-settings-anchor="scope">' +
      settingsSectionHtml('Kapsam',
      '<div class="settings-card full"><div class="kin3d-setting-label" style="margin-bottom:8px;">Arayüz kapsamı</div>' +
      '<select id="ui_interface_mode" class="coord-input" style="width:240px !important; text-align:left !important;">' +
      '<option value="simple"' + (s.interfaceMode === 'simple' ? ' selected' : '') + '>Basit</option>' +
      '<option value="medium"' + (s.interfaceMode === 'medium' ? ' selected' : '') + '>Orta</option>' +
      '<option value="comprehensive"' + (s.interfaceMode === 'comprehensive' ? ' selected' : '') + '>Kapsamlı</option>' +
      '<option value="ai-managed"' + (s.interfaceMode === 'ai-managed' ? ' selected' : '') + '>Ai Yönetimli</option>' +
      '</select><div style="font-size:11px; color:#aeb9c2; margin-top:8px;">Şimdilik seçim kaydedilir; görünüm kapsamı sonraki arayüz düzenlemesinde uygulanacak.</div></div>' +
      '<div class="settings-card"><div class="kin3d-setting-label" style="margin-bottom:8px;">Kontrol yoğunluğu</div>' +
      '<select id="ui_density_mode" class="coord-input" style="width:240px !important; text-align:left !important;">' +
      '<option value="comfortable"' + (s.densityMode === 'comfortable' ? ' selected' : '') + '>Rahat</option>' +
      '<option value="normal"' + (s.densityMode === 'normal' ? ' selected' : '') + '>Normal</option>' +
      '<option value="compact"' + (s.densityMode === 'compact' ? ' selected' : '') + '>Compact</option>' +
      '<option value="diagnostic"' + (s.densityMode === 'diagnostic' ? ' selected' : '') + '>Diagnostic</option>' +
      '</select><div style="font-size:11px; color:#aeb9c2; margin-top:8px;">Buton, tablo, sidepanel ve bottom panel yoğunluğunu değiştirir.</div></div>') + '</div>' +
      '<div data-settings-anchor="layout">' +
      settingsSectionHtml('Yerleşim',
      '<div class="settings-two-col">' +
      uiToggleRowHtml('ui_toggle_cad_theme', 'CAD arkaplanı sistem temasını izlesin', s.followCadTheme) +
      uiToggleRowHtml('ui_toggle_loop', 'Döngü göstergesini kapat', s.hideLoopIndicator) +
      uiToggleRowHtml('ui_toggle_log_dl', 'Log indir butonunu kapat', s.hideLogDownload) +
      '</div>') + '</div>' +
      '<div data-settings-anchor="visibility">' +
      settingsSectionHtml('Paneller',
      '<div class="settings-two-col">' +
      uiToggleRowHtml('ui_toggle_logs_panel', 'Sistem Logu ve Konsolu Kapat', s.hideLogsPanel) +
      uiToggleRowHtml('ui_toggle_p4_spi', 'P4 SPI Tablosunu Kapat', s.hideP4SpiPanel) +
      uiToggleRowHtml('ui_toggle_c3_spi', 'C3 SPI Tablosunu Kapat', s.hideC3SpiPanel) +
      '</div>') + '</div>' +
      '<div data-settings-anchor="shell-ui">' +
      settingsSectionHtml('MShell ve Çalışma Alanı',
      '<div class="settings-two-col">' +
      uiToggleRowHtml('ui_toggle_quick_settings', 'Hızlı ayarlar kartlarını göster', s.showQuickSettings) +
      uiToggleRowHtml('ui_toggle_keyboard_hints', 'Klavye kısayolu ipuçlarını göster', s.showKeyboardHints) +
      uiToggleRowHtml('ui_toggle_auto_collapse', 'Paneller boşta otomatik daralsın', s.autoCollapsePanels) +
      '<div class="settings-card muted">Alias yönetimi, completion cache ve shell görünüm yoğunluğu terminal/prefs ile birlikte burada kontrol edilir.</div>' +
      '</div>') + '</div>';

    var modeEl = document.getElementById('ui_interface_mode');
    if (modeEl) {
      modeEl.addEventListener('change', function() {
        var cur = uiPanelGetSettings();
        cur.interfaceMode = modeEl.value || UI_PANEL_DEFAULTS.interfaceMode;
        uiPanelSaveSettings(cur);
      });
    }

    var themeModeEl = document.getElementById('ui_theme_mode');
    if (themeModeEl) {
      themeModeEl.addEventListener('change', function() {
        var cur = uiPanelGetSettings();
        cur.themeMode = UI_THEME_MODE_VALUES.indexOf(themeModeEl.value) >= 0 ? themeModeEl.value : UI_THEME_MODE_DEFAULT;
        uiPanelSaveSettings(cur);
        applyUiPanelSettings(cur);
        mountSettingsUiTabContent();
      });
    }

    var densityEl = document.getElementById('ui_density_mode');
    if (densityEl) {
      densityEl.addEventListener('change', function() {
        var cur = uiPanelGetSettings();
        cur.densityMode = densityEl.value || UI_PANEL_DEFAULTS.densityMode;
        uiPanelSaveSettings(cur);
        applyUiPanelSettings(cur);
      });
    }

    body.querySelectorAll('.ui-palette-card').forEach(function(card) {
      card.addEventListener('click', function() {
        var cur = uiPanelGetSettings();
        cur.colorPalette = card.getAttribute('data-ui-palette') || UI_PANEL_DEFAULTS.colorPalette;
        uiPanelSaveSettings(cur);
        applyUiPanelSettings(cur);
        mountSettingsUiTabContent();
      });
    });

    [
      ['ui_toggle_logs_panel', 'hideLogsPanel'],
      ['ui_toggle_p4_spi', 'hideP4SpiPanel'],
      ['ui_toggle_c3_spi', 'hideC3SpiPanel'],
      ['ui_toggle_cad_theme', 'followCadTheme'],
      ['ui_toggle_loop', 'hideLoopIndicator'],
      ['ui_toggle_log_dl', 'hideLogDownload'],
      ['ui_toggle_quick_settings', 'showQuickSettings'],
      ['ui_toggle_keyboard_hints', 'showKeyboardHints'],
      ['ui_toggle_auto_collapse', 'autoCollapsePanels']
    ].forEach(function(pair) {
      var el = document.getElementById(pair[0]);
      if (!el) return;
      el.addEventListener('change', function() {
        var cur = uiPanelGetSettings();
        cur[pair[1]] = !!el.checked;
        uiPanelSaveSettings(cur);
        applyUiPanelSettings(cur);
      });
    });
  }

  function terminalThemeCardHtml(id, theme, active) {
    return '<div class="terminal-theme-card' + (active ? ' active' : '') + '" data-terminal-theme="' + id + '"' +
      ' style="background:' + theme.surface + '; color:' + theme.fg + '; border-color:' + theme.border + ';">' +
      '<div class="terminal-theme-name" style="color:' + theme.accent + ';">' + theme.label + '</div>' +
      '<div class="terminal-theme-sample" style="background:' + theme.bg + '; color:' + theme.fg + '; border:1px solid ' + theme.border + ';">' +
      '<span style="color:' + theme.accent + ';">mros@MROS7DOFS3</span>:<span style="color:' + theme.muted + ';">/fs</span>$ mfetch' +
      '</div>' +
      '</div>';
  }

  function mountSettingsTerminalTabContent() {
    var body = document.getElementById('settings-tab-terminal-body');
    if (!body) return;
    var s = terminalGetSettings();
    var cards = '';
    Object.keys(TERMINAL_THEMES).forEach(function(id) {
      cards += terminalThemeCardHtml(id, TERMINAL_THEMES[id], id === s.theme);
    });
    body.innerHTML =
      settingsHeaderHtml('settings-tab-terminal', s.shellProfile) +
      '<div data-settings-anchor="theme">' +
      settingsSectionHtml('Tema',
        '<div class="settings-card full" style="font-size:11px; color:#aeb9c2;">Web shell ve fullscreen terminalin arka plan, metin, vurgu ve imleç renkleri. Serial terminal etkilenmez.</div>' +
        '<div class="settings-two-col">' +
        boolRowHtml('terminal_follow_ui_theme', 'Terminal renkleri arayüz temasını takip etsin', s.followUiTheme) +
        '<div class="settings-card muted" style="font-size:11px;">Kapalıysa aşağıdaki terminal paleti ayrı uygulanır.</div>' +
        '</div>' +
        '<div class="settings-card full terminal-theme-preview"><div class="terminal-theme-grid">' + cards + '</div></div>') +
      '</div>' +
      '<div data-settings-anchor="font">' +
      settingsSectionHtml('Yazı ve çıktı',
        '<div class="settings-two-col">' +
        '<div class="settings-card"><div class="kin3d-setting-label">Panel font boyutu</div><input id="terminal_font_size" type="range" min="10" max="18" step="1" value="' + s.fontSize + '" style="width:100%; accent-color:var(--terminal-accent);"><div style="font-size:11px; color:#aeb9c2;">Şu an: <span id="terminal_font_size_val">' + s.fontSize + '</span> px</div></div>' +
        '<div class="settings-card"><div class="kin3d-setting-label">Fullscreen font boyutu</div><input id="terminal_full_font_size" type="range" min="11" max="22" step="1" value="' + s.fullscreenFontSize + '" style="width:100%; accent-color:var(--terminal-accent);"><div style="font-size:11px; color:#aeb9c2;">Şu an: <span id="terminal_full_font_size_val">' + s.fullscreenFontSize + '</span> px</div></div>' +
        '<div class="settings-card"><div class="kin3d-setting-label">Fullscreen arka plan matlığı</div><input id="terminal_full_opacity" type="range" min="10" max="92" step="1" value="' + s.fullscreenOpacity + '" style="width:100%; accent-color:var(--terminal-accent);"><div style="font-size:11px; color:#aeb9c2;">Şu an: <span id="terminal_full_opacity_val">' + s.fullscreenOpacity + '</span>%</div></div>' +
        '<div class="settings-card"><div class="kin3d-setting-label">Fullscreen blur</div><input id="terminal_full_blur" type="range" min="0" max="28" step="1" value="' + s.fullscreenBlurPx + '" style="width:100%; accent-color:var(--terminal-accent);"><div style="font-size:11px; color:#aeb9c2;">Şu an: <span id="terminal_full_blur_val">' + s.fullscreenBlurPx + '</span> px</div></div>' +
        '</div>' +
        '<div class="settings-two-col">' +
        boolRowHtml('terminal_wrap_output', 'Uzun çıktıyı terminal genişliğine göre sar', s.wrapLongOutput) +
        boolRowHtml('terminal_dynamic_fetch', 'mfetch satırlarını dinamik hizala', s.dynamicFetchLayout) +
        '</div>') +
      '</div>' +
      '<div data-settings-anchor="shell">' +
      settingsSectionHtml('Shell araçları',
        '<div class="settings-two-col">' +
        selectRowHtml('terminal_shell_profile', 'Shell profili', s.shellProfile, [['operator', 'Operatör'], ['safe', 'Güvenli'], ['developer', 'Geliştirici']]) +
        inputRowHtml('terminal_history_limit', 'Geçmiş limiti', s.historyLimit, 'number') +
        inputRowHtml('terminal_timeout_ms', 'Komut zaman aşımı ms', s.commandTimeoutMs, 'number') +
        boolRowHtml('terminal_man_hints', 'man/help ipuçlarını göster', s.showManHints) +
        boolRowHtml('terminal_show_refresh_btn', 'Fullscreen: Yenile butonu aktif', s.showRefreshButton) +
        boolRowHtml('terminal_show_kill_btn', 'Fullscreen: Süreci öldür butonu aktif', s.showKillButton) +
        '</div>') +
      '</div>';

    body.querySelectorAll('.terminal-theme-card').forEach(function(card) {
      card.addEventListener('click', function() {
        var cur = terminalGetSettings();
        cur.theme = card.getAttribute('data-terminal-theme') || TERMINAL_THEME_DEFAULT;
        terminalSaveSettings(cur);
        applyTerminalTheme(cur);
        mountSettingsTerminalTabContent();
      });
    });

    var fontEl = document.getElementById('terminal_font_size');
    var fontVal = document.getElementById('terminal_font_size_val');
    if (fontEl) {
      fontEl.addEventListener('input', function() {
        var cur = terminalGetSettings();
        cur.fontSize = Number(fontEl.value) || 12;
        if (fontVal) fontVal.textContent = String(cur.fontSize);
        terminalSaveSettings(cur);
        applyTerminalTheme(cur);
      });
    }

    var fullFontEl = document.getElementById('terminal_full_font_size');
    var fullFontVal = document.getElementById('terminal_full_font_size_val');
    if (fullFontEl) {
      fullFontEl.addEventListener('input', function() {
        var cur = terminalGetSettings();
        cur.fullscreenFontSize = Number(fullFontEl.value) || 13;
        if (fullFontVal) fullFontVal.textContent = String(cur.fullscreenFontSize);
        terminalSaveSettings(cur);
        applyTerminalTheme(cur);
      });
    }
    var opacityEl = document.getElementById('terminal_full_opacity');
    var opacityVal = document.getElementById('terminal_full_opacity_val');
    if (opacityEl) {
      opacityEl.addEventListener('input', function() {
        var cur = terminalGetSettings();
        cur.fullscreenOpacity = Number(opacityEl.value) || TERMINAL_DEFAULTS.fullscreenOpacity;
        if (opacityVal) opacityVal.textContent = String(cur.fullscreenOpacity);
        terminalSaveSettings(cur);
        applyTerminalTheme(cur);
      });
    }
    var blurEl = document.getElementById('terminal_full_blur');
    var blurVal = document.getElementById('terminal_full_blur_val');
    if (blurEl) {
      blurEl.addEventListener('input', function() {
        var cur = terminalGetSettings();
        cur.fullscreenBlurPx = Number(blurEl.value) || 0;
        if (blurVal) blurVal.textContent = String(cur.fullscreenBlurPx);
        terminalSaveSettings(cur);
        applyTerminalTheme(cur);
      });
    }

    bindSettingToggle('terminal_wrap_output', s, 'wrapLongOutput', terminalSaveSettings);
    bindSettingToggle('terminal_dynamic_fetch', s, 'dynamicFetchLayout', terminalSaveSettings);
    bindSettingToggle('terminal_man_hints', s, 'showManHints', terminalSaveSettings);
    bindSettingToggle('terminal_follow_ui_theme', s, 'followUiTheme', terminalSaveSettings);
    bindSettingToggle('terminal_show_refresh_btn', s, 'showRefreshButton', terminalSaveSettings);
    bindSettingToggle('terminal_show_kill_btn', s, 'showKillButton', terminalSaveSettings);
    ['terminal_follow_ui_theme', 'terminal_show_refresh_btn', 'terminal_show_kill_btn'].forEach(function(id) {
      var el = document.getElementById(id);
      if (!el) return;
      el.addEventListener('change', function() { applyTerminalTheme(terminalGetSettings()); });
    });
    bindSettingInput('terminal_shell_profile', s, 'shellProfile', terminalSaveSettings, false);
    bindSettingInput('terminal_history_limit', s, 'historyLimit', terminalSaveSettings, true);
    bindSettingInput('terminal_timeout_ms', s, 'commandTimeoutMs', terminalSaveSettings, true);
  }

  function inputRowHtml(id, title, value, type) {
    return '<label class="kin3d-setting-tile">' +
      '<span class="kin3d-setting-label">' + title + '</span>' +
      '<input id="' + id + '" type="' + (type || 'text') + '" class="coord-input" value="' + String(value).replace(/"/g, '&quot;') + '" style="width:180px !important; text-align:left !important;">' +
      '</label>';
  }

  function selectRowHtml(id, title, value, options) {
    var html = '<label class="kin3d-setting-tile"><span class="kin3d-setting-label">' + title + '</span><select id="' + id + '" class="coord-input" style="width:180px !important;">';
    options.forEach(function(opt) {
      html += '<option value="' + opt[0] + '"' + (value === opt[0] ? ' selected' : '') + '>' + opt[1] + '</option>';
    });
    html += '</select></label>';
    return html;
  }

  function bindSettingToggle(id, state, key, saveFn) {
    var el = document.getElementById(id);
    if (!el) return;
    el.addEventListener('change', function() {
      state[key] = !!el.checked;
      saveFn(state);
    });
  }

  function bindSettingInput(id, state, key, saveFn, numeric) {
    var el = document.getElementById(id);
    if (!el) return;
    el.addEventListener('change', function() {
      state[key] = numeric ? (Number(el.value) || 0) : String(el.value || '');
      saveFn(state);
    });
  }

  function bindActionButton(id, handler) {
    var el = document.getElementById(id);
    if (el && typeof handler === 'function') el.addEventListener('click', handler);
  }

  function settingsActionButtonHtml(id, title) {
    return '<button type="button" id="' + id + '" class="coord-btn" style="min-height:34px; padding:6px 10px;">' + title + '</button>';
  }

  function settingsLast3Html(last3) {
    var list = Array.isArray(last3) ? last3.filter(function(item) { return item && item.seq; }) : [];
    if (!list.length) return '<div class="settings-status-line">Henüz işlem yok.</div>';
    return list.map(function(item) {
      return '<div class="settings-status-line">#' + settingsEsc(item.seq) + ' ' +
        settingsEsc(item.op || '-') + ' | ' + settingsEsc(item.duration_us || 0) +
        ' us | ' + (item.ok ? 'ok' : 'fail') + '</div>';
    }).join('');
  }

  function mountSettingsGeneralTabContent() {
    var body = document.getElementById('settings-tab-general-body');
    if (!body) return;
    var s = getSectionSettings(GENERAL_SETTINGS_KEY, GENERAL_DEFAULTS);
    var uiQuick = uiPanelGetSettings();
    var terminalQuick = terminalGetSettings();
    var filesQuick = getSectionSettings(FILES_SETTINGS_KEY, FILES_DEFAULTS);
    var robotQuick = getSectionSettings(ROBOT_SETTINGS_KEY, ROBOT_DEFAULTS);
    body.innerHTML = settingsHeaderHtml('settings-tab-general', s.locale) +
      '<div data-settings-anchor="quick">' +
      settingsSectionHtml('Hızlı Ayarlar',
        '<div class="settings-grid-3">' +
        '<div class="settings-card"><div class="kin3d-setting-label">Tema</div><select id="general_quick_theme" class="coord-input" style="width:100% !important;text-align:left !important;">' +
        '<option value="dark"' + (uiQuick.themeMode === 'dark' ? ' selected' : '') + '>Karanlık</option><option value="light"' + (uiQuick.themeMode === 'light' ? ' selected' : '') + '>Aydınlık</option><option value="system"' + (uiQuick.themeMode === 'system' ? ' selected' : '') + '>Sistem</option></select></div>' +
        '<div class="settings-card"><div class="kin3d-setting-label">Terminal matlığı</div><input id="general_quick_terminal_opacity" type="range" min="10" max="92" value="' + terminalQuick.fullscreenOpacity + '" style="width:100%;accent-color:var(--primary);"><div class="settings-status-line"><span id="general_quick_terminal_opacity_val">' + terminalQuick.fullscreenOpacity + '</span>%</div></div>' +
        '<div class="settings-card"><div class="kin3d-setting-label">Dosya görünümü</div><select id="general_quick_file_view" class="coord-input" style="width:100% !important;text-align:left !important;">' +
        '<option value="list"' + (filesQuick.defaultView === 'list' ? ' selected' : '') + '>Liste</option><option value="grid"' + (filesQuick.defaultView === 'grid' ? ' selected' : '') + '>Grid</option><option value="details"' + (filesQuick.defaultView === 'details' ? ' selected' : '') + '>Ayrıntı</option></select></div>' +
        '<div class="settings-card"><div class="kin3d-setting-label">Kinematik merkezi</div><select id="general_quick_math_backend" class="coord-input" style="width:100% !important;text-align:left !important;">' +
        '<option value="auto"' + (robotQuick.mathBackend === 'auto' ? ' selected' : '') + '>Auto</option><option value="web"' + (robotQuick.mathBackend === 'web' ? ' selected' : '') + '>Web</option><option value="onboard-s3"' + (robotQuick.mathBackend === 'onboard-s3' ? ' selected' : '') + '>ESP32-S3 Local</option><option value="p4-spi"' + (robotQuick.mathBackend === 'p4-spi' ? ' selected' : '') + '>P4 SPI</option><option value="p4-esp-now"' + (robotQuick.mathBackend === 'p4-esp-now' ? ' selected' : '') + '>P4 ESP-NOW</option></select></div>' +
        '<div class="settings-card"><div class="kin3d-setting-label">WiFi</div><button type="button" class="settings-action-btn" data-settings-jump="settings-tab-net">Ağları Tara</button></div>' +
        '<div class="settings-card"><div class="kin3d-setting-label">Geliştirici</div><button type="button" class="settings-action-btn" data-settings-jump="settings-tab-devtools">Debug Araçları</button></div>' +
        '</div>') + '</div>' +
      '<div data-settings-anchor="identity">' +
      settingsSectionHtml('Kimlik',
        '<div class="settings-two-col">' +
        inputRowHtml('general_device_alias', 'Arayüz cihaz adı', s.deviceAlias, 'text') +
        selectRowHtml('general_startup_panel', 'Varsayılan açılış paneli', s.startupPanel, [['3d', '3D Görünüm'], ['console', 'Konsol'], ['files', 'Dosyalar'], ['settings', 'Ayarlar']]) +
        '</div>') + '</div>' +
      '<div data-settings-anchor="locale">' +
      settingsSectionHtml('Dil ve Bölge',
        '<div class="settings-two-col">' +
        selectRowHtml('general_locale', 'Kullanıcı dili', s.locale, [['tr_TR.utf8', 'Türkçe - tr_TR.utf8'], ['en_US.utf8', 'English - en_US.utf8']]) +
        uiToggleRowHtml('general_boot_summary', 'Açılışta sistem özetini göster', s.showBootSummary) +
        '</div><div id="profile_status" class="settings-status-line"></div>') + '</div>' +
      '<div data-settings-anchor="startup">' +
      settingsSectionHtml('Başlangıç',
        '<div class="settings-card muted">Dil seçimi şimdilik kullanıcı profiline kaydedilir. Arayüz metinleri kademeli olarak bu locale kapsamına alınacak; mfetch etiketi bu ayarı kullanır.</div>') + '</div>';
    var save = function(next) { saveSectionSettings(GENERAL_SETTINGS_KEY, GENERAL_DEFAULTS, next); };
    bindSettingInput('general_device_alias', s, 'deviceAlias', save, false);
    bindSettingInput('general_startup_panel', s, 'startupPanel', save, false);
    bindSettingToggle('general_boot_summary', s, 'showBootSummary', save);
    var quickTheme = document.getElementById('general_quick_theme');
    if (quickTheme) quickTheme.addEventListener('change', function() {
      var next = uiPanelGetSettings();
      next.themeMode = quickTheme.value || UI_THEME_MODE_DEFAULT;
      uiPanelSaveSettings(next);
      applyUiPanelSettings(next);
    });
    var quickOpacity = document.getElementById('general_quick_terminal_opacity');
    var quickOpacityVal = document.getElementById('general_quick_terminal_opacity_val');
    if (quickOpacity) quickOpacity.addEventListener('input', function() {
      var next = terminalGetSettings();
      next.fullscreenOpacity = Number(quickOpacity.value) || TERMINAL_DEFAULTS.fullscreenOpacity;
      if (quickOpacityVal) quickOpacityVal.textContent = String(next.fullscreenOpacity);
      terminalSaveSettings(next);
      applyTerminalTheme(next);
    });
    var quickFileView = document.getElementById('general_quick_file_view');
    if (quickFileView) quickFileView.addEventListener('change', function() {
      var next = getSectionSettings(FILES_SETTINGS_KEY, FILES_DEFAULTS);
      next.defaultView = quickFileView.value || FILES_DEFAULTS.defaultView;
      saveSectionSettings(FILES_SETTINGS_KEY, FILES_DEFAULTS, next);
    });
    var quickBackend = document.getElementById('general_quick_math_backend');
    if (quickBackend) quickBackend.addEventListener('change', function() {
      var next = getSectionSettings(ROBOT_SETTINGS_KEY, ROBOT_DEFAULTS);
      next.mathBackend = quickBackend.value || ROBOT_DEFAULTS.mathBackend;
      next.onboardMathEnabled = next.mathBackend === 'onboard-s3';
      saveSectionSettings(ROBOT_SETTINGS_KEY, ROBOT_DEFAULTS, next);
      if (typeof global.ikSetComputationPreference === 'function') global.ikSetComputationPreference(next.mathBackend);
      fetch('/api/robot/math/onboard', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'action=backend&mode=' + encodeURIComponent(next.mathBackend)
      }).catch(function() {});
    });
    body.querySelectorAll('[data-settings-jump]').forEach(function(btn) {
      btn.addEventListener('click', function() {
        var tabId = btn.getAttribute('data-settings-jump');
        var tabBtn = document.querySelector('.settings-tab-btn[data-settings-tab="' + tabId + '"]');
        switchSettingsTab(tabId, tabBtn);
      });
    });
    var localeEl = document.getElementById('general_locale');
    if (localeEl) {
      localeEl.addEventListener('change', function() {
        s.locale = localeEl.value || GENERAL_DEFAULTS.locale;
        save(s);
        fetch('/api/profile', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'locale=' + encodeURIComponent(s.locale)
        }).then(function(r) { return r.ok ? r.json() : null; })
          .then(function() {
            var status = document.getElementById('profile_status');
            if (status) status.textContent = 'Dil kaydedildi: ' + s.locale;
          }).catch(function() {
            var status = document.getElementById('profile_status');
            if (status) status.textContent = 'Dil kaydedilemedi.';
          });
      });
    }
    fetch('/api/profile', { cache: 'no-store' })
      .then(function(r) { return r.ok ? r.json() : null; })
      .then(function(profile) {
        if (!profile || !profile.locale) return;
        s.locale = profile.locale;
        save(s);
        var el = document.getElementById('general_locale');
        if (el) el.value = s.locale;
      }).catch(function() {});
  }

  function mountSettingsSecurityTabContent() {
    var body = document.getElementById('settings-tab-security-body');
    if (!body) return;
    var s = getSectionSettings(SECURITY_SETTINGS_KEY, SECURITY_DEFAULTS);
    body.innerHTML = settingsHeaderHtml('settings-tab-security', 'Oturum') +
      '<div data-settings-anchor="session">' +
      settingsSectionHtml('Oturum',
        '<div class="settings-two-col">' +
        inputRowHtml('sec_timeout', 'Oturum zaman aşımı (dk)', s.sessionTimeoutMin, 'number') +
        uiToggleRowHtml('sec_remember', 'Beni hatırla seçeneğine izin ver', s.allowRememberSession) +
        '</div><div id="security_session_overview" class="settings-status-line" style="margin-top:8px;">Oturum bilgisi yükleniyor.</div>' +
        '<div class="settings-action-row" style="margin-top:10px;">' +
        '<button id="sec_refresh_users" class="settings-action-btn" type="button">Oturumları Yenile</button>' +
        '<button id="sec_revoke_current" class="settings-action-btn" type="button">Bu Oturumu Kapat</button>' +
        '<button id="sec_revoke_all" class="settings-action-btn" type="button">Tüm Web Oturumlarını Kapat</button>' +
        '</div><div id="security_action_status" class="settings-status-line"></div>') + '</div>' +
      '<div data-settings-anchor="shell-sessions">' +
      settingsSectionHtml('Shell Oturumları',
        '<div id="security_shell_sessions_summary" class="settings-status-line">Shell oturumları yükleniyor.</div>' +
        '<div id="security_shell_sessions_list" class="settings-session-list" style="margin-top:10px;">' +
        '<div class="settings-card muted">Aktif shell oturumu yükleniyor.</div></div>' +
        '<div class="settings-action-row" style="margin-top:10px;">' +
        '<button id="sec_refresh_shell_sessions" class="settings-action-btn" type="button">Shell Oturumlarını Yenile</button>' +
        '<button id="sec_close_shell_sessions" class="settings-action-btn" type="button">Tüm Shell Panellerini Kapat</button>' +
        '</div>') + '</div>' +
      '<div data-settings-anchor="users">' +
      settingsSectionHtml('Kullanıcılar',
        '<div id="security_users_list" class="settings-security-users"><div class="settings-card muted">Kullanıcı listesi yükleniyor.</div></div>' +
        '<div class="settings-two-col" style="margin-top:10px;">' +
        inputRowHtml('sec_new_display', 'Görünen ad', '', 'text') +
        inputRowHtml('sec_new_user', 'Yeni kullanıcı adı', '', 'text') +
        inputRowHtml('sec_new_pass', 'Yeni kullanıcı şifresi', '', 'password') +
        selectRowHtml('sec_new_role', 'Rol', 'operator', [['operator', 'Operatör'], ['admin', 'Admin']]) +
        uiToggleRowHtml('sec_new_sudo', 'sudo / root onayı isteyebilsin', false) +
        '</div><div class="settings-action-row" style="margin-top:10px;">' +
        '<button id="sec_add_user" class="settings-action-btn" type="button">Kullanıcı Ekle</button>' +
        '</div>') + '</div>' +
      '<div data-settings-anchor="passwords">' +
      settingsSectionHtml('Şifre Yönetimi',
        '<div class="settings-two-col">' +
        '<label class="settings-input-row"><span>Kullanıcı</span><select id="sec_password_user"></select></label>' +
        inputRowHtml('sec_reauth_pass', 'Mevcut oturum şifresi', '', 'password') +
        inputRowHtml('sec_password_new', 'Yeni şifre', '', 'password') +
        inputRowHtml('sec_password_new2', 'Yeni şifre tekrar', '', 'password') +
        '</div><div class="settings-action-row" style="margin-top:10px;">' +
        '<button id="sec_change_password" class="settings-action-btn" type="button">Şifreyi Değiştir</button>' +
        '</div><div class="settings-card muted" style="margin-top:10px;">Normal kullanıcı şifreleri burada değiştirilir. Kök kullanıcı ayrı bölümde tutulur; böylece bakım ve operatör yetkisi karışmaz.</div>') + '</div>' +
      '<div data-settings-anchor="root-password">' +
      settingsSectionHtml('Kök Şifresi',
        '<div class="settings-two-col">' +
        inputRowHtml('sec_root_pass', 'Yeni kök şifresi', '', 'password') +
        inputRowHtml('sec_root_pass2', 'Yeni kök şifre tekrar', '', 'password') +
        '</div><div class="settings-action-row" style="margin-top:10px;">' +
        '<button id="sec_change_root_password" class="settings-action-btn" type="button">Kök Şifresini Değiştir</button>' +
        '</div>') + '</div>' +
      '<div data-settings-anchor="auth-reset">' +
      settingsSectionHtml('Hesapları Sıfırla ve İlk Kuruluma Dön',
        '<div class="settings-card settings-danger-card">Bu işlem web kimliğini, ek kullanıcıları ve açık oturumları sıfırlar. Cihaz tekrar hesap oluşturma ekranına döner.</div>' +
        '<div class="settings-two-col" style="margin-top:10px;">' +
        inputRowHtml('sec_auth_reset_confirm', 'Onay metni', '', 'text') +
        '</div><div class="settings-action-row" style="margin-top:10px;">' +
        '<button id="sec_auth_reset" class="settings-action-btn danger" type="button">Hesapları Sıfırla ve Kuruluma Dön</button>' +
        '</div>') + '</div>' +
      '<div data-settings-anchor="root">' +
      settingsSectionHtml('Root',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('sec_root_confirm', 'Root işlemlerinde yeniden onay iste', s.requireRootConfirm) +
        uiToggleRowHtml('sec_danger_confirm', 'Silme/update gibi riskli işlemlerde onay iste', s.requireDangerConfirm) +
        '</div>') + '</div>' +
      '<div data-settings-anchor="policy">' +
      settingsSectionHtml('Politika',
        '<div class="settings-card muted">Kullanıcı ve oturum kayıtları cihazın kullanıcı NVS alanında tutulur; dosya yöneticisinden görünen /ESPUSER çalışma alanına credential hash yazılmaz. WiFi şifreleri bu bölümden yönetilmez; ağ profili kendi güvenli yolunda kalır.</div>') + '</div>';
    var save = function(next) { saveSectionSettings(SECURITY_SETTINGS_KEY, SECURITY_DEFAULTS, next); };
    bindSettingInput('sec_timeout', s, 'sessionTimeoutMin', save, true);
    bindSettingToggle('sec_remember', s, 'allowRememberSession', save);
    bindSettingToggle('sec_root_confirm', s, 'requireRootConfirm', save);
    bindSettingToggle('sec_danger_confirm', s, 'requireDangerConfirm', save);
    function setSecurityStatus(text) {
      var node = document.getElementById('security_action_status');
      if (node) node.textContent = text || '';
    }
    function postSecurity(url, params) {
      return fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: new URLSearchParams(params || {}).toString(),
        cache: 'no-store'
      }).then(function(r) {
        return r.json().catch(function() { return { success: r.ok }; }).then(function(j) {
          if (!r.ok || j.success === false) throw new Error(j.error || j.error_code || ('HTTP ' + r.status));
          return j;
        });
      });
    }
    function roleText(user) {
      if (user.root) return 'root';
      if (user.admin) return 'admin';
      return 'operator';
    }
    function renderSecurityUsers(payload) {
      var users = Array.isArray(payload && payload.users) ? payload.users : [];
      var list = document.getElementById('security_users_list');
      var select = document.getElementById('sec_password_user');
      var overview = document.getElementById('security_session_overview');
      if (overview && payload) {
        overview.textContent = 'HTTP: ' + (payload.session_active ? 'aktif' : 'pasif') +
          ' | kullanıcı: ' + (payload.current_user || '-') +
          ' | shell: ' + (payload.shell_sessions || 0) + '/' + (payload.shell_session_capacity || 0) +
          ' | root shell: ' + (payload.root_shell_sessions || 0) +
          ' | lockout: ' + Math.round(Number(payload.login_lockout_ms || 0) / 1000) + ' sn';
      }
      if (select) {
        select.innerHTML = users.filter(function(u) { return !u.root; }).map(function(u) {
          return '<option value="' + settingsEsc(u.username) + '">' + settingsEsc(u.username) + ' (' + settingsEsc(roleText(u)) + ')</option>';
        }).join('');
      }
      if (!list) return;
      if (!users.length) {
        list.innerHTML = '<div class="settings-card muted">Kullanıcı bulunamadı.</div>';
        return;
      }
      list.innerHTML = users.map(function(u) {
        var chips = '<span class="settings-role-chip">' + settingsEsc(roleText(u)) + '</span>';
        if (u.sudo) chips += '<span class="settings-role-chip">sudo</span>';
        if (u.primary) chips += '<span class="settings-role-chip">primary</span>';
        if (u.current) chips += '<span class="settings-role-chip active">aktif</span>';
        return '<div class="settings-user-row" data-security-user="' + settingsEsc(u.username) + '">' +
          '<div><strong>' + settingsEsc(u.username) + '</strong><span>' + settingsEsc(u.display_name || u.username) + '</span></div>' +
          '<div class="settings-user-chips">' + chips + '</div>' +
          '<button type="button" data-security-delete="' + settingsEsc(u.username) + '"' +
          ((u.root || u.primary || u.current) ? ' disabled' : '') + '>Sil</button>' +
          '</div>';
      }).join('');
      list.querySelectorAll('[data-security-delete]').forEach(function(btn) {
        btn.addEventListener('click', function() {
          var username = btn.getAttribute('data-security-delete') || '';
          if (!username || !global.confirm(username + ' kullanıcısı silinsin mi?')) return;
          postSecurity('/api/security/users/delete', {
            username: username,
            current_password: (document.getElementById('sec_reauth_pass') || {}).value || ''
          })
            .then(function() { setSecurityStatus('Kullanıcı silindi: ' + username); loadSecurityUsers(); })
            .catch(function(err) { setSecurityStatus('Silme hatası: ' + err.message); });
        });
      });
    }
    function loadSecurityUsers() {
      fetch('/api/security/users', { cache: 'no-store' })
        .then(function(r) { return r.ok ? r.json() : Promise.reject(new Error('HTTP ' + r.status)); })
        .then(renderSecurityUsers)
        .catch(function(err) { setSecurityStatus('Kullanıcı bilgisi alınamadı: ' + err.message); });
    }
    function renderShellSessions(payload) {
      var sessions = Array.isArray(payload && payload.sessions) ? payload.sessions : [];
      var list = document.getElementById('security_shell_sessions_list');
      var summary = document.getElementById('security_shell_sessions_summary');
      if (summary) {
        summary.textContent = 'Aktif: ' + (payload && payload.active_sessions || 0) +
          '/' + (payload && payload.max_sessions || 0) +
          ' | root: ' + (payload && payload.root_sessions || 0) +
          ' | pane/client sınırı: ' + (payload && payload.max_panes_per_client || 0);
      }
      if (!list) return;
      if (!sessions.length) {
        list.innerHTML = '<div class="settings-card muted">Aktif web shell oturumu yok.</div>';
        return;
      }
      list.innerHTML = sessions.map(function(session) {
        var sessionId = Number(session.session_id || 0);
        var chips = '<span class="settings-role-chip">client ' + settingsEsc(session.client_id || 0) + '</span>' +
          '<span class="settings-role-chip">pane ' + settingsEsc(session.pane_id || 0) + '</span>';
        if (session.root) chips += '<span class="settings-role-chip active">root</span>';
        return '<div class="settings-session-row" data-shell-session="' + sessionId + '">' +
          '<div><strong>Shell #' + settingsEsc(sessionId) + '</strong><span>Web terminal oturumu</span></div>' +
          '<div class="settings-user-chips">' + chips + '</div>' +
          '<button type="button" data-shell-session-close="' + sessionId + '">Kapat</button>' +
          '</div>';
      }).join('');
      list.querySelectorAll('[data-shell-session-close]').forEach(function(btn) {
        btn.addEventListener('click', function() {
          var id = btn.getAttribute('data-shell-session-close') || '';
          if (!id || !global.confirm('Shell oturumu #' + id + ' kapatılsın mı?')) return;
          fetch('/api/shell/sessions?id=' + encodeURIComponent(id), {
            method: 'DELETE',
            cache: 'no-store'
          }).then(function(r) {
            return r.json().catch(function() { return { ok: r.ok }; }).then(function(j) {
              if (!r.ok || j.ok === false) throw new Error(j.error || ('HTTP ' + r.status));
              setSecurityStatus('Shell oturumu kapatıldı: #' + id);
              loadShellSessions();
            });
          }).catch(function(err) { setSecurityStatus('Shell oturumu kapatılamadı: ' + err.message); });
        });
      });
    }
    function loadShellSessions() {
      fetch('/api/shell/sessions', { cache: 'no-store' })
        .then(function(r) { return r.ok ? r.json() : Promise.reject(new Error('HTTP ' + r.status)); })
        .then(renderShellSessions)
        .catch(function(err) { setSecurityStatus('Shell oturumları alınamadı: ' + err.message); });
    }
    function refreshSecurityPanel() {
      loadSecurityUsers();
      loadShellSessions();
    }
    bindActionButton('sec_refresh_users', refreshSecurityPanel);
    bindActionButton('sec_refresh_shell_sessions', loadShellSessions);
    bindActionButton('sec_close_shell_sessions', function() {
      fetch('/api/shell/sessions', { cache: 'no-store' })
        .then(function(r) { return r.ok ? r.json() : Promise.reject(new Error('HTTP ' + r.status)); })
        .then(function(payload) {
          var sessions = Array.isArray(payload.sessions) ? payload.sessions : [];
          if (!sessions.length) {
            setSecurityStatus('Kapatılacak shell oturumu yok.');
            return;
          }
          if (!global.confirm(sessions.length + ' shell paneli kapatılsın mı?')) return;
          return Promise.all(sessions.map(function(session) {
            return fetch('/api/shell/sessions?id=' + encodeURIComponent(session.session_id), {
              method: 'DELETE',
              cache: 'no-store'
            }).then(function(r) { return r.ok; });
          })).then(function(results) {
            var closed = results.filter(Boolean).length;
            setSecurityStatus(closed + ' shell oturumu kapatıldı.');
            loadShellSessions();
          });
        })
        .catch(function(err) { setSecurityStatus('Shell oturumları kapatılamadı: ' + err.message); });
    });
    bindActionButton('sec_add_user', function() {
      var role = document.getElementById('sec_new_role');
      postSecurity('/api/security/users/add', {
        display_name: (document.getElementById('sec_new_display') || {}).value || '',
        username: (document.getElementById('sec_new_user') || {}).value || '',
        password: (document.getElementById('sec_new_pass') || {}).value || '',
        admin: role && role.value === 'admin' ? '1' : '0',
        sudo: (document.getElementById('sec_new_sudo') || {}).checked ? '1' : '0',
        current_password: (document.getElementById('sec_reauth_pass') || {}).value || ''
      }).then(function() {
        setSecurityStatus('Kullanıcı eklendi.');
        ['sec_new_display', 'sec_new_user', 'sec_new_pass'].forEach(function(id) { var node = document.getElementById(id); if (node) node.value = ''; });
        loadSecurityUsers();
      }).catch(function(err) { setSecurityStatus('Ekleme hatası: ' + err.message); });
    });
    bindActionButton('sec_change_password', function() {
      var user = document.getElementById('sec_password_user');
      postSecurity('/api/security/users/password', {
        username: user ? user.value : '',
        password: (document.getElementById('sec_password_new') || {}).value || '',
        password2: (document.getElementById('sec_password_new2') || {}).value || '',
        current_password: (document.getElementById('sec_reauth_pass') || {}).value || ''
      }).then(function() {
        setSecurityStatus('Şifre değiştirildi.');
        ['sec_password_new', 'sec_password_new2', 'sec_reauth_pass'].forEach(function(id) { var node = document.getElementById(id); if (node) node.value = ''; });
      }).catch(function(err) { setSecurityStatus('Şifre hatası: ' + err.message); });
    });
    bindActionButton('sec_change_root_password', function() {
      postSecurity('/api/security/users/password', {
        username: 'root',
        password: (document.getElementById('sec_root_pass') || {}).value || '',
        password2: (document.getElementById('sec_root_pass2') || {}).value || '',
        current_password: (document.getElementById('sec_reauth_pass') || {}).value || ''
      }).then(function() {
        setSecurityStatus('Kök şifresi değiştirildi.');
        ['sec_root_pass', 'sec_root_pass2', 'sec_reauth_pass'].forEach(function(id) { var node = document.getElementById(id); if (node) node.value = ''; });
      }).catch(function(err) { setSecurityStatus('Kök şifre hatası: ' + err.message); });
    });
    bindActionButton('sec_revoke_current', function() {
      postSecurity('/api/security/sessions/revoke', { action: 'current' })
        .then(function() { setSecurityStatus('Oturum kapatıldı; yeniden giriş gerekebilir.'); })
        .catch(function(err) { setSecurityStatus('Oturum kapatma hatası: ' + err.message); });
    });
    bindActionButton('sec_revoke_all', function() {
      if (!global.confirm('Tüm web ve websocket oturumları kapatılsın mı?')) return;
      postSecurity('/api/security/sessions/revoke', {
        action: 'all',
        current_password: (document.getElementById('sec_reauth_pass') || {}).value || ''
      })
        .then(function() { setSecurityStatus('Tüm web oturumları kapatıldı.'); })
        .catch(function(err) { setSecurityStatus('Toplu kapatma hatası: ' + err.message); });
    });
    bindActionButton('sec_auth_reset', function() {
      var confirmText = (document.getElementById('sec_auth_reset_confirm') || {}).value || '';
      if (confirmText !== 'RESET-SETUP') {
        setSecurityStatus('Devam etmek için onay metnine RESET-SETUP yazın.');
        return;
      }
      if (!global.confirm('Tüm hesap kimliği sıfırlanacak ve /setup ekranına dönülecek. Devam edilsin mi?')) return;
      postSecurity('/api/security/auth-reset', {
        confirm: confirmText,
        current_password: (document.getElementById('sec_reauth_pass') || {}).value || ''
      }).then(function(payload) {
        setSecurityStatus('Hesaplar sıfırlandı; kurulum ekranı açılıyor.');
        setTimeout(function() {
          global.location.href = (payload && payload.redirect) || '/setup';
        }, 400);
      }).catch(function(err) { setSecurityStatus('Hesap sıfırlama hatası: ' + err.message); });
    });
    refreshSecurityPanel();
  }

  function mountSettingsProfileTabContent() {
    var body = document.getElementById('settings-tab-profile-body');
    if (!body) return;
    body.innerHTML = settingsHeaderHtml('settings-tab-profile', 'Profil') +
      '<div data-settings-anchor="overview">' +
      settingsSectionHtml('Hızlı Özet',
        '<div class="settings-grid-3">' +
        '<div class="settings-kpi"><div class="settings-kpi-label">Kullanıcı</div><div id="profile_kpi_user" class="settings-kpi-value">--</div></div>' +
        '<div class="settings-kpi"><div class="settings-kpi-label">Oturum</div><div id="profile_kpi_session" class="settings-kpi-value">--</div></div>' +
        '<div class="settings-kpi"><div class="settings-kpi-label">Shell</div><div id="profile_kpi_shell" class="settings-kpi-value">--</div></div>' +
        '</div><div id="profile_manage_status" class="settings-status-line" style="margin-top:8px;">Yükleniyor...</div>') + '</div>' +
      '<div data-settings-anchor="users">' +
      settingsSectionHtml('Kullanıcı Yönetimi',
        '<div class="settings-action-row">' +
        '<button class="settings-action-btn" type="button" data-profile-jump="settings-tab-security" data-profile-anchor="users">Kullanıcı Ekle / Sil</button>' +
        '<button class="settings-action-btn" type="button" data-profile-jump="settings-tab-security" data-profile-anchor="passwords">Şifre Değiştir</button>' +
        '<button class="settings-action-btn" type="button" data-profile-jump="settings-tab-security" data-profile-anchor="root-password">Kök Şifresi</button>' +
        '<button class="settings-action-btn danger" type="button" data-profile-jump="settings-tab-security" data-profile-anchor="auth-reset">Hesap Reset</button>' +
        '</div>') + '</div>' +
      '<div data-settings-anchor="sessions">' +
      settingsSectionHtml('Oturum Yönetimi',
        '<div class="settings-action-row">' +
        '<button class="settings-action-btn" type="button" data-profile-jump="settings-tab-security" data-profile-anchor="session">Web Oturumları</button>' +
        '<button class="settings-action-btn" type="button" data-profile-jump="settings-tab-security" data-profile-anchor="shell-sessions">Shell Oturumları</button>' +
        '</div>') + '</div>' +
      '<div data-settings-anchor="passwords">' +
      settingsSectionHtml('Politika',
        '<div class="settings-card muted">Profil yönetimi, güvenlik sekmesindeki gerçek kullanıcı API’lerini kullanır. WiFi parolaları bu alana yazılmaz; ağ profili kendi güvenli akışında kalır.</div>') + '</div>';
    fetch('/api/security/users', { cache: 'no-store' })
      .then(function(r) { return r.ok ? r.json() : null; })
      .then(function(payload) {
        setTextIf('profile_kpi_user', payload && payload.current_user ? payload.current_user : '--');
        setTextIf('profile_kpi_session', payload && payload.session_active ? 'aktif' : 'pasif');
        setTextIf('profile_kpi_shell', payload ? String(payload.shell_sessions || 0) + '/' + String(payload.shell_session_capacity || 0) : '--');
        setTextIf('profile_manage_status', payload ? 'Profil ve oturum özeti alındı.' : 'Profil bilgisi alınamadı.');
      }).catch(function() { setTextIf('profile_manage_status', 'Profil bilgisi alınamadı.'); });
    body.querySelectorAll('[data-profile-jump]').forEach(function(btn) {
      btn.addEventListener('click', function() {
        var tabId = btn.getAttribute('data-profile-jump') || 'settings-tab-security';
        var tabBtn = document.querySelector('.settings-tab-btn[data-settings-tab="' + tabId + '"]');
        switchSettingsTab(tabId, tabBtn);
        var anchor = btn.getAttribute('data-profile-anchor');
        if (anchor) {
          setTimeout(function() {
            var target = document.querySelector('#' + tabId + ' [data-settings-anchor="' + anchor + '"]');
            if (target && target.scrollIntoView) target.scrollIntoView({ block: 'start', behavior: 'smooth' });
          }, 80);
        }
      });
    });
  }

  function mountSettingsNetTabContent() {
    var body = document.getElementById('settings-tab-net-body');
    if (!body) return;
    var s = getSectionSettings(NET_SETTINGS_KEY, NET_DEFAULTS);
    body.innerHTML =
      settingsHeaderHtml('settings-tab-net', 'WiFi OS Panel') +
      '<div data-settings-anchor="state">' +
      settingsSectionHtml('Ağ Durumu',
      '<div class="settings-grid-3">' +
      '<div class="settings-kpi"><div class="settings-kpi-label">Durum</div><div id="net_kpi_state" class="settings-kpi-value">--</div></div>' +
      '<div class="settings-kpi"><div class="settings-kpi-label">IP</div><div id="net_kpi_ip" class="settings-kpi-value">--</div></div>' +
      '<div class="settings-kpi"><div class="settings-kpi-label">RSSI</div><div id="net_kpi_rssi" class="settings-kpi-value">--</div></div>' +
      '</div><div class="settings-action-row" style="margin-top:10px;">' +
      '<button id="net_action_on" class="settings-action-btn" type="button">WiFi Aç</button>' +
      '<button id="net_action_off" class="settings-action-btn" type="button">WiFi Kapat</button>' +
      '<button id="net_action_hotspot" class="settings-action-btn" type="button">Hotspot</button>' +
      '<button id="net_action_reconnect" class="settings-action-btn" type="button">Yeniden Bağlan</button>' +
      '</div><div id="net_action_status" class="settings-status-line"></div>') + '</div>' +
      '<div data-settings-anchor="connect">' +
      settingsSectionHtml('Profil ve Bağlantı',
      '<div class="settings-two-col">' +
      inputRowHtml('net_ssid', 'SSID', '', 'text') +
      inputRowHtml('net_pass', 'Şifre', '', 'password') +
      '</div><div class="settings-action-row" style="margin-top:10px;">' +
      '<button id="net_connect" class="settings-action-btn" type="button">Bağlantıyı Test Et</button>' +
      '<button id="net_save" class="settings-action-btn" type="button">Profili Kaydet</button>' +
      '</div>') + '</div>' +
      '<div data-settings-anchor="scan">' +
      settingsSectionHtml('Ağlar',
      '<div class="settings-two-col">' +
      uiToggleRowHtml('net_auto_scan', 'WiFi taramasını otomatik yenile', s.autoScan) +
      uiToggleRowHtml('net_remember_ssid', 'Son SSID bilgisini hatırla', s.rememberLastSsid) +
      uiToggleRowHtml('net_signal_details', 'Sinyal ayrıntılarını göster', s.showSignalDetails) +
      inputRowHtml('net_mdns_name', 'mDNS cihaz adı', s.mdnsName, 'text') +
      '</div><div class="settings-action-row" style="margin-top:10px;">' +
      '<button id="net_scan" class="settings-action-btn" type="button">Ağları Tara</button>' +
      '<button id="net_refresh" class="settings-action-btn" type="button">Durumu Yenile</button>' +
      '</div><div id="net_scan_hint" class="settings-status-line" style="margin-top:8px;"></div>' +
      '<div id="net_scan_results" class="settings-networks" style="display:grid; grid-template-columns:repeat(auto-fit,minmax(230px,1fr)); gap:8px; max-height:330px; overflow:auto; margin-top:10px;"></div>') + '</div>';
    var save = function(next) { saveSectionSettings(NET_SETTINGS_KEY, NET_DEFAULTS, next); };
    bindSettingToggle('net_auto_scan', s, 'autoScan', save);
    bindSettingToggle('net_remember_ssid', s, 'rememberLastSsid', save);
    bindSettingToggle('net_signal_details', s, 'showSignalDetails', save);
    bindSettingInput('net_mdns_name', s, 'mdnsName', save, false);
    function setNetStatus(text) {
      var el = document.getElementById('net_action_status');
      if (el) el.textContent = text || '';
    }
    function netSecurityLabel(n) {
      var sec = String((n && (n.security || n.enc)) || 'unknown').toLowerCase();
      if (sec === '0' || sec === 'open') return 'Açık';
      if (sec.indexOf('wpa3') >= 0) return 'WPA3';
      if (sec.indexOf('wpa2') >= 0) return 'WPA2';
      if (sec.indexOf('wpa') >= 0) return 'WPA';
      if (sec.indexOf('wep') >= 0) return 'WEP';
      return sec.toUpperCase();
    }
    function netQualityClass(q) {
      if (q >= 75) return ['Mükemmel', '#B5EA6A'];
      if (q >= 55) return ['İyi', '#7EA8F2'];
      if (q >= 35) return ['Orta', '#EAC27C'];
      return ['Zayıf', '#EA7B7B'];
    }
    function renderNetworkList(list, state) {
      var out = document.getElementById('net_scan_results');
      var hint = document.getElementById('net_scan_hint');
      if (!out) return;
      if (!Array.isArray(list) || !list.length) {
        out.innerHTML = '<div class="settings-card muted">Görünür ağ yok. Tarama devam ediyorsa birkaç saniye sonra yenileyin.</div>';
        if (hint) hint.textContent = 'Ağ listesi boş.';
        return;
      }
      if (hint) {
        var age = state && state.manual_scan_age_ms != null ? Number(state.manual_scan_age_ms) : NaN;
        hint.textContent = isFinite(age) && age > 0 ? ('Son tarama yaşı: ' + Math.round(age / 1000) + ' sn') : 'Tarama sonucu hazır.';
      }
      out.innerHTML = list.map(function(n, idx) {
        var rssi = Number(n && n.rssi);
        var quality = Number(n && n.quality);
        if (!isFinite(quality)) quality = isFinite(rssi) ? Math.max(0, Math.min(100, (rssi + 100) * 2)) : 0;
        var qc = netQualityClass(quality);
        var badges = '';
        if (n && n.current) badges += '<span class="settings-pill">Bağlı</span>';
        if (n && n.known) badges += '<span class="settings-pill">Kayıtlı</span>';
        if (n && n.last_good) badges += '<span class="settings-pill">Son iyi</span>';
        var ssid = settingsEsc(n && n.ssid ? n.ssid : ('Ağ ' + (idx + 1)));
        return '<div class="settings-card net-network-card" data-ssid="' + ssid + '" style="text-align:left; cursor:pointer; border-color:' + qc[1] + '66;">' +
          '<div style="display:flex;align-items:center;justify-content:space-between;gap:8px;">' +
          '<div style="font-weight:700;color:var(--text);overflow:hidden;text-overflow:ellipsis;">' + ssid + '</div>' +
          '<div style="font-size:11px;color:' + qc[1] + ';">' + qc[0] + '</div></div>' +
          '<div style="height:6px;background:#222;border-radius:999px;margin:8px 0;overflow:hidden;"><i style="display:block;height:100%;width:' + Math.round(quality) + '%;background:' + qc[1] + ';"></i></div>' +
          '<div class="settings-status-line">RSSI ' + (isFinite(rssi) ? rssi + ' dBm' : '--') +
          ' | Kanal ' + settingsEsc(n && n.channel != null ? n.channel : '--') +
          ' | ' + settingsEsc(netSecurityLabel(n)) + '</div>' +
          '<div style="display:flex;gap:5px;flex-wrap:wrap;margin-top:8px;">' + badges + '</div>' +
          '<div class="settings-action-row" style="margin-top:8px;"><button type="button" class="settings-action-btn" data-net-connect="' + ssid + '">Bağlan</button></div>' +
          '</div>';
      }).join('');
      out.querySelectorAll('.net-network-card').forEach(function(card) {
        card.addEventListener('click', function() {
          var ssid = card.getAttribute('data-ssid') || '';
          var input = document.getElementById('net_ssid');
          if (input) input.value = ssid;
          setNetStatus('Seçilen ağ: ' + ssid);
        });
      });
      out.querySelectorAll('[data-net-connect]').forEach(function(btn) {
        btn.addEventListener('click', function(ev) {
          ev.preventDefault();
          ev.stopPropagation();
          var ssid = btn.getAttribute('data-net-connect') || '';
          var input = document.getElementById('net_ssid');
          if (input) input.value = ssid;
          connectWifiSelected();
        });
      });
    }
    function wifiAction(action, done) {
      fetch('/api/wifi/action', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'action=' + encodeURIComponent(action)
      }).then(function(r) { return r.ok ? r.json() : null; })
        .then(function() { setNetStatus('Komut çalıştı: wifi ' + action); if (done) done(); refreshWifiState(); })
        .catch(function() { setNetStatus('WiFi komutu başarısız.'); });
    }
    function refreshWifiState() {
      fetch('/api/wifi/state', { cache: 'no-store' })
        .then(function(r) { return r.ok ? r.json() : null; })
        .then(function(w) {
          setTextIf('net_kpi_state', w && w.sta_connected ? 'connected' : (w && w.ap_active ? 'hotspot' : 'down'));
          setTextIf('net_kpi_ip', w && w.ip ? w.ip : '--');
          setTextIf('net_kpi_rssi', w && w.rssi != null ? String(w.rssi) + ' dBm' : '--');
          var ssidInput = document.getElementById('net_ssid');
          if (ssidInput && !ssidInput.value && w && w.ssid) ssidInput.value = w.ssid;
        }).catch(function() {});
    }
    function connectWifiSelected() {
      var ssid = document.getElementById('net_ssid');
      var pass = document.getElementById('net_pass');
      if (!ssid || !ssid.value) { setNetStatus('SSID seçin veya yazın.'); return; }
      fetch('/api/wifi/connect', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'ssid=' + encodeURIComponent(ssid ? ssid.value : '') + '&pass=' + encodeURIComponent(pass ? pass.value : '')
      }).then(function(r) { setNetStatus(r.ok ? 'Bağlantı testi başlatıldı.' : 'Bağlantı başlatılamadı.'); refreshWifiState(); })
        .catch(function() { setNetStatus('Bağlantı testi başlatılamadı.'); });
    }
    function scanWifi() {
      fetch('/api/wifi/scan', { cache: 'no-store' })
        .then(function(r) {
          if (!r || !(r.ok || r.status === 202)) return null;
          return r.json();
        })
        .catch(function() { return null; })
        .then(function(list) {
          if (Array.isArray(list)) {
            fetch('/api/wifi/state', { cache: 'no-store' })
              .then(function(r) { return r.ok ? r.json() : null; })
              .then(function(state) { renderNetworkList(list, state); })
              .catch(function() { renderNetworkList(list, null); });
          } else {
            var hint = document.getElementById('net_scan_hint');
            if (hint) hint.textContent = 'Tarama başlatıldı; sonuçlar otomatik yenilenecek.';
            if (s.autoScan) setTimeout(scanWifi, 1800);
          }
        }).catch(function() { setNetStatus('Tarama alınamadı.'); });
    }
    bindActionButton('net_action_on', function() { wifiAction('on'); });
    bindActionButton('net_action_off', function() { wifiAction('off'); });
    bindActionButton('net_action_hotspot', function() { wifiAction('hotspot'); });
    bindActionButton('net_action_reconnect', function() { wifiAction('reconnect'); });
    bindActionButton('net_scan', scanWifi);
    bindActionButton('net_refresh', refreshWifiState);
    bindActionButton('net_connect', connectWifiSelected);
    bindActionButton('net_save', function() {
      var ssid = document.getElementById('net_ssid');
      var pass = document.getElementById('net_pass');
      if (!ssid || !ssid.value) { setNetStatus('Kaydetmek için SSID gerekli.'); return; }
      fetch('/api/wifi/save', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'ssid=' + encodeURIComponent(ssid ? ssid.value : '') + '&pass=' + encodeURIComponent(pass ? pass.value : '')
      }).then(function(r) { setNetStatus(r.ok ? 'WiFi profili kaydedildi.' : 'WiFi profili kaydedilemedi.'); });
    });
    refreshWifiState();
    if (s.autoScan) scanWifi();
  }

  function mountSettingsCommsTabContent() {
    var body = document.getElementById('settings-tab-comms-body');
    if (!body) return;
    var s = getSectionSettings(DEVICES_SETTINGS_KEY, DEVICES_DEFAULTS);
    body.innerHTML =
      settingsHeaderHtml('settings-tab-comms', 'SPI / UART / ESP-NOW') +
      '<div data-settings-anchor="spi">' +
      settingsSectionHtml('SPI Köprüleri',
        '<div class="settings-grid-3">' +
        '<div class="settings-kpi"><div class="settings-kpi-label">P4 SPI</div><div id="comms_p4_spi" class="settings-kpi-value">--</div></div>' +
        '<div class="settings-kpi"><div class="settings-kpi-label">C3 Link</div><div id="comms_c3_link" class="settings-kpi-value">--</div></div>' +
        '<div class="settings-kpi"><div class="settings-kpi-label">PCA Queue</div><div id="comms_pca" class="settings-kpi-value">--</div></div>' +
        '</div>') + '</div>' +
      '<div data-settings-anchor="uart">' +
      settingsSectionHtml('UART Shell Bridge',
        '<div class="settings-two-col">' +
        selectRowHtml('comms_uart_shell_bridge', 'MSHELL UART bridge', s.uartShellBridge, [['off', 'Kapalı'], ['listen', 'Sadece dinle'], ['on', 'Aktif']]) +
        uiToggleRowHtml('comms_p4_auto', 'P4 otomatik reconnect', s.p4AutoReconnect) +
        '</div><div class="settings-action-row" style="margin-top:10px;">' +
        settingsActionButtonHtml('comms_refresh', 'Tanıları Yenile') +
        '</div><div id="comms_status" class="settings-status-line"></div>') + '</div>' +
      '<div data-settings-anchor="espnow">' +
      settingsSectionHtml('ESP-NOW / Alt Cihazlar',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('comms_c3_visible', 'C3 durum kartını göster', s.c3StatusVisible) +
        uiToggleRowHtml('comms_espnow_pref', 'ESP-NOW köprüsünü tercih et', s.espnowPreferBridge) +
        '</div>') + '</div>';
    var save = function(next) { saveSectionSettings(DEVICES_SETTINGS_KEY, DEVICES_DEFAULTS, next); };
    bindSettingInput('comms_uart_shell_bridge', s, 'uartShellBridge', save, false);
    bindSettingToggle('comms_p4_auto', s, 'p4AutoReconnect', save);
    bindSettingToggle('comms_c3_visible', s, 'c3StatusVisible', save);
    bindSettingToggle('comms_espnow_pref', s, 'espnowPreferBridge', save);
    var bridgeEl = document.getElementById('comms_uart_shell_bridge');
    if (bridgeEl) {
      bridgeEl.addEventListener('change', function() {
        fetch('/api/settings/uart-shell', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'mode=' + encodeURIComponent(bridgeEl.value || 'off')
        }).then(function(r) { return r.ok ? r.json() : null; })
          .then(function(data) {
            var status = document.getElementById('comms_status');
            if (status) status.textContent = data && data.ok ? ('Bridge modu: ' + data.mode) : 'Bridge modu uygulanamadı.';
          }).catch(function() {
            var status = document.getElementById('comms_status');
            if (status) status.textContent = 'Bridge modu uygulanamadı.';
          });
      });
    }
    function refreshComms() {
      fetch('/api/health', { cache: 'no-store' })
        .then(function(r) { return r.ok ? r.json() : null; })
        .then(function(h) {
          setTextIf('comms_p4_spi', h && h.web ? String(h.web.telemetry_scene_clients || 0) + ' scene' : '--');
          setTextIf('comms_c3_link', h && h.web ? String(h.web.telemetry_background_clients || 0) + ' bg' : '--');
          setTextIf('comms_pca', h && h.pca ? String(h.pca.queue_depth || 0) + '/' + String(h.pca.queue_capacity || 0) : '--');
          var status = document.getElementById('comms_status');
          if (status) status.textContent = h ? 'Health snapshot alındı.' : 'Health alınamadı.';
        }).catch(function() {
          var status = document.getElementById('comms_status');
          if (status) status.textContent = 'Health alınamadı.';
        });
    }
    bindActionButton('comms_refresh', refreshComms);
    refreshComms();
  }

  function mountSettingsPerfTabContent() {
    var body = document.getElementById('settings-tab-perf-body');
    if (!body) return;
    var s = getSectionSettings(PERF_SETTINGS_KEY, PERF_DEFAULTS);
    body.innerHTML =
      settingsHeaderHtml('settings-tab-perf', s.telemetryProfile) +
      '<div data-settings-anchor="telemetry">' +
      settingsSectionHtml('Telemetri',
      '<div class="settings-two-col">' +
      selectRowHtml('perf_telemetry_profile', 'Telemetri profili', s.telemetryProfile, [['fast', 'Fast'], ['balanced', 'Balanced'], ['quiet', 'Quiet']]) +
      inputRowHtml('perf_log_tail', 'Log önizleme byte', s.logTailBytes, 'number') +
      '</div>') + '</div>' +
      '<div data-settings-anchor="metrics">' +
      settingsSectionHtml('Canlı Metrikler',
      '<div id="perf_metrics_box" class="settings-card muted">Metrikler bekleniyor.</div>' +
      '<div class="settings-actions"><button id="perf_metrics_refresh" class="settings-action-btn" type="button">yenile</button></div>') + '</div>' +
      '<div data-settings-anchor="power">' +
      settingsSectionHtml('Power & Thermal',
      '<div class="settings-two-col">' +
      selectRowHtml('perf_power_mode', 'Güç profili', s.powerMode, [['balanced', 'Balanced'], ['cool', 'Cool'], ['performance', 'Performance'], ['motion-safe', 'Motion-safe'], ['update-safe', 'Update-safe']]) +
      '<div class="settings-card"><div class="kin3d-setting-label">Durum</div><div id="perf_power_status" class="settings-status-line">Yükleniyor...</div></div>' +
      '</div><div id="perf_power_locks" class="settings-card full muted" style="white-space:pre-wrap; min-height:64px;">PM lock bekleniyor.</div>' +
      '<div class="settings-actions">' +
      settingsActionButtonHtml('perf_power_refresh', 'Power yenile') +
      settingsActionButtonHtml('perf_power_cool', 'Cool') +
      settingsActionButtonHtml('perf_power_balanced', 'Balanced') +
      '</div>') + '</div>' +
      '<div data-settings-anchor="dpm">' +
      settingsSectionHtml('Device Process Manager',
      '<div class="settings-two-col">' +
      selectRowHtml('perf_dpm_policy', 'DPM policy', s.dpmPolicy, [['observe', 'Observe'], ['conservative', 'Conservative'], ['adaptive', 'Adaptive'], ['performance', 'Performance'], ['power-save', 'Power-save']]) +
      '<div class="settings-card"><div class="kin3d-setting-label">RTOS orkestrasyon</div><div id="perf_dpm_status" class="settings-status-line">Yükleniyor...</div></div>' +
      '</div><div id="perf_dpm_tasks" class="settings-card full muted" style="white-space:pre-wrap; min-height:72px;">Task tablosu bekleniyor.</div>' +
      '<div class="settings-actions">' +
      settingsActionButtonHtml('perf_dpm_refresh', 'DPM yenile') +
      settingsActionButtonHtml('perf_dpm_wake_peers', 'Peer tasklarını uyandır') +
      '</div>') + '</div>' +
      '<div data-settings-anchor="memory">' +
      settingsSectionHtml('Bellek',
      '<div class="settings-two-col">' +
      uiToggleRowHtml('perf_psram', 'Büyük web bufferları PSRAM öncelikli kullan', s.preferPsramBuffers) +
      inputRowHtml('perf_log_tail_dup', 'Shell çıktı güvenli tampon byte', s.logTailBytes, 'number') +
      '</div>') + '</div>' +
      '<div data-settings-anchor="ui">' +
      settingsSectionHtml('UI Yükü',
      '<div class="settings-two-col">' +
      uiToggleRowHtml('perf_reduce_motion', 'Animasyonları azalt', s.reduceMotion) +
      '</div>') + '</div>';
    var save = function(next) { saveSectionSettings(PERF_SETTINGS_KEY, PERF_DEFAULTS, next); };
    bindSettingInput('perf_telemetry_profile', s, 'telemetryProfile', save, false);
    bindSettingInput('perf_log_tail', s, 'logTailBytes', save, true);
    bindSettingInput('perf_log_tail_dup', s, 'logTailBytes', save, true);
    bindSettingInput('perf_dpm_policy', s, 'dpmPolicy', save, false);
    bindSettingInput('perf_power_mode', s, 'powerMode', save, false);
    bindSettingToggle('perf_psram', s, 'preferPsramBuffers', save);
    bindSettingToggle('perf_reduce_motion', s, 'reduceMotion', save);
    function refreshPerfMetrics() {
      var box = document.getElementById('perf_metrics_box');
      if (!box) return;
      fetch('/api/debug/sysinfo', { cache: 'no-store' })
        .then(function(r) { return r && r.ok ? r.json() : null; })
        .then(function(sys) {
          if (!sys) throw new Error('empty');
          var rows = [
            ['JSON overflow', sys.json_overflow],
            ['Shell pool miss/drop', String(sys.shell_pool_miss || 0) + ' / ' + String(sys.shell_drop || 0)],
            ['RTOS deadline miss', sys.rtos_deadline_miss],
            ['RTOS max slip', String(sys.rtos_max_slip_ms || 0) + ' ms'],
            ['WiFi reconnect', String(sys.wifi_reconnect_ms || 0) + ' ms'],
            ['WiFi fast-path', String(sys.wifi_fast_path_successes || 0) + ' / ' + String(sys.wifi_fast_path_attempts || 0)],
            ['WS frame bytes', [sys.ws_last_fast_bytes || 0, sys.ws_last_medium_bytes || 0, sys.ws_last_slow_bytes || 0].join(' / ')]
          ];
          box.className = 'settings-card';
          box.innerHTML = rows.map(function(row) {
            return '<div class="settings-prop-row"><span>' + settingsEsc(row[0]) + '</span><code>' + settingsEsc(row[1]) + '</code></div>';
          }).join('');
        })
        .catch(function() {
          box.className = 'settings-card muted';
          box.textContent = 'Metrikler alınamadı.';
        });
    }
    function renderPower(status, locks) {
      var statusEl = document.getElementById('perf_power_status');
      var locksEl = document.getElementById('perf_power_locks');
      var modeEl = document.getElementById('perf_power_mode');
      if (statusEl) {
        if (!status || !status.ok) {
          statusEl.textContent = 'Power durumu alınamadı.';
        } else {
          var temp = status.temperature_valid ? String(status.temperature_c) + ' C' : 'n/a';
          statusEl.textContent = 'mode=' + status.mode +
            ' | cpu=' + status.actual_cpu_mhz + 'MHz' +
            ' | demand=' + status.net_demand_mhz + '/' + status.rt_demand_mhz +
            ' | wifi=' + status.wifi_ps_mode +
            ' | temp=' + temp;
          if (modeEl && modeEl.value !== status.mode) {
            modeEl.value = status.mode;
            s.powerMode = status.mode;
            save(s);
          }
        }
      }
      if (!locksEl) return;
      if (!locks || !Array.isArray(locks.locks)) {
        locksEl.className = 'settings-card full muted';
        locksEl.textContent = 'PM lock listesi alınamadı.';
        return;
      }
      locksEl.className = 'settings-card full';
      locksEl.textContent = locks.locks.map(function(lock) {
        return String(lock.name || '-').padEnd(20) + ' ' +
          (lock.active ? 'active ' : 'idle   ') +
          ' reason=' + String(lock.reason || '-');
      }).join('\n') || 'PM lock yok.';
    }
    function refreshPower() {
      Promise.all([
        fetch('/api/power/status', { cache: 'no-store' }).then(function(r) { return r && r.ok ? r.json() : null; }),
        fetch('/api/power/locks', { cache: 'no-store' }).then(function(r) { return r && r.ok ? r.json() : null; })
      ]).then(function(pair) {
        renderPower(pair[0], pair[1]);
      }).catch(function() {
        renderPower(null, null);
      });
    }
    function setPowerMode(mode) {
      fetch('/api/power/mode', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'mode=' + encodeURIComponent(mode)
      }).then(function(r) { return r && r.ok ? r.json() : null; })
        .then(function() { refreshPower(); refreshDpm(); })
        .catch(function() { refreshPower(); });
    }
    function renderDpm(status, tasks) {
      var statusEl = document.getElementById('perf_dpm_status');
      var tasksEl = document.getElementById('perf_dpm_tasks');
      var policyEl = document.getElementById('perf_dpm_policy');
      if (statusEl) {
        if (!status || !status.ok) {
          statusEl.textContent = 'DPM durumu alınamadı.';
        } else {
          statusEl.textContent = 'policy=' + status.mode +
            ' | sleeping=' + status.sleeping +
            ' | active=' + status.active +
            ' | critical=' + status.critical +
            ' | degraded=' + status.degraded;
          if (policyEl && policyEl.value !== status.mode) {
            policyEl.value = status.mode;
            s.dpmPolicy = status.mode;
            save(s);
          }
        }
      }
      if (!tasksEl) return;
      if (!tasks || !Array.isArray(tasks.tasks)) {
        tasksEl.className = 'settings-card full muted';
        tasksEl.textContent = 'Task tablosu alınamadı.';
        return;
      }
      tasksEl.className = 'settings-card full';
      var lines = tasks.tasks.slice(0, 18).map(function(task) {
        return String(task.name || '-').padEnd(26) + ' ' +
          String(task.state || '-').padEnd(10) +
          ' wake=' + String(task.wake_count || 0).padStart(4) +
          ' sleep=' + String(task.sleep_count || 0).padStart(4) +
          ' wait=' + String(task.wait_ms || 0).padStart(4) +
          ' reason=' + String(task.last_wake_reason || '-');
      });
      tasksEl.textContent = lines.join('\n') || 'Kayıt yok.';
    }
    function refreshDpm() {
      Promise.all([
        fetch('/api/dpm/status', { cache: 'no-store' }).then(function(r) { return r && r.ok ? r.json() : null; }),
        fetch('/api/dpm/tasks', { cache: 'no-store' }).then(function(r) { return r && r.ok ? r.json() : null; })
      ]).then(function(pair) {
        renderDpm(pair[0], pair[1]);
      }).catch(function() {
        renderDpm(null, null);
      });
    }
    function setDpmPolicy(mode) {
      fetch('/api/dpm/policy', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'mode=' + encodeURIComponent(mode)
      }).then(function(r) { return r && r.ok ? r.json() : null; })
        .then(function() { refreshDpm(); })
        .catch(function() { refreshDpm(); });
    }
    var dpmPolicyEl = document.getElementById('perf_dpm_policy');
    if (dpmPolicyEl) {
      dpmPolicyEl.addEventListener('change', function() {
        s.dpmPolicy = dpmPolicyEl.value;
        save(s);
        setDpmPolicy(dpmPolicyEl.value);
      });
    }
    var powerModeEl = document.getElementById('perf_power_mode');
    if (powerModeEl) {
      powerModeEl.addEventListener('change', function() {
        s.powerMode = powerModeEl.value;
        save(s);
        setPowerMode(powerModeEl.value);
      });
    }
    bindActionButton('perf_power_refresh', refreshPower);
    bindActionButton('perf_power_cool', function() { setPowerMode('cool'); });
    bindActionButton('perf_power_balanced', function() { setPowerMode('balanced'); });
    bindActionButton('perf_dpm_refresh', refreshDpm);
    bindActionButton('perf_dpm_wake_peers', function() {
      fetch('/api/dpm/wake', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'task=peers&reason=web'
      }).then(function() { refreshDpm(); })
        .catch(function() { refreshDpm(); });
    });
    bindActionButton('perf_metrics_refresh', refreshPerfMetrics);
    refreshPerfMetrics();
    refreshPower();
    refreshDpm();
  }

  function mountSettingsPrefsTabContent() {
    var body = document.getElementById('settings-tab-prefs-body');
    if (!body) return;
    var s = getSectionSettings(PREFS_SETTINGS_KEY, PREFS_DEFAULTS);
    body.innerHTML =
      settingsHeaderHtml('settings-tab-prefs', s.defaultPanel) +
      '<div data-settings-anchor="defaults">' +
      settingsSectionHtml('Varsayılanlar',
      '<div class="settings-two-col">' +
      selectRowHtml('prefs_default_panel', 'Varsayılan panel', s.defaultPanel, [['3d', '3D Görünüm'], ['console', 'Konsol'], ['files', 'Dosyalar'], ['settings', 'Ayarlar']]) +
      uiToggleRowHtml('prefs_reopen_popup', 'Son popup durumunu hatırla', s.reopenLastPopup) +
      '</div>') + '</div>' +
      '<div data-settings-anchor="confirm">' +
      settingsSectionHtml('Onaylar',
      '<div class="settings-two-col">' +
      uiToggleRowHtml('prefs_confirm_destructive', 'Silme/üzerine yazma onayı iste', s.confirmDestructive) +
      uiToggleRowHtml('prefs_confirm_update', 'Update/recovery işlemlerinde ikinci onay iste', s.confirmUpdate) +
      uiToggleRowHtml('prefs_confirm_power', 'Poweroff/reboot komutlarında onay iste', s.confirmPower) +
      '</div>') + '</div>' +
      '<div data-settings-anchor="dock">' +
      settingsSectionHtml('Dock',
      '<div class="settings-two-col">' +
      uiToggleRowHtml('prefs_compact_dock', 'Hızlı araç dock kompakt olsun', s.compactToolDock) +
      uiToggleRowHtml('prefs_restore_last_view', 'Son görünüm ve panel durumunu geri yükle', s.restoreLastView) +
      '</div>') + '</div>';
    body.innerHTML += '<div data-settings-anchor="operator">' +
      settingsSectionHtml('Operatör Deneyimi',
      '<div class="settings-two-col">' +
      uiToggleRowHtml('prefs_keyboard_shortcuts', 'Klavye kısayollarını etkin tut', s.keyboardShortcuts) +
      uiToggleRowHtml('prefs_show_tooltips', 'Buton tooltiplerini göster', s.showTooltips) +
      selectRowHtml('prefs_date_format', 'Tarih biçimi', s.dateFormat, [['locale', 'Yerel'], ['iso', 'ISO 8601'], ['relative', 'Göreli']]) +
      selectRowHtml('prefs_unit_preset', 'Birim profili', s.unitPreset, [['metric', 'Metrik'], ['robotics', 'Robotik'], ['electronics', 'Elektronik']]) +
      '</div>') + '</div>';
    var save = function(next) { saveSectionSettings(PREFS_SETTINGS_KEY, PREFS_DEFAULTS, next); };
    bindSettingInput('prefs_default_panel', s, 'defaultPanel', save, false);
    bindSettingToggle('prefs_confirm_destructive', s, 'confirmDestructive', save);
    bindSettingToggle('prefs_confirm_update', s, 'confirmUpdate', save);
    bindSettingToggle('prefs_confirm_power', s, 'confirmPower', save);
    bindSettingToggle('prefs_reopen_popup', s, 'reopenLastPopup', save);
    bindSettingToggle('prefs_compact_dock', s, 'compactToolDock', save);
    bindSettingToggle('prefs_restore_last_view', s, 'restoreLastView', save);
    bindSettingToggle('prefs_keyboard_shortcuts', s, 'keyboardShortcuts', save);
    bindSettingToggle('prefs_show_tooltips', s, 'showTooltips', save);
    bindSettingInput('prefs_date_format', s, 'dateFormat', save, false);
    bindSettingInput('prefs_unit_preset', s, 'unitPreset', save, false);
  }

  function mountSettingsDevicesTabContent() {
    var body = document.getElementById('settings-tab-devs-body');
    if (!body) return;
    var s = getSectionSettings(DEVICES_SETTINGS_KEY, DEVICES_DEFAULTS);
    body.innerHTML =
      settingsHeaderHtml('settings-tab-devs', 'P4/C3') +
      '<div data-settings-anchor="p4">' +
      settingsSectionHtml('ESP32-P4',
      '<div class="settings-two-col">' +
      uiToggleRowHtml('dev_p4_auto', 'P4 bağlantısını otomatik toparla', s.p4AutoReconnect) +
      uiToggleRowHtml('dev_recovery_warn', 'Recovery/update uyarılarını göster', s.recoveryWarnings) +
      '</div>') + '</div>' +
      '<div data-settings-anchor="c3">' +
      settingsSectionHtml('ESP32-C3',
      '<div class="settings-two-col">' +
      uiToggleRowHtml('dev_c3_visible', 'C3 durum panelini göster', s.c3StatusVisible) +
      uiToggleRowHtml('dev_espnow_bridge', 'ESP-NOW köprü profilini tercih et', s.espnowPreferBridge) +
      '</div>') + '</div>' +
      '<div data-settings-anchor="tests">' +
      settingsSectionHtml('Bağlantı Testleri',
      '<div class="settings-two-col">' +
      uiToggleRowHtml('dev_passive_diag', 'Pasif bağlantı snapshot kartlarını göster', s.passiveDiagVisible) +
      selectRowHtml('dev_uart_shell_bridge', 'UART shell bridge', s.uartShellBridge, [['off', 'Off'], ['listen', 'Listen'], ['on', 'On']]) +
      '<div class="settings-card"><div class="kin3d-setting-label">Aktif test</div><div style="display:flex; flex-wrap:wrap; gap:6px; margin-top:8px;">' +
      settingsActionButtonHtml('dev_test_refresh', 'Yenile') +
      settingsActionButtonHtml('dev_test_all', 'All') +
      settingsActionButtonHtml('dev_test_p4', 'P4') +
      settingsActionButtonHtml('dev_test_c3', 'C3') +
      settingsActionButtonHtml('dev_test_wifi', 'WiFi') +
      settingsActionButtonHtml('dev_test_pca', 'PCA') +
      settingsActionButtonHtml('dev_test_web', 'WS') +
      '</div></div>' +
      '</div><div id="devices_status_cards" class="settings-card full muted" style="white-space:pre-wrap; min-height:72px;">Yükleniyor...</div>') + '</div>' +
      '<div data-settings-anchor="drivers">' +
      settingsSectionHtml('Sürücüler',
      '<div class="settings-card muted">PCA9685, servo sürüş ve SPI/UART sağlık eşikleri ilerleyen aşamada bu bölümde toplanacak.</div>') + '</div>';
    var save = function(next) { saveSectionSettings(DEVICES_SETTINGS_KEY, DEVICES_DEFAULTS, next); };
    bindSettingToggle('dev_p4_auto', s, 'p4AutoReconnect', save);
    bindSettingToggle('dev_c3_visible', s, 'c3StatusVisible', save);
    bindSettingToggle('dev_espnow_bridge', s, 'espnowPreferBridge', save);
    bindSettingToggle('dev_recovery_warn', s, 'recoveryWarnings', save);
    bindSettingToggle('dev_passive_diag', s, 'passiveDiagVisible', save);
    bindSettingInput('dev_uart_shell_bridge', s, 'uartShellBridge', save, false);
    var uartBridgeEl = document.getElementById('dev_uart_shell_bridge');
    if (uartBridgeEl) {
      uartBridgeEl.addEventListener('change', function() {
        fetch('/api/settings/uart-shell', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'mode=' + encodeURIComponent(uartBridgeEl.value || 'off')
        }).catch(function() {});
      });
    }
    var statusEl = document.getElementById('devices_status_cards');
    function renderDeviceStatus(data, testResult) {
      if (!statusEl) return;
      if (!data) {
        statusEl.textContent = 'Bağlantı durumu alınamadı.';
        return;
      }
      var lines = [
        'P4: ' + (data.p4_connected ? 'connected' : 'offline') + ' | tx=' + (data.p4_transactions || 0) + ' | crc=' + (data.p4_crc_errors || 0),
        'C3: ' + (data.c3_connected ? 'connected' : 'offline') + ' | rx=' + (data.c3_total_rx || 0) + ' | crc=' + (data.c3_crc_errors || 0),
        'WiFi: ' + (data.wifi_connected ? 'STA' : '-') + ' | AP=' + (data.wifi_ap_active ? 'on' : 'off') + ' | rssi=' + (data.wifi_rssi || 0),
        'PCA9685: ' + (data.pca_ready ? 'ready' : 'not-ready') + ' | ESP-NOW=' + (data.espnow_connected ? 'connected' : (data.espnow_active ? 'active' : 'off')) + ' | WS=' + (data.ws_clients || 0)
      ];
      if (testResult) lines.push('Son test: ' + (testResult.ok ? 'ok' : 'fail') + ' | ' + (testResult.duration_us || 0) + ' us | ' + (testResult.message || ''));
      statusEl.textContent = lines.join('\n');
    }
    function refreshDeviceStatus() {
      if (statusEl) statusEl.textContent = 'Yükleniyor...';
      fetch('/api/mshell/devices', { cache: 'no-store' })
        .then(function(r) { return r.ok ? r.json() : null; })
        .then(function(data) {
          if (data && data.bridge_mode && uartBridgeEl) {
            uartBridgeEl.value = data.bridge_mode;
            s.uartShellBridge = data.bridge_mode;
            save(s);
          }
        }).catch(function() {});
      fetch('/api/devices/status', { cache: 'no-store' })
        .then(function(r) { return r.ok ? r.json() : null; })
        .then(function(data) { renderDeviceStatus(data, null); })
        .catch(function() { renderDeviceStatus(null, null); });
    }
    function runDeviceTest(target) {
      if (statusEl) statusEl.textContent = 'Test çalışıyor...';
      fetch('/api/devices/test', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'target=' + encodeURIComponent(target)
      }).then(function(r) { return r.json().catch(function() { return null; }); })
        .then(function(result) {
          fetch('/api/devices/status', { cache: 'no-store' })
            .then(function(r) { return r.ok ? r.json() : null; })
            .then(function(data) { renderDeviceStatus(data, result); })
            .catch(function() { renderDeviceStatus(null, result); });
        }).catch(function() { renderDeviceStatus(null, null); });
    }
    bindActionButton('dev_test_refresh', refreshDeviceStatus);
    [['dev_test_all', 'all'], ['dev_test_p4', 'p4'], ['dev_test_c3', 'c3'], ['dev_test_wifi', 'wifi'], ['dev_test_pca', 'pca'], ['dev_test_web', 'web']].forEach(function(pair) {
      bindActionButton(pair[0], function() { runDeviceTest(pair[1]); });
    });
    refreshDeviceStatus();
  }

  function mountSettingsFilesTabContent() {
    var body = document.getElementById('settings-tab-files-body');
    if (!body) return;
    var s = getSectionSettings(FILES_SETTINGS_KEY, FILES_DEFAULTS);
    body.innerHTML = settingsHeaderHtml('settings-tab-files', s.defaultView) +
      '<div data-settings-anchor="view">' +
      settingsSectionHtml('Görünüm',
        '<div class="settings-two-col">' +
        selectRowHtml('files_view', 'Varsayılan görünüm', s.defaultView, [['grid', 'Grid'], ['list', 'Liste'], ['details', 'Ayrıntı']]) +
        selectRowHtml('files_sort', 'Varsayılan sıralama', s.defaultSort, [['name', 'Ad'], ['type', 'Tür'], ['size', 'Boyut'], ['mtime', 'Değiştirilme']]) +
        selectRowHtml('files_sort_dir', 'Sıralama yönü', s.defaultSortDir, [['asc', 'Artan'], ['desc', 'Azalan']]) +
        inputRowHtml('files_default_scale', 'Dosya yöneticisi ölçeği (%)', s.defaultScale, 'number') +
        uiToggleRowHtml('files_hidden', 'Gizli dosyaları göster', s.showHidden) +
        '</div>') + '</div>' +
      '<div data-settings-anchor="transfer">' +
      settingsSectionHtml('Transfer',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('files_multi', 'Çoklu seçime izin ver', s.multiSelect) +
        inputRowHtml('files_upload_limit', 'Upload limiti (KB)', s.uploadLimitKb, 'number') +
        '</div>') + '</div>' +
      '<div data-settings-anchor="editor">' +
      settingsSectionHtml('Editör',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('files_editor', 'Metin dosyalarını editörde aç', s.openTextEditor) +
        '</div>') + '</div>';
    var save = function(next) { saveSectionSettings(FILES_SETTINGS_KEY, FILES_DEFAULTS, next); };
    bindSettingInput('files_view', s, 'defaultView', function(next) {
      save(next);
      try { global.localStorage.setItem('mros.fileManager.view', next.defaultView === 'details' ? 'detail' : next.defaultView); } catch (err) {}
    }, false);
    bindSettingInput('files_sort', s, 'defaultSort', save, false);
    bindSettingInput('files_sort_dir', s, 'defaultSortDir', save, false);
    bindSettingInput('files_default_scale', s, 'defaultScale', function(next) {
      next.defaultScale = Math.max(75, Math.min(145, Number(next.defaultScale) || 100));
      save(next);
      try { global.localStorage.setItem('mros.fileManager.scale', String(next.defaultScale / 100)); } catch (err) {}
    }, true);
    bindSettingToggle('files_hidden', s, 'showHidden', save);
    bindSettingToggle('files_multi', s, 'multiSelect', save);
    bindSettingInput('files_upload_limit', s, 'uploadLimitKb', save, true);
    bindSettingToggle('files_editor', s, 'openTextEditor', save);
  }

  function mountSettingsRobotTabContent() {
    var body = document.getElementById('settings-tab-robot-body');
    if (!body) return;
    var s = getSectionSettings(ROBOT_SETTINGS_KEY, ROBOT_DEFAULTS);
    var math = (typeof global.ikGetMathState === 'function') ? global.ikGetMathState() : {};
    Object.assign(s, {
      solver: s.solver || math.solver || ROBOT_DEFAULTS.solver,
      jacobian: s.jacobian || math.jacobian || ROBOT_DEFAULTS.jacobian,
      nullspace: s.nullspace || math.nullspace || ROBOT_DEFAULTS.nullspace,
      seedPolicy: s.seedPolicy || math.seed_policy || ROBOT_DEFAULTS.seedPolicy,
      limitsProfile: s.limitsProfile || math.limits_profile || ROBOT_DEFAULTS.limitsProfile,
      frame: s.frame || math.frame || ROBOT_DEFAULTS.frame,
      units: s.units || math.units || ROBOT_DEFAULTS.units,
      posTolMm: s.posTolMm || math.pos_tol_mm || ROBOT_DEFAULTS.posTolMm,
      oriTolDeg: s.oriTolDeg || math.ori_tol_deg || ROBOT_DEFAULTS.oriTolDeg,
      singularityThreshold: s.singularityThreshold || math.singularity_threshold || ROBOT_DEFAULTS.singularityThreshold,
      alphaStep: s.alphaStep || math.alpha_step || ROBOT_DEFAULTS.alphaStep,
      nullGain: s.nullGain || math.null_gain || ROBOT_DEFAULTS.nullGain,
      lambdaMax: s.lambdaMax || math.lambda_max || ROBOT_DEFAULTS.lambdaMax,
      maxStepDeg: s.maxStepDeg || math.max_step_deg || ROBOT_DEFAULTS.maxStepDeg,
      maxIter: s.maxIter || math.max_iter || ROBOT_DEFAULTS.maxIter,
      pathHeightMode: s.pathHeightMode || math.path_height_mode || ROBOT_DEFAULTS.pathHeightMode,
      groundZMm: (s.groundZMm != null ? s.groundZMm : (math.ground_z_mm != null ? math.ground_z_mm : ROBOT_DEFAULTS.groundZMm)),
      turretMode: s.turretMode || math.turret_mode || ROBOT_DEFAULTS.turretMode,
      cartStepMm: s.cartStepMm || math.cart_step_mm || ROBOT_DEFAULTS.cartStepMm,
      yawStepDeg: s.yawStepDeg || math.yaw_step_deg || ROBOT_DEFAULTS.yawStepDeg,
      jumpRevoluteDeg: s.jumpRevoluteDeg || math.jump_revolute_deg || ROBOT_DEFAULTS.jumpRevoluteDeg
    });
    body.innerHTML = settingsHeaderHtml('settings-tab-robot', 'Kinematik') +
      '<div data-settings-anchor="math">' +
      settingsSectionHtml('Kinematik Hesaplama',
        '<div class="settings-two-col">' +
        selectRowHtml('robot_math_profile', 'Matematik profili', s.mathProfile, [['default', 'Default'], ['safe', 'Safe'], ['fast', 'Fast'], ['precision', 'Precision']]) +
        selectRowHtml('robot_math_backend', 'Hesaplama motoru', s.mathBackend, [['auto', 'Auto'], ['web', 'Web Tarayıcı'], ['onboard-s3', 'ESP32-S3 Local'], ['p4-spi', 'P4 SPI'], ['p4-esp-now', 'P4 ESP-NOW']]) +
        selectRowHtml('robot_traj', 'Trajectory modu', s.trajectoryMode, [['quintic', 'Quintic'], ['heptic', 'Heptic'], ['scurve', 'S-Curve / Jerk limited'], ['time-optimal', 'Time Optimal / Trapezoid'], ['linear', 'Linear']]) +
        '<div class="settings-card"><div class="kin3d-setting-label">Aktif hesaplama</div><div id="robot_math_active_mode" class="settings-status-line">--</div></div>' +
        '<div class="settings-card"><div class="kin3d-setting-label">Benchmark</div><div id="robot_math_benchmark_result" class="settings-status-line">Hazır.</div><div style="display:flex;gap:6px;flex-wrap:wrap;margin-top:8px;">' +
        settingsActionButtonHtml('robot_math_benchmark', 'Test Et') +
        '</div></div>' +
        '</div>') + '</div>' +
      '<div data-settings-anchor="onboard">' +
      settingsSectionHtml('ESP32-S3 Local Kinematik',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('robot_onboard_math', 'ESP32-S3 üzerinde yerel kinematik hesaplama', s.onboardMathEnabled || s.mathBackend === 'onboard-s3') +
        '<div class="settings-card"><div class="kin3d-setting-label">Local worker</div><div id="robot_onboard_state" class="settings-status-line">Yükleniyor...</div><div style="display:flex; gap:6px; flex-wrap:wrap; margin-top:8px;">' +
        settingsActionButtonHtml('robot_onboard_refresh', 'Yenile') +
        settingsActionButtonHtml('robot_onboard_clear', 'Clear') +
        settingsActionButtonHtml('robot_onboard_cancel', 'Cancel') +
        '</div></div>' +
        '</div><div class="settings-card full"><div class="kin3d-setting-label">Son 3 işlem</div><div id="robot_onboard_last3" style="margin-top:8px;">Henüz işlem yok.</div></div>') + '</div>' +
      '<div data-settings-anchor="advanced">' +
      settingsSectionHtml('Gelişmiş IK / FK',
        '<div class="settings-two-col">' +
        selectRowHtml('robot_ik_solver', 'Solver', s.solver, [['dls', 'Damped least squares'], ['qp', 'Joint-limit aware QP'], ['hybrid', 'Hybrid']]) +
        selectRowHtml('robot_ik_jacobian', 'Jacobian', s.jacobian, [['numerical', 'Numerical'], ['analytic', 'Analytic'], ['spatial', 'Spatial']]) +
        selectRowHtml('robot_ik_nullspace', 'Nullspace', s.nullspace, [['joint-center', 'Joint center'], ['limits', 'Limit avoidance'], ['off', 'Off']]) +
        selectRowHtml('robot_seed_policy', 'Seed policy', s.seedPolicy, [['current', 'Current'], ['nearest', 'Nearest'], ['last-good', 'Last good'], ['multi-start', 'Multi-start']]) +
        selectRowHtml('robot_limits_profile', 'Limit profili', s.limitsProfile, [['soft', 'Soft'], ['strict', 'Strict'], ['wide', 'Wide']]) +
        selectRowHtml('robot_frame', 'Frame', s.frame, [['base', 'Base'], ['tool', 'Tool'], ['world', 'World']]) +
        selectRowHtml('robot_units', 'Units', s.units, [['mm-deg', 'mm / derece'], ['m-rad', 'm / rad']]) +
        inputRowHtml('robot_pos_tol', 'Pozisyon toleransı (mm)', s.posTolMm, 'number') +
        inputRowHtml('robot_ori_tol', 'Ori toleransı (deg)', s.oriTolDeg, 'number') +
        inputRowHtml('robot_sigma', 'Singularity threshold', s.singularityThreshold, 'number') +
        inputRowHtml('robot_alpha_step', 'DLS alpha step', s.alphaStep, 'number') +
        inputRowHtml('robot_null_gain', 'Null gain', s.nullGain, 'number') +
        inputRowHtml('robot_lambda_max', 'Lambda max', s.lambdaMax, 'number') +
        inputRowHtml('robot_max_step', 'Max step (deg)', s.maxStepDeg, 'number') +
        inputRowHtml('robot_max_iter', 'Max iter', s.maxIter, 'number') +
        selectRowHtml('robot_path_height', 'Path height', s.pathHeightMode, [['auto', 'Auto'], ['fixed', 'Fixed'], ['off', 'Off']]) +
        inputRowHtml('robot_ground_z', 'Ground Z (mm)', s.groundZMm, 'number') +
        selectRowHtml('robot_turret_mode', 'Turret mode', s.turretMode, [['nearest', 'Nearest'], ['shortest', 'Shortest'], ['continuous', 'Continuous']]) +
        inputRowHtml('robot_cart_step', 'Cart step (mm)', s.cartStepMm, 'number') +
        inputRowHtml('robot_yaw_step', 'Yaw step (deg)', s.yawStepDeg, 'number') +
        inputRowHtml('robot_jump_deg', 'Jump guard (deg)', s.jumpRevoluteDeg, 'number') +
        '</div>') + '</div>' +
      '<div data-settings-anchor="motion">' +
      settingsSectionHtml('Hareket',
        '<div class="settings-two-col">' +
        inputRowHtml('robot_cart_speed', 'Kartesyen hız (mm/s)', s.cartesianSpeedMmS, 'number') +
        uiToggleRowHtml('robot_preview_required', 'Apply öncesi preview zorunlu', s.previewRequired) +
        '</div>') + '</div>' +
      '<div data-settings-anchor="safety">' +
      settingsSectionHtml('Güvenlik',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('robot_singularity', 'Singularity uyarılarını göster', s.singularityWarnings) +
        '</div>') + '</div>';
    var save = function(next) { saveSectionSettings(ROBOT_SETTINGS_KEY, ROBOT_DEFAULTS, next); };
    bindSettingInput('robot_math_profile', s, 'mathProfile', save, false);
    bindSettingInput('robot_math_backend', s, 'mathBackend', save, false);
    bindSettingInput('robot_traj', s, 'trajectoryMode', save, false);
    bindSettingInput('robot_cart_speed', s, 'cartesianSpeedMmS', save, true);
    bindSettingToggle('robot_preview_required', s, 'previewRequired', save);
    bindSettingToggle('robot_singularity', s, 'singularityWarnings', save);
    function robotSyncMathMode() {
      var el = document.getElementById('robot_math_active_mode');
      if (el) {
        var mode = (typeof global.ikGetComputationMode === 'function') ? global.ikGetComputationMode() : String(s.mathBackend || 'auto').toUpperCase();
        el.textContent = mode === 'ONBOARD-S3' ? 'ESP32-S3 LOCAL' : mode;
      }
    }
    function runRobotMathBenchmark() {
      var out = document.getElementById('robot_math_benchmark_result');
      var backend = (document.getElementById('robot_math_backend') || {}).value || s.mathBackend || 'auto';
      if (out) out.textContent = backend + ' benchmark çalışıyor...';
      var started = (global.performance && performance.now) ? performance.now() : Date.now();
      if (backend === 'web' && typeof global.solveIK === 'function') {
        try {
          global.solveIK({ x: 250, y: 0, z: 180 });
        } catch (err) {}
        var done = (global.performance && performance.now) ? performance.now() : Date.now();
        if (out) out.textContent = 'Web çözücü: ' + (done - started).toFixed(2) + ' ms';
        return;
      }
      fetch('/api/robot/math/onboard/run', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'op=ik&x=250&y=0&z=180'
      }).then(function(r) {
        return r.json().catch(function() { return { ok: r.ok, error: 'json_parse' }; });
      }).then(function(j) {
        var done = (global.performance && performance.now) ? performance.now() : Date.now();
        var ms = j && j.duration_ms != null ? Number(j.duration_ms) : (done - started);
        if (out) out.textContent = (j && j.ok ? 'Başarılı' : 'Tamamlanamadı') + ' | backend=' + backend + ' | ' + ms.toFixed(2) + ' ms';
      }).catch(function(err) {
        if (out) out.textContent = 'Benchmark hatası: ' + err.message;
      });
    }
    function saveAdvancedMath() {
      var next = {
        solver: (document.getElementById('robot_ik_solver') || {}).value || s.solver,
        jacobian: (document.getElementById('robot_ik_jacobian') || {}).value || s.jacobian,
        nullspace: (document.getElementById('robot_ik_nullspace') || {}).value || s.nullspace,
        seed_policy: (document.getElementById('robot_seed_policy') || {}).value || s.seedPolicy,
        limits_profile: (document.getElementById('robot_limits_profile') || {}).value || s.limitsProfile,
        frame: (document.getElementById('robot_frame') || {}).value || s.frame,
        units: (document.getElementById('robot_units') || {}).value || s.units,
        pos_tol_mm: Number((document.getElementById('robot_pos_tol') || {}).value) || s.posTolMm,
        ori_tol_deg: Number((document.getElementById('robot_ori_tol') || {}).value) || s.oriTolDeg,
        singularity_threshold: Number((document.getElementById('robot_sigma') || {}).value) || s.singularityThreshold,
        alpha_step: Number((document.getElementById('robot_alpha_step') || {}).value) || s.alphaStep,
        null_gain: Number((document.getElementById('robot_null_gain') || {}).value),
        lambda_max: Number((document.getElementById('robot_lambda_max') || {}).value) || s.lambdaMax,
        max_step_deg: Number((document.getElementById('robot_max_step') || {}).value) || s.maxStepDeg,
        max_iter: Number((document.getElementById('robot_max_iter') || {}).value) || s.maxIter,
        path_height_mode: (document.getElementById('robot_path_height') || {}).value || s.pathHeightMode,
        ground_z_mm: Number((document.getElementById('robot_ground_z') || {}).value),
        turret_mode: (document.getElementById('robot_turret_mode') || {}).value || s.turretMode,
        cart_step_mm: Number((document.getElementById('robot_cart_step') || {}).value) || s.cartStepMm,
        yaw_step_deg: Number((document.getElementById('robot_yaw_step') || {}).value) || s.yawStepDeg,
        jump_revolute_deg: Number((document.getElementById('robot_jump_deg') || {}).value) || s.jumpRevoluteDeg
      };
      if (!isFinite(next.null_gain)) next.null_gain = s.nullGain;
      if (!isFinite(next.ground_z_mm)) next.ground_z_mm = s.groundZMm;
      s.solver = next.solver;
      s.jacobian = next.jacobian;
      s.nullspace = next.nullspace;
      s.seedPolicy = next.seed_policy;
      s.limitsProfile = next.limits_profile;
      s.frame = next.frame;
      s.units = next.units;
      s.posTolMm = next.pos_tol_mm;
      s.oriTolDeg = next.ori_tol_deg;
      s.singularityThreshold = next.singularity_threshold;
      s.alphaStep = next.alpha_step;
      s.nullGain = next.null_gain;
      s.lambdaMax = next.lambda_max;
      s.maxStepDeg = next.max_step_deg;
      s.maxIter = next.max_iter;
      s.pathHeightMode = next.path_height_mode;
      s.groundZMm = next.ground_z_mm;
      s.turretMode = next.turret_mode;
      s.cartStepMm = next.cart_step_mm;
      s.yawStepDeg = next.yaw_step_deg;
      s.jumpRevoluteDeg = next.jump_revolute_deg;
      save(s);
      if (typeof global.ikSetMathState === 'function') global.ikSetMathState(next, true);
    }
    ['robot_ik_solver', 'robot_ik_jacobian', 'robot_ik_nullspace', 'robot_seed_policy',
     'robot_limits_profile', 'robot_frame', 'robot_units', 'robot_pos_tol', 'robot_ori_tol',
     'robot_sigma', 'robot_alpha_step', 'robot_null_gain', 'robot_lambda_max', 'robot_max_step',
     'robot_max_iter', 'robot_path_height', 'robot_ground_z', 'robot_turret_mode',
     'robot_cart_step', 'robot_yaw_step', 'robot_jump_deg'].forEach(function(id) {
      var el = document.getElementById(id);
      if (el) el.addEventListener('change', saveAdvancedMath);
    });
    var backendEl = document.getElementById('robot_math_backend');
    if (backendEl) {
      backendEl.addEventListener('change', function() {
        var mode = backendEl.value || 'auto';
        s.mathBackend = mode;
        if (mode === 'onboard-s3') s.onboardMathEnabled = true;
        save(s);
        if (typeof global.ikSetComputationPreference === 'function') global.ikSetComputationPreference(mode);
        robotSyncMathMode();
        if (mode === 'onboard-s3') postOnboardAction('enable');
        fetch('/api/robot/math/onboard', {
          method: 'POST',
          headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
          body: 'action=backend&mode=' + encodeURIComponent(mode)
        }).catch(function() {});
      });
    }
    var onboardEl = document.getElementById('robot_onboard_math');
    var stateEl = document.getElementById('robot_onboard_state');
    var last3El = document.getElementById('robot_onboard_last3');
    function renderOnboardState(data) {
      if (!data) {
        if (stateEl) stateEl.textContent = 'Durum alınamadı.';
        return;
      }
      if (onboardEl) onboardEl.checked = !!data.enabled;
      s.onboardMathEnabled = !!data.enabled;
      save(s);
      if (stateEl) {
        stateEl.textContent = (data.enabled ? 'enabled' : 'disabled') +
          ' | ' + (data.busy ? 'busy' : 'sleeping') +
          ' | wake=' + (data.wake_count || 0) +
          ' | rejected=' + ((data.rejected_disabled || 0) + (data.rejected_busy || 0));
        robotSyncMathMode();
      }
      if (last3El) last3El.innerHTML = settingsLast3Html(data.last3);
    }
    function refreshOnboardState() {
      fetch('/api/robot/math/onboard', { cache: 'no-store' })
        .then(function(r) { return r.ok ? r.json() : null; })
        .then(renderOnboardState)
        .catch(function() { renderOnboardState(null); });
    }
    function postOnboardAction(action) {
      fetch('/api/robot/math/onboard', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'action=' + encodeURIComponent(action)
      }).then(function(r) { return r.ok ? r.json() : null; })
        .then(renderOnboardState)
        .catch(function() { renderOnboardState(null); });
    }
    if (onboardEl) {
      onboardEl.addEventListener('change', function() {
        s.onboardMathEnabled = !!onboardEl.checked;
        if (s.onboardMathEnabled) {
          s.mathBackend = 'onboard-s3';
          if (backendEl) backendEl.value = 'onboard-s3';
          if (typeof global.ikSetComputationPreference === 'function') global.ikSetComputationPreference('onboard-s3');
        }
        save(s);
        postOnboardAction(onboardEl.checked ? 'enable' : 'disable');
        robotSyncMathMode();
      });
    }
    bindActionButton('robot_onboard_refresh', refreshOnboardState);
    bindActionButton('robot_onboard_clear', function() { postOnboardAction('clear'); });
    bindActionButton('robot_onboard_cancel', function() { postOnboardAction('cancel'); });
    bindActionButton('robot_math_benchmark', runRobotMathBenchmark);
    refreshOnboardState();
    robotSyncMathMode();
  }

  function mountSettingsUpdateTabContent() {
    var body = document.getElementById('settings-tab-update-body');
    if (!body) return;
    var s = getSectionSettings(UPDATE_SETTINGS_KEY, UPDATE_DEFAULTS);
    body.innerHTML = settingsHeaderHtml('settings-tab-update', 'Recovery') +
      '<div data-settings-anchor="firmware">' +
      settingsSectionHtml('Firmware',
        '<div class="settings-two-col">' +
        inputRowHtml('upd_path', 'Firmware klasörü', s.firmwarePath, 'text') +
        '</div>') + '</div>' +
      '<div data-settings-anchor="recovery">' +
      settingsSectionHtml('Recovery',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('upd_recovery', 'Recovery durumunu otomatik kontrol et', s.autoCheckRecovery) +
        uiToggleRowHtml('upd_rollback', 'Rollback guard aktif olsun', s.rollbackGuard) +
        uiToggleRowHtml('upd_recovery_reads_cfg', 'Recovery ortak NVS ayarlarını okusun', s.recoveryReadsDeviceSettings) +
        '<div class="settings-card muted">Recovery, <code>nvs_sys_usr/web_cfg/device_v1</code> içindeki firmware yolu, rollback ve zamanlama anahtarlarını tanı olarak okur.</div>' +
        '</div>') + '</div>' +
      '<div data-settings-anchor="guard">' +
      settingsSectionHtml('Koruma',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('upd_battery', 'Güç güvenli değilse update onayı iste', s.requireBatterySafe) +
        '</div>') + '</div>' +
      '<div data-settings-anchor="schedule">' +
      settingsSectionHtml('OTA ve Yeniden Başlatma',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('upd_auto_ota_scan', 'Otomatik OTA taraması açık', s.autoOtaScan) +
        inputRowHtml('upd_ota_hour', 'OTA tarama saati', s.otaScanHour, 'time') +
        uiToggleRowHtml('upd_scheduled_reboot', 'Planlı yeniden başlatma açık', s.scheduledReboot) +
        inputRowHtml('upd_reboot_hour', 'Yeniden başlatma saati', s.scheduledRebootHour, 'time') +
        selectRowHtml('upd_window', 'Update penceresi', s.updateWindow, [['manual', 'Manuel'], ['night', 'Gece'], ['maintenance', 'Bakım penceresi']]) +
        '</div>') + '</div>' +
      '<div data-settings-anchor="errors">' +
      settingsSectionHtml('Hata Kodları ve Tanılar',
        '<div class="settings-card full"><div class="settings-prop-row"><span>OK</span><code>İşlem sağlıklı.</code></div>' +
        '<div class="settings-prop-row"><span>DOWNLOAD_BUSY</span><code>Aktif firmware/dosya indirme işlemi var.</code></div>' +
        '<div class="settings-prop-row"><span>TARGET_BUSY</span><code>Aynı hedef dosya yazılıyor.</code></div>' +
        '<div class="settings-prop-row"><span>RECOVERY_LAYOUT</span><code>Recovery/app0 partition doğrulaması geçmedi.</code></div>' +
        '<div class="settings-prop-row"><span>POWER_UNSAFE</span><code>Güç, motion veya update lockout güvenli değil.</code></div>' +
        '<div class="settings-status-line">Son kod: ' + settingsEsc(s.lastErrorCode || 'OK') + '</div></div>') + '</div>';
    var save = function(next) { saveSectionSettings(UPDATE_SETTINGS_KEY, UPDATE_DEFAULTS, next); };
    bindSettingInput('upd_path', s, 'firmwarePath', save, false);
    bindSettingToggle('upd_recovery', s, 'autoCheckRecovery', save);
    bindSettingToggle('upd_rollback', s, 'rollbackGuard', save);
    bindSettingToggle('upd_recovery_reads_cfg', s, 'recoveryReadsDeviceSettings', save);
    bindSettingToggle('upd_battery', s, 'requireBatterySafe', save);
    bindSettingToggle('upd_auto_ota_scan', s, 'autoOtaScan', save);
    bindSettingInput('upd_ota_hour', s, 'otaScanHour', save, false);
    bindSettingToggle('upd_scheduled_reboot', s, 'scheduledReboot', save);
    bindSettingInput('upd_reboot_hour', s, 'scheduledRebootHour', save, false);
    bindSettingInput('upd_window', s, 'updateWindow', save, false);
  }

  function mountSettingsStorageTabContent() {
    var body = document.getElementById('settings-tab-storage-body');
    if (!body) return;
    var s = getSectionSettings(STORAGE_SETTINGS_KEY, STORAGE_DEFAULTS);
    body.innerHTML = settingsHeaderHtml('settings-tab-storage', '/ESPUSER') +
      '<div data-settings-anchor="littlefs">' +
      settingsSectionHtml('LittleFS',
        '<div class="settings-two-col">' +
        inputRowHtml('st_warn_free', 'Boş alan uyarı eşiği (KB)', s.warnFreeKb, 'number') +
        uiToggleRowHtml('st_cache_assets', 'Statik asset cache aktif', s.cacheStaticAssets) +
        '</div>') + '</div>' +
      '<div data-settings-anchor="usage">' +
      settingsSectionHtml('Kullanım Oranları',
        '<div id="storage_usage_grid" class="settings-health-grid">' +
        '<div class="settings-card muted">Depolama snapshot bekleniyor...</div>' +
        '</div><div id="storage_mount_status" class="settings-status-line"></div>') + '</div>' +
      '<div data-settings-anchor="logs">' +
      settingsSectionHtml('Loglar',
        '<div class="settings-two-col">' +
        inputRowHtml('st_log_tail', 'Log tail byte', s.logTailBytes, 'number') +
        inputRowHtml('st_keep_days', 'Log saklama günü', s.keepLogDays, 'number') +
        '</div>') + '</div>' +
      '<div data-settings-anchor="cache">' +
      settingsSectionHtml('Cache',
        '<div class="settings-card muted">Asset manifest, gzip ve tail endpoint ayarları ilerleyen aşamada buradan tek noktadan yönetilecek.</div>') + '</div>';
    var save = function(next) { saveSectionSettings(STORAGE_SETTINGS_KEY, STORAGE_DEFAULTS, next); };
    bindSettingInput('st_warn_free', s, 'warnFreeKb', save, true);
    bindSettingToggle('st_cache_assets', s, 'cacheStaticAssets', save);
    bindSettingInput('st_log_tail', s, 'logTailBytes', save, true);
    bindSettingInput('st_keep_days', s, 'keepLogDays', save, true);
    function pct(used, total) {
      used = Number(used || 0);
      total = Number(total || 0);
      return total > 0 ? Math.max(0, Math.min(100, used * 100 / total)) : 0;
    }
    function bytes(v) {
      return (root.utils && root.utils.format && root.utils.format.bytesHuman) ? root.utils.format.bytesHuman(v || 0) : String(v || 0);
    }
    function storageMeter(label, used, total, accent, note) {
      var p = pct(used, total);
      return '<div class="settings-health-card" style="--meter-accent:' + settingsEsc(accent) + ';">' +
        '<div class="settings-health-card-head"><span>' + settingsEsc(label) + '</span><strong>' + p.toFixed(0) + '%</strong></div>' +
        '<div class="settings-health-meter"><i style="width:' + p.toFixed(1) + '%"></i></div>' +
        '<div class="settings-health-note">' + settingsEsc(bytes(used)) + ' / ' + settingsEsc(bytes(total)) + (note ? ' · ' + settingsEsc(note) : '') + '</div></div>';
    }
    function refreshStorageUsage() {
      var grid = document.getElementById('storage_usage_grid');
      var mountStatus = document.getElementById('storage_mount_status');
      Promise.all([
        fetch('/api/debug/sysinfo', { cache: 'no-store' }).then(function(r) { return r && r.ok ? r.json() : null; }).catch(function() { return null; }),
        fetch('/api/files/mounts', { cache: 'no-store' }).then(function(r) { return r && r.ok ? r.json() : null; }).catch(function() { return null; })
      ]).then(function(pair) {
        var sys = pair[0] || {};
        if (grid) {
          grid.innerHTML =
            storageMeter('Recovery', sys.recovery_used || 0, sys.recovery_total || 0, '#EA7B7B', 'recovery slot') +
            storageMeter('App/System', sys.app_used || 0, sys.app_total || 0, '#EAC27C', 'app0') +
            storageMeter('LittleFS', sys.fs_used || 0, sys.fs_total || 0, '#B98BEA', 'asset + /ESPUSER') +
            storageMeter('/ESPUSER', sys.fs_used || 0, sys.fs_total || 0, '#9BEB5D', 'kullanıcı alanı') +
            storageMeter('PSRAM', Math.max(0, Number(sys.psram_total || 0) - Number(sys.psram_free || 0)), sys.psram_total || 0, '#7EA8F2', 'runtime buffers');
        }
        if (mountStatus) {
          var mounts = pair[1] && Array.isArray(pair[1].mounts) ? pair[1].mounts : [];
          mountStatus.textContent = mounts.length ? mounts.map(function(m) {
            return String(m.name || m.provider || '-') + ':' + (m.mounted ? 'mounted' : 'offline');
          }).join(' | ') : 'Remote mount snapshot yok.';
        }
      });
    }
    refreshStorageUsage();
  }

  function mountSettingsAiTabContent() {
    var body = document.getElementById('settings-tab-ai-body');
    if (!body) return;
    var s = getSectionSettings(AI_SETTINGS_KEY, AI_DEFAULTS);
    body.innerHTML = settingsHeaderHtml('settings-tab-ai', s.mode) +
      '<div data-settings-anchor="mode">' +
      settingsSectionHtml('Mod',
        '<div class="settings-two-col">' +
        selectRowHtml('ai_mode', 'AI çalışma modu', s.mode, [['proposal', 'Sadece öneri'], ['approval', 'Onaylı işlem'], ['supervised', 'Gözetimli otomasyon']]) +
        uiToggleRowHtml('ai_plan', 'AI plan panelini göster', s.showPlanPanel) +
        '</div>') + '</div>' +
      '<div data-settings-anchor="approval">' +
      settingsSectionHtml('Onay',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('ai_approval', 'Her işlem için kullanıcı onayı iste', s.requireApproval) +
        '</div>') + '</div>' +
      '<div data-settings-anchor="tools">' +
      settingsSectionHtml('Araçlar',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('ai_shell_tools', 'AI shell araçlarını kullanabilsin', s.allowShellTools) +
        '</div>') + '</div>';
    var save = function(next) { saveSectionSettings(AI_SETTINGS_KEY, AI_DEFAULTS, next); };
    bindSettingInput('ai_mode', s, 'mode', save, false);
    bindSettingToggle('ai_plan', s, 'showPlanPanel', save);
    bindSettingToggle('ai_approval', s, 'requireApproval', save);
    bindSettingToggle('ai_shell_tools', s, 'allowShellTools', save);
  }

  function mountSettingsDevtoolsTabContent() {
    var body = document.getElementById('settings-tab-devtools-body');
    if (!body) return;
    var s = getSectionSettings(DEVTOOLS_SETTINGS_KEY, DEVTOOLS_DEFAULTS);
    body.innerHTML = settingsHeaderHtml('settings-tab-devtools', 'Debug') +
      '<div data-settings-anchor="debug">' +
      settingsSectionHtml('Debug',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('dev_debug_ep', 'Debug endpointleri görünür olsun', s.debugEndpoints) +
        uiToggleRowHtml('dev_ws_inspect', 'WebSocket inspect modu', s.websocketInspect) +
        '</div>') + '</div>' +
      '<div data-settings-anchor="api">' +
      settingsSectionHtml('API',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('dev_json_export', 'Ham JSON export/import göster', s.rawJsonExport) +
        '</div>') + '</div>' +
      '<div data-settings-anchor="flags">' +
      settingsSectionHtml('Bayraklar',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('dev_flags', 'Deneysel ayarları göster', s.experimentalFlags) +
        '<div class="settings-card"><div class="kin3d-setting-label">Onboard math</div><div id="dev_onboard_exp_status" class="settings-status-line">Yükleniyor...</div></div>' +
        '</div>') + '</div>' +
      '<div data-settings-anchor="performance-flags">' +
      settingsSectionHtml('Performans Kararları',
        '<div class="settings-two-col">' +
        uiToggleRowHtml('dev_binary_telemetry', 'Binary telemetry bin-v1 kullanılsın', s.binaryTelemetry) +
        uiToggleRowHtml('dev_shell_binary', 'Shell binary stream shell-bin-v1 kullanılsın', s.shellBinary) +
        uiToggleRowHtml('dev_cbor_control', 'Typed control RPC / CBOR deneyi görünür olsun', s.cborControl) +
        uiToggleRowHtml('dev_field_gating', 'Telemetry field gating aktif', s.telemetryFieldGating) +
        uiToggleRowHtml('dev_native_http', 'Native HTTP pilot metrikleri görünür olsun', s.nativeHttpPilotVisible) +
        uiToggleRowHtml('dev_verbose_logs', 'Verbose debug logları aç', s.verboseLogs) +
        uiToggleRowHtml('dev_perf_overlay', 'Performans overlay göster', s.perfOverlay) +
        '</div>') + '</div>';
    var save = function(next) { saveSectionSettings(DEVTOOLS_SETTINGS_KEY, DEVTOOLS_DEFAULTS, next); };
    bindSettingToggle('dev_debug_ep', s, 'debugEndpoints', save);
    bindSettingToggle('dev_ws_inspect', s, 'websocketInspect', save);
    bindSettingToggle('dev_json_export', s, 'rawJsonExport', save);
    bindSettingToggle('dev_flags', s, 'experimentalFlags', save);
    bindSettingToggle('dev_binary_telemetry', s, 'binaryTelemetry', save);
    bindSettingToggle('dev_shell_binary', s, 'shellBinary', save);
    bindSettingToggle('dev_cbor_control', s, 'cborControl', save);
    bindSettingToggle('dev_field_gating', s, 'telemetryFieldGating', save);
    bindSettingToggle('dev_native_http', s, 'nativeHttpPilotVisible', save);
    bindSettingToggle('dev_verbose_logs', s, 'verboseLogs', save);
    bindSettingToggle('dev_perf_overlay', s, 'perfOverlay', save);
    fetch('/api/robot/math/onboard', { cache: 'no-store' })
      .then(function(r) { return r.ok ? r.json() : null; })
      .then(function(data) {
        var el = document.getElementById('dev_onboard_exp_status');
        if (!el) return;
        el.textContent = data ? ((data.enabled ? 'enabled' : 'disabled') + ' | ' + (data.busy ? 'busy' : 'sleeping') + ' | backend=onboard-s3') : 'Durum alınamadı.';
      }).catch(function() {
        var el = document.getElementById('dev_onboard_exp_status');
        if (el) el.textContent = 'Durum alınamadı.';
      });
  }

  function serviceUpdate(service, fields, done) {
    var body = 'service=' + encodeURIComponent(service);
    Object.keys(fields || {}).forEach(function(key) {
      body += '&' + encodeURIComponent(key) + '=' + encodeURIComponent(fields[key] ? '1' : '0');
    });
    fetch('/api/services/update', {
      method: 'POST',
      headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
      body: body
    }).then(function(r) { return r.ok ? r.json() : null; })
      .then(function() { if (typeof done === 'function') done(); })
      .catch(function() { if (typeof done === 'function') done(); });
  }

  function mountSettingsServicesTabContent() {
    var body = document.getElementById('settings-tab-services-body');
    if (!body) return;
    var local = getSectionSettings(SERVICES_SETTINGS_KEY, SERVICES_DEFAULTS);
    body.innerHTML =
      settingsHeaderHtml('settings-tab-services', 'Uyku/notify') +
      '<div data-settings-anchor="core">' +
      settingsSectionHtml('Çekirdek Servisler',
      '<div class="settings-card muted">Varsayılan kapalı çalışan servisler burada açılır. Kapalı servis taskları bildirim gelene kadar uykuda kalır.</div>' +
      '<div class="settings-two-col">' +
      uiToggleRowHtml('svc_ssh_enabled', 'SSH servisini etkinleştir', local.sshEnabled) +
      '</div>') + '</div>' +
      '<div data-settings-anchor="mcp">' +
      settingsSectionHtml('MCP',
      '<div class="settings-two-col">' +
      uiToggleRowHtml('svc_mcp_enabled', 'MCP AI soketini etkinleştir', local.mcpEnabled) +
      uiToggleRowHtml('svc_mcp_shell', 'MCP üzerinden shell komutlarına izin ver', local.mcpAllowShell) +
      '</div>') + '</div>' +
      '<div data-settings-anchor="status">' +
      settingsSectionHtml('Durum',
      '<div class="settings-two-col">' +
      '<div class="settings-card"><div class="kin3d-setting-label">Servis durumu</div><div id="svc_status_text" style="font-size:11px; color:#aeb9c2;">Yükleniyor...</div></div>' +
      '</div>') + '</div>';

    function saveLocal() {
      saveSectionSettings(SERVICES_SETTINGS_KEY, SERVICES_DEFAULTS, local);
    }
    function loadLive() {
      fetch('/api/services/state', { cache: 'no-store' })
        .then(function(r) { return r.ok ? r.json() : null; })
        .then(function(data) {
          if (!data || !data.success) return;
          local.sshEnabled = !!(data.ssh && data.ssh.enabled);
          local.mcpEnabled = !!(data.mcp && data.mcp.enabled);
          local.mcpAllowShell = !!(data.mcp && data.mcp.allow_shell);
          saveLocal();
          var sshEl = document.getElementById('svc_ssh_enabled');
          var mcpEl = document.getElementById('svc_mcp_enabled');
          var shellEl = document.getElementById('svc_mcp_shell');
          if (sshEl) sshEl.checked = local.sshEnabled;
          if (mcpEl) mcpEl.checked = local.mcpEnabled;
          if (shellEl) shellEl.checked = local.mcpAllowShell;
          var text = document.getElementById('svc_status_text');
          if (text) {
            text.textContent = 'SSH: ' + (local.sshEnabled ? 'açık' : 'kapalı') +
              ' / MCP: ' + (local.mcpEnabled ? 'açık' : 'kapalı') +
              ' / Shell yetkisi: ' + (local.mcpAllowShell ? 'var' : 'kapalı');
          }
        }).catch(function() {
          var text = document.getElementById('svc_status_text');
          if (text) text.textContent = 'Servis durumu alınamadı.';
        });
    }

    var sshEl = document.getElementById('svc_ssh_enabled');
    if (sshEl) sshEl.addEventListener('change', function() {
      local.sshEnabled = !!sshEl.checked;
      saveLocal();
      serviceUpdate('ssh', { enabled: local.sshEnabled }, loadLive);
    });
    var mcpEl = document.getElementById('svc_mcp_enabled');
    if (mcpEl) mcpEl.addEventListener('change', function() {
      local.mcpEnabled = !!mcpEl.checked;
      saveLocal();
      serviceUpdate('mcp', { enabled: local.mcpEnabled, allow_shell: local.mcpAllowShell }, loadLive);
    });
    var shellEl = document.getElementById('svc_mcp_shell');
    if (shellEl) shellEl.addEventListener('change', function() {
      local.mcpAllowShell = !!shellEl.checked;
      saveLocal();
      serviceUpdate('mcp', { enabled: local.mcpEnabled, allow_shell: local.mcpAllowShell }, loadLive);
    });
    loadLive();
  }

  function parseNumFromEl(id) {
    var el = document.getElementById(id);
    if (!el) return NaN;
    var m = String(el.textContent || '').replace(',', '.').match(/-?\d+(\.\d+)?/);
    return m ? Number(m[0]) : NaN;
  }

  function buildSettingsDebugBarCard(label, key, initial) {
    return '<div class="settings-debug-card"><div class="settings-debug-head"><span class="settings-debug-label">' + label + '</span><span id="' + key + '-val" class="settings-debug-value">' + initial + '</span></div><div class="settings-debug-bar"><div id="' + key + '-bar" class="settings-debug-bar-fill"></div></div></div>';
  }

  function setSettingsDebugBar(key, ratioPct) {
    var bar = document.getElementById(key + '-bar');
    if (!bar) return;
    var ratio = Math.max(0, Math.min(100, Number(ratioPct) || 0));
    bar.style.width = ratio.toFixed(1) + '%';
  }

  function setTextIf(id, txt) {
    var el = document.getElementById(id);
    if (el) el.textContent = txt;
  }

  function updateSettingsAboutDebugCards(sys) {
    var loopMs = parseNumFromEl('loop_ms');
    var spiCrc = parseNumFromEl('spi_crc_err');
    var uartCrc = parseNumFromEl('uart_crc_err');
    var quality = parseNumFromEl('c3_quality');
    var cpuLoad = (sys && isFinite(Number(sys.cpu_load_pct))) ? Number(sys.cpu_load_pct) : NaN;
    var actualCpu = (sys && isFinite(Number(sys.actual_cpu_mhz))) ? Number(sys.actual_cpu_mhz) : NaN;
    var tempC = (sys && isFinite(Number(sys.temperature_c))) ? Number(sys.temperature_c) : NaN;
    var ikMs = (sys && isFinite(Number(sys.ik_last_ms))) ? Number(sys.ik_last_ms) : NaN;
    var fkMs = (sys && isFinite(Number(sys.fk_last_ms))) ? Number(sys.fk_last_ms) : NaN;
    var total = (isFinite(ikMs) ? ikMs : 0) + (isFinite(fkMs) ? fkMs : 0);
    setTextIf('setdbg-loop-val', isFinite(loopMs) ? loopMs.toFixed(1) + ' ms' : '-- ms');
    setTextIf('setdbg-cpu-val', isFinite(cpuLoad) ? cpuLoad.toFixed(1) + ' %' : '-- %');
    setTextIf('setdbg-power-val', (sys && sys.pm_mode ? String(sys.pm_mode) : '--') + (isFinite(actualCpu) ? ' / ' + actualCpu + ' MHz' : ''));
    setTextIf('setdbg-temp-val', isFinite(tempC) ? tempC.toFixed(1) + ' C' : '-- C');
    setTextIf('setdbg-quality-val', isFinite(quality) ? quality.toFixed(2) + ' %' : '-- %');
    setTextIf('setdbg-spicrc-val', isFinite(spiCrc) ? String(Math.round(spiCrc)) : '--');
    setTextIf('setdbg-uartcrc-val', isFinite(uartCrc) ? String(Math.round(uartCrc)) : '--');
    setTextIf('setdbg-ikfk-val', isFinite(total) ? total.toFixed(1) + ' ms' : '-- ms');
    if (sys && sys.uptime_s != null) setTextIf('settings-about-uptime', String(sys.uptime_s) + ' sn');
    setSettingsDebugBar('setdbg-loop', isFinite(loopMs) ? (loopMs / 50) * 100 : 0);
    setSettingsDebugBar('setdbg-cpu', isFinite(cpuLoad) ? cpuLoad : 0);
    setSettingsDebugBar('setdbg-power', isFinite(actualCpu) ? (actualCpu / 240) * 100 : 0);
    setSettingsDebugBar('setdbg-temp', isFinite(tempC) ? Math.max(0, Math.min(100, (tempC / 90) * 100)) : 0);
    setSettingsDebugBar('setdbg-quality', isFinite(quality) ? quality : 0);
    setSettingsDebugBar('setdbg-spicrc', isFinite(spiCrc) ? Math.min(spiCrc * 10, 100) : 0);
    setSettingsDebugBar('setdbg-uartcrc', isFinite(uartCrc) ? Math.min(uartCrc * 10, 100) : 0);
    setSettingsDebugBar('setdbg-ikfk', isFinite(total) ? (total / 40) * 100 : 0);
  }

  function renderSettingsAbout(data, sys) {
    var body = document.getElementById('settings-about-body');
    if (!body) return;
    if (!data || typeof data !== 'object') {
      body.className = 'settings-empty';
      body.textContent = 'Hakkında bilgisi alınamadı.';
      return;
    }
    var fmt = (root.utils && root.utils.format && root.utils.format.bytesHuman)
      ? root.utils.format.bytesHuman
      : function(v) { return String(v); };
    function pct(used, total) {
      used = Number(used || 0);
      total = Number(total || 0);
      return total > 0 ? Math.max(0, Math.min(100, (used * 100 / total))) : 0;
    }
    function kv(label, value) {
      return '<div class="settings-prop-row"><span>' + settingsEsc(label) + '</span><code>' + settingsEsc(value == null || value === '' ? '--' : value) + '</code></div>';
    }
    function kvSmart(label, value) {
      var text = value == null || value === '' ? '--' : String(value);
      if (/^https?:\/\//i.test(text)) {
        return '<div class="settings-prop-row"><span>' + settingsEsc(label) + '</span><a href="' + settingsEsc(text) + '" target="_blank" rel="noopener noreferrer">' + settingsEsc(text) + '</a></div>';
      }
      return kv(label, text);
    }
    function partitionText(name, offset, size) {
      if (!name && !offset && !size) return '--';
      var off = '0x' + Number(offset || 0).toString(16).toUpperCase();
      return String(name || '-') + ' (' + off + ', ' + fmt(size || 0) + ')';
    }
    function resetReasonText(code) {
      var n = Number(code);
      var map = {
        1: 'Açılış (normal)',
        3: 'Yazılımsal reset',
        4: 'Watchdog reset',
        5: 'Derin uykudan dönüş',
        12: 'Panic/exception reset',
        16: 'Brownout (düşük voltaj)'
      };
      return (map[n] || ('Kod ' + (isFinite(n) ? n : '--')));
    }
    function meter(label, used, total, accent) {
      var ratio = pct(used, total);
      return '<div class="settings-health-card" style="--meter-accent:' + settingsEsc(accent || '#9BEB5D') + ';">' +
        '<div class="settings-health-card-head"><span>' + settingsEsc(label) + '</span><strong>' + ratio.toFixed(0) + '%</strong></div>' +
        '<div class="settings-health-meter"><i style="width:' + ratio.toFixed(1) + '%"></i></div>' +
        '<div class="settings-health-note">' + settingsEsc(fmt(used || 0)) + ' / ' + settingsEsc(fmt(total || 0)) + '</div></div>';
    }
    var appUsed = sys && sys.app_used != null ? sys.app_used : 0;
    var appTotal = sys && sys.app_total != null ? sys.app_total : data.app0_size;
    var fsUsed = sys && sys.fs_used != null ? sys.fs_used : 0;
    var fsTotal = sys && sys.fs_total != null ? sys.fs_total : data.littlefs_size;
    var recUsed = sys && sys.recovery_used != null ? sys.recovery_used : 0;
    var recTotal = sys && sys.recovery_total != null ? sys.recovery_total : data.recovery_size;
    var heapFree = sys && sys.internal_free != null ? sys.internal_free : (sys ? sys.free_heap : 0);
    var heapTotal = sys && sys.internal_total != null ? sys.internal_total : 0;
    var heapUsed = Math.max(0, Number(heapTotal || 0) - Number(heapFree || 0));
    var psramFree = sys && sys.psram_free != null ? sys.psram_free : 0;
    var psramTotal = sys && sys.psram_total != null ? sys.psram_total : 0;
    var psramUsed = Math.max(0, Number(psramTotal || 0) - Number(psramFree || 0));
    var uptime = sys && sys.uptime_s != null ? Number(sys.uptime_s) : 0;
    var html = '<div class="settings-about-hero">' +
      '<div class="settings-about-identity">' +
        '<div class="settings-about-mark">MROS</div>' +
        '<div><h3>' + settingsEsc(data.system_name || '--') + '</h3>' +
        '<p>' + settingsEsc((data.chip_model || data.target || 'ESP32') + ' / ' + (data.version || '--')) + '</p></div>' +
      '</div>' +
      '<div class="settings-about-kpis">' +
        '<div><span>Build</span><strong>' + settingsEsc((data.build_date || '--') + ' ' + (data.build_time || '')) + '</strong></div>' +
        '<div><span>SDK</span><strong>' + settingsEsc(data.idf_version || '--') + '</strong></div>' +
        '<div><span>Uptime</span><strong id="settings-about-uptime">' + settingsEsc(uptime ? uptime + ' sn' : '--') + '</strong></div>' +
        '<div><span>WebSocket</span><strong>' + settingsEsc(String(data.ws_open_count != null ? data.ws_open_count : '--')) + ' / ' + settingsEsc(String(data.ws_auth_count != null ? data.ws_auth_count : '--')) + '</strong></div>' +
      '</div>' +
      '</div>';
    if (sys && typeof sys === 'object' && sys.uptime_s !== undefined) {
      html += '<div class="settings-health-grid">' +
        meter('SRAM kullanımı', heapUsed, heapTotal, '#9BEB5D') +
        meter('PSRAM kullanımı', psramUsed, psramTotal, '#7EA8F2') +
        meter('LittleFS doluluk', fsUsed, fsTotal, '#B98BEA') +
        meter('App partition', appUsed, appTotal, '#EAC27C') +
        meter('Recovery partition', recUsed, recTotal, '#EA7B7B') +
        '<div class="settings-health-card"><div class="settings-health-card-head"><span>Kontrol döngüsü</span><strong>' + settingsEsc((Number(sys.ctrl_actual_hz) || 0).toFixed(1)) + ' Hz</strong></div><div class="settings-health-note">RSSI ' + settingsEsc(sys.rssi) + ' dBm / görev ' + settingsEsc(sys.task_queue_pending || 0) + '</div></div>' +
        '</div>';
    }
    html += '<div class="settings-about-section-title">Sistem Bilgileri</div>' +
      '<div class="settings-prop-grid">' +
      kv('Sistem adı', data.system_name) +
      kv('Yazılım sürümü', data.version) +
      kv('Geliştirici', data.developer) +
      kv('Build tarihi', (data.build_date || '--') + ' ' + (data.build_time || '')) +
      kv('Hedef kart', data.target) +
      kv('Çip modeli', data.chip_model) +
      kv('Çip revizyonu', data.chip_revision) +
      kv('Çekirdek sayısı', data.chip_cores) +
      kv('ESP-IDF sürümü', data.idf_version) +
      kv('Flash kapasitesi', fmt(data.flash_size || 0)) +
      kv('Flash çalışma modu', (data.flash_mode || '--') + '@' + (data.flash_freq_mhz ? (data.flash_freq_mhz + ' MHz') : (data.flash_freq || '--'))) +
      kv('PSRAM çalışma hızı', data.psram_freq_mhz ? (data.psram_freq_mhz + ' MHz') : '--') +
      kv('Flash konfig boyutu', data.flash_config_size) +
      kv('Partition tablosu', data.partition_table) +
      kv('Partition başlangıcı', '0x' + Number(data.partition_offset || 0).toString(16).toUpperCase()) +
      kv('Aktif uygulama bölümü', partitionText(data.running_partition, data.running_offset, data.app0_size)) +
      kv('Uygulama alanı (APP0)', partitionText('app0', data.app0_offset, data.app0_size)) +
      kv('Recovery alanı', partitionText('recovery', data.recovery_offset, data.recovery_size)) +
      kv('Dosya sistemi (LittleFS)', partitionText('littlefs', data.littlefs_offset, data.littlefs_size)) +
      kv('FreeRTOS çekirdek', data.freertos_cores) +
      kv('FreeRTOS frekansı', String(data.freertos_hz || '--') + ' Hz') +
      kv('Derleme optimizasyonu', data.compiler_opt) +
      kv('Panic davranışı', data.panic_mode) +
      kv('Son reset nedeni', resetReasonText(data.reset_reason)) +
      kvSmart('GitHub adresi', data.github_url) +
      '</div>';
    html += '<div class="settings-debug-title">Canlı Debug Göstergeleri (2 sn)</div><div class="settings-debug-grid">' +
      buildSettingsDebugBarCard('P4 Döngü', 'setdbg-loop', '-- ms') +
      buildSettingsDebugBarCard('CPU Yükü', 'setdbg-cpu', '-- %') +
      buildSettingsDebugBarCard('Power Modu', 'setdbg-power', '--') +
      buildSettingsDebugBarCard('Sıcaklık', 'setdbg-temp', '-- C') +
      buildSettingsDebugBarCard('C3 Kalite', 'setdbg-quality', '-- %') +
      buildSettingsDebugBarCard('SPI CRC Hata', 'setdbg-spicrc', '--') +
      buildSettingsDebugBarCard('UART CRC Hata', 'setdbg-uartcrc', '--') +
      buildSettingsDebugBarCard('IK/FK Toplam', 'setdbg-ikfk', '-- ms') +
      '</div>';
    body.className = '';
    body.innerHTML = html;
    updateSettingsAboutDebugCards(sys || null);
  }

  function refreshSettingsAboutLive() {
    var body = document.getElementById('settings-about-body');
    if (!body || !document.getElementById('setdbg-loop-bar')) return;
    fetch('/api/debug/sysinfo', { cache: 'no-store' })
      .then(function(r) { return r && r.ok ? r.json() : null; })
      .then(function(sys) { updateSettingsAboutDebugCards(sys || null); })
      .catch(function() { updateSettingsAboutDebugCards(null); });
  }

  function startSettingsAboutLive() {
    if (settingsAboutLiveTimer) return;
    refreshSettingsAboutLive();
    settingsAboutLiveTimer = setInterval(refreshSettingsAboutLive, 2000);
  }

  function stopSettingsAboutLive() {
    if (!settingsAboutLiveTimer) return;
    clearInterval(settingsAboutLiveTimer);
    settingsAboutLiveTimer = null;
  }

  function loadSettingsAbout(forceReload) {
    var body = document.getElementById('settings-about-body');
    if (!body) return;
    if (!forceReload && settingsAboutCache) {
      if (body.className === 'settings-empty') body.className = '';
      startSettingsAboutLive();
      return;
    }
    if (settingsAboutLoading) return;
    settingsAboutLoading = true;
    body.className = 'settings-empty';
    body.textContent = 'Hakkında ve Debug bilgisi yükleniyor...';
    Promise.all([
      fetch('/api/about', { cache: 'no-store' }).then(function(r) { return r.ok ? r.json() : null; }),
      fetch('/api/debug/sysinfo', { cache: 'no-store' }).then(function(r) { return r.ok ? r.json() : null; })
    ]).then(function(results) {
      settingsAboutCache = results[0] || {};
      renderSettingsAbout(settingsAboutCache, results[1] || null);
      startSettingsAboutLive();
    }).catch(function() {
      renderSettingsAbout(null, null);
    }).finally(function() {
      settingsAboutLoading = false;
    });
  }

  function switchSettingsTab(tabId, el) {
    var modal = document.getElementById('settingsModal');
    if (!modal) return;
    modal.querySelectorAll('.settings-tab-content').forEach(function(node) { node.classList.remove('active'); });
    modal.querySelectorAll('.settings-tab-btn').forEach(function(node) { node.classList.remove('active'); });
    var content = document.getElementById(tabId);
    if (content) content.classList.add('active');
    if (el) el.classList.add('active');
    var firstSection = SETTINGS_NAV[tabId] && SETTINGS_NAV[tabId].sections && SETTINGS_NAV[tabId].sections[0]
      ? SETTINGS_NAV[tabId].sections[0][0]
      : '';
    renderSettingsSubnav(tabId, firstSection);
    if (tabId === 'settings-tab-general') {
      mountSettingsGeneralTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-ui') {
      mountSettingsUiTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-security') {
      mountSettingsSecurityTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-profile') {
      mountSettingsProfileTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-3d' && typeof global.kin3d_mountSettingsTabContent === 'function') {
      global.kin3d_mountSettingsTabContent('settings-tab-3d-body');
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-files') {
      mountSettingsFilesTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-robot') {
      mountSettingsRobotTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-terminal') {
      mountSettingsTerminalTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-services') {
      mountSettingsServicesTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-net') {
      mountSettingsNetTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-comms') {
      mountSettingsCommsTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-perf') {
      mountSettingsPerfTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-update') {
      mountSettingsUpdateTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-storage') {
      mountSettingsStorageTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-prefs') {
      mountSettingsPrefsTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-devs') {
      mountSettingsDevicesTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-ai') {
      mountSettingsAiTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-devtools') {
      mountSettingsDevtoolsTabContent();
      stopSettingsAboutLive();
    } else if (tabId === 'settings-tab-about') {
      loadSettingsAbout(true);
    } else {
      stopSettingsAboutLive();
    }
  }

  root.panels.settings = {
    openSettingsModal: openSettingsModal,
    closeSettingsModal: closeSettingsModal,
    applyUiPanelSettings: applyUiPanelSettings,
    uiPanelGetSettings: uiPanelGetSettings,
    terminalGetSettings: terminalGetSettings,
    applyTerminalTheme: applyTerminalTheme,
    applyUiColorTheme: applyUiColorTheme,
    switchSettingsTab: switchSettingsTab,
    updateFloatingToolsDockLayout: updateFloatingToolsDockLayout
  };

  global.UI_PANEL_SETTINGS_KEY = UI_PANEL_SETTINGS_KEY;
  global.TERMINAL_SETTINGS_KEY = TERMINAL_SETTINGS_KEY;
  global.NET_SETTINGS_KEY = NET_SETTINGS_KEY;
  global.PERF_SETTINGS_KEY = PERF_SETTINGS_KEY;
  global.PREFS_SETTINGS_KEY = PREFS_SETTINGS_KEY;
  global.DEVICES_SETTINGS_KEY = DEVICES_SETTINGS_KEY;
  global.SERVICES_SETTINGS_KEY = SERVICES_SETTINGS_KEY;
  global.GENERAL_SETTINGS_KEY = GENERAL_SETTINGS_KEY;
  global.SECURITY_SETTINGS_KEY = SECURITY_SETTINGS_KEY;
  global.FILES_SETTINGS_KEY = FILES_SETTINGS_KEY;
  global.ROBOT_SETTINGS_KEY = ROBOT_SETTINGS_KEY;
  global.UPDATE_SETTINGS_KEY = UPDATE_SETTINGS_KEY;
  global.STORAGE_SETTINGS_KEY = STORAGE_SETTINGS_KEY;
  global.AI_SETTINGS_KEY = AI_SETTINGS_KEY;
  global.DEVTOOLS_SETTINGS_KEY = DEVTOOLS_SETTINGS_KEY;
  global.UI_PANEL_DEFAULTS = UI_PANEL_DEFAULTS;
  global.TERMINAL_DEFAULTS = TERMINAL_DEFAULTS;
  global.TERMINAL_THEME_DEFAULT = TERMINAL_THEME_DEFAULT;
  global.TERMINAL_THEMES = TERMINAL_THEMES;
  global.UI_COLOR_PALETTES = UI_COLOR_PALETTES;
  global.openSettingsModal = openSettingsModal;
  global.closeSettingsModal = closeSettingsModal;
  global.uiPanelGetSettings = uiPanelGetSettings;
  global.uiPanelSaveSettings = uiPanelSaveSettings;
  global.terminalGetSettings = terminalGetSettings;
  global.terminalSaveSettings = terminalSaveSettings;
  global.applyTerminalTheme = applyTerminalTheme;
  global.applyUiColorTheme = applyUiColorTheme;
  global.updateFloatingToolsDockLayout = updateFloatingToolsDockLayout;
  global.applyUiPanelSettings = applyUiPanelSettings;
  global.mountSettingsUiTabContent = mountSettingsUiTabContent;
  global.mountSettingsTerminalTabContent = mountSettingsTerminalTabContent;
  global.mountSettingsServicesTabContent = mountSettingsServicesTabContent;
  global.switchSettingsTab = switchSettingsTab;
  global.stopSettingsAboutLive = stopSettingsAboutLive;
  bindSystemThemeWatcher();
})(window);
