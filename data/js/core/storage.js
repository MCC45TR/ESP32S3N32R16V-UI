(function(global) {
  var root = global.MROS = global.MROS || {};
  root.core = root.core || {};

  var popupSettingsState = {};
  var popupSettingsLoadPromise = null;
  var popupSettingsSaveTimer = null;
  var popupSettingsLastSerialized = '';
  var popupSettingsSaveInFlight = false;
  var deviceSettingsState = {};
  var deviceSettingsLoadPromise = null;
  var deviceSettingsSaveTimer = null;
  var deviceSettingsLastSerialized = '';
  var deviceSettingsSaveInFlight = false;

  function popupSettingsNormalizeRoot(obj) {
    if (!obj || typeof obj !== 'object' || Array.isArray(obj)) return {};
    return obj;
  }

  function cloneJson(obj) {
    return JSON.parse(JSON.stringify(obj || {}));
  }

  function popupSettingsGetSection(name) {
    if (!name || typeof name !== 'string') return null;
    var section = popupSettingsNormalizeRoot(popupSettingsState)[name];
    if (!section || typeof section !== 'object' || Array.isArray(section)) return null;
    return cloneJson(section);
  }

  function popupSettingsDispatchLoaded() {
    try {
      global.dispatchEvent(new CustomEvent('mros-popup-settings-loaded'));
    } catch (err) {}
  }

  function popupSettingsLoadFromServer() {
    if (popupSettingsLoadPromise) return popupSettingsLoadPromise;
    popupSettingsLoadPromise = fetch('/api/settings/popup', { cache: 'no-store' })
      .then(function(resp) {
        if (!resp || !resp.ok) throw new Error('popup settings load failed');
        return resp.json();
      })
      .then(function(data) {
        popupSettingsState = popupSettingsNormalizeRoot(data);
        try {
          popupSettingsLastSerialized = JSON.stringify(popupSettingsState);
        } catch (err) {
          popupSettingsLastSerialized = '';
        }
        popupSettingsDispatchLoaded();
        return popupSettingsState;
      })
      .catch(function() {
        popupSettingsState = {};
        popupSettingsDispatchLoaded();
        return popupSettingsState;
      });
    return popupSettingsLoadPromise;
  }

  function popupSettingsFlushSave() {
    popupSettingsSaveTimer = null;
    if (popupSettingsSaveInFlight) return;
    popupSettingsSaveInFlight = true;

    var serialized = '';
    try {
      serialized = JSON.stringify(popupSettingsNormalizeRoot(popupSettingsState));
    } catch (err) {
      popupSettingsSaveInFlight = false;
      return;
    }
    if (!serialized || serialized === popupSettingsLastSerialized) {
      popupSettingsSaveInFlight = false;
      return;
    }

    fetch('/api/settings/popup', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: serialized
    }).then(function(resp) {
      if (!resp || !resp.ok) throw new Error('popup settings save failed');
      popupSettingsLastSerialized = serialized;
    }).catch(function() {
    }).finally(function() {
      popupSettingsSaveInFlight = false;
      var needsRetry = false;
      try {
        needsRetry = JSON.stringify(popupSettingsNormalizeRoot(popupSettingsState)) !== popupSettingsLastSerialized;
      } catch (err) {
        needsRetry = false;
      }
      if (needsRetry && !popupSettingsSaveTimer) {
        popupSettingsSaveTimer = setTimeout(popupSettingsFlushSave, 250);
      }
    });
  }

  function popupSettingsScheduleSave() {
    if (popupSettingsSaveTimer) clearTimeout(popupSettingsSaveTimer);
    popupSettingsSaveTimer = setTimeout(popupSettingsFlushSave, 700);
  }

  function popupSettingsSetSection(name, value) {
    if (!name || typeof name !== 'string') return;
    var nextRoot = popupSettingsNormalizeRoot(popupSettingsState);
    if (value === null || value === undefined) {
      delete nextRoot[name];
    } else if (typeof value === 'object' && !Array.isArray(value)) {
      nextRoot[name] = cloneJson(value);
    } else {
      return;
    }
    popupSettingsState = nextRoot;
    popupSettingsScheduleSave();
  }

  function deviceSettingsDispatchLoaded() {
    try {
      global.dispatchEvent(new CustomEvent('mros-device-settings-loaded'));
    } catch (err) {}
  }

  function deviceSettingsGetSection(name) {
    if (!name || typeof name !== 'string') return null;
    var section = popupSettingsNormalizeRoot(deviceSettingsState)[name];
    if (!section || typeof section !== 'object' || Array.isArray(section)) return null;
    return cloneJson(section);
  }

  function deviceSettingsLoadFromServer() {
    if (deviceSettingsLoadPromise) return deviceSettingsLoadPromise;
    deviceSettingsLoadPromise = fetch('/api/settings/device', { cache: 'no-store' })
      .then(function(resp) {
        if (!resp || !resp.ok) throw new Error('device settings load failed');
        return resp.json();
      })
      .then(function(data) {
        deviceSettingsState = popupSettingsNormalizeRoot(data);
        try {
          deviceSettingsLastSerialized = JSON.stringify(deviceSettingsState);
        } catch (err) {
          deviceSettingsLastSerialized = '';
        }
        deviceSettingsDispatchLoaded();
        return deviceSettingsState;
      })
      .catch(function() {
        deviceSettingsState = {};
        deviceSettingsDispatchLoaded();
        return deviceSettingsState;
      });
    return deviceSettingsLoadPromise;
  }

  function deviceSettingsFlushSave() {
    deviceSettingsSaveTimer = null;
    if (deviceSettingsSaveInFlight) return;
    deviceSettingsSaveInFlight = true;
    var rootObj = popupSettingsNormalizeRoot(deviceSettingsState);
    if (!rootObj.schema_version) rootObj.schema_version = 1;
    var serialized = '';
    try {
      serialized = JSON.stringify(rootObj);
    } catch (err) {
      deviceSettingsSaveInFlight = false;
      return;
    }
    if (!serialized || serialized === deviceSettingsLastSerialized) {
      deviceSettingsSaveInFlight = false;
      return;
    }
    fetch('/api/settings/device', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: serialized
    }).then(function(resp) {
      if (!resp || !resp.ok) throw new Error('device settings save failed');
      deviceSettingsLastSerialized = serialized;
    }).catch(function() {
    }).finally(function() {
      deviceSettingsSaveInFlight = false;
      var needsRetry = false;
      try {
        needsRetry = JSON.stringify(popupSettingsNormalizeRoot(deviceSettingsState)) !== deviceSettingsLastSerialized;
      } catch (err) {
        needsRetry = false;
      }
      if (needsRetry && !deviceSettingsSaveTimer) {
        deviceSettingsSaveTimer = setTimeout(deviceSettingsFlushSave, 300);
      }
    });
  }

  function deviceSettingsScheduleSave() {
    if (deviceSettingsSaveTimer) clearTimeout(deviceSettingsSaveTimer);
    deviceSettingsSaveTimer = setTimeout(deviceSettingsFlushSave, 850);
  }

  function deviceSettingsSetSection(name, value) {
    if (!name || typeof name !== 'string') return;
    var nextRoot = popupSettingsNormalizeRoot(deviceSettingsState);
    if (!nextRoot.schema_version) nextRoot.schema_version = 1;
    if (value === null || value === undefined) {
      delete nextRoot[name];
    } else if (typeof value === 'object' && !Array.isArray(value)) {
      nextRoot[name] = cloneJson(value);
    } else {
      return;
    }
    deviceSettingsState = nextRoot;
    deviceSettingsScheduleSave();
  }

  root.core.storage = {
    popupSettingsNormalizeRoot: popupSettingsNormalizeRoot,
    popupSettingsGetSection: popupSettingsGetSection,
    popupSettingsLoadFromServer: popupSettingsLoadFromServer,
    popupSettingsSetSection: popupSettingsSetSection,
    deviceSettingsGetSection: deviceSettingsGetSection,
    deviceSettingsLoadFromServer: deviceSettingsLoadFromServer,
    deviceSettingsSetSection: deviceSettingsSetSection
  };

  global.popupSettingsGetSection = popupSettingsGetSection;
  global.popupSettingsLoadFromServer = popupSettingsLoadFromServer;
  global.popupSettingsSetSection = popupSettingsSetSection;
  global.mrosPopupSettingsGetSection = popupSettingsGetSection;
  global.mrosPopupSettingsSetSection = popupSettingsSetSection;
  global.mrosPopupSettingsLoad = popupSettingsLoadFromServer;
  global.deviceSettingsGetSection = deviceSettingsGetSection;
  global.deviceSettingsLoadFromServer = deviceSettingsLoadFromServer;
  global.deviceSettingsSetSection = deviceSettingsSetSection;
  global.mrosDeviceSettingsGetSection = deviceSettingsGetSection;
  global.mrosDeviceSettingsSetSection = deviceSettingsSetSection;
  global.mrosDeviceSettingsLoad = deviceSettingsLoadFromServer;
})(window);
