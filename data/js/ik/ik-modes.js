(function(global) {
  var root = global.MROS = global.MROS || {};
  root.ik = root.ik || {};

  function normalizeInteractionMode(mode) {
    return (typeof global.ikNormalizeInteractionMode === 'function') ? global.ikNormalizeInteractionMode(mode) : String(mode || 'plan');
  }

  function normalizeManipulatorMode(mode) {
    return (typeof global.ikNormalizeManipulatorMode === 'function') ? global.ikNormalizeManipulatorMode(mode) : String(mode || 'translate');
  }

  function isPlannedMode(mode) {
    return (typeof global.ikIsPlannedMode === 'function') ? global.ikIsPlannedMode(mode) : normalizeInteractionMode(mode) === 'plan';
  }

  function isUnplannedMode(mode) {
    return (typeof global.ikIsUnplannedMode === 'function') ? global.ikIsUnplannedMode(mode) : normalizeInteractionMode(mode) === 'realtime';
  }

  function getManipulatorFrameLabel(mode) {
    return (typeof global.ikGetManipulatorFrameLabel === 'function')
      ? global.ikGetManipulatorFrameLabel(mode)
      : (normalizeManipulatorMode(mode) === 'rotate' ? 'LOCAL' : 'WORLD');
  }

  function setInteractionMode(mode) {
    if (typeof global.setIkInteractionMode === 'function') global.setIkInteractionMode(mode);
  }

  function setManipulatorMode(mode) {
    if (typeof global.setIkManipulatorMode === 'function') global.setIkManipulatorMode(mode);
  }

  function refresh() {
    if (typeof global.refreshIkInteractionModeButtons === 'function') global.refreshIkInteractionModeButtons();
    if (typeof global.refreshIkInteractionModeUi === 'function') global.refreshIkInteractionModeUi();
    if (typeof global.refreshIkManipulatorButtons === 'function') global.refreshIkManipulatorButtons();
  }

  root.ik.modes = {
    normalizeInteractionMode: normalizeInteractionMode,
    normalizeManipulatorMode: normalizeManipulatorMode,
    isPlannedMode: isPlannedMode,
    isUnplannedMode: isUnplannedMode,
    getManipulatorFrameLabel: getManipulatorFrameLabel,
    setInteractionMode: setInteractionMode,
    setManipulatorMode: setManipulatorMode,
    refresh: refresh
  };
})(window);
