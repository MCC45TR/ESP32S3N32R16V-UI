(function(global) {
  var KEY = 'pickPlaceSim';
  var root = global.MROS = global.MROS || {};
  root.scene = root.scene || {};

  var state = {
    ready: false,
    boxes: [],
    selected: null,
    group: null,
    transform: null,
    raycaster: null,
    pointer: null,
    snapMm: 5,
    mode: 'move',
    nextId: 1
  };

  function getSettings() {
    var src = {};
    if (typeof global.mrosPopupSettingsGetSection === 'function') {
      src = global.mrosPopupSettingsGetSection(KEY) || {};
    } else {
      try { src = JSON.parse(localStorage.getItem(KEY) || '{}') || {}; } catch (e) { src = {}; }
    }
    return {
      snapMm: Math.max(1, Math.min(100, Number(src.snapMm) || 5)),
      boxes: Array.isArray(src.boxes) ? src.boxes : []
    };
  }

  function saveSettings() {
    var boxes = state.boxes.map(function(mesh) {
      return {
        id: mesh.userData.ppId,
        name: mesh.userData.ppName,
        x: Math.round(mesh.position.x * 10) / 10,
        y: Math.round(mesh.position.y * 10) / 10,
        z: Math.round(mesh.position.z * 10) / 10,
        sx: mesh.userData.ppSize.x,
        sy: mesh.userData.ppSize.y,
        sz: mesh.userData.ppSize.z,
        color: mesh.userData.ppColor
      };
    });
    var payload = { snapMm: state.snapMm, boxes: boxes };
    if (typeof global.mrosPopupSettingsSetSection === 'function') {
      global.mrosPopupSettingsSetSection(KEY, payload);
    } else {
      localStorage.setItem(KEY, JSON.stringify(payload));
    }
  }

  function makeMaterial(color, selected) {
    return new THREE.MeshStandardMaterial({
      color: color || 0x6A97EA,
      roughness: 0.58,
      metalness: 0.12,
      transparent: true,
      opacity: selected ? 0.92 : 0.78
    });
  }

  function createBox(spec) {
    if (!state.group || !global.THREE) return null;
    var sx = Math.max(10, Number(spec && spec.sx) || 80);
    var sy = Math.max(10, Number(spec && spec.sy) || 80);
    var sz = Math.max(10, Number(spec && spec.sz) || 80);
    var color = Number(spec && spec.color) || 0x6A97EA;
    var mesh = new THREE.Mesh(new THREE.BoxGeometry(sx, sy, sz), makeMaterial(color, false));
    mesh.position.set(
      Number(spec && spec.x) || 250,
      Number(spec && spec.y) || (sy / 2),
      Number(spec && spec.z) || 0
    );
    mesh.castShadow = true;
    mesh.receiveShadow = true;
    mesh.userData.ppBox = true;
    mesh.userData.ppId = Number(spec && spec.id) || state.nextId++;
    mesh.userData.ppName = String((spec && spec.name) || ('Kutu ' + mesh.userData.ppId));
    mesh.userData.ppSize = { x: sx, y: sy, z: sz };
    mesh.userData.ppColor = color;
    state.group.add(mesh);
    state.boxes.push(mesh);
    state.nextId = Math.max(state.nextId, mesh.userData.ppId + 1);
    return mesh;
  }

  function setSelected(mesh) {
    if (state.selected && state.selected.material) {
      state.selected.material.dispose();
      state.selected.material = makeMaterial(state.selected.userData.ppColor, false);
    }
    state.selected = mesh || null;
    if (state.selected && state.selected.material) {
      state.selected.material.dispose();
      state.selected.material = makeMaterial(state.selected.userData.ppColor, true);
    }
    if (state.transform) {
      if (state.selected) state.transform.attach(state.selected);
      else state.transform.detach();
    }
    updateStatus();
  }

  function updateStatus() {
    var el = document.getElementById('pickplace-status');
    if (!el) return;
    el.textContent = state.selected
      ? (state.selected.userData.ppName + ' @ X ' + state.selected.position.x.toFixed(0) + ' Y ' + state.selected.position.y.toFixed(0) + ' Z ' + state.selected.position.z.toFixed(0))
      : (state.boxes.length + ' kutu');
  }

  function addBox() {
    ensureReady();
    var spread = state.boxes.length * 55;
    var mesh = createBox({
      x: 240 + spread,
      y: 40,
      z: (state.boxes.length % 2 ? 80 : -80),
      sx: 80,
      sy: 80,
      sz: 80,
      color: [0x6A97EA, 0x9BEB5D, 0xF0B55B, 0xD36AEA][state.boxes.length % 4]
    });
    setSelected(mesh);
    saveSettings();
  }

  function duplicateSelected() {
    if (!state.selected) return addBox();
    var s = state.selected;
    var mesh = createBox({
      x: s.position.x + 90,
      y: s.position.y,
      z: s.position.z + 30,
      sx: s.userData.ppSize.x,
      sy: s.userData.ppSize.y,
      sz: s.userData.ppSize.z,
      color: s.userData.ppColor
    });
    setSelected(mesh);
    saveSettings();
  }

  function deleteSelected() {
    if (!state.selected) return;
    var mesh = state.selected;
    setSelected(null);
    state.boxes = state.boxes.filter(function(item) { return item !== mesh; });
    if (state.group) state.group.remove(mesh);
    if (mesh.geometry) mesh.geometry.dispose();
    if (mesh.material) mesh.material.dispose();
    saveSettings();
  }

  function clearBoxes() {
    setSelected(null);
    state.boxes.forEach(function(mesh) {
      if (state.group) state.group.remove(mesh);
      if (mesh.geometry) mesh.geometry.dispose();
      if (mesh.material) mesh.material.dispose();
    });
    state.boxes = [];
    saveSettings();
  }

  function snapSelected() {
    if (!state.selected) return;
    var snap = state.snapMm || 5;
    state.selected.position.x = Math.round(state.selected.position.x / snap) * snap;
    state.selected.position.y = Math.max(state.selected.userData.ppSize.y / 2, Math.round(state.selected.position.y / snap) * snap);
    state.selected.position.z = Math.round(state.selected.position.z / snap) * snap;
    saveSettings();
    updateStatus();
  }

  function handlePointerDown(ev) {
    if (!state.ready || !state.raycaster || !state.pointer) return;
    if (ev.target && ev.target.closest && ev.target.closest('.pickplace-toolbar')) return;
    if (state.transform && state.transform.dragging) return;
    var rect = global.kin3d_renderer.domElement.getBoundingClientRect();
    state.pointer.x = ((ev.clientX - rect.left) / rect.width) * 2 - 1;
    state.pointer.y = -((ev.clientY - rect.top) / rect.height) * 2 + 1;
    state.raycaster.setFromCamera(state.pointer, global.kin3d_camera);
    var hits = state.raycaster.intersectObjects(state.boxes, false);
    if (hits && hits.length) {
      setSelected(hits[0].object);
      ev.preventDefault();
    }
  }

  function mountToolbar() {
    var container = document.getElementById('canvas-container');
    if (!container || document.getElementById('pickplace-toolbar')) return;
    var bar = document.createElement('div');
    bar.id = 'pickplace-toolbar';
    bar.className = 'pickplace-toolbar';
    bar.innerHTML =
      '<button type="button" data-act="add">Kutu Ekle</button>' +
      '<button type="button" data-act="dup">Çoğalt</button>' +
      '<button type="button" data-act="snap">Izgaraya Al</button>' +
      '<button type="button" data-act="del">Sil</button>' +
      '<button type="button" data-act="clear">Temizle</button>' +
      '<span id="pickplace-status">0 kutu</span>';
    var collapseTimer = null;
    function showToolbar() {
      bar.classList.remove('collapsed');
      if (collapseTimer) clearTimeout(collapseTimer);
      collapseTimer = setTimeout(function() {
        if (!bar.matches(':hover')) bar.classList.add('collapsed');
      }, 4000);
    }
    bar.addEventListener('mouseenter', showToolbar);
    bar.addEventListener('mousemove', showToolbar);
    bar.addEventListener('mouseleave', function() {
      if (collapseTimer) clearTimeout(collapseTimer);
      collapseTimer = setTimeout(function() { bar.classList.add('collapsed'); }, 4000);
    });
    bar.addEventListener('click', function(ev) {
      var btn = ev.target && ev.target.closest ? ev.target.closest('button[data-act]') : null;
      if (!btn) return;
      var act = btn.getAttribute('data-act');
      showToolbar();
      if (act === 'add') addBox();
      else if (act === 'dup') duplicateSelected();
      else if (act === 'snap') snapSelected();
      else if (act === 'del') deleteSelected();
      else if (act === 'clear') clearBoxes();
    });
    container.appendChild(bar);
    showToolbar();
  }

  function loadBoxes() {
    var s = getSettings();
    state.snapMm = s.snapMm;
    if (!state.boxes.length && s.boxes.length) {
      s.boxes.forEach(function(box) { createBox(box); });
    }
    updateStatus();
  }

  function ensureReady() {
    if (state.ready) return true;
    if (!global.THREE || !global.kin3d_scene || !global.kin3d_renderer || !global.kin3d_camera) return false;
    state.group = new THREE.Group();
    state.group.name = 'MROS Pick Place Simulation';
    global.kin3d_scene.add(state.group);
    state.raycaster = new THREE.Raycaster();
    state.pointer = new THREE.Vector2();
    if (THREE.TransformControls) {
      state.transform = new THREE.TransformControls(global.kin3d_camera, global.kin3d_renderer.domElement);
      state.transform.setMode('translate');
      state.transform.setSpace('world');
      state.transform.addEventListener('dragging-changed', function(ev) {
        if (global.kin3d_controls) global.kin3d_controls.enabled = !ev.value;
        if (!ev.value) {
          snapSelected();
          saveSettings();
        }
      });
      global.kin3d_scene.add(state.transform);
    }
    global.kin3d_renderer.domElement.addEventListener('pointerdown', handlePointerDown, true);
    mountToolbar();
    loadBoxes();
    state.ready = true;
    return true;
  }

  function boot() {
    if (ensureReady()) return;
    var tries = 0;
    var timer = setInterval(function() {
      tries++;
      if (ensureReady() || tries > 80) clearInterval(timer);
    }, 250);
  }

  document.addEventListener('DOMContentLoaded', boot);
  window.addEventListener('mros-popup-settings-loaded', function() {
    if (!state.ready) return;
    loadBoxes();
  });

  global.mrosPickPlaceAddBox = addBox;
  global.mrosPickPlaceDuplicate = duplicateSelected;
  global.mrosPickPlaceDelete = deleteSelected;
  global.mrosPickPlaceClear = clearBoxes;
  root.scene.pickPlace = {
    addBox: addBox,
    duplicateSelected: duplicateSelected,
    deleteSelected: deleteSelected,
    clear: clearBoxes,
    getBoxes: function() { return state.boxes.slice(); }
  };
})(window);
