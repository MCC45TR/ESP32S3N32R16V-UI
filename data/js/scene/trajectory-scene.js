(function(global) {
  function kin3d_isInterpTrajectoryPoint(p) {
    if (!p || typeof p !== 'object') return false;
    return !!(p.is_interp || p.isInterp || p.interp || p.kind === 'interp' || p.meta_interp);
  }

  function kin3d_setTrajMarkerLabel(group, txt, visible) {
    if (!group || !group.userData) return;
    var ud = group.userData;
    if (!ud.sprite || !ud.labelCtx || !ud.labelCanvas || !ud.labelTex) return;
    ud.sprite.visible = !!visible;
    if (!visible) return;
    var nextText = String(txt || '');
    if (ud.labelText === nextText) return;
    ud.labelText = nextText;
    ud.labelCtx.clearRect(0, 0, ud.labelCanvas.width, ud.labelCanvas.height);
    ud.labelCtx.font = 'Bold 40px sans-serif';
    ud.labelCtx.fillStyle = 'white';
    ud.labelCtx.textAlign = 'center';
    ud.labelCtx.shadowColor = 'rgba(0,0,0,1.0)';
    ud.labelCtx.shadowBlur = 6;
    ud.labelCtx.fillText(nextText, ud.labelCanvas.width * 0.5, 45);
    ud.labelTex.needsUpdate = true;
  }

  function kin3d_isFiniteTrajectoryPoint(p) {
    return !!(p && typeof p === 'object' && isFinite(Number(p.x)) && isFinite(Number(p.y)) && isFinite(Number(p.z)));
  }

  function clearTrajectoryTrace() {
    global.traj_tracePoints = [];
    if (global.traj_traceLine && global.traj_traceLine.geometry) {
      global.traj_traceLine.geometry.setDrawRange(0, 0);
    }
  }

  global.kin3d_isInterpTrajectoryPoint = kin3d_isInterpTrajectoryPoint;
  global.kin3d_setTrajMarkerLabel = kin3d_setTrajMarkerLabel;
  global.kin3d_isFiniteTrajectoryPoint = kin3d_isFiniteTrajectoryPoint;
  global.clearTrajectoryTrace = clearTrajectoryTrace;
})(window);
