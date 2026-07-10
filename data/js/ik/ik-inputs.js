(function(global) {
  var root = global.MROS = global.MROS || {};
  root.ik = root.ik || {};

  function readTargetPose() {
    return (typeof global.ikReadTargetPoseInputs === 'function') ? global.ikReadTargetPoseInputs() : null;
  }

  function writeTargetPose(pose, keepAuto) {
    if (typeof global.ikWriteTargetPoseInputs === 'function') global.ikWriteTargetPoseInputs(pose, keepAuto);
  }

  function normalizeTargetPose(pose) {
    return (typeof global.ikNormalizePoseTarget === 'function') ? global.ikNormalizePoseTarget(pose) : pose;
  }

  function buildTargetFromInputs() {
    return (typeof global.ikBuildTargetFromInputs === 'function') ? global.ikBuildTargetFromInputs() : readTargetPose();
  }

  function syncManipulatorToInputs() {
    if (typeof global.ikSyncManipulatorToInputs === 'function') global.ikSyncManipulatorToInputs();
  }

  function clear() {
    if (typeof global.clearIKInputs === 'function') global.clearIKInputs();
  }

  function setInputValue(id, value, digits) {
    if (typeof global.ikSetInputValue === 'function') global.ikSetInputValue(id, value, digits);
  }

  root.ik.inputs = {
    readTargetPose: readTargetPose,
    writeTargetPose: writeTargetPose,
    normalizeTargetPose: normalizeTargetPose,
    buildTargetFromInputs: buildTargetFromInputs,
    syncManipulatorToInputs: syncManipulatorToInputs,
    clear: clear,
    setInputValue: setInputValue
  };
})(window);
