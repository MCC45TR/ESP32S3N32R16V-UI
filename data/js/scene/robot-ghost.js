(function(global) {
  function kin3d_setCadGhostVisibility(vis) {
    var visible = !!vis;
    for (var key in global.kin3d_cadGhostRoots) {
      if (global.kin3d_cadGhostRoots[key]) {
        global.kin3d_cadGhostRoots[key].visible = visible && global.kin3d_isCadPartVisibleBySettings(key);
      }
    }
  }

  function kin3d_buildCadGhostRoot(sourceRoot, partId) {
    if (!sourceRoot || !global.kin3d_scene) return null;
    var prev = global.kin3d_cadGhostRoots[partId];
    if (prev) global.kin3d_scene.remove(prev);
    var ghostRoot = sourceRoot.clone(true);
    ghostRoot.name = 'ghost_' + (sourceRoot.name || partId);
    ghostRoot.userData = ghostRoot.userData || {};
    ghostRoot.userData.kin3dCadPartId = partId;
    ghostRoot.traverse(function(node) {
      node.userData = node.userData || {};
      node.userData.kin3dCadPartId = partId;
      if (!node.isMesh || !node.material) return;
      node.castShadow = false;
      node.receiveShadow = false;
      var mats = Array.isArray(node.material) ? node.material : [node.material];
      var ghostMats = [];
      for (var i = 0; i < mats.length; i++) {
        var src = mats[i];
        if (!src || typeof src.clone !== 'function') {
          ghostMats.push(src);
          continue;
        }
        var gm = src.clone();
        gm.transparent = true;
        gm.opacity = 0.25;
        gm.depthWrite = false;
        gm.needsUpdate = true;
        ghostMats.push(gm);
      }
      node.material = Array.isArray(node.material) ? ghostMats : ghostMats[0];
    });
    ghostRoot.visible = false;
    global.kin3d_scene.add(ghostRoot);
    global.kin3d_cadGhostRoots[partId] = ghostRoot;
    return ghostRoot;
  }

  function showGhostPose(target_angles_deg, opts) {
    if (!global.kin3d_initialized) return;
    if (global.kin3d_settings.showGhost === false) {
      hideGhost();
      return;
    }
    global.kin3d_lastGhostAngles = Array.isArray(target_angles_deg) ? target_angles_deg.slice(0, 7) : null;
    var skipPath = !!(opts && opts.skipPath);
    var T_all = compute_FK(target_angles_deg);
    var P = extract_positions(T_all);
    var useCadGhost = (global.kin3d_settings.useCAD === true);

    if (global.kin3d_planned_label) {
      var eePos = mat4_get_pos(T_all[6]);
      var plannedPose = (window && typeof window.ikPoseFromAnglesDeg === 'function')
        ? window.ikPoseFromAnglesDeg(target_angles_deg)
        : null;
      if (!plannedPose) plannedPose = { x: eePos.x, y: eePos.y, z: eePos.z, roll_deg: 0, pitch_deg: 0, yaw_deg: 0, alpha: 0 };
      global.kin3d_planned_label.innerHTML = global.kin3d_poseText('PLANLANAN', plannedPose, '#6A97EA');
    }

    if (useCadGhost) {
      for (var i = 0; i < global.ghost_spheres.length; i++) global.ghost_spheres[i].visible = false;
      for (var j = 0; j < global.ghost_tubes.length; j++) global.ghost_tubes[j].visible = false;
      if (global.ghost_eeAxes) global.ghost_eeAxes.visible = false;
      global.kin3d_updateCadPlacementsForRoots(global.kin3d_cadGhostRoots, target_angles_deg, P, T_all);
      kin3d_setCadGhostVisibility(true);
    } else {
      kin3d_setCadGhostVisibility(false);
      for (var gs = 0; gs < global.ghost_spheres.length; gs++) {
        global.ghost_spheres[gs].position.set(P[gs].x, P[gs].z, P[gs].y);
        global.ghost_spheres[gs].visible = true;
      }
      for (var gt = 0; gt < global.ghost_tubes.length; gt++) {
        global.kin3d_positionTube(global.ghost_tubes[gt], new THREE.Vector3(P[gt].x, P[gt].z, P[gt].y), new THREE.Vector3(P[gt + 1].x, P[gt + 1].z, P[gt + 1].y));
        global.ghost_tubes[gt].visible = true;
      }
      var eeP = mat4_get_pos(T_all[6]);
      global.ghost_eeAxes.position.set(eeP.x, eeP.z, eeP.y);
      global.ghost_eeAxes.visible = true;
    }

    global.ghost_visible = true;
    global.kin3d_updateTorqueInfoModal();
    global.kin3d_updateTorqueSidebarSummary();
    if (!skipPath) global.showTrajectoryPath(target_angles_deg);
  }

  function hideGhost() {
    for (var i = 0; i < global.ghost_spheres.length; i++) global.ghost_spheres[i].visible = false;
    for (var j = 0; j < global.ghost_tubes.length; j++) global.ghost_tubes[j].visible = false;
    if (global.ghost_eeAxes) global.ghost_eeAxes.visible = false;
    kin3d_setCadGhostVisibility(false);
    if (global.traj_pathLine) global.traj_pathLine.visible = false;
    global.kin3d_ghostPreview.active = false;
    global.ghost_visible = false;
    global.kin3d_lastGhostAngles = null;
    global.kin3d_updateTorqueInfoModal();
    global.kin3d_updateTorqueSidebarSummary();
  }

  global.kin3d_setCadGhostVisibility = kin3d_setCadGhostVisibility;
  global.kin3d_buildCadGhostRoot = kin3d_buildCadGhostRoot;
  global.showGhostPose = showGhostPose;
  global.hideGhost = hideGhost;
})(window);
