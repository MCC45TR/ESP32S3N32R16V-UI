(function(global) {
  function kin3d_projectToDOM(element, pos, offsetUp) {
    if (!element || !pos || !global.kin3d_camera) return;
    var vector = new THREE.Vector3(pos.x, pos.z + (offsetUp || 0), pos.y);
    vector.project(global.kin3d_camera);
    var container = document.getElementById('canvas-container');
    var metrics = global.kin3d_getContainerMetrics ? global.kin3d_getContainerMetrics(container) : { width: container.clientWidth, height: container.clientHeight };
    var x = (vector.x * 0.5 + 0.5) * metrics.width;
    var y = (-vector.y * 0.5 + 0.5) * metrics.height;
    element.style.left = x + 'px';
    element.style.top = y + 'px';
    element.style.transform = 'translate(-50%, -50%)';
  }

  function kin3d_poseText(prefix, pose, accent) {
    var p = pose || {};
    return '<span style="font-weight:800;margin-right:8px;color:#fff;background:rgba(255,255,255,0.15);padding:1px 4px;border-radius:2px;">' +
      String(prefix || '') +
      '</span> X: ' + Number(p.x || 0).toFixed(1) +
      ' &nbsp; Y: ' + Number(p.y || 0).toFixed(1) +
      ' &nbsp; Z: ' + Number(p.z || 0).toFixed(1) +
      ' &nbsp; R: <span style="color:' + (accent || '#fff') + ';">' + Number(p.roll_deg || 0).toFixed(1) + '&deg;</span>' +
      ' &nbsp; P: <span style="color:' + (accent || '#fff') + ';">' + Number((p.pitch_deg !== undefined ? p.pitch_deg : p.alpha) || 0).toFixed(1) + '&deg;</span>' +
      ' &nbsp; Y: <span style="color:' + (accent || '#fff') + ';">' + Number(p.yaw_deg || 0).toFixed(1) + '&deg;</span>';
  }

  function updateEELabel(cx, cy, cz, alpha) {
    var pose = (typeof cx === 'object' && cx !== null)
      ? cx
      : { x: cx, y: cy, z: cz, roll_deg: 0, pitch_deg: alpha, yaw_deg: 0, alpha: alpha };
    global.kin3d_p4_coords.x = Number(pose.x) || 0;
    global.kin3d_p4_coords.y = Number(pose.y) || 0;
    global.kin3d_p4_coords.z = Number(pose.z) || 0;
    global.kin3d_p4_coords.alpha = Number((pose.pitch_deg !== undefined ? pose.pitch_deg : pose.alpha) || 0);
    if (global.kin3d_ee_label) {
      global.kin3d_ee_label.innerHTML = kin3d_poseText('ANLIK', pose, '#B5EA6A');
    }
  }

  function kin3d_makeAxisLabel(text, color) {
    var canvas = document.createElement('canvas');
    canvas.width = 128;
    canvas.height = 64;
    var ctx = canvas.getContext('2d');
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.font = 'Bold 36px sans-serif';
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillStyle = color || '#ffffff';
    ctx.shadowColor = 'rgba(0,0,0,0.75)';
    ctx.shadowBlur = 6;
    ctx.fillText(String(text || ''), canvas.width / 2, canvas.height / 2);
    var tex = new THREE.CanvasTexture(canvas);
    var mat = new THREE.SpriteMaterial({ map: tex, transparent: true, depthTest: false });
    var sprite = new THREE.Sprite(mat);
    sprite.scale.set(48, 24, 1);
    return sprite;
  }

  global.kin3d_projectToDOM = kin3d_projectToDOM;
  global.kin3d_poseText = kin3d_poseText;
  global.updateEELabel = updateEELabel;
  global.kin3d_makeAxisLabel = kin3d_makeAxisLabel;
})(window);
