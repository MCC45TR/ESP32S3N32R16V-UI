(function(global) {
  var root = global.MROS = global.MROS || {};
  root.panels = root.panels || {};

  function getAngles() {
    return (typeof global.getSliderAngles === 'function') ? global.getSliderAngles().slice() : [];
  }

  function updateSlider(uiId, wsId, value, step) {
    if (typeof global.updateSlider === 'function') global.updateSlider(uiId, wsId, value, step);
  }

  function applyAngles(anglesDeg, sendToRobot, options) {
    if (typeof global.ikApplyAngles === 'function') global.ikApplyAngles(anglesDeg, sendToRobot, options);
  }

  function resetAll() {
    if (typeof global.resetAllJoints === 'function') global.resetAllJoints();
  }

  function resetTurret() {
    if (typeof global.resetTurretPosition === 'function') global.resetTurretPosition();
  }

  root.panels.joint = {
    getAngles: getAngles,
    updateSlider: updateSlider,
    applyAngles: applyAngles,
    resetAll: resetAll,
    resetTurret: resetTurret
  };
})(window);
