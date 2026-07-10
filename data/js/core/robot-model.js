(function(global) {
  var root = global.MROS = global.MROS || {};
  root.robotModel = {
    name: 'mros-7dof-v1',
    revision: 'matlab-mdl_robot_model-2026-04-29',
    n_joints: 7,
    units: { length: 'mm', angle: 'rad' },
    theta_offset_rad: [0, Math.PI / 2, 0, 0, 0, 0, -Math.PI / 2],
    d_mm: [210.40, 0, 0, 202.25, 0, 272.00, 0],
    a_mm: [0, 240.00, 90.00, 0, 0, 0, 160.00],
    alpha_rad: [Math.PI / 2, 0, Math.PI / 2, -Math.PI / 2, Math.PI / 2, -Math.PI / 2, 0],
    q_min_deg: [-270, -90, -90, -90, -90, -90, -90],
    q_max_deg: [270, 90, 90, 90, 90, 90, 90],
    q_center_deg: [0, 0, 0, 0, 0, 0, 0],
    v_max_deg_s: [120, 120, 120, 120, 120, 120, 120],
    a_max_deg_s2: [240, 240, 240, 240, 240, 240, 240],
    ctrl: { dt_s: 0.02, ik_max_iter: 500, ik_pos_tol_mm: 0.5, ik_ori_tol_rad: Math.PI / 360 }
  };
  global.MROS_ROBOT_MODEL = root.robotModel;
})(window);
