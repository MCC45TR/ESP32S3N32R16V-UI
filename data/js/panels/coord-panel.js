(function(global) {
  var root = global.MROS = global.MROS || {};
  root.panels = root.panels || {};

  function readInputs() {
    return (typeof global.ikReadTargetPoseInputs === 'function') ? global.ikReadTargetPoseInputs() : null;
  }

  function writeInputs(pose, keepAuto) {
    if (typeof global.ikWriteTargetPoseInputs === 'function') global.ikWriteTargetPoseInputs(pose, keepAuto);
  }

  function setStatus(message, isError) {
    if (typeof global.ikSetStatus === 'function') global.ikSetStatus(message, isError);
  }

  function setTargetStatus(message, ok) {
    if (typeof global.ikSetTargetStatus === 'function') global.ikSetTargetStatus(message, ok);
  }

  function updateActualPoseDisplay(pose) {
    if (typeof global.ikUpdateActualPoseDisplay === 'function') global.ikUpdateActualPoseDisplay(pose);
  }

  function clear() {
    if (typeof global.clearIKInputs === 'function') global.clearIKInputs();
  }

  root.panels.coord = {
    readInputs: readInputs,
    writeInputs: writeInputs,
    setStatus: setStatus,
    setTargetStatus: setTargetStatus,
    updateActualPoseDisplay: updateActualPoseDisplay,
    clear: clear
  };
})(window);
