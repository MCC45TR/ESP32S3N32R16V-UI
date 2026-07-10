(function(global) {
  var root = global.MROS = global.MROS || {};
  root.ik = root.ik || {};

  function buildWsPoseCommand(prefix, target, applyToRobot) {
    return (typeof global.ikBuildWsPoseCommand === 'function')
      ? global.ikBuildWsPoseCommand(prefix, target, applyToRobot)
      : '';
  }

  function sendAnglesViaWs(anglesDeg) {
    if (typeof global.ikSendAnglesViaWs === 'function') global.ikSendAnglesViaWs(anglesDeg);
  }

  function scheduleRealtimePoseApply(target) {
    if (typeof global.ikScheduleRealtimePoseApply === 'function') global.ikScheduleRealtimePoseApply(target);
  }

  function schedulePreviewPoseApply(target, planMode) {
    if (typeof global.ikSchedulePreviewPoseApply === 'function') global.ikSchedulePreviewPoseApply(target, planMode);
  }

  function sendIK() {
    if (typeof global.sendIK === 'function') global.sendIK();
  }

  function calculateIK() {
    if (typeof global.calculateIK === 'function') global.calculateIK();
  }

  root.ik.transport = {
    buildWsPoseCommand: buildWsPoseCommand,
    sendAnglesViaWs: sendAnglesViaWs,
    scheduleRealtimePoseApply: scheduleRealtimePoseApply,
    schedulePreviewPoseApply: schedulePreviewPoseApply,
    sendIK: sendIK,
    calculateIK: calculateIK
  };
})(window);
