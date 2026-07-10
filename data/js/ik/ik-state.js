(function(global) {
  var root = global.MROS = global.MROS || {};
  root.ik = root.ik || {};

  function readSelectValue(id, fallback) {
    var el = document.getElementById(id);
    return el ? String(el.value || fallback || '') : String(fallback || '');
  }

  function readCheckbox(id, fallback) {
    var el = document.getElementById(id);
    return el ? !!el.checked : !!fallback;
  }

  function snapshot() {
    return {
      targetPose: (typeof global.ikReadTargetPoseInputs === 'function') ? global.ikReadTargetPoseInputs() : null,
      currentAnglesDeg: (typeof global.getSliderAngles === 'function') ? global.getSliderAngles().slice() : [],
      computationMode: (typeof global.ikGetComputationMode === 'function') ? global.ikGetComputationMode() : 'WEB',
      interactionMode: readSelectValue('ik_mode_state', ''),
      manipulatorMode: readSelectValue('ik_manip_mode_state', ''),
      eeAuto: readCheckbox('ik_ee_auto', false),
      trajScale: (typeof global.ikGetTrajScale === 'function') ? global.ikGetTrajScale() : 1,
      mathState: {
        solver: readSelectValue('ik_solver', 'dls'),
        jacobian: readSelectValue('ik_jacobian', 'geometric'),
        nullspace: readSelectValue('ik_nullspace', 'center'),
        trajectory: readSelectValue('ik_traj_mode', 'quintic')
      }
    };
  }

  root.ik.state = {
    snapshot: snapshot
  };
})(window);
