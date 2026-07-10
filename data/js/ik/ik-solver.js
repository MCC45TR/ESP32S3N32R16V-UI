(function(global) {
  var root = global.MROS = global.MROS || {};
  root.ik = root.ik || {};

  function poseFromAnglesDeg(anglesDeg) {
    return (typeof global.ikPoseFromAnglesDeg === 'function') ? global.ikPoseFromAnglesDeg(anglesDeg) : null;
  }

  function solveWeb(target, seedAnglesDeg) {
    return (typeof global.ikSolveWeb === 'function') ? global.ikSolveWeb(target, seedAnglesDeg) : null;
  }

  function solveWebSingle(target, seedAnglesDeg) {
    return (typeof global.ikSolveWebSingle === 'function') ? global.ikSolveWebSingle(target, seedAnglesDeg) : null;
  }

  function clampAnglesDeg(qDeg) {
    return (typeof global.ikClampAnglesDeg === 'function') ? global.ikClampAnglesDeg(qDeg) : (Array.isArray(qDeg) ? qDeg.slice() : []);
  }

  function buildTargetFromInputs() {
    return (typeof global.ikBuildTargetFromInputs === 'function') ? global.ikBuildTargetFromInputs() : null;
  }

  root.ik.solver = {
    poseFromAnglesDeg: poseFromAnglesDeg,
    solveWeb: solveWeb,
    solveWebSingle: solveWebSingle,
    clampAnglesDeg: clampAnglesDeg,
    buildTargetFromInputs: buildTargetFromInputs
  };
})(window);
