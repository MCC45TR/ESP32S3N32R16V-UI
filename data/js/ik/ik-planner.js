(function(global) {
  var root = global.MROS = global.MROS || {};
  root.ik = root.ik || {};

  function normalizeOptions(incoming) {
    return (typeof global.plannerNormalizeOptions === 'function') ? global.plannerNormalizeOptions(incoming) : (incoming || {});
  }

  function normalizeWaypoint(wp, index, fallbackTimeMs) {
    return (typeof global.plannerNormalizeWaypoint === 'function')
      ? global.plannerNormalizeWaypoint(wp, index, fallbackTimeMs)
      : wp;
  }

  function planCartesianTrajectory(request) {
    return (typeof global.planCartesianTrajectory === 'function') ? global.planCartesianTrajectory(request) : null;
  }

  function previewIKMotion(customWaypoints, forceRoundTrip) {
    if (typeof global.previewIKMotion === 'function') return global.previewIKMotion(customWaypoints, forceRoundTrip);
    return false;
  }

  function applyPlannedTarget() {
    if (typeof global.applyPlannedIkTarget === 'function') global.applyPlannedIkTarget();
  }

  root.ik.planner = {
    normalizeOptions: normalizeOptions,
    normalizeWaypoint: normalizeWaypoint,
    planCartesianTrajectory: planCartesianTrajectory,
    previewIKMotion: previewIKMotion,
    applyPlannedTarget: applyPlannedTarget
  };
})(window);
