(function(global) {
  var root = global.MROS = global.MROS || {};
  root.scene = root.scene || {};

  function kin3d_positionTube(tube, p1, p2) {
    var dir = new THREE.Vector3().subVectors(p2, p1);
    var len = dir.length();
    if (len < 0.01) {
      tube.visible = false;
      return;
    }
    tube.visible = true;
    tube.scale.set(1, len, 1);
    tube.position.copy(new THREE.Vector3().addVectors(p1, p2).multiplyScalar(0.5));
    dir.normalize();
    tube.quaternion.copy(new THREE.Quaternion().setFromUnitVectors(new THREE.Vector3(0, 1, 0), dir));
  }

  root.scene.bootstrap = {
    version: '20260422_structure_r2',
    positionTube: kin3d_positionTube
  };
  global.kin3d_positionTube = kin3d_positionTube;
})(window);
