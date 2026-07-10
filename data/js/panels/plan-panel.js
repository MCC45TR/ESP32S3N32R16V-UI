(function(global) {
  var root = global.MROS = global.MROS || {};
  root.panels = root.panels || {};

  function previewNow() {
    if (typeof global.previewTrajectoryNow === 'function') global.previewTrajectoryNow();
  }

  function updateTrajectoryUi() {
    if (typeof global.updateTrajectoryUI === 'function') global.updateTrajectoryUI();
  }

  function updateSummary() {
    if (typeof global.updateTrajectorySummary === 'function') global.updateTrajectorySummary();
  }

  function openMotionPlanModal() {
    if (typeof global.openMotionPlanModal === 'function') global.openMotionPlanModal();
  }

  function closeMotionPlanModal() {
    if (typeof global.closeMotionPlanModal === 'function') global.closeMotionPlanModal();
  }

  function openMotionBlocksModal() {
    if (typeof global.openMotionBlocksModal === 'function') global.openMotionBlocksModal();
  }

  function closeMotionBlocksModal() {
    if (typeof global.closeMotionBlocksModal === 'function') global.closeMotionBlocksModal();
  }

  function previewMotionBlocks() {
    if (typeof global.previewMotionBlocks === 'function') global.previewMotionBlocks();
  }

  function applyMotionBlocks(appendMode) {
    if (typeof global.applyMotionBlocks === 'function') global.applyMotionBlocks(appendMode);
  }

  root.panels.plan = {
    previewNow: previewNow,
    updateTrajectoryUi: updateTrajectoryUi,
    updateSummary: updateSummary,
    openMotionPlanModal: openMotionPlanModal,
    closeMotionPlanModal: closeMotionPlanModal,
    openMotionBlocksModal: openMotionBlocksModal,
    closeMotionBlocksModal: closeMotionBlocksModal,
    previewMotionBlocks: previewMotionBlocks,
    applyMotionBlocks: applyMotionBlocks
  };
})(window);
