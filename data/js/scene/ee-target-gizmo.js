(function(global) {
  function kin3d_getTargetPivotOffsetLocal() {
    return new THREE.Vector3(global.kin3d_targetPivotOffsetMm, 0, 0);
  }

  function kin3d_getTargetPivotOffsetScene(sceneQuaternion) {
    return kin3d_getTargetPivotOffsetLocal().applyQuaternion(sceneQuaternion.clone().normalize());
  }

  function kin3d_robotQuaternionFromEulerDeg(rollDeg, pitchDeg, yawDeg) {
    var e = new THREE.Euler(
      THREE.MathUtils.degToRad(Number(rollDeg) || 0),
      THREE.MathUtils.degToRad(Number(pitchDeg) || 0),
      THREE.MathUtils.degToRad(Number(yawDeg) || 0),
      'XYZ'
    );
    return new THREE.Quaternion().setFromEuler(e).normalize();
  }

  function kin3d_sceneQuaternionFromRobotPose(pose) {
    var qRobot = kin3d_robotQuaternionFromEulerDeg(pose.roll_deg, pose.pitch_deg, pose.yaw_deg);
    var robotMatrix = new THREE.Matrix4().makeRotationFromQuaternion(qRobot);
    var e = robotMatrix.elements;
    var sceneMatrix = new THREE.Matrix4();
    sceneMatrix.set(e[0], e[8], e[4], 0, e[2], e[10], e[6], 0, e[1], e[9], e[5], 0, 0, 0, 0, 1);
    return new THREE.Quaternion().setFromRotationMatrix(sceneMatrix).normalize();
  }

  function kin3d_robotEulerDegFromSceneQuaternion(qScene) {
    var sceneMatrix = new THREE.Matrix4().makeRotationFromQuaternion(qScene.clone().normalize());
    var e = sceneMatrix.elements;
    var robotMatrix = new THREE.Matrix4();
    robotMatrix.set(e[0], e[8], e[4], 0, e[2], e[10], e[6], 0, e[1], e[9], e[5], 0, 0, 0, 0, 1);
    var euler = new THREE.Euler().setFromRotationMatrix(robotMatrix, 'XYZ');
    return {
      roll_deg: THREE.MathUtils.radToDeg(euler.x),
      pitch_deg: THREE.MathUtils.radToDeg(euler.y),
      yaw_deg: THREE.MathUtils.radToDeg(euler.z)
    };
  }

  function kin3d_poseFromTransformObject(obj) {
    if (!obj) return null;
    var qScene = (obj.quaternion || new THREE.Quaternion()).clone().normalize();
    var euler = kin3d_robotEulerDegFromSceneQuaternion(qScene);
    var pivotPos = obj.position ? obj.position.clone() : new THREE.Vector3();
    var actualScenePos = pivotPos.sub(kin3d_getTargetPivotOffsetScene(qScene));
    return {
      x: Number(actualScenePos.x) || 0,
      y: Number(actualScenePos.z) || 0,
      z: Number(actualScenePos.y) || 0,
      roll_deg: euler.roll_deg,
      pitch_deg: euler.pitch_deg,
      yaw_deg: euler.yaw_deg,
      alpha: euler.pitch_deg,
      ee_auto: false
    };
  }

  function kin3d_dispatchTargetPoseFromManipulator() {
    if (!global.kin3d_targetGroup || global.kin3d_targetPoseSilent || !global.kin3d_manipulatorEnabled) return;
    var pose = kin3d_poseFromTransformObject(global.kin3d_targetGroup);
    if (!pose) return;
    global.kin3d_targetPose = pose;
    global.kin3d_targetPoseInitialized = true;
    if (typeof global.ikHandlePoseTarget === 'function') {
      global.ikHandlePoseTarget(pose, { skipManipulatorSync: true });
    }
  }

  function kin3d_setTargetPose(pose, options) {
    if (!global.kin3d_targetGroup || !pose) return false;
    global.kin3d_targetPoseSilent = !!(options && options.silent);
    var qScene = kin3d_sceneQuaternionFromRobotPose(pose);
    var actualScenePos = new THREE.Vector3(Number(pose.x) || 0, Number(pose.z) || 0, Number(pose.y) || 0);
    var pivotScenePos = actualScenePos.clone().add(kin3d_getTargetPivotOffsetScene(qScene));
    global.kin3d_targetGroup.position.copy(pivotScenePos);
    global.kin3d_targetGroup.quaternion.copy(qScene);
    global.kin3d_targetPose = {
      x: Number(pose.x) || 0,
      y: Number(pose.y) || 0,
      z: Number(pose.z) || 0,
      roll_deg: Number(pose.roll_deg) || 0,
      pitch_deg: Number((pose.pitch_deg !== undefined ? pose.pitch_deg : pose.alpha) || 0),
      yaw_deg: Number(pose.yaw_deg) || 0,
      alpha: Number((pose.pitch_deg !== undefined ? pose.pitch_deg : pose.alpha) || 0)
    };
    global.kin3d_targetPoseInitialized = true;
    global.kin3d_targetPoseSilent = false;
    return true;
  }

  function kin3d_setManipulatorMode(mode) {
    global.kin3d_manipulatorMode = (String(mode || 'translate').trim().toLowerCase() === 'rotate') ? 'rotate' : 'translate';
    if (!global.kin3d_transformControls) return;
    global.kin3d_transformControls.setMode(global.kin3d_manipulatorMode);
    global.kin3d_transformControls.setSpace(global.kin3d_manipulatorMode === 'rotate' ? 'local' : 'world');
    global.kin3d_transformControls.setSize(global.kin3d_manipulatorMode === 'rotate' ? 1.15 : 0.95);
    global.kin3d_transformControls.showX = global.kin3d_manipulatorEnabled;
    global.kin3d_transformControls.showY = global.kin3d_manipulatorEnabled;
    global.kin3d_transformControls.showZ = global.kin3d_manipulatorEnabled;
  }

  function kin3d_setManipulatorEnabled(enabled) {
    global.kin3d_manipulatorEnabled = !!enabled;
    if (global.kin3d_targetGroup) global.kin3d_targetGroup.visible = global.kin3d_manipulatorEnabled;
    if (global.kin3d_transformControls) {
      global.kin3d_transformControls.enabled = global.kin3d_manipulatorEnabled;
      global.kin3d_transformControls.visible = global.kin3d_manipulatorEnabled;
      global.kin3d_transformControls.showX = global.kin3d_manipulatorEnabled;
      global.kin3d_transformControls.showY = global.kin3d_manipulatorEnabled;
      global.kin3d_transformControls.showZ = global.kin3d_manipulatorEnabled;
    }
    if (!global.kin3d_manipulatorEnabled && global.kin3d_controls) global.kin3d_controls.enabled = true;
  }

  global.kin3d_getTargetPivotOffsetLocal = kin3d_getTargetPivotOffsetLocal;
  global.kin3d_getTargetPivotOffsetScene = kin3d_getTargetPivotOffsetScene;
  global.kin3d_robotQuaternionFromEulerDeg = kin3d_robotQuaternionFromEulerDeg;
  global.kin3d_sceneQuaternionFromRobotPose = kin3d_sceneQuaternionFromRobotPose;
  global.kin3d_robotEulerDegFromSceneQuaternion = kin3d_robotEulerDegFromSceneQuaternion;
  global.kin3d_poseFromTransformObject = kin3d_poseFromTransformObject;
  global.kin3d_dispatchTargetPoseFromManipulator = kin3d_dispatchTargetPoseFromManipulator;
  global.kin3d_setTargetPose = kin3d_setTargetPose;
  global.kin3d_setManipulatorMode = kin3d_setManipulatorMode;
  global.kin3d_setManipulatorEnabled = kin3d_setManipulatorEnabled;
})(window);
