// 3D Kinematics Viewer for MROS DEUSCARA
// Uses proper DH forward kinematics matching MATLAB implementation
// Served as /kinematics3d.js (pure JavaScript, no HTML tags)

const char *html_kinematics3d = R"rawliteral(
// ============================================================
// MROS DEUSCARA Kinematics 3D Stick Figure Viewer
// DH Parameters from get_DH_params.m
// Forward Kinematics from calc_forward_kinematics.m
// Joint Positions from extract_joint_positions.m
// ============================================================

var kin3d_scene, kin3d_camera, kin3d_renderer, kin3d_controls;
var kin3d_linkLine, kin3d_jointSpheres = [];
var kin3d_dashLine, kin3d_eeAxes;
var kin3d_ambientLight, kin3d_dirLight, kin3d_fillLight, kin3d_backLight, kin3d_hemiLight;
var kin3d_coordFrames = [];
var kin3d_initialized = false;
var kin3d_jointPositions = []; // Global cache for animate loop
var kin3d_auxOrthoHalfHeight = 700;

// Orientation Gizmo (SolidWorks-style corner axes)
var gizmo_scene, gizmo_camera;

// Auxiliary View Cameras
var kin3d_camTop, kin3d_camSide, kin3d_camFront;

// CAD Models
var kin3d_stlLoader;
var kin3d_gltfLoader;
var kin3d_dracoLoader;
var kin3d_cadLoadingManager;
var kin3d_cadVersion = 'unknown';
var kin3d_cadVersionLabel = null;
var kin3d_mechanicsLabel = null;
var kin3d_cadRoots = {};
var kin3d_cadGhostRoots = {};
var kin3d_raycaster;
var kin3d_planetarySunTeeth = (typeof PLANETARY_SUN_TEETH !== 'undefined') ? Number(PLANETARY_SUN_TEETH) : 12.0;
var kin3d_planetaryPlanetTeeth = (typeof PLANETARY_PLANET_TEETH !== 'undefined') ? Number(PLANETARY_PLANET_TEETH) : 18.0;
var kin3d_planetaryRingTeeth = (typeof PLANETARY_RING_TEETH !== 'undefined') ? Number(PLANETARY_RING_TEETH) : 48.0;
var kin3d_cadContextMenu = null;
var kin3d_cadContextTitle = null;
var kin3d_cadContextOpacityBtn = null;
var kin3d_cadContextMaterialSelect = null;
var kin3d_cadContextMaterialInfo = null;
var kin3d_cadContextMaterialResetBtn = null;
var kin3d_cadContextTargetPartId = null;
var kin3d_cadParts = [
    {
        id: 'fixed_turret_body',
        label: 'Sabit Taret Gövdesi',
        url: '/cad/MROS_BASE_TURRET/MROS_BASE_TURRET.gltf',
        placement: 'world_static',
        centerOnLoad: false,
        calibratable: true,
        calibDeg: { x: 0.0, y: 0.0, z: 0.0 },
        calibPosMm: { x: 0.0, y: 0.0, z: 0.0 },
        pos: { x: 0, y: 0, z: 0 },
        rotDeg: { x: 0, y: 90, z: 0 }
    },
    {
        id: 'turret_holder',
        label: 'Döner Taret Gövdesi',
        url: '/cad/MROS_TOP_TURRET/MROS_TOP_TURRET.gltf',
        placement: 'turret_rotating_height',
        heightMm: 0.0,
        yawSign: -1,
        centerOnLoad: false,
        calibratable: true,
        calibDeg: { x: 0.0, y: 0.0, z: 0.0 },
        calibPosMm: { x: 0.0, y: 0.0, z: 0.0 },
        pos: { x: 0, y: 107.0, z: 0 },
        rotDeg: { x: 0, y: 0, z: 0 }
    },
    {
        id: 'turret_carrier',
        label: 'Döner Taret Taşıyıcı',
        url: '/cad/MROS_TURRET_CARRIER/MROS_TURRET_CARRIER.gltf',
        placement: 'turret_rotating_height',
        heightMm: 0.0,
        yawSign: -1,
        centerOnLoad: false,
        calibratable: true,
        calibDeg: { x: 0.0, y: 0.0, z: 0.0 },
        calibPosMm: { x: 0.0, y: 0.0, z: 0.0 },
        pos: { x: 0, y: 30.0, z: 0 },
        rotDeg: { x: 0, y: 300, z: 0 }
    },
    {
        id: 'turret_top_cover',
        label: 'Döner Taret Gövdesi Kapağı',
        url: '/cad/MROS_TURRET_COVER/MROS_TURRET_COVER.gltf',
        placement: 'turret_rotating_height',
        heightMm: 181.4,
        yawSign: -1,
        centerOnLoad: false,
        calibratable: true,
        calibDeg: { x: 0.0, y: 0.0, z: 0.0 },
        pos: { x: 0, y: 0, z: 0 },
        rotDeg: { x: 0, y: 90, z: 0 }
    },
    {
        id: 'planet_gear_1',
        label: 'Gezegen Dişli 1',
        url: '/cad/MROS_PLANET_GEAR_18T/MROS_PLANET_GEAR_18T.gltf',
        placement: 'turret_planetary_gear',
        yawSign: -1,
        orbitRadiusMm: 45.0,
        orbitDeg: 0.0,
        spinOffsetDeg: 30.0,
        centerOnLoad: true,
        calibratable: true,
        calibDeg: { x: 0.0, y: -30.0, z: 0.0 },
        calibPosMm: { x: 0.0, y: 0.0, z: 0.0 },
        pos: { x: 0, y: 85.0, z: 0 },
        rotDeg: { x: 0, y: -30, z: 0 }
    },
    {
        id: 'planet_gear_2',
        label: 'Gezegen Dişli 2',
        url: '/cad/MROS_PLANET_GEAR_18T/MROS_PLANET_GEAR_18T.gltf',
        placement: 'turret_planetary_gear',
        yawSign: -1,
        orbitRadiusMm: 45.0,
        orbitDeg: 120.0,
        spinOffsetDeg: 30.0,
        centerOnLoad: true,
        calibratable: true,
        calibDeg: { x: 0.0, y: -30.0, z: 0.0 },
        calibPosMm: { x: 0.0, y: 0.0, z: 0.0 },
        pos: { x: 0, y: 85.0, z: 0 },
        rotDeg: { x: 0, y: -30, z: 0 }
    },
    {
        id: 'planet_gear_3',
        label: 'Gezegen Dişli 3',
        url: '/cad/MROS_PLANET_GEAR_18T/MROS_PLANET_GEAR_18T.gltf',
        placement: 'turret_planetary_gear',
        yawSign: -1,
        orbitRadiusMm: 45.0,
        orbitDeg: 240.0,
        spinOffsetDeg: 30.0,
        centerOnLoad: true,
        calibratable: true,
        calibDeg: { x: 0.0, y: -30.0, z: 0.0 },
        calibPosMm: { x: 0.0, y: 0.0, z: 0.0 },
        pos: { x: 0, y: 85.0, z: 0 },
        rotDeg: { x: 0, y: -30, z: 0 }
    },
    {
        id: 'arm1',
        label: 'Birinci Kol',
        url: '/cad/ARM1/ARM1.gltf',
        placement: 'j1_j2_mid',
        centerOnLoad: true,
        calibratable: true,
        calibDeg: { x: -180.0, y: 0.0, z: 270.0 },
        globalRotSeq: [{ axis: 'x', deg: 90 }, { axis: 'y', deg: 90 }],
        pos: { x: 0, y: 0, z: 0 },
        rotDeg: { x: 0, y: 0, z: 0 }
    },
    {
        id: 'arm2',
        label: 'İkinci Kol',
        url: '/cad/ARM2/ARM2.gltf',
        placement: 'j2_center',
        centerOnLoad: true,
        calibratable: true,
        calibDeg: { x: 90.0, y: -90.0, z: 180.0 },
        globalRotSeq: [{ axis: 'z', deg: -90 }, { axis: 'y', deg: -90 }],
        pos: { x: 0, y: 0, z: 0 },
        rotDeg: { x: 0, y: 0, z: 0 }
    },
    {
        id: 'arm3',
        label: 'Üçüncü Kol',
        url: '/cad/ARM3/ARM3.gltf',
        placement: 'j5_center_j4_frame',
        centerOnLoad: true,
        calibratable: true,
        calibDeg: { x: 180.0, y: 0.0, z: -90.0 },
        pos: { x: 0, y: 0, z: 0 },
        rotDeg: { x: 0, y: 0, z: 0 }
    },
    {
        id: 'arm4',
        label: 'Dördüncü Kol',
        url: '/cad/ARM4/ARM4.gltf',
        placement: 'j5_center_j5_frame',
        centerOnLoad: true,
        calibratable: true,
        calibDeg: { x: -90.0, y: 0.0, z: -90.0 },
        pos: { x: 0, y: 0, z: 0 },
        rotDeg: { x: 0, y: 0, z: 0 }
    },
    {
        id: 'arm5',
        label: 'Beşinci Kol',
        url: '/cad/ARM5/ARM5.gltf',
        placement: 'j7_center_j6_frame',
        centerOnLoad: true,
        calibratable: true,
        calibDeg: { x: 0.0, y: 0.0, z: 90.0 },
        pos: { x: 0, y: 0, z: 0 },
        rotDeg: { x: 0, y: 0, z: 0 }
    },
    {
        id: 'arm6',
        label: 'Modüler Uç',
        url: '/cad/ARM6/ARM6.gltf',
        placement: 'j7_center_j7_frame',
        centerOnLoad: true,
        calibratable: true,
        calibDeg: { x: -90.0, y: -90.0, z: 0.0 },
        calibPosMm: { x: 0.0, y: 0.0, z: 10.0 },
        pos: { x: 0, y: 0, z: 0 },
        rotDeg: { x: 0, y: 0, z: 0 }
    }
];

// Temporary per-part local orientation calibration
var kin3d_cadCalibState = {
    activePartId: null,
    stepDeg: 5,
    stepMm: 1
};
var kin3d_cadCalibModal = null;
var kin3d_cadCalibValueEl = null;
var kin3d_cadCalibPartSelect = null;
var kin3d_cadCalibStepInput = null;
var kin3d_cadCalibStepPosInput = null;
var kin3d_cadCalibPlanetExtraWrap = null;
var kin3d_cadCalibPlanetSpacingInput = null;
var kin3d_cadCalibPlanetRadiusInput = null;
var kin3d_cadCalibPlanetSpin1Input = null;
var kin3d_cadCalibPlanetSpin2Input = null;
var kin3d_cadCalibPlanetSpin3Input = null;
var kin3d_cadCalibPlanetInfoEl = null;
var kin3d_cadCalibPlanetCopyBtn = null;

var kin3d_jointNameLabels = [];
var kin3d_jointNames = ["Base", "Turret", "Joint 2", "Joint 3", "Joint 4", "Joint 5", "Joint 6", "Joint 7", "EE"];
var kin3d_collisionLabel = null;
var kin3d_ikBackendLabel = null;
var kin3d_cadCalibBtn = null;
var kin3d_ikBackendMode = 'WEB';
var kin3d_ikFallbackEnabled = true;
var kin3d_collisionMarkers = [];
var kin3d_collisionPairs = [];
var kin3d_collisionLastCheckMs = 0;
var kin3d_collisionCheckIntervalMs = 120;
var kin3d_collisionMinAxisMm = 2.0;
var kin3d_collisionMinVolumeMm3 = 80.0;
var kin3d_collisionIgnoredPairs = {
    'arm1|arm2': true,
    'arm2|arm3': true,
    'arm3|arm4': true,
    'arm4|arm5': true,
    'arm5|arm6': true
};

// Ghost Robot (0.25 opacity IK target)
var ghost_spheres = [], ghost_tubes = [], ghost_eeAxes;
var ghost_visible = false;
var kin3d_ghostPreview = {
    active: false,
    startMs: 0,
    durationMs: 1000,
    jointPath: []
};

// Trajectory path visualization
var traj_pathLine, traj_traceLine, traj_previewLine;
var traj_tracePoints = [];
var traj_maxTrace = 500;

var kin3d_trajSpheres = []; // Render queued trajectory waypoints

// EE Coordinate labels
var kin3d_ee_label = null;
var kin3d_planned_label = null;
var kin3d_t41_coords = { x: 0, y: 0, z: 0, alpha: 0 };

// Scene references for settings
var kin3d_worldAxes, kin3d_grid;
var kin3d_jointAngleLabels = [];
var kin3d_cadRuntimeTag = String(Date.now());
var kin3d_bboxHelpers = {};
var kin3d_lastRenderMs = 0;
var kin3d_navHotkeysInstalled = false;
var kin3d_navKeyState = { ctrl: false, shift: false, meta: false };
var kin3d_mechanics = {
    loaded: false,
    loading: false,
    error: '',
    materials: {},
    materialIndex: {},
    partAssignments: {},
    massProperties: {},
    computedMassProperties: {},
    bearingMounts: [],
    linkFrames: {},
    jointActuatorProfiles: {},
    actuatorDeratingProfiles: {},
    actuatorSpecs: {},
    prrAxisMap: {},
    prrGeometrySeed: {},
    canopenActuatorBusMap: {},
    processFeedModes: {},
    processRecipeTemplates: {},
    processInterlocks: [],
    processValidationMatrix: [],
    robotDataSources: {},
    structuralPolicies: {},
    actuatorTelemetry: {},
    payloadProfiles: {},
    activeProcessModeId: 'dry_run_motion_check',
    lastForceSnapshot: null,
    materialOverrides: {},
    activePayloadId: 'none',
    kinematicState: {},
    todo: []
};

function kin3d_escapeHtml(text) {
    return String(text === undefined || text === null ? '' : text)
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;')
        .replace(/'/g, '&#39;');
}

// Settings state
var kin3d_settings = {
    showWorldAxes: true,
    showGrid: true,
    gridSize: 1200,
    gridDivisions: 24,
    gridOpacity: 0.5,
    gridColor: 0x4D4646,
    gridCenterColor: 0x6A97EA,
    worldAxisSize: 200,
    worldAxisOpacity: 0.95,
    worldAxisColor: 0xFFFFFF,
    localAxisSize: 40,
    localAxisOpacity: 0.95,
    localAxisColor: 0xFFFFFF,
    showLocalFrames: false,
    showJoints: true,
    showPartBase: true,
    showPartTurret: true,
    showPartJ2: true,
    showPartJ3: true,
    showPartJ4: true,
    showPartJ5: true,
    showPartJ6: true,
    showPartJ7: true,
    showPartGripper: true,
    showInnerComponents: true,
    showTrajectory: true,
    showTrajectoryLabels: true,
    trajColor: 0xCC44CC,
    trajDashed: true,
    trajLineWidth: 1.0,
    trajPointDensity: 1.0,
    showGhost: true,
    ghostOpacity: 0.25,
    traceLength: 500,
    startEndMarkerStyle: 'dot',
    showDashLine: true,
    showAngleLabels: true,
    showEELabel: true,
    autoRefreshGhost: true,
    modelWireframe: false,
    modelEdgeOutline: false,
    modelFlatShading: false,
    modelMetalness: 0.30,
    modelRoughness: 0.55,
    perfLodLevel: 'high',
    perfAntialias: true,
    perfPixelRatioCap: 1.5,
    perfFpsLimit: 60,
    perfDisablePostprocess: true,
    cadFrustumCulling: true,
    measureDistance: false,
    measureAngle: false,
    measureShowBBox: false,
    measureSnapMm: 5,
    cadCacheMaxMb: 64,
    cadCacheVersionOnly: true,
    perspective: true,
    cameraFov: 45,
    cameraNear: 1,
    cameraFar: 5000,
    cameraDamping: 0.08,
    cameraAutoRotate: false,
    cameraAutoRotateSpeed: 1.0,
    cameraMinDistance: 100,
    cameraMaxDistance: 2600,
    cameraEnablePan: true,
    cameraNavPreset: 'default',
    linkRadius: 6,
    bgColor: 0x302D2D,
    showGizmo: true,
    showCamTop: true,
    showCamSide: true,
    showCamFront: true,
    showKinematics: false,
    useCAD: true,
    showCollisionAlerts: true,
    showJointNames: true,
    lightAmbient: 0.95,
    lightHemi: 0.45,
    lightKey: 1.05,
    lightFill: 0.75,
    lightBack: 0.55,
    lightExposure: 1.15
};

function kin3d_parseCsv(text) {
    var rows = [];
    var row = [];
    var field = '';
    var quoted = false;
    text = String(text || '');
    for (var i = 0; i < text.length; i++) {
        var ch = text.charAt(i);
        if (quoted) {
            if (ch === '"' && text.charAt(i + 1) === '"') {
                field += '"';
                i++;
            } else if (ch === '"') {
                quoted = false;
            } else {
                field += ch;
            }
        } else if (ch === '"') {
            quoted = true;
        } else if (ch === ',') {
            row.push(field);
            field = '';
        } else if (ch === '\n') {
            row.push(field);
            if (row.length > 1 || String(row[0] || '').trim() !== '') rows.push(row);
            row = [];
            field = '';
        } else if (ch !== '\r') {
            field += ch;
        }
    }
    row.push(field);
    if (row.length > 1 || String(row[0] || '').trim() !== '') rows.push(row);
    if (!rows.length) return [];
    var headers = rows.shift().map(function(h) { return String(h || '').trim(); });
    return rows.map(function(cols) {
        var obj = {};
        for (var j = 0; j < headers.length; j++) obj[headers[j]] = cols[j] !== undefined ? cols[j] : '';
        return obj;
    });
}

function kin3d_numField(obj, key, fallback) {
    var v = Number(obj && obj[key]);
    return isFinite(v) ? v : fallback;
}

function kin3d_nowMs() {
    return (typeof performance !== 'undefined' && performance && typeof performance.now === 'function')
        ? performance.now()
        : Date.now();
}

function kin3d_vecRobotToThree(v) {
    return new THREE.Vector3(v.x || 0, v.z || 0, v.y || 0);
}

function kin3d_dhPointToThree(T, p) {
    var x = (T[0] * p.x) + (T[1] * p.y) + (T[2] * p.z) + T[3];
    var y = (T[4] * p.x) + (T[5] * p.y) + (T[6] * p.z) + T[7];
    var z = (T[8] * p.x) + (T[9] * p.y) + (T[10] * p.z) + T[11];
    return new THREE.Vector3(x, z, y);
}

function kin3d_transformFrameLocalToWorldThree(frameId, localRobot, T_all) {
    var frame = kin3d_mechanics.linkFrames[String(frameId || '')] || null;
    var p = {
        x: (localRobot.x || 0) + kin3d_numField(frame, 'origin_x_mm', 0),
        y: (localRobot.y || 0) + kin3d_numField(frame, 'origin_y_mm', 0),
        z: (localRobot.z || 0) + kin3d_numField(frame, 'origin_z_mm', 0)
    };
    if (!frame || frame.frame_id === 'WORLD' || frame.frame_id === 'J0_BASE' || !T_all || !T_all.length) {
        return kin3d_vecRobotToThree(p);
    }
    var idx = Math.max(0, Math.min(6, Math.floor(kin3d_numField(frame, 'joint_index', 0))));
    return kin3d_dhPointToThree(T_all[idx], p);
}

function kin3d_fetchText(url) {
    return fetch(url, { cache: 'no-store' }).then(function(resp) {
        if (!resp.ok) throw new Error(url + ' HTTP ' + resp.status);
        return resp.text();
    });
}

function kin3d_indexBy(rows, key) {
    var out = {};
    for (var i = 0; i < rows.length; i++) {
        var id = rows[i] && rows[i][key];
        if (id) out[id] = rows[i];
    }
    return out;
}

function kin3d_loadMaterialOverrides() {
    try {
        var raw = localStorage.getItem('mros_kin3d_material_overrides');
        kin3d_mechanics.materialOverrides = raw ? (JSON.parse(raw) || {}) : {};
    } catch (e) {
        kin3d_mechanics.materialOverrides = {};
    }
}

function kin3d_loadActivePayload() {
    try {
        kin3d_mechanics.activePayloadId = localStorage.getItem('mros_kin3d_active_payload') || 'none';
    } catch (e) {
        kin3d_mechanics.activePayloadId = 'none';
    }
}

function kin3d_loadActiveProcessMode() {
    try {
        kin3d_mechanics.activeProcessModeId = localStorage.getItem('mros_kin3d_active_process_mode') || 'dry_run_motion_check';
    } catch (e) {
        kin3d_mechanics.activeProcessModeId = 'dry_run_motion_check';
    }
    if (!kin3d_mechanics.processFeedModes[kin3d_mechanics.activeProcessModeId]) {
        kin3d_mechanics.activeProcessModeId = 'dry_run_motion_check';
    }
}

function kin3d_setActiveProcessMode(modeId) {
    var id = String(modeId || '').trim();
    if (!id || !kin3d_mechanics.processFeedModes[id]) return false;
    kin3d_mechanics.activeProcessModeId = id;
    try { localStorage.setItem('mros_kin3d_active_process_mode', id); } catch (e) {}
    if (kin3d_lastAngles && kin3d_lastAngles.length >= 7) {
        kin3d_updateMechanicalForceSnapshot(kin3d_lastAngles, compute_FK(kin3d_lastAngles));
    }
    kin3d_updateMechanicsLabel();
    return true;
}

function kin3d_getActiveProcessMode() {
    return kin3d_mechanics.processFeedModes[kin3d_mechanics.activeProcessModeId] ||
        kin3d_mechanics.processFeedModes.dry_run_motion_check || null;
}

function kin3d_yes(value) {
    var s = String(value === undefined || value === null ? '' : value).trim().toLowerCase();
    return s === 'yes' || s === 'true' || s === '1' || s === 'required';
}

function kin3d_splitList(value) {
    return String(value || '').split(/[;,]/).map(function(item) {
        return String(item || '').trim();
    }).filter(function(item) { return !!item; });
}

function kin3d_selectProcessModeForInput(input) {
    input = input || {};
    var explicitId = String(input.mode_id || input.modeId || kin3d_mechanics.activeProcessModeId || '').trim();
    if (explicitId && kin3d_mechanics.processFeedModes[explicitId]) return kin3d_mechanics.processFeedModes[explicitId];
    var family = String(input.process_family || input.processFamily || '').trim().toLowerCase();
    var material = String(input.material_family || input.materialFamily || '').trim().toLowerCase();
    var thickness = Number(input.thickness_mm !== undefined ? input.thickness_mm : input.thicknessMm);
    var rows = Object.keys(kin3d_mechanics.processFeedModes || {}).map(function(key) {
        return kin3d_mechanics.processFeedModes[key];
    });
    for (var i = 0; i < rows.length; i++) {
        var row = rows[i];
        var rowFamily = String(row.process_family || '').trim().toLowerCase();
        var rowMaterial = String(row.material_family || '').trim().toLowerCase();
        var minT = kin3d_numField(row, 'thickness_min_mm', -Infinity);
        var maxT = kin3d_numField(row, 'thickness_max_mm', Infinity);
        var familyOk = !family || rowFamily === family;
        var materialOk = !material || rowMaterial === material || rowMaterial === 'all' || rowMaterial.indexOf(material) >= 0;
        var thicknessOk = !isFinite(thickness) || (thickness >= minT && thickness <= maxT);
        if (familyOk && materialOk && thicknessOk) return row;
    }
    return kin3d_getActiveProcessMode();
}

function kin3d_processInterlocksForMode(modeId) {
    var mode = String(modeId || '');
    var out = [];
    var rows = kin3d_mechanics.processInterlocks || [];
    for (var i = 0; i < rows.length; i++) {
        var applies = String(rows[i].applies_to_modes || '');
        var modes = kin3d_splitList(applies);
        if (applies === 'all' || modes.indexOf(mode) >= 0) out.push(rows[i]);
    }
    return out;
}

function kin3d_processTemplateForMode(modeId, thicknessMm) {
    var rows = Object.keys(kin3d_mechanics.processRecipeTemplates || {}).map(function(key) {
        return kin3d_mechanics.processRecipeTemplates[key];
    });
    for (var i = 0; i < rows.length; i++) {
        var row = rows[i];
        if (String(row.mode_id || '') !== String(modeId || '')) continue;
        var minT = kin3d_numField(row, 'thickness_min_mm', -Infinity);
        var maxT = kin3d_numField(row, 'thickness_max_mm', Infinity);
        if (!isFinite(thicknessMm) || (thicknessMm >= minT && thicknessMm <= maxT)) return row;
    }
    return null;
}

function kin3d_axisSpeedLimitFeedMmMin() {
    var minFeed = Infinity;
    var axes = Object.keys(kin3d_mechanics.prrAxisMap || {});
    for (var i = 0; i < axes.length; i++) {
        var axis = kin3d_mechanics.prrAxisMap[axes[i]];
        var type = String(axis.kinematic_type || '').toLowerCase();
        var nativeSpeed = kin3d_numField(axis, 'max_speed_native', 0);
        var nativeUnit = String(axis.max_speed_native_unit || '').toLowerCase();
        if (type.indexOf('prismatic') >= 0 && nativeUnit === 'mm/min' && nativeSpeed > 0) {
            minFeed = Math.min(minFeed, nativeSpeed);
        }
    }
    return isFinite(minFeed) ? minFeed : 0;
}

function kin3d_computeProcessFeedPlan(input) {
    input = input || {};
    var mode = kin3d_selectProcessModeForInput(input);
    if (!mode) {
        return {
            schema: 'mros.robot_process.feed_plan.v1',
            status: 'DATA_MISSING',
            reason: 'no process mode loaded'
        };
    }
    var thicknessMm = Number(input.thickness_mm !== undefined ? input.thickness_mm : input.thicknessMm);
    var requestedFeed = Number(input.requested_feed_mm_min !== undefined ? input.requested_feed_mm_min : input.requestedFeedMmMin);
    var vendorFeed = Number(input.vendor_feed_mm_min !== undefined ? input.vendor_feed_mm_min : input.vendorFeedMmMin);
    var curvatureFeed = Number(input.path_curvature_feed_limit_mm_min !== undefined ? input.path_curvature_feed_limit_mm_min : input.pathCurvatureFeedLimitMmMin);
    var operatorFeed = Number(input.operator_feed_limit_mm_min !== undefined ? input.operator_feed_limit_mm_min : input.operatorFeedLimitMmMin);
    var minFeed = kin3d_numField(mode, 'feed_min_mm_min', 0);
    var nominalFeed = kin3d_numField(mode, 'feed_nominal_mm_min', minFeed);
    var maxFeed = kin3d_numField(mode, 'feed_max_mm_min', nominalFeed);
    var baseFeed = isFinite(requestedFeed) && requestedFeed > 0 ? requestedFeed : nominalFeed;
    var clampReasons = [];
    var clamped = Math.max(minFeed, Math.min(maxFeed, baseFeed));
    if (clamped !== baseFeed) clampReasons.push('mode_feed_bounds');
    if (isFinite(vendorFeed) && vendorFeed > 0 && vendorFeed < clamped) {
        clamped = vendorFeed;
        clampReasons.push('vendor_chart_feed');
    }
    if (isFinite(curvatureFeed) && curvatureFeed > 0 && curvatureFeed < clamped) {
        clamped = curvatureFeed;
        clampReasons.push('path_curvature_limit');
    }
    if (isFinite(operatorFeed) && operatorFeed > 0 && operatorFeed < clamped) {
        clamped = operatorFeed;
        clampReasons.push('operator_limit');
    }
    var verticalMotionMm = Number(input.vertical_motion_mm !== undefined ? input.vertical_motion_mm : input.verticalMotionMm);
    var includePrismaticLimit = input.include_prismatic_axis_limit === true ||
        input.includePrismaticAxisLimit === true ||
        (isFinite(verticalMotionMm) && Math.abs(verticalMotionMm) > 0);
    var prismaticFeedLimit = includePrismaticLimit ? kin3d_axisSpeedLimitFeedMmMin() : 0;
    if (prismaticFeedLimit > 0 && prismaticFeedLimit < clamped) {
        clamped = prismaticFeedLimit;
        clampReasons.push('prismatic_axis_speed_limit');
    }
    var template = kin3d_processTemplateForMode(mode.mode_id, thicknessMm);
    var requiresVendor = kin3d_yes(mode.status === 'vendor_chart_required' ? 'yes' : (template && template.requires_vendor_chart_id));
    var requiresWps = kin3d_yes(mode.status === 'vendor_wps_required' ? 'yes' : (template && template.requires_wps_id));
    var interlocks = kin3d_processInterlocksForMode(mode.mode_id);
    var missingQualifications = [];
    if (requiresVendor && !input.vendor_chart_id && !input.machine_vendor_chart_id) missingQualifications.push('vendor_chart_id');
    if (requiresWps && !input.wps_id) missingQualifications.push('wps_id');
    if (kin3d_yes(template && template.requires_dry_run) && !input.dry_run_passed) missingQualifications.push('dry_run_passed');
    var volts = Number(input.voltage_v !== undefined ? input.voltage_v : input.voltageV);
    var amps = Number(input.current_a !== undefined ? input.current_a : input.currentA);
    var eff = Number(input.process_efficiency !== undefined ? input.process_efficiency : input.processEfficiency);
    if (!isFinite(eff) || eff <= 0) eff = 0.8;
    var heatInputKjMm = null;
    if (isFinite(volts) && volts > 0 && isFinite(amps) && amps > 0 && clamped > 0) {
        heatInputKjMm = volts * amps * 60 * eff / (1000 * clamped);
    }
    var status = missingQualifications.length ? 'QUALIFICATION_REQUIRED' : 'READY_FOR_PLANNING';
    if (String(mode.mode_id || '').indexOf('dry_run') >= 0) status = 'READY_FOR_DRY_RUN';
    return {
        schema: 'mros.robot_process.feed_plan.v1',
        generatedAtMs: Date.now(),
        status: status,
        mode_id: mode.mode_id || '',
        display_name_tr: mode.display_name_tr || '',
        process_family: mode.process_family || '',
        tooling_family: mode.tooling_family || '',
        material_family: input.material_family || input.materialFamily || mode.material_family || '',
        material_thickness_mm: isFinite(thicknessMm) ? thicknessMm : null,
        requested_feed_mm_min: isFinite(requestedFeed) ? requestedFeed : null,
        nominal_feed_mm_min: nominalFeed,
        planned_feed_mm_min: clamped,
        feed_bounds_mm_min: { min: minFeed, max: maxFeed },
        prismatic_axis_feed_limit_mm_min: prismaticFeedLimit || null,
        clamp_reasons: clampReasons,
        template_id: template ? (template.template_id || '') : '',
        requires_vendor_chart_id: requiresVendor,
        requires_wps_id: requiresWps,
        missing_qualifications: missingQualifications,
        heat_input_kj_mm: heatInputKjMm,
        kerf_width_mm_seed: kin3d_numField(mode, 'kerf_width_mm_seed', 0),
        stand_off_mm_seed: kin3d_numField(mode, 'stand_off_mm_seed', 0),
        pierce_delay_s_seed: kin3d_numField(mode, 'pierce_delay_s_seed', 0),
        sync_tolerance_mm: kin3d_numField(mode, 'sync_tolerance_mm', 0),
        accel_limit_mm_s2_seed: kin3d_numField(mode, 'accel_limit_mm_s2_seed', 0),
        corner_speed_factor: kin3d_numField(mode, 'corner_speed_factor', 1),
        required_interlocks: interlocks.map(function(row) {
            return {
                interlock_id: row.interlock_id || '',
                authority: row.authority || '',
                fail_action: row.fail_action || '',
                blocking_for_process_energy: row.blocking_for_process_energy || ''
            };
        }),
        safety_notes: mode.safety_notes || '',
        source_basis: mode.source_basis || ''
    };
}

function kin3d_loadActuatorTelemetry() {
    try {
        var raw = localStorage.getItem('mros_kin3d_actuator_telemetry');
        kin3d_mechanics.actuatorTelemetry = raw ? (JSON.parse(raw) || {}) : {};
    } catch (e) {
        kin3d_mechanics.actuatorTelemetry = {};
    }
}

function kin3d_saveActuatorTelemetry() {
    try {
        localStorage.setItem('mros_kin3d_actuator_telemetry', JSON.stringify(kin3d_mechanics.actuatorTelemetry || {}));
    } catch (e) {}
}

function kin3d_setActuatorTelemetry(jointId, telemetry) {
    var id = String(jointId || '').trim().toUpperCase();
    if (!id) return false;
    var current = kin3d_mechanics.actuatorTelemetry[id] || {};
    var next = {};
    for (var k in current) next[k] = current[k];
    telemetry = telemetry || {};
    var allowed = [
        'motor_temp_c', 'controller_temp_c', 'bus_voltage_v',
        'phase_current_a', 'q_current_a', 'ambient_temp_c',
        'source', 'timestamp_ms'
    ];
    for (var i = 0; i < allowed.length; i++) {
        var key = allowed[i];
        if (telemetry[key] !== undefined && telemetry[key] !== null && telemetry[key] !== '') next[key] = telemetry[key];
    }
    if (!next.timestamp_ms) next.timestamp_ms = Date.now();
    if (!next.source) next.source = 'browser_runtime_override';
    kin3d_mechanics.actuatorTelemetry[id] = next;
    kin3d_saveActuatorTelemetry();
    if (kin3d_lastAngles && kin3d_lastAngles.length >= 7) {
        kin3d_updateMechanicalForceSnapshot(kin3d_lastAngles, compute_FK(kin3d_lastAngles));
    }
    kin3d_updateMechanicsLabel();
    return true;
}

function kin3d_clearActuatorTelemetry(jointId) {
    var id = String(jointId || '').trim().toUpperCase();
    if (!id) return false;
    delete kin3d_mechanics.actuatorTelemetry[id];
    kin3d_saveActuatorTelemetry();
    if (kin3d_lastAngles && kin3d_lastAngles.length >= 7) {
        kin3d_updateMechanicalForceSnapshot(kin3d_lastAngles, compute_FK(kin3d_lastAngles));
    }
    kin3d_updateMechanicsLabel();
    return true;
}

function kin3d_setActivePayload(payloadId) {
    var id = String(payloadId || 'none');
    if (!kin3d_mechanics.payloadProfiles[id]) id = 'none';
    kin3d_mechanics.activePayloadId = id;
    try { localStorage.setItem('mros_kin3d_active_payload', id); } catch (e) {}
    if (kin3d_lastAngles && kin3d_lastAngles.length >= 7) {
        kin3d_updateMechanicalForceSnapshot(kin3d_lastAngles, compute_FK(kin3d_lastAngles));
    }
    kin3d_updateMechanicsLabel();
}

function kin3d_saveMaterialOverrides() {
    try {
        localStorage.setItem('mros_kin3d_material_overrides', JSON.stringify(kin3d_mechanics.materialOverrides || {}));
    } catch (e) {}
}

function kin3d_getMaterialDisplayName(materialId) {
    var id = String(materialId || '');
    var idx = kin3d_mechanics.materialIndex[id] || {};
    return (idx.display_name || idx.standard_designation || id || '').trim();
}

function kin3d_getMaterialSummary(materialId) {
    var id = String(materialId || '');
    var mat = kin3d_mechanics.materials[id] || null;
    if (!id || !mat) return 'Malzeme verisi yok';
    var density = kin3d_numField(mat, 'density_kg_m3', 0);
    var e = kin3d_numField(mat, 'young_modulus_gpa', 0);
    var yieldMpa = kin3d_numField(mat, 'yield_strength_mpa', 0);
    return id + ' | rho ' + density.toFixed(0) + ' kg/m3 | E ' +
        e.toFixed(1) + ' GPa | Sy ' + yieldMpa.toFixed(0) + ' MPa';
}

function kin3d_applyPartMaterial(part, materialId, source) {
    if (!part || !materialId) return false;
    var id = String(materialId || '').trim();
    if (!kin3d_mechanics.materials[id] && !kin3d_mechanics.materialIndex[id]) return false;
    part.materialId = id;
    part.materialOverrideSource = source || '';
    part.materialMeta = kin3d_mechanics.materialIndex[id] || null;
    part.mechanicalMaterial = kin3d_mechanics.materials[id] || null;
    delete kin3d_mechanics.computedMassProperties[part.id];
    if (kin3d_cadRoots[part.id]) {
        kin3d_cadRoots[part.id].userData.mrosMaterialId = id;
        kin3d_cadRoots[part.id].userData.mrosMaterialOverrideSource = source || '';
    }
    return true;
}

function kin3d_setPartMaterialOverride(partId, materialId) {
    var part = kin3d_getCadPartById(partId);
    if (!part || !materialId) return;
    if (!kin3d_applyPartMaterial(part, materialId, 'browser_override')) return;
    kin3d_mechanics.materialOverrides[partId] = String(materialId);
    kin3d_saveMaterialOverrides();
    kin3d_refreshMechanicalEstimatesFromCad();
    if (kin3d_lastAngles && kin3d_lastAngles.length >= 7) {
        kin3d_updateMechanicalForceSnapshot(kin3d_lastAngles, compute_FK(kin3d_lastAngles));
    }
    kin3d_updateCadContextMenuText();
}

function kin3d_clearPartMaterialOverride(partId) {
    if (!partId) return;
    delete kin3d_mechanics.materialOverrides[partId];
    kin3d_saveMaterialOverrides();
    kin3d_applyMechanicalSidecars();
    kin3d_refreshMechanicalEstimatesFromCad();
    if (kin3d_lastAngles && kin3d_lastAngles.length >= 7) {
        kin3d_updateMechanicalForceSnapshot(kin3d_lastAngles, compute_FK(kin3d_lastAngles));
    }
    kin3d_updateCadContextMenuText();
}

function kin3d_applyMaterialOverrides() {
    var overrides = kin3d_mechanics.materialOverrides || {};
    for (var partId in overrides) {
        var part = kin3d_getCadPartById(partId);
        if (part) kin3d_applyPartMaterial(part, overrides[partId], 'browser_override');
    }
}

function kin3d_csvEscape(value) {
    var s = String(value === undefined || value === null ? '' : value);
    return /[",\r\n]/.test(s) ? ('"' + s.replace(/"/g, '""') + '"') : s;
}

function kin3d_exportMaterialAssignmentsCsv() {
    var rows = ['part_id,label,link_frame_id,material_id,assignment_status,notes'];
    for (var i = 0; i < kin3d_cadParts.length; i++) {
        var part = kin3d_cadParts[i];
        var override = kin3d_mechanics.materialOverrides && kin3d_mechanics.materialOverrides[part.id];
        rows.push([
            part.id || '',
            part.label || '',
            part.linkFrameId || '',
            part.materialId || '',
            override ? 'browser_override_reviewed' : 'csv_assignment',
            override ? 'Browser override exported; review before committing to data/robot/part_material_assignments.csv.' : 'Existing CSV assignment.'
        ].map(kin3d_csvEscape).join(','));
    }
    return rows.join('\n');
}

function kin3d_exportBearingLoadsJson() {
    var snap = kin3d_mechanics.lastForceSnapshot || {};
    return JSON.stringify({
        schema: 'mros.robot_runtime_loads.v1',
        source: 'web_cad_force_snapshot',
        description: 'Bearing loads and joint actuator margins exported from the browser CAD/dynamics snapshot.',
        generatedAtMs: Date.now(),
        jointAnglesDeg: snap.jointAnglesDeg || [],
        dynamics: snap.dynamics || {},
        activePayloadId: snap.activePayloadId || kin3d_mechanics.activePayloadId || 'none',
        activeProcessModeId: snap.activeProcessModeId || kin3d_mechanics.activeProcessModeId || 'dry_run_motion_check',
        processFeedPlan: snap.processFeedPlan || kin3d_computeProcessFeedPlan({ mode_id: kin3d_mechanics.activeProcessModeId }),
        prrAxisMap: snap.prrAxisMap || kin3d_mechanics.prrAxisMap || {},
        canopenActuatorBusMap: snap.canopenActuatorBusMap || kin3d_mechanics.canopenActuatorBusMap || {},
        jointLoads: snap.jointLoads || [],
        worstJointStatus: snap.worstJointStatus || 'UNKNOWN',
        structuralChecks: snap.structuralChecks || [],
        worstStructuralStatus: snap.worstStructuralStatus || 'UNKNOWN',
        bearingLoads: (snap.bearingLoads || []).map(function(row) {
            return {
                bearing_mount_id: row.placement_id || row.bearing_mount_id || '',
                radial_load_N: Number(row.radial_load_N || row.estimated_static_load_n || 0),
                axial_load_N: Number(row.axial_load_N || 0),
                equivalent_load_N: Number(row.equivalent_load_N || 0),
                estimated_static_load_n: Number(row.estimated_static_load_n || 0),
                source: row.source || 'web_cad_static_equilibrium',
                ttl_ms: 2000
            };
        })
    }, null, 2);
}

function kin3d_downloadRobotRuntimeLoadsJson(filename) {
    var name = filename || 'force-snapshot.json';
    var text = kin3d_exportBearingLoadsJson();
    var blob = new Blob([text], { type: 'application/json' });
    var url = URL.createObjectURL(blob);
    var a = document.createElement('a');
    a.href = url;
    a.download = name;
    a.style.display = 'none';
    document.body.appendChild(a);
    a.click();
    document.body.removeChild(a);
    setTimeout(function() { URL.revokeObjectURL(url); }, 1000);
    return text;
}

function kin3d_applyMechanicalSidecars() {
    var mech = kin3d_mechanics;
    if (!mech || !mech.loaded) return;
    for (var i = 0; i < kin3d_cadParts.length; i++) {
        var part = kin3d_cadParts[i];
        var assignment = mech.partAssignments[part.id];
        if (assignment) {
            part.linkFrameId = assignment.link_frame_id || part.linkFrameId || '';
            kin3d_applyPartMaterial(part, assignment.material_id || part.materialId || '', 'csv_assignment');
        }
        var mass = mech.massProperties[part.id];
        if (mass) {
            part.massKg = kin3d_numField(mass, 'mass_kg', 0);
            part.centerOfMassLocalMm = {
                x: kin3d_numField(mass, 'com_x_mm', 0),
                y: kin3d_numField(mass, 'com_y_mm', 0),
                z: kin3d_numField(mass, 'com_z_mm', 0)
            };
            part.inertiaTensorLocalKgM2 = {
                Ixx: kin3d_numField(mass, 'Ixx_kg_m2', 0),
                Ixy: kin3d_numField(mass, 'Ixy_kg_m2', 0),
                Ixz: kin3d_numField(mass, 'Ixz_kg_m2', 0),
                Iyy: kin3d_numField(mass, 'Iyy_kg_m2', 0),
                Iyz: kin3d_numField(mass, 'Iyz_kg_m2', 0),
                Izz: kin3d_numField(mass, 'Izz_kg_m2', 0)
            };
            part.massStatus = mass.status || 'unknown';
        }
        if (kin3d_cadRoots[part.id]) {
            kin3d_cadRoots[part.id].userData.mrosMaterialId = part.materialId || '';
            kin3d_cadRoots[part.id].userData.mrosLinkFrameId = part.linkFrameId || '';
        }
    }
    kin3d_applyMaterialOverrides();
}

function kin3d_loadMechanicalData() {
    if (kin3d_mechanics.loading || kin3d_mechanics.loaded) return;
    kin3d_mechanics.loading = true;
    Promise.all([
        kin3d_fetchText('/materials/materials_index.csv'),
        kin3d_fetchText('/materials/materials_mechanical.csv'),
        kin3d_fetchText('/robot-data/part_material_assignments.csv'),
        kin3d_fetchText('/robot-data/part_mass_properties.csv'),
        kin3d_fetchText('/robot-data/bearing_mounts.csv'),
        kin3d_fetchText('/robot-data/link_frames.csv'),
        kin3d_fetchText('/robot-data/joint_actuator_profiles.csv'),
        kin3d_fetchText('/robot-data/actuator_derating_profiles.csv'),
        kin3d_fetchText('/robot-data/atar_m_series_actuator_specs.csv'),
        kin3d_fetchText('/robot-data/prr_scara_axis_map.csv'),
        kin3d_fetchText('/robot-data/prr_scara_geometry_seed.csv'),
        kin3d_fetchText('/robot-data/canopen_actuator_bus_map.csv'),
        kin3d_fetchText('/robot-data/robot_process_feed_modes.csv'),
        kin3d_fetchText('/robot-data/robot_process_recipe_templates.csv'),
        kin3d_fetchText('/robot-data/robot_process_interlocks.csv'),
        kin3d_fetchText('/robot-data/robot_process_validation_matrix.csv'),
        kin3d_fetchText('/robot-data/robot_data_sources.csv'),
        kin3d_fetchText('/robot-data/structural_design_policies.csv'),
        kin3d_fetchText('/robot-data/payload_profiles.csv'),
        kin3d_fetchText('/robot-data/analysis_todo.json')
    ]).then(function(values) {
        var materialIndexRows = kin3d_parseCsv(values[0]);
        var materialRows = kin3d_parseCsv(values[1]);
        var assignments = kin3d_parseCsv(values[2]);
        var masses = kin3d_parseCsv(values[3]);
        kin3d_mechanics.materialIndex = kin3d_indexBy(materialIndexRows, 'material_id');
        kin3d_mechanics.materials = kin3d_indexBy(materialRows, 'material_id');
        kin3d_mechanics.partAssignments = kin3d_indexBy(assignments, 'part_id');
        kin3d_mechanics.massProperties = kin3d_indexBy(masses, 'part_id');
        kin3d_mechanics.bearingMounts = kin3d_parseCsv(values[4]);
        kin3d_mechanics.linkFrames = kin3d_indexBy(kin3d_parseCsv(values[5]), 'frame_id');
        kin3d_mechanics.jointActuatorProfiles = kin3d_indexBy(kin3d_parseCsv(values[6]), 'joint_id');
        kin3d_mechanics.actuatorDeratingProfiles = kin3d_indexBy(kin3d_parseCsv(values[7]), 'joint_id');
        kin3d_mechanics.actuatorSpecs = kin3d_indexBy(kin3d_parseCsv(values[8]), 'model');
        kin3d_mechanics.prrAxisMap = kin3d_indexBy(kin3d_parseCsv(values[9]), 'axis_id');
        kin3d_mechanics.prrGeometrySeed = kin3d_indexBy(kin3d_parseCsv(values[10]), 'parameter_id');
        kin3d_mechanics.canopenActuatorBusMap = kin3d_indexBy(kin3d_parseCsv(values[11]), 'axis_id');
        kin3d_mechanics.processFeedModes = kin3d_indexBy(kin3d_parseCsv(values[12]), 'mode_id');
        kin3d_mechanics.processRecipeTemplates = kin3d_indexBy(kin3d_parseCsv(values[13]), 'template_id');
        kin3d_mechanics.processInterlocks = kin3d_parseCsv(values[14]);
        kin3d_mechanics.processValidationMatrix = kin3d_parseCsv(values[15]);
        kin3d_mechanics.robotDataSources = kin3d_indexBy(kin3d_parseCsv(values[16]), 'source_id');
        kin3d_mechanics.structuralPolicies = kin3d_indexBy(kin3d_parseCsv(values[17]), 'policy_id');
        kin3d_mechanics.payloadProfiles = kin3d_indexBy(kin3d_parseCsv(values[18]), 'payload_id');
        kin3d_loadMaterialOverrides();
        kin3d_loadActivePayload();
        kin3d_loadActiveProcessMode();
        kin3d_loadActuatorTelemetry();
        try {
            var todo = JSON.parse(values[19]);
            kin3d_mechanics.todo = (todo && Array.isArray(todo.items)) ? todo.items : [];
        } catch (e) {
            kin3d_mechanics.todo = [];
        }
        kin3d_mechanics.loaded = true;
        kin3d_mechanics.error = '';
        kin3d_applyMechanicalSidecars();
        kin3d_refreshMechanicalEstimatesFromCad();
        if (kin3d_lastAngles && kin3d_lastAngles.length >= 7) {
            kin3d_updateMechanicalForceSnapshot(kin3d_lastAngles, compute_FK(kin3d_lastAngles));
        }
        kin3d_updateMechanicsLabel();
        if (window && typeof window.dispatchEvent === 'function') {
            var evt = (typeof CustomEvent === 'function')
                ? new CustomEvent('mros-mechanics-loaded', { detail: kin3d_mechanics })
                : null;
            if (evt) window.dispatchEvent(evt);
        }
    }).catch(function(err) {
        kin3d_mechanics.error = String(err && err.message ? err.message : err);
        console.warn('Mekanik veri yuklenemedi:', kin3d_mechanics.error);
        kin3d_updateMechanicsLabel();
    }).finally(function() {
        kin3d_mechanics.loading = false;
        kin3d_updateMechanicsLabel();
    });
}

function kin3d_getMechanicsSnapshot() {
    return {
        loaded: !!kin3d_mechanics.loaded,
        loading: !!kin3d_mechanics.loading,
        error: kin3d_mechanics.error || '',
        materialCount: Object.keys(kin3d_mechanics.materials || {}).length,
        assignedPartCount: Object.keys(kin3d_mechanics.partAssignments || {}).length,
        massPropertyCount: Object.keys(kin3d_mechanics.massProperties || {}).length,
        computedMassPropertyCount: Object.keys(kin3d_mechanics.computedMassProperties || {}).length,
        bearingMountCount: (kin3d_mechanics.bearingMounts || []).length,
        actuatorDeratingProfileCount: Object.keys(kin3d_mechanics.actuatorDeratingProfiles || {}).length,
        actuatorSpecCount: Object.keys(kin3d_mechanics.actuatorSpecs || {}).length,
        prrAxisCount: Object.keys(kin3d_mechanics.prrAxisMap || {}).length,
        processModeCount: Object.keys(kin3d_mechanics.processFeedModes || {}).length,
        processTemplateCount: Object.keys(kin3d_mechanics.processRecipeTemplates || {}).length,
        processInterlockCount: (kin3d_mechanics.processInterlocks || []).length,
        structuralPolicyCount: Object.keys(kin3d_mechanics.structuralPolicies || {}).length,
        actuatorTelemetryCount: Object.keys(kin3d_mechanics.actuatorTelemetry || {}).length,
        totalMassKg: kin3d_mechanics.lastForceSnapshot ? kin3d_mechanics.lastForceSnapshot.totalMassKg : 0,
        totalWeightN: kin3d_mechanics.lastForceSnapshot ? kin3d_mechanics.lastForceSnapshot.totalWeightN : 0,
        activePayloadId: kin3d_mechanics.activePayloadId || 'none',
        activeProcessModeId: kin3d_mechanics.activeProcessModeId || 'dry_run_motion_check',
        activeProcessFeedPlan: kin3d_computeProcessFeedPlan({ mode_id: kin3d_mechanics.activeProcessModeId, dry_run_passed: false }),
        prrAxisMap: kin3d_mechanics.prrAxisMap || {},
        canopenActuatorBusMap: kin3d_mechanics.canopenActuatorBusMap || {},
        bearingLoads: kin3d_mechanics.lastForceSnapshot ? kin3d_mechanics.lastForceSnapshot.bearingLoads : [],
        jointLoads: kin3d_mechanics.lastForceSnapshot ? kin3d_mechanics.lastForceSnapshot.jointLoads : [],
        structuralChecks: kin3d_mechanics.lastForceSnapshot ? kin3d_mechanics.lastForceSnapshot.structuralChecks : [],
        worstJointStatus: kin3d_mechanics.lastForceSnapshot ? kin3d_mechanics.lastForceSnapshot.worstJointStatus : 'UNKNOWN',
        worstStructuralStatus: kin3d_mechanics.lastForceSnapshot ? kin3d_mechanics.lastForceSnapshot.worstStructuralStatus : 'UNKNOWN',
        todo: kin3d_mechanics.todo || []
    };
}

function kin3d_triangleSignedVolumeMm3(a, b, c) {
    return a.dot(new THREE.Vector3().crossVectors(b, c)) / 6.0;
}

function kin3d_estimateCadVolumeMm3(root) {
    if (!root) return { volumeMm3: 0, method: 'missing_cad' };
    var volumeMm3 = 0;
    root.updateMatrixWorld(true);
    root.traverse(function(node) {
        if (!node || !node.isMesh || !node.geometry || !node.geometry.attributes || !node.geometry.attributes.position) return;
        var geom = node.geometry;
        var pos = geom.attributes.position;
        var index = geom.index;
        var meshVolume = 0;
        var a = new THREE.Vector3();
        var b = new THREE.Vector3();
        var c = new THREE.Vector3();
        var readVertex = function(vertexIndex, out) {
            out.fromBufferAttribute(pos, vertexIndex);
            out.applyMatrix4(node.matrixWorld);
            return out;
        };
        if (index && index.count >= 3) {
            for (var i = 0; i + 2 < index.count; i += 3) {
                readVertex(index.getX(i), a);
                readVertex(index.getX(i + 1), b);
                readVertex(index.getX(i + 2), c);
                meshVolume += kin3d_triangleSignedVolumeMm3(a, b, c);
            }
        } else {
            for (var j = 0; j + 2 < pos.count; j += 3) {
                readVertex(j, a);
                readVertex(j + 1, b);
                readVertex(j + 2, c);
                meshVolume += kin3d_triangleSignedVolumeMm3(a, b, c);
            }
        }
        volumeMm3 += Math.abs(meshVolume);
    });
    if (volumeMm3 > 0) return { volumeMm3: volumeMm3, method: 'cad_mesh_volume' };

    var box = new THREE.Box3().setFromObject(root);
    var size = new THREE.Vector3();
    box.getSize(size);
    var bboxVolumeMm3 = Math.max(0, size.x * size.y * size.z);
    return { volumeMm3: bboxVolumeMm3 * 0.12, method: 'bbox_12pct_fallback' };
}

function kin3d_isFrameDescendant(frameId, ancestorFrameId) {
    var cur = String(frameId || '');
    var ancestor = String(ancestorFrameId || '');
    if (!cur || !ancestor) return false;
    var guard = 0;
    while (cur && guard++ < 16) {
        if (cur === ancestor) return true;
        var frame = kin3d_mechanics.linkFrames[cur] || null;
        cur = frame ? String(frame.parent_frame || '') : '';
    }
    return false;
}

function kin3d_transformFrameVectorToWorldThree(frameId, localRobotVec, T_all) {
    var origin = kin3d_transformFrameLocalToWorldThree(frameId, {x: 0, y: 0, z: 0}, T_all);
    var tip = kin3d_transformFrameLocalToWorldThree(frameId, localRobotVec || {x: 0, y: 0, z: 1}, T_all);
    var vec = tip.clone().sub(origin);
    if (vec.lengthSq() <= 1e-9) return new THREE.Vector3(0, 0, 1);
    return vec.normalize();
}

function kin3d_groupBearingMounts() {
    var groups = {};
    var order = [];
    var mounts = kin3d_mechanics.bearingMounts || [];
    for (var i = 0; i < mounts.length; i++) {
        var mount = mounts[i];
        var key = mount.bearing_spacing_pair_id || mount.bearing_mount_id || ('bearing-' + i);
        if (!groups[key]) {
            groups[key] = {
                id: key,
                joint_id: mount.joint_id || '',
                link_frame_id: mount.link_frame_id || '',
                mounts: []
            };
            order.push(key);
        }
        groups[key].mounts.push(mount);
    }
    return order.map(function(key) { return groups[key]; });
}

function kin3d_vectorToObject(v) {
    return { x: v.x, y: v.y, z: v.z };
}

function kin3d_clampVectorLength(vec, maxLen) {
    if (!vec || !(maxLen > 0)) return vec;
    var len = vec.length();
    if (len > maxLen) vec.multiplyScalar(maxLen / len);
    return vec;
}

function kin3d_updatePartKinematics(partId, comWorldMm, nowMs) {
    var key = String(partId || '');
    var prev = kin3d_mechanics.kinematicState[key] || null;
    var sample = {
        velocity_m_s: new THREE.Vector3(0, 0, 0),
        acceleration_m_s2: new THREE.Vector3(0, 0, 0),
        dynamic_valid: false,
        dt_s: 0
    };
    if (prev && prev.pos && prev.vel && isFinite(prev.t)) {
        var dt = (nowMs - prev.t) / 1000.0;
        if (dt >= 0.005 && dt <= 0.500) {
            var vel = comWorldMm.clone().sub(prev.pos).multiplyScalar(0.001 / dt);
            var accel = vel.clone().sub(prev.vel).multiplyScalar(1.0 / dt);
            kin3d_clampVectorLength(accel, 250.0);
            if (prev.accel) {
                accel.multiplyScalar(0.35).add(prev.accel.clone().multiplyScalar(0.65));
            }
            sample.velocity_m_s = vel;
            sample.acceleration_m_s2 = accel;
            sample.dynamic_valid = true;
            sample.dt_s = dt;
        }
    }
    kin3d_mechanics.kinematicState[key] = {
        pos: comWorldMm.clone(),
        vel: sample.velocity_m_s.clone(),
        accel: sample.acceleration_m_s2.clone(),
        t: nowMs
    };
    return sample;
}

function kin3d_buildBearingLoadDistribution(parts, T_all) {
    var groups = kin3d_groupBearingMounts();
    var bearingLoads = [];
    var bearingPairLoads = [];
    for (var g = 0; g < groups.length; g++) {
        var group = groups[g];
        var centers = [];
        for (var m = 0; m < group.mounts.length; m++) {
            var mount = group.mounts[m];
            centers.push(kin3d_transformFrameLocalToWorldThree(mount.link_frame_id || '', {
                x: kin3d_numField(mount, 'local_x_mm', 0),
                y: kin3d_numField(mount, 'local_y_mm', 0),
                z: kin3d_numField(mount, 'local_z_mm', 0)
            }, T_all));
        }
        if (!centers.length) continue;

        var pairCenter = new THREE.Vector3(0, 0, 0);
        for (var c = 0; c < centers.length; c++) pairCenter.add(centers[c]);
        pairCenter.multiplyScalar(1.0 / centers.length);

        var axis = centers.length >= 2 ? centers[centers.length - 1].clone().sub(centers[0]) : new THREE.Vector3();
        var spacingMm = axis.length();
        if (spacingMm > 1e-6) {
            axis.normalize();
        } else {
            var firstMount = group.mounts[0];
            axis = kin3d_transformFrameVectorToWorldThree(firstMount.link_frame_id || '', {
                x: kin3d_numField(firstMount, 'axis_x', 0),
                y: kin3d_numField(firstMount, 'axis_y', 0),
                z: kin3d_numField(firstMount, 'axis_z', 1)
            }, T_all);
        }

        var resultant = new THREE.Vector3(0, 0, 0);
        var momentNm = new THREE.Vector3(0, 0, 0);
        var affectedPartIds = [];
        for (var p = 0; p < parts.length; p++) {
            var part = parts[p];
            if (!kin3d_isFrameDescendant(part.link_frame_id || '', group.link_frame_id || '')) continue;
            var force = new THREE.Vector3(part.force_n.x || 0, part.force_n.y || 0, part.force_n.z || 0);
            var com = new THREE.Vector3(part.com_world_three_mm.x || 0, part.com_world_three_mm.y || 0, part.com_world_three_mm.z || 0);
            var armM = com.clone().sub(pairCenter).multiplyScalar(0.001);
            resultant.add(force);
            momentNm.add(new THREE.Vector3().crossVectors(armM, force));
            affectedPartIds.push(part.part_id || '');
        }

        var axialSignedN = resultant.dot(axis);
        var axialN = Math.abs(axialSignedN);
        var radialVec = resultant.clone().sub(axis.clone().multiplyScalar(axialSignedN));
        var radialN = radialVec.length();
        var momentAxisNm = momentNm.dot(axis);
        var momentPerpVec = momentNm.clone().sub(axis.clone().multiplyScalar(momentAxisNm));
        var momentPerpNm = momentPerpVec.length();
        var spacingM = spacingMm * 0.001;
        var count = Math.max(1, group.mounts.length);
        var forcePerBearingRadialN = radialN / count;
        var forcePerBearingAxialN = axialN / count;
        var couplePerBearingN = spacingM > 1e-6 && count >= 2 ? (momentPerpNm / spacingM) : 0;

        var pairRow = {
            bearing_pair_id: group.id,
            joint_id: group.joint_id,
            link_frame_id: group.link_frame_id,
            bearing_count: group.mounts.length,
            spacing_mm: spacingMm,
            axis_world_three: kin3d_vectorToObject(axis),
            center_world_three_mm: kin3d_vectorToObject(pairCenter),
            resultant_force_n: kin3d_vectorToObject(resultant),
            resultant_radial_load_n: radialN,
            resultant_axial_load_n: axialN,
            moment_about_pair_center_nm: kin3d_vectorToObject(momentNm),
            moment_perpendicular_nm: momentPerpNm,
            moment_about_axis_nm: momentAxisNm,
            couple_radial_load_per_bearing_n: couplePerBearingN,
            affected_part_ids: affectedPartIds,
            method: 'frame-subtree static equilibrium: F + r_cross_F over downstream CAD parts'
        };
        bearingPairLoads.push(pairRow);

        for (var bi = 0; bi < group.mounts.length; bi++) {
            var bearing = group.mounts[bi];
            var share = kin3d_numField(bearing, 'load_share_factor', 1);
            var radialLoad = Math.max(0, (forcePerBearingRadialN + couplePerBearingN) * share);
            var axialLoad = Math.max(0, forcePerBearingAxialN * share);
            bearingLoads.push({
                bearing_mount_id: bearing.bearing_mount_id || '',
                placement_id: bearing.placement_id || bearing.bearing_mount_id || '',
                bearing_pair_id: group.id,
                joint_id: bearing.joint_id || '',
                link_frame_id: bearing.link_frame_id || '',
                center_world_three_mm: kin3d_vectorToObject(centers[bi]),
                radial_load_N: radialLoad,
                axial_load_N: axialLoad,
                equivalent_load_N: 0,
                estimated_static_load_n: radialLoad + axialLoad,
                resultant_radial_load_n: radialN,
                resultant_axial_load_n: axialN,
                couple_radial_load_n: couplePerBearingN,
                spacing_mm: spacingMm,
                support_type: bearing.support_type || '',
                status: bearing.status || 'unknown',
                source: 'web_cad_static_equilibrium',
                warning: spacingMm <= 1e-6 ? 'Bearing pair spacing is zero; moment couple load disabled.' : ''
            });
        }
    }
    return { bearingLoads: bearingLoads, bearingPairLoads: bearingPairLoads };
}

function kin3d_outputTorqueLimit(profile, continuous) {
    if (!profile) return 0;
    var explicit = kin3d_numField(
        profile,
        continuous ? 'output_continuous_torque_nm' : 'output_peak_torque_nm',
        0
    );
    if (explicit > 0) return explicit;
    var motor = kin3d_numField(
        profile,
        continuous ? 'motor_continuous_torque_nm' : 'motor_peak_torque_nm',
        0
    );
    var ratio = kin3d_numField(profile, 'gear_ratio', 1);
    var eff = kin3d_numField(profile, 'gear_efficiency', 1);
    return Math.max(0, motor * ratio * eff);
}

function kin3d_clamp01(v) {
    return Math.max(0, Math.min(1, v));
}

function kin3d_tempDeratingFactor(actualC, nominalC, maxC, minFactor) {
    if (!isFinite(actualC)) return 1;
    nominalC = isFinite(nominalC) ? nominalC : 75;
    maxC = isFinite(maxC) ? maxC : 120;
    minFactor = Math.max(0.05, Math.min(1, isFinite(minFactor) ? minFactor : 0.35));
    if (actualC <= nominalC) return 1;
    if (actualC >= maxC) return minFactor;
    var t = (actualC - nominalC) / Math.max(1e-6, maxC - nominalC);
    return 1 - (1 - minFactor) * kin3d_clamp01(t);
}

function kin3d_voltageDeratingFactor(actualV, nominalV, minV) {
    if (!isFinite(actualV)) return 1;
    nominalV = isFinite(nominalV) ? nominalV : 48;
    minV = isFinite(minV) ? minV : nominalV * 0.75;
    if (actualV >= nominalV) return 1;
    if (actualV <= minV) return 0.50;
    return 0.50 + 0.50 * kin3d_clamp01((actualV - minV) / Math.max(1e-6, nominalV - minV));
}

function kin3d_currentOutputTorqueLimit(profile, derating, continuous) {
    if (!profile || !derating) return 0;
    var kt = kin3d_numField(derating, 'torque_constant_nm_per_a', 0);
    var currentA = kin3d_numField(derating, continuous ? 'continuous_current_a' : 'peak_current_a', 0);
    if (!(kt > 0) || !(currentA > 0)) return 0;
    var ratio = kin3d_numField(profile, 'gear_ratio', 1);
    var eff = kin3d_numField(profile, 'gear_efficiency', 1);
    return Math.max(0, kt * currentA * ratio * eff);
}

function kin3d_buildActuatorDerating(jointId, profile) {
    var derating = kin3d_mechanics.actuatorDeratingProfiles[jointId] || null;
    var telemetry = (kin3d_mechanics.actuatorTelemetry || {})[jointId] || {};
    var nominalContinuousNm = kin3d_outputTorqueLimit(profile, true);
    var nominalPeakNm = kin3d_outputTorqueLimit(profile, false);
    var currentContinuousNm = kin3d_currentOutputTorqueLimit(profile, derating, true);
    var currentPeakNm = kin3d_currentOutputTorqueLimit(profile, derating, false);
    if (!(currentContinuousNm > 0)) currentContinuousNm = nominalContinuousNm;
    if (!(currentPeakNm > 0)) currentPeakNm = nominalPeakNm;

    var motorTempC = Number(telemetry.motor_temp_c);
    var controllerTempC = Number(telemetry.controller_temp_c);
    var busVoltageV = Number(telemetry.bus_voltage_v);
    var minThermalFactor = kin3d_numField(derating, 'min_thermal_factor', 0.35);
    var motorFactor = kin3d_tempDeratingFactor(
        motorTempC,
        kin3d_numField(derating, 'motor_temp_nominal_c', 75),
        kin3d_numField(derating, 'motor_temp_max_c', 120),
        minThermalFactor
    );
    var controllerFactor = kin3d_tempDeratingFactor(
        controllerTempC,
        kin3d_numField(derating, 'controller_temp_nominal_c', 65),
        kin3d_numField(derating, 'controller_temp_max_c', 95),
        minThermalFactor
    );
    var thermalFactor = Math.min(motorFactor, controllerFactor);
    var voltageFactor = kin3d_voltageDeratingFactor(
        busVoltageV,
        kin3d_numField(derating, 'bus_voltage_nominal_v', 48),
        kin3d_numField(derating, 'bus_voltage_min_v', 38)
    );
    var reducerHotFactor = 1;
    if (thermalFactor < 0.999) {
        reducerHotFactor = kin3d_numField(derating, 'reducer_hot_efficiency_factor', 0.94);
    }
    reducerHotFactor = Math.max(0.50, Math.min(1, reducerHotFactor));
    var totalFactor = Math.max(0, Math.min(1, thermalFactor * voltageFactor * reducerHotFactor));
    var deratedContinuousNm = Math.min(nominalContinuousNm || currentContinuousNm, currentContinuousNm) * totalFactor;
    var deratedPeakNm = Math.min(nominalPeakNm || currentPeakNm, currentPeakNm) * totalFactor;
    if (!(deratedPeakNm >= deratedContinuousNm)) deratedPeakNm = deratedContinuousNm;
    return {
        schema: 'mros.web_mechanics.actuator_derating.v1',
        joint_id: jointId,
        profile_id: derating ? (derating.profile_id || '') : '',
        motor_model: derating ? (derating.motor_model || '') : '',
        drive_model: derating ? (derating.drive_model || '') : '',
        nominal_continuous_limit_nm: nominalContinuousNm,
        nominal_peak_limit_nm: nominalPeakNm,
        current_limited_continuous_nm: currentContinuousNm,
        current_limited_peak_nm: currentPeakNm,
        derated_continuous_limit_nm: deratedContinuousNm,
        derated_peak_limit_nm: deratedPeakNm,
        motor_temp_c: isFinite(motorTempC) ? motorTempC : null,
        controller_temp_c: isFinite(controllerTempC) ? controllerTempC : null,
        bus_voltage_v: isFinite(busVoltageV) ? busVoltageV : null,
        phase_current_a: isFinite(Number(telemetry.phase_current_a)) ? Number(telemetry.phase_current_a) : null,
        q_current_a: isFinite(Number(telemetry.q_current_a)) ? Number(telemetry.q_current_a) : null,
        motor_thermal_factor: motorFactor,
        controller_thermal_factor: controllerFactor,
        thermal_factor: thermalFactor,
        voltage_factor: voltageFactor,
        reducer_hot_efficiency_factor: reducerHotFactor,
        total_derating_factor: totalFactor,
        source: telemetry.source || (derating ? (derating.status || 'actuator_derating_profile') : 'missing_derating_profile')
    };
}

function kin3d_jointStatus(absTorqueNm, continuousLimitNm, peakLimitNm) {
    if (!(peakLimitNm > 0)) return 'DATA_MISSING';
    if (absTorqueNm >= peakLimitNm) return 'OVER_PEAK';
    if (continuousLimitNm > 0 && absTorqueNm >= continuousLimitNm) return 'OVER_CONT';
    if (continuousLimitNm > 0 && absTorqueNm >= 0.80 * continuousLimitNm) return 'WATCH';
    return 'OK';
}

function kin3d_buildJointLoadSummary(parts, T_all) {
    var rows = [];
    var frames = Object.keys(kin3d_mechanics.linkFrames || {});
    frames.sort(function(a, b) {
        return kin3d_numField(kin3d_mechanics.linkFrames[a], 'joint_index', 0) -
               kin3d_numField(kin3d_mechanics.linkFrames[b], 'joint_index', 0);
    });
    for (var i = 0; i < frames.length; i++) {
        var frameId = frames[i];
        if (frameId === 'WORLD' || frameId === 'J0_BASE') continue;
        var frame = kin3d_mechanics.linkFrames[frameId] || {};
        var jointIndex = Math.max(0, Math.min(6, Math.floor(kin3d_numField(frame, 'joint_index', 0))));
        var jointId = 'J' + (jointIndex + 1);
        var profile = kin3d_mechanics.jointActuatorProfiles[jointId] || null;
        var origin = kin3d_transformFrameLocalToWorldThree(frameId, {x: 0, y: 0, z: 0}, T_all);
        var axis = kin3d_transformFrameVectorToWorldThree(frameId, {
            x: kin3d_numField(frame, 'axis_x', 0),
            y: kin3d_numField(frame, 'axis_y', 0),
            z: kin3d_numField(frame, 'axis_z', 1)
        }, T_all);
        var resultant = new THREE.Vector3(0, 0, 0);
        var momentNm = new THREE.Vector3(0, 0, 0);
        var affectedPartIds = [];
        for (var p = 0; p < parts.length; p++) {
            var part = parts[p];
            if (!kin3d_isFrameDescendant(part.link_frame_id || '', frameId)) continue;
            var force = new THREE.Vector3(part.force_n.x || 0, part.force_n.y || 0, part.force_n.z || 0);
            var com = new THREE.Vector3(part.com_world_three_mm.x || 0, part.com_world_three_mm.y || 0, part.com_world_three_mm.z || 0);
            var armM = com.clone().sub(origin).multiplyScalar(0.001);
            resultant.add(force);
            momentNm.add(new THREE.Vector3().crossVectors(armM, force));
            affectedPartIds.push(part.part_id || '');
        }
        var axisTorqueNm = momentNm.dot(axis);
        var absTorqueNm = Math.abs(axisTorqueNm);
        var derating = kin3d_buildActuatorDerating(jointId, profile);
        var continuousLimitNm = derating.derated_continuous_limit_nm;
        var peakLimitNm = derating.derated_peak_limit_nm;
        rows.push({
            joint_id: jointId,
            joint_index: jointIndex,
            link_frame_id: frameId,
            axis_world_three: kin3d_vectorToObject(axis),
            origin_world_three_mm: kin3d_vectorToObject(origin),
            resultant_force_n: kin3d_vectorToObject(resultant),
            moment_about_joint_nm: kin3d_vectorToObject(momentNm),
            axis_torque_nm: axisTorqueNm,
            abs_axis_torque_nm: absTorqueNm,
            continuous_limit_nm: continuousLimitNm,
            peak_limit_nm: peakLimitNm,
            nominal_continuous_limit_nm: derating.nominal_continuous_limit_nm,
            nominal_peak_limit_nm: derating.nominal_peak_limit_nm,
            current_limited_continuous_nm: derating.current_limited_continuous_nm,
            current_limited_peak_nm: derating.current_limited_peak_nm,
            continuous_margin_ratio: continuousLimitNm > 0 ? absTorqueNm / continuousLimitNm : 0,
            peak_margin_ratio: peakLimitNm > 0 ? absTorqueNm / peakLimitNm : 0,
            gear_ratio: kin3d_numField(profile, 'gear_ratio', 0),
            gear_efficiency: kin3d_numField(profile, 'gear_efficiency', 0),
            holding_brake_torque_nm: kin3d_numField(profile, 'holding_brake_torque_nm', 0),
            actuator_derating: derating,
            status: kin3d_jointStatus(absTorqueNm, continuousLimitNm, peakLimitNm),
            affected_part_ids: affectedPartIds,
            source: profile ? (profile.status || 'actuator_profile') + '+derating' : 'missing_actuator_profile'
        });
    }
    return rows;
}

function kin3d_worstJointStatus(jointLoads) {
    var rank = { DATA_MISSING: 1, OK: 2, WATCH: 3, OVER_CONT: 4, OVER_PEAK: 5 };
    var worst = 'UNKNOWN';
    var worstRank = 0;
    for (var i = 0; i < (jointLoads || []).length; i++) {
        var status = jointLoads[i].status || 'DATA_MISSING';
        var r = rank[status] || 1;
        if (r > worstRank) {
            worstRank = r;
            worst = status;
        }
    }
    return worst;
}

function kin3d_structuralStatus(yieldSafetyFactor, shearSafetyFactor, fatigueSafetyFactor, policy) {
    var sf = Math.min(
        yieldSafetyFactor > 0 ? yieldSafetyFactor : Infinity,
        shearSafetyFactor > 0 ? shearSafetyFactor : Infinity,
        fatigueSafetyFactor > 0 ? fatigueSafetyFactor : Infinity
    );
    if (!isFinite(sf)) return 'DATA_MISSING';
    var staticMin = kin3d_numField(policy, 'minimum_acceptable_static_sf', 1.5);
    var fatigueMin = kin3d_numField(policy, 'minimum_acceptable_fatigue_sf', 1.3);
    if (sf < 1.0) return 'CRITICAL';
    if (fatigueSafetyFactor > 0 && fatigueSafetyFactor < fatigueMin) return 'FATIGUE_WATCH';
    if (sf < staticMin) return 'WATCH';
    if (sf < Math.max(2.0, staticMin * 1.25)) return 'LOW_MARGIN';
    return 'OK';
}

function kin3d_worstStructuralStatus(structuralChecks) {
    var rank = { DATA_MISSING: 1, OK: 2, LOW_MARGIN: 3, FATIGUE_WATCH: 4, WATCH: 5, CRITICAL: 6 };
    var worst = 'UNKNOWN';
    var worstRank = 0;
    for (var i = 0; i < (structuralChecks || []).length; i++) {
        var status = structuralChecks[i].status || 'DATA_MISSING';
        var r = rank[status] || 1;
        if (r > worstRank) {
            worst = status;
            worstRank = r;
        }
    }
    return worst;
}

function kin3d_selectStructuralPolicy(materialId) {
    var idx = kin3d_mechanics.materialIndex[String(materialId || '')] || {};
    var family = String(idx.family || '').toLowerCase();
    var policies = kin3d_mechanics.structuralPolicies || {};
    var fallback = policies.default_seed || null;
    for (var policyId in policies) {
        var policy = policies[policyId];
        var match = String(policy.material_family_match || '').toLowerCase();
        if (match && match !== '*' && family.indexOf(match) >= 0) return policy;
    }
    return fallback;
}

function kin3d_structuralTemperatureFactor(policy) {
    var actuatorTemps = kin3d_mechanics.actuatorTelemetry || {};
    var maxTemp = 25;
    for (var jointId in actuatorTemps) {
        var mt = Number(actuatorTemps[jointId] && actuatorTemps[jointId].motor_temp_c);
        var ct = Number(actuatorTemps[jointId] && actuatorTemps[jointId].controller_temp_c);
        if (isFinite(mt)) maxTemp = Math.max(maxTemp, mt);
        if (isFinite(ct)) maxTemp = Math.max(maxTemp, ct);
    }
    var deratePer100 = kin3d_numField(policy, 'temperature_derate_per_100c', 0.10);
    var factor = 1.0 - Math.max(0, maxTemp - 25.0) * deratePer100 / 100.0;
    return {
        factor: Math.max(0.50, Math.min(1.0, factor)),
        reference_temp_c: maxTemp,
        source: maxTemp > 25 ? 'actuator_telemetry_max_temp' : 'ambient_seed_25c'
    };
}

function kin3d_rectSectionPropertiesFromBbox(sizeMm) {
    var dims = [
        Math.max(0, Number(sizeMm && sizeMm.x) * 0.001),
        Math.max(0, Number(sizeMm && sizeMm.y) * 0.001),
        Math.max(0, Number(sizeMm && sizeMm.z) * 0.001)
    ].filter(function(v) { return v > 1e-6; });
    if (dims.length < 2) {
        return { valid: false, area_m2: 0, min_section_modulus_m3: 0, min_dimension_m: 0, method: 'missing_bbox' };
    }
    dims.sort(function(a, b) { return a - b; });
    var area = dims[0] * dims[1];
    var minZ = Infinity;
    for (var i = 0; i < dims.length; i++) {
        for (var j = 0; j < dims.length; j++) {
            if (i === j) continue;
            var z = dims[i] * dims[j] * dims[j] / 6.0;
            if (z > 0 && z < minZ) minZ = z;
        }
    }
    return {
        valid: isFinite(minZ) && area > 0,
        area_m2: area,
        min_section_modulus_m3: isFinite(minZ) ? minZ : 0,
        min_dimension_m: dims[0],
        method: 'bbox_rectangular_screening'
    };
}

function kin3d_buildStructuralScreening(parts, T_all) {
    var rows = [];
    for (var i = 0; i < (parts || []).length; i++) {
        var part = parts[i];
        if (!part || String(part.part_id || '').indexOf('payload:') === 0) continue;
        var computed = kin3d_mechanics.computedMassProperties[part.part_id] || null;
        var mat = kin3d_mechanics.materials[part.material_id] || null;
        var section = kin3d_rectSectionPropertiesFromBbox(computed && computed.bbox_size_mm);
        var yieldMpa = kin3d_numField(mat, 'yield_strength_mpa', 0);
        var shearYieldMpa = kin3d_numField(mat, 'shear_yield_mpa', 0);
        var ultimateMpa = kin3d_numField(mat, 'ultimate_tensile_strength_mpa', 0);
        var fatigueMpa = kin3d_numField(mat, 'fatigue_strength_mpa', 0);
        var policy = kin3d_selectStructuralPolicy(part.material_id);
        var seedKnockdown = kin3d_numField(policy, 'seed_data_knockdown', 0.75);
        var temp = kin3d_structuralTemperatureFactor(policy);
        var yieldFactor = kin3d_numField(policy, 'default_static_yield_factor', 2.25);
        var shearFactor = kin3d_numField(policy, 'default_shear_factor', 2.0);
        var ultimateFactor = kin3d_numField(policy, 'default_ultimate_factor', 3.0);
        var fatigueFactor = kin3d_numField(policy, 'default_fatigue_factor', 3.5);
        var allowableYieldMpa = yieldMpa > 0 ? yieldMpa * seedKnockdown * temp.factor / Math.max(1e-6, yieldFactor) : 0;
        var allowableShearMpa = shearYieldMpa > 0 ? shearYieldMpa * seedKnockdown * temp.factor / Math.max(1e-6, shearFactor) : 0;
        var allowableUltimateMpa = ultimateMpa > 0 ? ultimateMpa * seedKnockdown * temp.factor / Math.max(1e-6, ultimateFactor) : 0;
        var allowableFatigueMpa = fatigueMpa > 0 ? fatigueMpa * seedKnockdown * temp.factor / Math.max(1e-6, fatigueFactor) : 0;
        var force = new THREE.Vector3(part.force_n.x || 0, part.force_n.y || 0, part.force_n.z || 0);
        var forceN = force.length();
        var origin = kin3d_transformFrameLocalToWorldThree(part.link_frame_id || '', {x: 0, y: 0, z: 0}, T_all);
        var com = new THREE.Vector3(part.com_world_three_mm.x || 0, part.com_world_three_mm.y || 0, part.com_world_three_mm.z || 0);
        var armM = com.clone().sub(origin).multiplyScalar(0.001);
        var moment = new THREE.Vector3().crossVectors(armM, force);
        var momentNm = moment.length();
        var axialStressMpa = section.valid ? (forceN / section.area_m2) / 1e6 : 0;
        var bendingStressMpa = section.valid ? (momentNm / Math.max(section.min_section_modulus_m3, 1e-12)) / 1e6 : 0;
        var shearStressMpa = section.valid ? (1.5 * forceN / section.area_m2) / 1e6 : 0;
        var combinedNormalMpa = axialStressMpa + bendingStressMpa;
        var vonMisesMpa = Math.sqrt(combinedNormalMpa * combinedNormalMpa + 3.0 * shearStressMpa * shearStressMpa);
        var yieldSf = yieldMpa > 0 && vonMisesMpa > 0 ? yieldMpa / vonMisesMpa : 0;
        var shearSf = shearYieldMpa > 0 && shearStressMpa > 0 ? shearYieldMpa / shearStressMpa : 0;
        var ultimateSf = ultimateMpa > 0 && combinedNormalMpa > 0 ? ultimateMpa / combinedNormalMpa : 0;
        var allowableYieldSf = allowableYieldMpa > 0 && vonMisesMpa > 0 ? allowableYieldMpa / vonMisesMpa : 0;
        var allowableShearSf = allowableShearMpa > 0 && shearStressMpa > 0 ? allowableShearMpa / shearStressMpa : 0;
        var allowableUltimateSf = allowableUltimateMpa > 0 && combinedNormalMpa > 0 ? allowableUltimateMpa / combinedNormalMpa : 0;
        var fatigueAlternatingMpa = Math.max(0, bendingStressMpa + 0.5 * axialStressMpa);
        var fatigueSf = fatigueMpa > 0 && fatigueAlternatingMpa > 0 ? fatigueMpa / fatigueAlternatingMpa : 0;
        var allowableFatigueSf = allowableFatigueMpa > 0 && fatigueAlternatingMpa > 0 ? allowableFatigueMpa / fatigueAlternatingMpa : 0;
        var status = (!section.valid || !mat || !(yieldMpa > 0) || !policy)
            ? 'DATA_MISSING'
            : kin3d_structuralStatus(allowableYieldSf, allowableShearSf, allowableFatigueSf, policy);
        rows.push({
            part_id: part.part_id || '',
            link_frame_id: part.link_frame_id || '',
            material_id: part.material_id || '',
            mass_kg: part.mass_kg || 0,
            force_n: forceN,
            moment_about_link_origin_nm: momentNm,
            section_area_m2: section.area_m2,
            min_section_modulus_m3: section.min_section_modulus_m3,
            min_dimension_m: section.min_dimension_m,
            axial_stress_mpa: axialStressMpa,
            bending_stress_mpa: bendingStressMpa,
            shear_stress_mpa: shearStressMpa,
            combined_normal_stress_mpa: combinedNormalMpa,
            von_mises_stress_mpa: vonMisesMpa,
            yield_strength_mpa: yieldMpa,
            shear_yield_mpa: shearYieldMpa,
            ultimate_tensile_strength_mpa: ultimateMpa,
            fatigue_strength_mpa: fatigueMpa,
            allowable_yield_mpa: allowableYieldMpa,
            allowable_shear_mpa: allowableShearMpa,
            allowable_ultimate_mpa: allowableUltimateMpa,
            allowable_fatigue_mpa: allowableFatigueMpa,
            yield_safety_factor: yieldSf,
            shear_safety_factor: shearSf,
            ultimate_safety_factor: ultimateSf,
            fatigue_safety_factor: fatigueSf,
            design_yield_safety_factor: allowableYieldSf,
            design_shear_safety_factor: allowableShearSf,
            design_ultimate_safety_factor: allowableUltimateSf,
            design_fatigue_safety_factor: allowableFatigueSf,
            fatigue_alternating_stress_mpa: fatigueAlternatingMpa,
            design_policy_id: policy ? (policy.policy_id || '') : '',
            design_policy_status: policy ? (policy.status || '') : 'missing',
            seed_data_knockdown: seedKnockdown,
            structural_temperature_factor: temp.factor,
            structural_reference_temp_c: temp.reference_temp_c,
            structural_temperature_source: temp.source,
            status: status,
            method: section.method,
            warning: status === 'DATA_MISSING'
                ? 'Missing material strength or CAD bbox section data.'
                : 'Seed rectangular-section screening only; replace with validated CAD/FEM stress analysis.'
        });
    }
    return rows;
}

function kin3d_refreshMechanicalEstimatesFromCad() {
    if (!kin3d_mechanics.loaded) return;
    for (var i = 0; i < kin3d_cadParts.length; i++) {
        var part = kin3d_cadParts[i];
        var root = kin3d_cadRoots[part.id];
        if (!root || !part.mechanicalMaterial) continue;
        var explicitMass = Number(part.massKg || 0);
        var density = kin3d_numField(part.mechanicalMaterial, 'density_kg_m3', 0);
        if (!(density > 0)) continue;

        var volume = kin3d_estimateCadVolumeMm3(root);
        var massKg = explicitMass > 0 ? explicitMass : (volume.volumeMm3 * 1e-9 * density);
        var box = new THREE.Box3().setFromObject(root);
        var size = new THREE.Vector3();
        var center = new THREE.Vector3();
        box.getSize(size);
        box.getCenter(center);
        var sx = size.x / 1000.0;
        var sy = size.y / 1000.0;
        var sz = size.z / 1000.0;
        kin3d_mechanics.computedMassProperties[part.id] = {
            part_id: part.id,
            material_id: part.materialId || '',
            density_kg_m3: density,
            volume_mm3: volume.volumeMm3,
            mass_kg: massKg,
            center_world_three_mm: { x: center.x, y: center.y, z: center.z },
            bbox_size_mm: { x: size.x, y: size.y, z: size.z },
            inertia_box_kg_m2: {
                Ixx: massKg * (sy * sy + sz * sz) / 12.0,
                Iyy: massKg * (sx * sx + sz * sz) / 12.0,
                Izz: massKg * (sx * sx + sy * sy) / 12.0
            },
            source: explicitMass > 0 ? 'csv_mass_property' : volume.method,
            warning: explicitMass > 0 ? '' : 'Seed CAD mesh/bbox estimate; replace with final CAD mass properties.'
        };
    }
}

function kin3d_pushForcePart(parts, partData, T_all, nowMs) {
    var massKg = Number(partData.mass_kg || 0);
    if (!(massKg > 0)) return null;
    var com = partData.com_world_three_mm
        ? new THREE.Vector3(partData.com_world_three_mm.x || 0, partData.com_world_three_mm.y || 0, partData.com_world_three_mm.z || 0)
        : kin3d_transformFrameLocalToWorldThree(partData.link_frame_id || '', partData.centerOfMassLocalMm || {x:0,y:0,z:0}, T_all);
    var kin = kin3d_updatePartKinematics(partData.part_id || '', com, nowMs);
    var gravityMps2 = new THREE.Vector3(0, -9.80665, 0);
    var supportLoadAccel = gravityMps2.clone().sub(kin.acceleration_m_s2);
    var forceN = supportLoadAccel.multiplyScalar(massKg);
    var rM = com.clone().multiplyScalar(0.001);
    var momentNm = new THREE.Vector3().crossVectors(rM, forceN);
    var row = {
        part_id: partData.part_id || '',
        link_frame_id: partData.link_frame_id || '',
        material_id: partData.material_id || '',
        mass_kg: massKg,
        force_n: kin3d_vectorToObject(forceN),
        gravity_force_n: { x: 0, y: -massKg * 9.80665, z: 0 },
        inertial_force_n: kin3d_vectorToObject(kin.acceleration_m_s2.clone().multiplyScalar(-massKg)),
        com_world_three_mm: kin3d_vectorToObject(com),
        velocity_m_s: kin3d_vectorToObject(kin.velocity_m_s),
        acceleration_m_s2: kin3d_vectorToObject(kin.acceleration_m_s2),
        dynamic_valid: !!kin.dynamic_valid,
        dynamics_dt_s: kin.dt_s,
        moment_about_base_nm: kin3d_vectorToObject(momentNm),
        source: partData.source || 'unknown'
    };
    parts.push(row);
    return { row: row, momentNm: momentNm };
}

function kin3d_getActivePayloadVirtualPart(T_all) {
    var payloadId = kin3d_mechanics.activePayloadId || 'none';
    var profile = kin3d_mechanics.payloadProfiles[payloadId] || kin3d_mechanics.payloadProfiles.none || null;
    if (!profile) return null;
    var massKg = kin3d_numField(profile, 'mass_kg', 0);
    if (!(massKg > 0)) return null;
    return {
        part_id: 'payload:' + (profile.payload_id || payloadId),
        link_frame_id: 'J7_TOOL',
        material_id: 'PAYLOAD',
        mass_kg: massKg,
        centerOfMassLocalMm: {
            x: kin3d_numField(profile, 'com_x_mm', 0),
            y: kin3d_numField(profile, 'com_y_mm', 0),
            z: kin3d_numField(profile, 'com_z_mm', 0)
        },
        source: 'payload_profile:' + (profile.payload_id || payloadId)
    };
}

function kin3d_updateMechanicalForceSnapshot(angles_deg, T_all) {
    if (!kin3d_mechanics.loaded) return null;
    var parts = [];
    var totalMassKg = 0;
    var totalMomentNm = new THREE.Vector3(0, 0, 0);
    var dynamicValidCount = 0;
    var nowMs = kin3d_nowMs();

    for (var i = 0; i < kin3d_cadParts.length; i++) {
        var part = kin3d_cadParts[i];
        var computed = kin3d_mechanics.computedMassProperties[part.id] || null;
        var massKg = part.massKg > 0 ? part.massKg : kin3d_numField(computed, 'mass_kg', 0);
        if (!(massKg > 0)) continue;

        var pushed = kin3d_pushForcePart(parts, {
            part_id: part.id,
            link_frame_id: part.linkFrameId || '',
            material_id: part.materialId || '',
            mass_kg: massKg,
            com_world_three_mm: computed && computed.center_world_three_mm ? computed.center_world_three_mm : null,
            centerOfMassLocalMm: part.centerOfMassLocalMm || {x:0,y:0,z:0},
            source: computed ? computed.source : 'csv_mass_property'
        }, T_all, nowMs);
        if (!pushed) continue;
        totalMassKg += massKg;
        totalMomentNm.add(pushed.momentNm);
        if (pushed.row.dynamic_valid) dynamicValidCount++;
    }

    var payloadPart = kin3d_getActivePayloadVirtualPart(T_all);
    if (payloadPart) {
        var pushedPayload = kin3d_pushForcePart(parts, payloadPart, T_all, nowMs);
        if (pushedPayload) {
            totalMassKg += payloadPart.mass_kg;
            totalMomentNm.add(pushedPayload.momentNm);
            if (pushedPayload.row.dynamic_valid) dynamicValidCount++;
        }
    }

    var loadDistribution = kin3d_buildBearingLoadDistribution(parts, T_all);
    var jointLoads = kin3d_buildJointLoadSummary(parts, T_all);
    var structuralChecks = kin3d_buildStructuralScreening(parts, T_all);

    kin3d_mechanics.lastForceSnapshot = {
        schema: 'mros.web_mechanics.force_snapshot.v1',
        generatedAtMs: Date.now(),
        jointAnglesDeg: (angles_deg || []).slice(0, 7),
        totalMassKg: totalMassKg,
        totalWeightN: totalMassKg * 9.80665,
        totalMomentAboutBaseNm: { x: totalMomentNm.x, y: totalMomentNm.y, z: totalMomentNm.z },
        dynamics: {
            method: 'finite_difference_com_acceleration',
            sampleTimeMs: nowMs,
            validPartCount: dynamicValidCount,
            partCount: parts.length,
            accelerationFrame: 'three_world_m_s2_y_up',
            forceModel: 'support_load = mass * (gravity - com_acceleration)'
        },
        activePayloadId: kin3d_mechanics.activePayloadId || 'none',
        activeProcessModeId: kin3d_mechanics.activeProcessModeId || 'dry_run_motion_check',
        processFeedPlan: kin3d_computeProcessFeedPlan({ mode_id: kin3d_mechanics.activeProcessModeId }),
        prrAxisMap: kin3d_mechanics.prrAxisMap || {},
        canopenActuatorBusMap: kin3d_mechanics.canopenActuatorBusMap || {},
        actuatorTelemetry: kin3d_mechanics.actuatorTelemetry || {},
        parts: parts,
        jointLoads: jointLoads,
        worstJointStatus: kin3d_worstJointStatus(jointLoads),
        structuralChecks: structuralChecks,
        worstStructuralStatus: kin3d_worstStructuralStatus(structuralChecks),
        bearingPairLoads: loadDistribution.bearingPairLoads,
        bearingLoads: loadDistribution.bearingLoads,
        coordinateSystem: 'three_world_mm_y_up_from_robot_dh',
        warnings: (kin3d_mechanics.todo || []).map(function(item) { return item.title || item.id || 'mechanics warning'; })
    };
    kin3d_updateMechanicsLabel();
    return kin3d_mechanics.lastForceSnapshot;
}

function kin3d_saveSettings() {
    if (window && typeof window.mrosPopupSettingsSetSection === 'function') {
        window.mrosPopupSettingsSetSection('kin3d', kin3d_settings);
        return;
    }
    localStorage.setItem('kin3d_settings', JSON.stringify(kin3d_settings));
}

function kin3d_loadSettings() {
    var loaded = false;
    if (window && typeof window.mrosPopupSettingsGetSection === 'function') {
        var remoteSaved = window.mrosPopupSettingsGetSection('kin3d');
        if (remoteSaved && typeof remoteSaved === 'object') {
            Object.assign(kin3d_settings, remoteSaved);
            loaded = true;
        }
    }

    if (!loaded) {
        var saved = localStorage.getItem('kin3d_settings');
        if (saved) {
            try {
                var parsed = JSON.parse(saved);
                Object.assign(kin3d_settings, parsed);
                loaded = true;
            } catch(e) { console.error("Failed to load settings:", e); }
        }
    }
    if (kin3d_enforceRenderModeSettings()) kin3d_saveSettings();
}

function kin3d_reloadSettingsFromUserStore() {
    if (!(window && typeof window.mrosPopupSettingsGetSection === 'function')) return;
    var remoteSaved = window.mrosPopupSettingsGetSection('kin3d');
    if (!remoteSaved || typeof remoteSaved !== 'object') return;
    Object.assign(kin3d_settings, remoteSaved);
    if (kin3d_enforceRenderModeSettings()) kin3d_saveSettings();
    if (kin3d_initialized) {
        kin3d_applySettings();
        kin3d_mountSettingsTabContent('settings-tab-3d-body');
    }
}

if (window && typeof window.addEventListener === 'function') {
    window.addEventListener('mros-popup-settings-loaded', function() {
        try {
            kin3d_reloadSettingsFromUserStore();
        } catch (e) {}
    });
}

function kin3d_enforceRenderModeSettings() {
    var prevCad = (kin3d_settings.useCAD === true);
    var prevKin = (kin3d_settings.showKinematics === true);
    var cadOn = prevCad;
    var kinOn = prevKin;

    // Mutually exclusive mode: either CAD or kinematics is visible.
    if (cadOn) {
        kinOn = false;
    } else if (!kinOn) {
        kinOn = true;
    }

    kin3d_settings.useCAD = cadOn;
    kin3d_settings.showKinematics = kinOn;
    return (cadOn !== prevCad) || (kinOn !== prevKin);
}

function kin3d_getCameraNavPreset() {
    var preset = String(kin3d_settings.cameraNavPreset || 'default').toLowerCase();
    return (preset === 'solidworks') ? 'solidworks' : 'default';
}

function kin3d_updateNavKeyStateFromEvent(ev, isDown) {
    if (!ev || !ev.key) return false;
    var key = String(ev.key).toLowerCase();
    var changed = false;
    if (key === 'control') {
        changed = (kin3d_navKeyState.ctrl !== !!isDown);
        kin3d_navKeyState.ctrl = !!isDown;
    } else if (key === 'shift') {
        changed = (kin3d_navKeyState.shift !== !!isDown);
        kin3d_navKeyState.shift = !!isDown;
    } else if (key === 'meta') {
        changed = (kin3d_navKeyState.meta !== !!isDown);
        kin3d_navKeyState.meta = !!isDown;
    }
    return changed;
}

function kin3d_resetNavKeyState() {
    kin3d_navKeyState.ctrl = false;
    kin3d_navKeyState.shift = false;
    kin3d_navKeyState.meta = false;
}

function kin3d_applyCameraMouseBindings() {
    if (!kin3d_controls || !THREE || !THREE.MOUSE) return;
    var preset = kin3d_getCameraNavPreset();
    if (preset === 'solidworks') {
        // SolidWorks profile:
        // MMB drag = rotate, Ctrl+MMB = pan, Shift+MMB = zoom.
        var mmbAction = THREE.MOUSE.ROTATE;
        if (kin3d_navKeyState.ctrl || kin3d_navKeyState.meta) {
            mmbAction = THREE.MOUSE.PAN;
        } else if (kin3d_navKeyState.shift) {
            mmbAction = THREE.MOUSE.DOLLY;
        }
        kin3d_controls.mouseButtons.LEFT = THREE.MOUSE.ROTATE;
        kin3d_controls.mouseButtons.MIDDLE = mmbAction;
        kin3d_controls.mouseButtons.RIGHT = THREE.MOUSE.PAN;
    } else {
        // OrbitControls default profile.
        kin3d_controls.mouseButtons.LEFT = THREE.MOUSE.ROTATE;
        kin3d_controls.mouseButtons.MIDDLE = THREE.MOUSE.DOLLY;
        kin3d_controls.mouseButtons.RIGHT = THREE.MOUSE.PAN;
    }
}

function kin3d_installCameraNavHotkeys() {
    if (kin3d_navHotkeysInstalled || !window || typeof window.addEventListener !== 'function') return;
    kin3d_navHotkeysInstalled = true;

    window.addEventListener('keydown', function(ev) {
        if (!kin3d_controls || kin3d_getCameraNavPreset() !== 'solidworks') return;
        if (kin3d_updateNavKeyStateFromEvent(ev, true)) kin3d_applyCameraMouseBindings();
    });

    window.addEventListener('keyup', function(ev) {
        if (!kin3d_controls || kin3d_getCameraNavPreset() !== 'solidworks') return;
        if (kin3d_updateNavKeyStateFromEvent(ev, false)) kin3d_applyCameraMouseBindings();
    });

    window.addEventListener('blur', function() {
        kin3d_resetNavKeyState();
        if (kin3d_controls) kin3d_applyCameraMouseBindings();
    });
}

function kin3d_num(v, fallback, minVal, maxVal) {
    var n = Number(v);
    if (!isFinite(n)) n = Number(fallback);
    if (!isFinite(n)) n = 0;
    if (isFinite(minVal)) n = Math.max(minVal, n);
    if (isFinite(maxVal)) n = Math.min(maxVal, n);
    return n;
}

function kin3d_parseColorHex(v, fallback) {
    if (typeof v === 'number' && isFinite(v)) return (v >>> 0) & 0xFFFFFF;
    if (typeof v === 'string') {
        var s = v.trim();
        if (s.indexOf('#') === 0) s = s.substring(1);
        if (s.indexOf('0x') === 0 || s.indexOf('0X') === 0) s = s.substring(2);
        if (/^[0-9a-fA-F]{6}$/.test(s)) return parseInt(s, 16) & 0xFFFFFF;
    }
    return (typeof fallback === 'number' && isFinite(fallback)) ? ((fallback >>> 0) & 0xFFFFFF) : 0xFFFFFF;
}

function kin3d_colorToInputHex(v, fallback) {
    var n = kin3d_parseColorHex(v, fallback);
    var s = n.toString(16);
    while (s.length < 6) s = '0' + s;
    return '#' + s.toUpperCase();
}

function kin3d_getCadVisibilityCategory(partId) {
    switch (partId) {
        case 'fixed_turret_body': return 'base';
        case 'turret_holder':
        case 'turret_carrier':
        case 'turret_top_cover':
        case 'planet_gear_1':
        case 'planet_gear_2':
        case 'planet_gear_3': return 'turret';
        case 'arm1': return 'j2';
        case 'arm2': return 'j3';
        case 'arm3': return 'j4';
        case 'arm4': return 'j5';
        case 'arm5':
        case 'arm6': return 'j6';
        default: return 'other';
    }
}

function kin3d_isCadPartVisibleBySettings(partId) {
    var s = kin3d_settings;
    var cat = kin3d_getCadVisibilityCategory(partId);
    if (cat === 'base' && s.showPartBase === false) return false;
    if (cat === 'turret' && s.showPartTurret === false) return false;
    if (cat === 'j2' && s.showPartJ2 === false) return false;
    if (cat === 'j3' && s.showPartJ3 === false) return false;
    if (cat === 'j4' && s.showPartJ4 === false) return false;
    if (cat === 'j5' && s.showPartJ5 === false) return false;
    if (cat === 'j6' && (s.showPartJ6 === false && s.showPartJ7 === false && s.showPartGripper === false)) return false;
    return true;
}

// DH Parameter Constants (mm, rad)
// J1: d=210.4,  a=0,    alpha=+pi/2
// J2: d=0,      a=240,  alpha=0        (theta offset +pi/2)
// J3: d=0,      a=90,   alpha=+pi/2
// J4: d=202.25, a=0,    alpha=-pi/2
// J5: d=0,      a=0,    alpha=+pi/2
// J6: d=272.0,  a=0,    alpha=-pi/2
// J7: d=0,      a=160,  alpha=0        (theta offset -pi/2)

var DH_d     = [210.40, 0,      0,     202.25, 0,     272.00, 0];
var DH_a     = [0,      240.00, 90.00, 0,      0,     0,      160.00];
var DH_alpha = [Math.PI/2, 0, Math.PI/2, -Math.PI/2, Math.PI/2, -Math.PI/2, 0];
var DH_theta_offset = [0, Math.PI/2, 0, 0, 0, 0, -Math.PI/2];
var FK_J4_PHYS_OFFSET_MM = 48.0;
var FK_J6_PHYS_OFFSET_MM = 156.0;

// ---- Matrix Math Utilities ----
function mat4_identity() {
    return [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1];
}

function mat4_multiply(A, B) {
    var r = new Array(16);
    for (var row = 0; row < 4; row++) {
        for (var col = 0; col < 4; col++) {
            var sum = 0;
            for (var k = 0; k < 4; k++) {
                sum += A[row * 4 + k] * B[k * 4 + col];
            }
            r[row * 4 + col] = sum;
        }
    }
    return r;
}

function mat4_get_pos(M) {
    return new THREE.Vector3(M[3], M[7], M[11]);
}

function mat4_get_axis(M, axis) {
    // axis: 0=X, 1=Y, 2=Z
    return new THREE.Vector3(M[axis], M[4 + axis], M[8 + axis]);
}

// Standard DH transformation: T(theta, d, a, alpha)
function dh_transform(theta, d, a, alpha) {
    var ct = Math.cos(theta), st = Math.sin(theta);
    var ca = Math.cos(alpha), sa = Math.sin(alpha);
    // Row-major [row0_col0..col3, row1_col0..col3, ...]
    return [
        ct, -st*ca,  st*sa,  a*ct,
        st,  ct*ca, -ct*sa,  a*st,
        0,   sa,     ca,     d,
        0,   0,      0,      1
    ];
}

// Compute forward kinematics: returns array of 7 cumulative 4x4 transforms
function compute_FK(joint_angles_deg) {
    var T_all = [];
    var T_cum = mat4_identity();
    for (var i = 0; i < 7; i++) {
        var theta = joint_angles_deg[i] * Math.PI / 180.0 + DH_theta_offset[i];
        var Ai = dh_transform(theta, DH_d[i], DH_a[i], DH_alpha[i]);
        T_cum = mat4_multiply(T_cum, Ai);
        T_all.push(T_cum.slice()); // copy
    }
    return T_all;
}

// Extract joint positions matching MATLAB extract_joint_positions.m
// Returns array of Vector3: [Base, J1, J2, J3_corner, J4_phys, J5, J6_phys, J7, EE]
function extract_positions(T_all) {
    var P = [];

    // Base
    P.push(new THREE.Vector3(0, 0, 0));

    // J1 output (DH O1)
    P.push(mat4_get_pos(T_all[0]));

    // J2 output (DH O2)
    P.push(mat4_get_pos(T_all[1]));

    // J3 corner (DH O3)
    P.push(mat4_get_pos(T_all[2]));

    // J4 physical location = DH_O3 + FK_J4_PHYS_OFFSET_MM * Z3
    var Z3 = mat4_get_axis(T_all[2], 2);
    var O3 = mat4_get_pos(T_all[2]);
    P.push(new THREE.Vector3(
        O3.x + FK_J4_PHYS_OFFSET_MM * Z3.x,
        O3.y + FK_J4_PHYS_OFFSET_MM * Z3.y,
        O3.z + FK_J4_PHYS_OFFSET_MM * Z3.z
    ));

    // J5 (DH O4)
    P.push(mat4_get_pos(T_all[3]));

    // J6 physical location = DH_O5 + FK_J6_PHYS_OFFSET_MM * Z5
    var Z5 = mat4_get_axis(T_all[4], 2);
    var O5 = mat4_get_pos(T_all[4]);
    P.push(new THREE.Vector3(
        O5.x + FK_J6_PHYS_OFFSET_MM * Z5.x,
        O5.y + FK_J6_PHYS_OFFSET_MM * Z5.y,
        O5.z + FK_J6_PHYS_OFFSET_MM * Z5.z
    ));

    // J7 (DH O6)
    P.push(mat4_get_pos(T_all[5]));

    // End-effector (DH O7)
    P.push(mat4_get_pos(T_all[6]));

    return P;
}

function kin3d_detectCadScale(model) {
    var box = new THREE.Box3().setFromObject(model);
    var size = new THREE.Vector3();
    box.getSize(size);
    var maxDim = Math.max(size.x, size.y, size.z);
    if (!isFinite(maxDim) || maxDim <= 0) return 1.0;
    // SolidWorks glTF ciktilari genelde metre birimindedir; sahne mm oldugu icin m->mm cevir.
    return (maxDim < 2.0) ? 1000.0 : 1.0;
}

function kin3d_setCadVisibility() {
    var vis = (kin3d_settings.useCAD !== false);
    for (var key in kin3d_cadRoots) {
        if (kin3d_cadRoots[key]) kin3d_cadRoots[key].visible = vis && kin3d_isCadPartVisibleBySettings(key);
    }
}

function kin3d_applyCadFrustumCulling() {
    var enableCulling = (kin3d_settings.cadFrustumCulling !== false);
    var applyMap = function(rootMap) {
        for (var key in rootMap) {
            var root = rootMap[key];
            if (!root || !root.traverse) continue;
            root.traverse(function(node) {
                if (!node) return;
                if (node.userData && node.userData.kin3dForceNoFrustumCulling) {
                    node.frustumCulled = false;
                    return;
                }
                if (node.isMesh || node.isLine || node.isPoints || node.isSprite) {
                    node.frustumCulled = !!enableCulling;
                }
            });
        }
    };
    applyMap(kin3d_cadRoots);
    applyMap(kin3d_cadGhostRoots);
}

function kin3d_setCadGhostVisibility(vis) {
    var v = !!vis;
    for (var key in kin3d_cadGhostRoots) {
        if (kin3d_cadGhostRoots[key]) kin3d_cadGhostRoots[key].visible = v && kin3d_isCadPartVisibleBySettings(key);
    }
}

function kin3d_buildCadGhostRoot(sourceRoot, partId) {
    if (!sourceRoot || !kin3d_scene) return null;

    var prev = kin3d_cadGhostRoots[partId];
    if (prev) kin3d_scene.remove(prev);

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
    kin3d_scene.add(ghostRoot);
    kin3d_cadGhostRoots[partId] = ghostRoot;
    return ghostRoot;
}

function kin3d_quatFromDH(Ti) {
    var rm = new THREE.Matrix4();
    rm.set(
        Ti[0], Ti[2], Ti[1], 0,
        Ti[8], Ti[10], Ti[9], 0,
        Ti[4], Ti[6], Ti[5], 0,
        0, 0, 0, 1
    );
    return new THREE.Quaternion().setFromRotationMatrix(rm);
}

function kin3d_partGlobalOffsetQuat(part) {
    var qOff = new THREE.Quaternion().identity();
    if (part.globalRotSeq && part.globalRotSeq.length) {
        for (var i = 0; i < part.globalRotSeq.length; i++) {
            var step = part.globalRotSeq[i];
            var axis = new THREE.Vector3(
                step.axis === 'x' ? 1 : 0,
                step.axis === 'y' ? 1 : 0,
                step.axis === 'z' ? 1 : 0
            );
            var qs = new THREE.Quaternion().setFromAxisAngle(
                axis,
                THREE.MathUtils.degToRad(step.deg || 0)
            );
            // Global sirali rotasyon: q_total = q_stepN * ... * q_step1
            qOff.premultiply(qs);
        }
        return qOff;
    }

    var eOff = new THREE.Euler(
        THREE.MathUtils.degToRad(part.rotDeg.x || 0),
        THREE.MathUtils.degToRad(part.rotDeg.y || 0),
        THREE.MathUtils.degToRad(part.rotDeg.z || 0),
        'XYZ'
    );
    qOff.setFromEuler(eOff);
    return qOff;
}

function kin3d_getCadPartOpacity(part) {
    if (!part) return 1.0;
    if (!isFinite(part.viewOpacity)) part.viewOpacity = 1.0;
    return Math.max(0.05, Math.min(1.0, part.viewOpacity));
}

function kin3d_setCadPartOpacity(partId, opacity) {
    var part = kin3d_getCadPartById(partId);
    var root = kin3d_cadRoots[partId];
    if (!part || !root) return;

    part.viewOpacity = Math.max(0.05, Math.min(1.0, opacity));
    var op = part.viewOpacity;

    root.traverse(function(node) {
        if (!node.isMesh || !node.material) return;
        var mats = Array.isArray(node.material) ? node.material : [node.material];
        for (var i = 0; i < mats.length; i++) {
            var m = mats[i];
            if (!m) continue;
            m.transparent = (op < 0.999);
            m.opacity = op;
            m.needsUpdate = true;
        }
    });
}

function kin3d_toggleCadPartOpacity(partId) {
    var part = kin3d_getCadPartById(partId);
    if (!part) return;
    var cur = kin3d_getCadPartOpacity(part);
    var next = (cur < 0.999) ? 1.0 : 0.5;
    kin3d_setCadPartOpacity(partId, next);
}

function kin3d_findCadPartIdFromObject(obj) {
    var cur = obj;
    while (cur) {
        if (cur.userData && cur.userData.kin3dCadPartId) return cur.userData.kin3dCadPartId;
        cur = cur.parent;
    }
    return null;
}

function kin3d_updateCadContextMenuText() {
    var part = kin3d_getCadPartById(kin3d_cadContextTargetPartId);
    if (!part) return;
    if (kin3d_cadContextOpacityBtn) {
        kin3d_cadContextOpacityBtn.textContent = (kin3d_getCadPartOpacity(part) < 0.999)
            ? 'Tam opak yap'
            : 'Yari seffaf yap';
    }
    if (kin3d_cadContextMaterialSelect) {
        kin3d_populateCadMaterialSelect();
        kin3d_cadContextMaterialSelect.value = part.materialId || '';
        kin3d_cadContextMaterialSelect.disabled = !kin3d_mechanics.loaded;
    }
    if (kin3d_cadContextMaterialInfo) {
        var computed = kin3d_mechanics.computedMassProperties[part.id] || null;
        var massText = computed && computed.mass_kg > 0 ? (' | m ' + Number(computed.mass_kg).toFixed(3) + ' kg') : '';
        var source = part.materialOverrideSource === 'browser_override' ? 'override' : 'csv';
        kin3d_cadContextMaterialInfo.textContent = kin3d_getMaterialSummary(part.materialId) + massText + ' | ' + source;
    }
}

function kin3d_hideCadContextMenu() {
    if (kin3d_cadContextMenu) kin3d_cadContextMenu.style.display = 'none';
    kin3d_cadContextTargetPartId = null;
}

function kin3d_toggleCadContextOpacity() {
    if (!kin3d_cadContextTargetPartId) return;
    kin3d_toggleCadPartOpacity(kin3d_cadContextTargetPartId);
    kin3d_updateCadContextMenuText();
}

function kin3d_populateCadMaterialSelect() {
    if (!kin3d_cadContextMaterialSelect) return;
    var currentCount = Number(kin3d_cadContextMaterialSelect.getAttribute('data-material-count') || 0);
    var ids = Object.keys(kin3d_mechanics.materials || {});
    if (currentCount === ids.length && currentCount > 0) return;
    ids.sort(function(a, b) {
        var ia = kin3d_mechanics.materialIndex[a] || {};
        var ib = kin3d_mechanics.materialIndex[b] || {};
        var ga = String(ia.family || '') + ' ' + kin3d_getMaterialDisplayName(a);
        var gb = String(ib.family || '') + ' ' + kin3d_getMaterialDisplayName(b);
        return ga.localeCompare(gb);
    });
    kin3d_cadContextMaterialSelect.innerHTML = '';
    for (var i = 0; i < ids.length; i++) {
        var opt = document.createElement('option');
        opt.value = ids[i];
        opt.textContent = ids[i] + ' - ' + kin3d_getMaterialDisplayName(ids[i]);
        kin3d_cadContextMaterialSelect.appendChild(opt);
    }
    kin3d_cadContextMaterialSelect.setAttribute('data-material-count', String(ids.length));
}

function kin3d_applyCadContextMaterial() {
    if (!kin3d_cadContextTargetPartId || !kin3d_cadContextMaterialSelect) return;
    var materialId = kin3d_cadContextMaterialSelect.value;
    if (!materialId) return;
    kin3d_setPartMaterialOverride(kin3d_cadContextTargetPartId, materialId);
}

function kin3d_resetCadContextMaterial() {
    if (!kin3d_cadContextTargetPartId) return;
    kin3d_clearPartMaterialOverride(kin3d_cadContextTargetPartId);
}

function kin3d_showCadContextMenu(clientX, clientY, partId) {
    if (!kin3d_cadContextMenu) return;
    var part = kin3d_getCadPartById(partId);
    if (!part) return;

    kin3d_cadContextTargetPartId = partId;
    if (kin3d_cadContextTitle) kin3d_cadContextTitle.textContent = kin3d_getCadPartLabel(part);
    kin3d_updateCadContextMenuText();

    var container = document.getElementById('canvas-container');
    if (!container) return;
    var rect = container.getBoundingClientRect();
    var x = clientX - rect.left + 10;
    var y = clientY - rect.top + 10;
    kin3d_cadContextMenu.style.left = x + 'px';
    kin3d_cadContextMenu.style.top = y + 'px';
    kin3d_cadContextMenu.style.display = 'block';
}

function kin3d_createCadContextMenu(container) {
    var menu = document.createElement('div');
    menu.id = 'kin3d-cad-context-menu';
    menu.style.cssText = 'position:absolute;display:none;left:0;top:0;min-width:300px;max-width:420px;background:rgba(25,25,28,0.96);border:1px solid #5A7EBF;border-radius:8px;padding:8px;z-index:35;font-family:Inter,sans-serif;font-size:12px;color:#D8E1F2;box-shadow:0 8px 20px rgba(0,0,0,0.45);';
    menu.onclick = function(e){ e.stopPropagation(); };

    var title = document.createElement('div');
    title.style.cssText = 'font-size:11px;color:#9FB5D8;margin-bottom:6px;';
    title.textContent = 'Parca';

    var btn = document.createElement('button');
    btn.style.cssText = 'width:100%;background:#3A3A40;color:#E8ECF0;border:1px solid #5A5A62;border-radius:6px;padding:6px 8px;cursor:pointer;font-size:11px;font-weight:700;';
    btn.textContent = 'Yari seffaf yap';
    btn.onclick = function(e){ e.stopPropagation(); kin3d_toggleCadContextOpacity(); };

    var materialLabel = document.createElement('div');
    materialLabel.style.cssText = 'font-size:10px;color:#9FB5D8;margin:8px 0 4px;';
    materialLabel.textContent = 'Malzeme';

    var materialSelect = document.createElement('select');
    materialSelect.style.cssText = 'width:100%;background:#20242A;color:#E8ECF0;border:1px solid #5A5A62;border-radius:6px;padding:6px 8px;cursor:pointer;font-size:11px;';
    materialSelect.onchange = function(e){ e.stopPropagation(); kin3d_applyCadContextMaterial(); };

    var resetBtn = document.createElement('button');
    resetBtn.style.cssText = 'width:100%;margin-top:6px;background:#2F3640;color:#D8E1F2;border:1px solid #5A5A62;border-radius:6px;padding:6px 8px;cursor:pointer;font-size:11px;font-weight:700;';
    resetBtn.textContent = 'CSV varsayilana don';
    resetBtn.onclick = function(e){ e.stopPropagation(); kin3d_resetCadContextMaterial(); };

    var materialInfo = document.createElement('div');
    materialInfo.style.cssText = 'margin-top:6px;color:#B8C6D8;font-family:monospace;font-size:10px;line-height:1.35;white-space:normal;';
    materialInfo.textContent = 'Malzeme verisi yukleniyor';

    menu.appendChild(title);
    menu.appendChild(btn);
    menu.appendChild(materialLabel);
    menu.appendChild(materialSelect);
    menu.appendChild(resetBtn);
    menu.appendChild(materialInfo);
    container.appendChild(menu);

    kin3d_cadContextMenu = menu;
    kin3d_cadContextTitle = title;
    kin3d_cadContextOpacityBtn = btn;
    kin3d_cadContextMaterialSelect = materialSelect;
    kin3d_cadContextMaterialInfo = materialInfo;
    kin3d_cadContextMaterialResetBtn = resetBtn;
}

function kin3d_openCadContextMenuFromEvent(evt) {
    if (!kin3d_renderer || !kin3d_camera || !kin3d_raycaster) return;
    var dom = kin3d_renderer.domElement;
    if (!dom) return;

    var rect = dom.getBoundingClientRect();
    var x = ((evt.clientX - rect.left) / rect.width) * 2 - 1;
    var y = -((evt.clientY - rect.top) / rect.height) * 2 + 1;
    var pointer = new THREE.Vector2(x, y);
    kin3d_raycaster.setFromCamera(pointer, kin3d_camera);

    var cadObjects = [];
    for (var key in kin3d_cadRoots) {
        if (kin3d_cadRoots[key] && kin3d_cadRoots[key].visible) cadObjects.push(kin3d_cadRoots[key]);
    }
    if (!cadObjects.length) {
        kin3d_hideCadContextMenu();
        return;
    }

    var hits = kin3d_raycaster.intersectObjects(cadObjects, true);
    if (!hits || !hits.length) {
        kin3d_hideCadContextMenu();
        return;
    }

    var partId = null;
    for (var i = 0; i < hits.length; i++) {
        partId = kin3d_findCadPartIdFromObject(hits[i].object);
        if (partId) break;
    }
    if (!partId) {
        kin3d_hideCadContextMenu();
        return;
    }

    kin3d_showCadContextMenu(evt.clientX, evt.clientY, partId);
}

function kin3d_getCadPartById(partId) {
    for (var i = 0; i < kin3d_cadParts.length; i++) {
        if (kin3d_cadParts[i].id === partId) return kin3d_cadParts[i];
    }
    return null;
}

function kin3d_getCadPartLabel(part) {
    if (!part) return '';
    var base = (part.label && typeof part.label === 'string' && part.label.length) ? part.label : (part.id || '');
    return part.materialId ? (base + ' [' + part.materialId + ']') : base;
}

function kin3d_getCalibratableCadParts() {
    var out = [];
    for (var i = 0; i < kin3d_cadParts.length; i++) {
        var part = kin3d_cadParts[i];
        var id = (part.id || '').toLowerCase();
        if (id === 'planet_gear_2' || id === 'planet_gear_3') continue;
        if (part.calibratable === true || id.indexOf('arm') === 0) out.push(part);
    }
    return out;
}

function kin3d_isPlanetGearGroupPartId(partId) {
    return partId === 'planet_gear_1';
}

function kin3d_getPlanetGearParts() {
    var ids = ['planet_gear_1', 'planet_gear_2', 'planet_gear_3'];
    var out = [];
    for (var i = 0; i < ids.length; i++) {
        var p = kin3d_getCadPartById(ids[i]);
        if (p) out.push(p);
    }
    return out;
}

function kin3d_getPlanetGearGroupAnchor() {
    return kin3d_getCadPartById('planet_gear_1');
}

function kin3d_getActiveCadCalibParts() {
    var activeId = kin3d_cadCalibState.activePartId;
    if (kin3d_isPlanetGearGroupPartId(activeId)) {
        var anchor = kin3d_getPlanetGearGroupAnchor();
        return anchor ? [anchor] : [];
    }
    var part = kin3d_getCadPartById(activeId);
    return part ? [part] : [];
}

function kin3d_refreshPlanetGearExtraInputs() {
    if (!kin3d_cadCalibPlanetExtraWrap) return;
    var visible = kin3d_isPlanetGearGroupPartId(kin3d_cadCalibState.activePartId);
    kin3d_cadCalibPlanetExtraWrap.style.display = visible ? 'block' : 'none';
    if (!visible) return;

    var gears = kin3d_getPlanetGearParts();
    if (!gears.length) return;
    var g1 = gears[0];
    var g2 = gears.length > 1 ? gears[1] : g1;
    var radius = Number(g1.orbitRadiusMm || 0);
    var spacing = Number((g2.orbitDeg || 0) - (g1.orbitDeg || 0));
    if (!isFinite(spacing)) spacing = 120;
    if (!isFinite(radius)) radius = 45;

    if (kin3d_cadCalibPlanetSpacingInput) kin3d_cadCalibPlanetSpacingInput.value = spacing.toFixed(1);
    if (kin3d_cadCalibPlanetRadiusInput) kin3d_cadCalibPlanetRadiusInput.value = radius.toFixed(1);
    if (kin3d_cadCalibPlanetSpin1Input) kin3d_cadCalibPlanetSpin1Input.value = Number(g1.spinOffsetDeg || 0).toFixed(1);
    if (kin3d_cadCalibPlanetSpin2Input) kin3d_cadCalibPlanetSpin2Input.value = Number((gears[1] && gears[1].spinOffsetDeg) || 0).toFixed(1);
    if (kin3d_cadCalibPlanetSpin3Input) kin3d_cadCalibPlanetSpin3Input.value = Number((gears[2] && gears[2].spinOffsetDeg) || 0).toFixed(1);
}

function kin3d_parseLocaleFloat(inputEl, fallback) {
    if (!inputEl) return fallback;
    var raw = String(inputEl.value || '').trim();
    if (!raw.length) return fallback;
    raw = raw.replace(',', '.');
    var v = parseFloat(raw);
    return isFinite(v) ? v : fallback;
}

function kin3d_applyPlanetGearGroupInputs() {
    var gears = kin3d_getPlanetGearParts();
    if (gears.length < 3) return;

    var spacing = kin3d_parseLocaleFloat(kin3d_cadCalibPlanetSpacingInput, 120);
    if (!isFinite(spacing)) spacing = 120;
    spacing = Math.max(-360, Math.min(360, spacing));

    var radius = kin3d_parseLocaleFloat(kin3d_cadCalibPlanetRadiusInput, 45);
    if (!isFinite(radius)) radius = 45;
    radius = Math.max(0, Math.min(500, radius));

    var spin1 = kin3d_parseLocaleFloat(kin3d_cadCalibPlanetSpin1Input, 0);
    var spin2 = kin3d_parseLocaleFloat(kin3d_cadCalibPlanetSpin2Input, 0);
    var spin3 = kin3d_parseLocaleFloat(kin3d_cadCalibPlanetSpin3Input, 0);
    if (!isFinite(spin1)) spin1 = 0;
    if (!isFinite(spin2)) spin2 = 0;
    if (!isFinite(spin3)) spin3 = 0;

    var baseOrbit = Number(gears[0].orbitDeg || 0);
    gears[0].orbitRadiusMm = radius;
    gears[1].orbitRadiusMm = radius;
    gears[2].orbitRadiusMm = radius;
    gears[1].orbitDeg = baseOrbit + spacing;
    gears[2].orbitDeg = baseOrbit + (2 * spacing);
    gears[0].spinOffsetDeg = spin1;
    gears[1].spinOffsetDeg = spin2;
    gears[2].spinOffsetDeg = spin3;

    if (kin3d_initialized && kin3d_lastAngles && kin3d_lastAngles.length >= 7) {
        kin3d_updatePose(kin3d_lastAngles);
    }
    kin3d_refreshCadCalibInfo();
}

function kin3d_ensureCadCalib(part) {
    if (!part) return { x: 0, y: 0, z: 0 };
    if (!part.calibDeg) part.calibDeg = { x: 0, y: 0, z: 0 };
    if (!isFinite(part.calibDeg.x)) part.calibDeg.x = 0;
    if (!isFinite(part.calibDeg.y)) part.calibDeg.y = 0;
    if (!isFinite(part.calibDeg.z)) part.calibDeg.z = 0;
    return part.calibDeg;
}

function kin3d_ensureCadCalibPos(part) {
    if (!part) return { x: 0, y: 0, z: 0 };
    if (!part.calibPosMm) part.calibPosMm = { x: 0, y: 0, z: 0 };
    if (!isFinite(part.calibPosMm.x)) part.calibPosMm.x = 0;
    if (!isFinite(part.calibPosMm.y)) part.calibPosMm.y = 0;
    if (!isFinite(part.calibPosMm.z)) part.calibPosMm.z = 0;
    return part.calibPosMm;
}

function kin3d_applyCadCalibPos(root, part) {
    var p = kin3d_ensureCadCalibPos(part);
    root.position.x += (p.x || 0);
    root.position.y += (p.y || 0);
    root.position.z += (p.z || 0);
}

function kin3d_partLocalCalibQuat(part) {
    var q = new THREE.Quaternion().identity();
    if (!part) return q;
    var c = kin3d_ensureCadCalib(part);
    var e = new THREE.Euler(
        THREE.MathUtils.degToRad(c.x || 0),
        THREE.MathUtils.degToRad(c.y || 0),
        THREE.MathUtils.degToRad(c.z || 0),
        'XYZ'
    );
    q.setFromEuler(e);
    return q;
}

function kin3d_getCadCalibInfoHtml(part) {
    var c = kin3d_ensureCadCalib(part);
    var p = kin3d_ensureCadCalibPos(part);
    return 'X: ' + c.x.toFixed(1) + '&deg; &nbsp; Y: ' + c.y.toFixed(1) + '&deg; &nbsp; Z: ' + c.z.toFixed(1) + '&deg;'
        + '<br><span style="color:#A5B5BF;">Local rot: { x:' + c.x.toFixed(1) + ', y:' + c.y.toFixed(1) + ', z:' + c.z.toFixed(1) + ' }</span>'
        + '<br><span style="color:#8FD8C3;">Local pos(mm): { x:' + p.x.toFixed(1) + ', y:' + p.y.toFixed(1) + ', z:' + p.z.toFixed(1) + ' }</span>';
}

function kin3d_getCadCalibInfoText(part) {
    var c = kin3d_ensureCadCalib(part);
    var p = kin3d_ensureCadCalibPos(part);
    return 'X: ' + c.x.toFixed(1) + '\u00b0   Y: ' + c.y.toFixed(1) + '\u00b0   Z: ' + c.z.toFixed(1) + '\u00b0\n'
        + 'Local rot: { x:' + c.x.toFixed(1) + ', y:' + c.y.toFixed(1) + ', z:' + c.z.toFixed(1) + ' }\n'
        + 'Local pos(mm): { x:' + p.x.toFixed(1) + ', y:' + p.y.toFixed(1) + ', z:' + p.z.toFixed(1) + ' }';
}

function kin3d_copyCadCalibInfo() {
    var part = kin3d_getCadPartById(kin3d_cadCalibState.activePartId);
    if (!part) return;
    var txt = kin3d_getCadCalibInfoText(part);

    var done = function(ok) {
        if (window && typeof window.showToast === 'function') {
            window.showToast(ok ? 'Kalibrasyon bilgisi kopyalandi' : 'Kopyalama basarisiz');
        }
    };

    if (navigator && navigator.clipboard && typeof navigator.clipboard.writeText === 'function') {
        navigator.clipboard.writeText(txt).then(function() { done(true); }).catch(function() { done(false); });
        return;
    }

    try {
        var ta = document.createElement('textarea');
        ta.value = txt;
        ta.style.position = 'fixed';
        ta.style.left = '-9999px';
        document.body.appendChild(ta);
        ta.select();
        var ok = document.execCommand('copy');
        document.body.removeChild(ta);
        done(!!ok);
    } catch (e) {
        done(false);
    }
}

function kin3d_refreshCadCalibInfo() {
    if (!kin3d_cadCalibValueEl) return;

    var part = kin3d_getCadPartById(kin3d_cadCalibState.activePartId);
    if (!part) {
        kin3d_cadCalibValueEl.innerHTML = 'Parca secili degil';
        kin3d_cadCalibValueEl.style.display = 'block';
        if (kin3d_cadCalibPlanetInfoEl) kin3d_cadCalibPlanetInfoEl.innerHTML = '';
        kin3d_refreshPlanetGearExtraInputs();
        return;
    }

    var infoHtml = kin3d_getCadCalibInfoHtml(part);
    if (kin3d_isPlanetGearGroupPartId(part.id) && kin3d_cadCalibPlanetInfoEl) {
        kin3d_cadCalibPlanetInfoEl.innerHTML = infoHtml;
        kin3d_cadCalibValueEl.style.display = 'none';
    } else {
        kin3d_cadCalibValueEl.innerHTML = infoHtml;
        kin3d_cadCalibValueEl.style.display = 'block';
        if (kin3d_cadCalibPlanetInfoEl) kin3d_cadCalibPlanetInfoEl.innerHTML = '';
    }
    kin3d_refreshPlanetGearExtraInputs();
}

function kin3d_setCadCalibStepFromInput() {
    if (!kin3d_cadCalibStepInput) return;
    var step = parseFloat(kin3d_cadCalibStepInput.value);
    if (!isFinite(step) || step <= 0) step = 5;
    step = Math.min(45, Math.max(0.1, step));
    kin3d_cadCalibState.stepDeg = step;
    kin3d_cadCalibStepInput.value = step.toString();
}

function kin3d_setCadCalibPosStepFromInput() {
    if (!kin3d_cadCalibStepPosInput) return;
    var step = parseFloat(kin3d_cadCalibStepPosInput.value);
    if (!isFinite(step) || step <= 0) step = 1;
    step = Math.min(100, Math.max(0.1, step));
    kin3d_cadCalibState.stepMm = step;
    kin3d_cadCalibStepPosInput.value = step.toString();
}

function kin3d_selectCadCalibPart(partId) {
    var part = kin3d_getCadPartById(partId);
    if (!part) return;
    kin3d_cadCalibState.activePartId = part.id;
    if (kin3d_cadCalibPartSelect) kin3d_cadCalibPartSelect.value = part.id;
    kin3d_refreshCadCalibInfo();
}

function kin3d_nudgeCadCalib(axis, sign) {
    var parts = kin3d_getActiveCadCalibParts();
    if (!parts.length) return;
    var step = kin3d_cadCalibState.stepDeg;
    if (!isFinite(step) || step <= 0) step = 5;
    var delta = (sign >= 0 ? 1 : -1) * step;

    for (var i = 0; i < parts.length; i++) {
        var c = kin3d_ensureCadCalib(parts[i]);
        if (axis === 'x') c.x += delta;
        if (axis === 'y') c.y += delta;
        if (axis === 'z') c.z += delta;
    }

    if (kin3d_initialized && kin3d_lastAngles && kin3d_lastAngles.length >= 7) {
        kin3d_updatePose(kin3d_lastAngles);
    }
    kin3d_refreshCadCalibInfo();
}

function kin3d_nudgeCadCalibPos(axis, sign) {
    var parts = kin3d_getActiveCadCalibParts();
    if (!parts.length) return;
    var step = kin3d_cadCalibState.stepMm;
    if (!isFinite(step) || step <= 0) step = 1;
    var delta = (sign >= 0 ? 1 : -1) * step;

    for (var i = 0; i < parts.length; i++) {
        var p = kin3d_ensureCadCalibPos(parts[i]);
        if (axis === 'x') p.x += delta;
        if (axis === 'y') p.y += delta;
        if (axis === 'z') p.z += delta;
    }

    if (kin3d_initialized && kin3d_lastAngles && kin3d_lastAngles.length >= 7) {
        kin3d_updatePose(kin3d_lastAngles);
    }
    kin3d_refreshCadCalibInfo();
}

function kin3d_resetCadCalibSelected() {
    var parts = kin3d_getActiveCadCalibParts();
    if (!parts.length) return;
    for (var i = 0; i < parts.length; i++) {
        parts[i].calibDeg = { x: 0, y: 0, z: 0 };
        parts[i].calibPosMm = { x: 0, y: 0, z: 0 };
    }
    if (kin3d_isPlanetGearGroupPartId(kin3d_cadCalibState.activePartId)) {
        var planets = kin3d_getPlanetGearParts();
        if (planets.length >= 3) {
            planets[0].orbitRadiusMm = 45;
            planets[1].orbitRadiusMm = 45;
            planets[2].orbitRadiusMm = 45;
            planets[0].orbitDeg = 0;
            planets[1].orbitDeg = 120;
            planets[2].orbitDeg = 240;
            planets[0].calibDeg = { x: 0.0, y: -30.0, z: 0.0 };
            planets[1].calibDeg = { x: 0.0, y: -30.0, z: 0.0 };
            planets[2].calibDeg = { x: 0.0, y: -30.0, z: 0.0 };
            planets[0].spinOffsetDeg = 30;
            planets[1].spinOffsetDeg = 30;
            planets[2].spinOffsetDeg = 30;
        }
    }
    if (kin3d_initialized && kin3d_lastAngles && kin3d_lastAngles.length >= 7) {
        kin3d_updatePose(kin3d_lastAngles);
    }
    kin3d_refreshCadCalibInfo();
}

function kin3d_resetCadCalibAll() {
    var parts = kin3d_getCalibratableCadParts();
    var planets = kin3d_getPlanetGearParts();
    for (var p = 0; p < planets.length; p++) {
        if (parts.indexOf(planets[p]) < 0) parts.push(planets[p]);
    }
    for (var i = 0; i < parts.length; i++) {
        parts[i].calibDeg = { x: 0, y: 0, z: 0 };
        parts[i].calibPosMm = { x: 0, y: 0, z: 0 };
    }
    if (planets.length >= 3) {
        planets[0].orbitRadiusMm = 45;
        planets[1].orbitRadiusMm = 45;
        planets[2].orbitRadiusMm = 45;
        planets[0].orbitDeg = 0;
        planets[1].orbitDeg = 120;
        planets[2].orbitDeg = 240;
        planets[0].calibDeg = { x: 0.0, y: -30.0, z: 0.0 };
        planets[1].calibDeg = { x: 0.0, y: -30.0, z: 0.0 };
        planets[2].calibDeg = { x: 0.0, y: -30.0, z: 0.0 };
        planets[0].spinOffsetDeg = 30;
        planets[1].spinOffsetDeg = 30;
        planets[2].spinOffsetDeg = 30;
    }
    if (kin3d_initialized && kin3d_lastAngles && kin3d_lastAngles.length >= 7) {
        kin3d_updatePose(kin3d_lastAngles);
    }
    kin3d_refreshCadCalibInfo();
}

function kin3d_toggleCadCalibPopup() {
    if (!kin3d_cadCalibModal) return;
    kin3d_cadCalibModal.style.display = (kin3d_cadCalibModal.style.display === 'none') ? 'block' : 'none';
    if (kin3d_cadCalibModal.style.display !== 'none') {
        kin3d_refreshCadCalibInfo();
    }
}

function kin3d_attachCadModel(model, part) {
    if (!model) return false;

    if (part.centerOnLoad) {
        // Koordinat merkezi model bbox merkezine cekilir.
        var preBox = new THREE.Box3().setFromObject(model);
        var preCenter = new THREE.Vector3();
        preBox.getCenter(preCenter);
        model.position.sub(preCenter);
    }

    var cadRoot = new THREE.Group();
    cadRoot.name = 'cad_' + part.id;
    cadRoot.userData.kin3dCadPartId = part.id;
    var unitScale = kin3d_detectCadScale(model);
    model.scale.set(unitScale, unitScale, unitScale);
    model.userData.kin3dCadPartId = part.id;
    model.traverse(function(node) {
        node.userData = node.userData || {};
        node.userData.kin3dCadPartId = part.id;
        if (node.isMesh) {
            node.castShadow = false;
            node.receiveShadow = true;
            if (node.material) {
                node.material.side = THREE.DoubleSide;
                node.material.needsUpdate = true;
            }
        }
    });
    cadRoot.add(model);
    kin3d_scene.add(cadRoot);

    kin3d_cadRoots[part.id] = cadRoot;
    kin3d_buildCadGhostRoot(cadRoot, part.id);
    kin3d_setCadPartOpacity(part.id, kin3d_getCadPartOpacity(part));
    kin3d_setCadVisibility();
    kin3d_setCadGhostVisibility(false);
    var _a0 = kin3d_lastAngles || [0,0,0,0,0,0,0];
    var _t0 = compute_FK(_a0);
    var _p0 = extract_positions(_t0);
    kin3d_updateCadPlacements(_a0, _p0, _t0);
    kin3d_updateCadPlacementsForRoots(kin3d_cadGhostRoots, _a0, _p0, _t0);
    kin3d_refreshMechanicalEstimatesFromCad();
    kin3d_updateMechanicalForceSnapshot(_a0, _t0);
    kin3d_applyMeshMaterialSettings(cadRoot);
    kin3d_applyGhostSettings();
    kin3d_applyCadFrustumCulling();
    console.log('CAD loaded:', part.id);
    return true;
}

function kin3d_planetGearSpinDegFromCarrierYaw(carrierYawDeg, part) {
    var ns = Number((part && isFinite(part.sunTeeth)) ? part.sunTeeth : kin3d_planetarySunTeeth);
    var np = Number((part && isFinite(part.planetTeeth)) ? part.planetTeeth : kin3d_planetaryPlanetTeeth);
    var nr = Number((part && isFinite(part.ringTeeth)) ? part.ringTeeth : kin3d_planetaryRingTeeth);
    if (!(ns > 0) || !(np > 0) || !(nr > 0)) return carrierYawDeg;

    // Willis (ring fixed) with CAD transform chain:
    // root already contains carrier yaw, so here we must return the planet's
    // LOCAL spin relative to carrier: (w_p - w_c) = -(Nr/Np) * w_c
    var spinScale = -(nr / np);
    return carrierYawDeg * spinScale;
}

function kin3d_updateCadPlacementsForRoots(rootMap, angles_deg, P, T_all) {
    var turretYawDeg = (angles_deg && angles_deg.length > 0) ? angles_deg[0] : 0;
    var qYaw = new THREE.Quaternion().setFromAxisAngle(
        new THREE.Vector3(0, 1, 0),
        THREE.MathUtils.degToRad(turretYawDeg)
    );

    for (var i = 0; i < kin3d_cadParts.length; i++) {
        var part = kin3d_cadParts[i];
        var root = rootMap[part.id];
        if (!root) continue;

        var qOff = kin3d_partGlobalOffsetQuat(part);
        var qCal = kin3d_partLocalCalibQuat(part);

        if (part.placement === 'world_static') {
            root.position.set(part.pos.x || 0, part.pos.y || 0, part.pos.z || 0);
            kin3d_applyCadCalibPos(root, part);
            root.quaternion.copy(qOff);
            root.quaternion.multiply(qCal);
            continue;
        }

        if (part.placement === 'turret_rotating_height') {
            var h = part.heightMm || 0;
            var partYawDeg = turretYawDeg * (part.yawSign || 1) + (part.yawOffsetDeg || 0);
            var qPartYaw = new THREE.Quaternion().setFromAxisAngle(
                new THREE.Vector3(0, 1, 0),
                THREE.MathUtils.degToRad(partYawDeg)
            );
            root.position.set(part.pos.x || 0, h + (part.pos.y || 0), part.pos.z || 0);
            kin3d_applyCadCalibPos(root, part);
            root.quaternion.copy(qPartYaw);
            root.quaternion.premultiply(qOff);
            root.quaternion.multiply(qCal);
            continue;
        }

        if (part.placement === 'turret_planetary_gear') {
            var carrierYawDeg = turretYawDeg * (part.yawSign || 1) + (part.yawOffsetDeg || 0);
            var orbitDeg = (part.orbitDeg || 0);
            var orbitRad = THREE.MathUtils.degToRad(orbitDeg);
            var radiusMm = part.orbitRadiusMm || 0;
            var hPlanet = part.heightMm || 0;
            var qCarrierYaw = new THREE.Quaternion().setFromAxisAngle(
                new THREE.Vector3(0, 1, 0),
                THREE.MathUtils.degToRad(carrierYawDeg)
            );
            var groupAnchor = kin3d_getPlanetGearGroupAnchor();
            var qGroup = kin3d_partLocalCalibQuat(groupAnchor);
            var groupPos = kin3d_ensureCadCalibPos(groupAnchor);
            var localVec = new THREE.Vector3(
                (part.pos.x || 0) + radiusMm * Math.cos(orbitRad),
                hPlanet + (part.pos.y || 0),
                (part.pos.z || 0) + radiusMm * Math.sin(orbitRad)
            );
            localVec.applyQuaternion(qGroup);
            localVec.x += (groupPos.x || 0);
            localVec.y += (groupPos.y || 0);
            localVec.z += (groupPos.z || 0);
            localVec.applyQuaternion(qCarrierYaw);

            var planetSpinDeg = kin3d_planetGearSpinDegFromCarrierYaw(carrierYawDeg, part) + (part.spinOffsetDeg || 0);
            var qPlanetSpin = new THREE.Quaternion().setFromAxisAngle(
                new THREE.Vector3(0, 1, 0),
                THREE.MathUtils.degToRad(planetSpinDeg)
            );

            root.position.copy(localVec);
            root.quaternion.copy(qCarrierYaw);
            root.quaternion.multiply(qGroup);
            root.quaternion.multiply(qPlanetSpin);
            root.quaternion.multiply(qOff);
            continue;
        }

        if (part.placement === 'j1_j2_mid') {
            if (!P || P.length < 3) continue;
            var p1 = new THREE.Vector3(P[1].x, P[1].z, P[1].y);
            var p2 = new THREE.Vector3(P[2].x, P[2].z, P[2].y);
            var mid = new THREE.Vector3().addVectors(p1, p2).multiplyScalar(0.5);

            root.position.copy(mid);
            root.position.x += (part.pos.x || 0);
            root.position.y += (part.pos.y || 0);
            root.position.z += (part.pos.z || 0);
            kin3d_applyCadCalibPos(root, part);
            if (T_all && T_all.length > 1) {
                // Arm1 (J1->J2 link) should follow the post-J1 link angle, including J2 contribution
                root.quaternion.copy(kin3d_quatFromDH(T_all[1]));
            } else {
                root.quaternion.copy(qYaw);
            }
            // Arms must follow FK matrix first; fixed model offsets are applied in local frame.
            root.quaternion.multiply(qOff);
            root.quaternion.multiply(qCal);
            continue;
        }

        if (part.placement === 'j2_center') {
            if (!P || P.length < 3) continue;

            root.position.set(
                P[2].x + (part.pos.x || 0),
                P[2].z + (part.pos.y || 0),
                P[2].y + (part.pos.z || 0)
            );
            kin3d_applyCadCalibPos(root, part);

            if (T_all && T_all.length > 2) {
                // Arm2 follows the FK orientation after J2 link (towards J3)
                root.quaternion.copy(kin3d_quatFromDH(T_all[2]));
            } else if (T_all && T_all.length > 1) {
                root.quaternion.copy(kin3d_quatFromDH(T_all[1]));
            } else {
                root.quaternion.copy(qYaw);
            }
            root.quaternion.multiply(qOff);
            root.quaternion.multiply(qCal);
            continue;
        }

        if (part.placement === 'j5_center_j4_frame') {
            if (!P || P.length < 6) continue;

            root.position.set(
                P[5].x + (part.pos.x || 0),
                P[5].z + (part.pos.y || 0),
                P[5].y + (part.pos.z || 0)
            );
            kin3d_applyCadCalibPos(root, part);

            // Place origin at J5, but orientation follows J4 frame (affected by J4)
            if (T_all && T_all.length > 3) {
                root.quaternion.copy(kin3d_quatFromDH(T_all[3]));
            } else {
                root.quaternion.copy(qYaw);
            }
            root.quaternion.multiply(qOff);
            root.quaternion.multiply(qCal);
            continue;
        }

        if (part.placement === 'j5_center_j5_frame') {
            if (!P || P.length < 6) continue;

            root.position.set(
                P[5].x + (part.pos.x || 0),
                P[5].z + (part.pos.y || 0),
                P[5].y + (part.pos.z || 0)
            );
            kin3d_applyCadCalibPos(root, part);

            // Place origin at J5, orientation follows J5 frame (affected by J5)
            if (T_all && T_all.length > 4) {
                root.quaternion.copy(kin3d_quatFromDH(T_all[4]));
            } else if (T_all && T_all.length > 3) {
                root.quaternion.copy(kin3d_quatFromDH(T_all[3]));
            } else {
                root.quaternion.copy(qYaw);
            }
            root.quaternion.multiply(qOff);
            root.quaternion.multiply(qCal);
            continue;
        }

        if (part.placement === 'j7_center_j6_frame') {
            if (!P || P.length < 8) continue;

            root.position.set(
                P[7].x + (part.pos.x || 0),
                P[7].z + (part.pos.y || 0),
                P[7].y + (part.pos.z || 0)
            );
            kin3d_applyCadCalibPos(root, part);

            // Place origin at J7, orientation follows J6 frame (affected by J6).
            if (T_all && T_all.length > 5) {
                root.quaternion.copy(kin3d_quatFromDH(T_all[5]));
            } else if (T_all && T_all.length > 4) {
                root.quaternion.copy(kin3d_quatFromDH(T_all[4]));
            } else {
                root.quaternion.copy(qYaw);
            }
            root.quaternion.multiply(qOff);
            root.quaternion.multiply(qCal);
            continue;
        }

        if (part.placement === 'j7_center_j7_frame') {
            if (!P || P.length < 8) continue;

            root.position.set(
                P[7].x + (part.pos.x || 0),
                P[7].z + (part.pos.y || 0),
                P[7].y + (part.pos.z || 0)
            );
            kin3d_applyCadCalibPos(root, part);

            // Place origin at J7, orientation follows J7 frame (affected by J7).
            if (T_all && T_all.length > 6) {
                root.quaternion.copy(kin3d_quatFromDH(T_all[6]));
            } else if (T_all && T_all.length > 5) {
                root.quaternion.copy(kin3d_quatFromDH(T_all[5]));
            } else {
                root.quaternion.copy(qYaw);
            }
            root.quaternion.multiply(qOff);
            root.quaternion.multiply(qCal);
            continue;
        }
    }
}

function kin3d_updateCadPlacements(angles_deg, P, T_all) {
    kin3d_updateCadPlacementsForRoots(kin3d_cadRoots, angles_deg, P, T_all);
}

function kin3d_collisionPairKey(idA, idB) {
    var a = String(idA || '').toLowerCase();
    var b = String(idB || '').toLowerCase();
    return (a < b) ? (a + '|' + b) : (b + '|' + a);
}

function kin3d_shouldCheckCollisionPair(idA, idB) {
    var key = kin3d_collisionPairKey(idA, idB);
    if (key === '|') return false;
    if (key.indexOf('arm') !== 0) return false;
    return !kin3d_collisionIgnoredPairs[key];
}

function kin3d_createCollisionOverlay(container) {
    if (!container || kin3d_collisionLabel) return;
    kin3d_collisionLabel = document.createElement('div');
    kin3d_collisionLabel.id = 'kin3d-collision-label';
    kin3d_collisionLabel.style.cssText = 'position:absolute;bottom:10px;right:10px;background:rgba(24,24,24,0.92);color:#9BEB5D;font-family:monospace;font-size:12px;padding:0 10px;border-radius:8px;pointer-events:none;z-index:22;border:none;max-width:340px;height:34px;display:flex;align-items:center;';
    kin3d_collisionLabel.innerText = 'CAD CAKISMA: Yok';
    container.appendChild(kin3d_collisionLabel);
    kin3d_layoutStatusOverlays();
}

function kin3d_createIkBackendOverlay(container) {
    if (!container || kin3d_ikBackendLabel) return;
    kin3d_ikBackendLabel = document.createElement('div');
    kin3d_ikBackendLabel.id = 'kin3d-ik-backend-label';
    kin3d_ikBackendLabel.style.cssText = 'position:absolute;bottom:10px;right:140px;background:rgba(24,24,24,0.92);color:#4DB8FF;font-family:monospace;font-size:12px;padding:0 10px;border-radius:8px;pointer-events:none;z-index:22;border:none;height:34px;display:flex;align-items:center;';
    kin3d_ikBackendLabel.innerText = 'IK: WEB';
    container.appendChild(kin3d_ikBackendLabel);
    kin3d_layoutStatusOverlays();
    kin3d_refreshIkBackendLabel();
}

function kin3d_layoutStatusOverlays() {
    var baseRight = 10;
    var gap = 8;
    var currentRight = baseRight;

    // Place status labels to the left of CAD calibration button.
    if (kin3d_cadCalibBtn) {
        var btnRight = parseFloat(kin3d_cadCalibBtn.style.right);
        if (!Number.isFinite(btnRight)) btnRight = baseRight;
        var btnWidth = kin3d_cadCalibBtn.offsetWidth || 0;
        currentRight = btnRight + btnWidth + gap;
    }

    if (kin3d_collisionLabel && kin3d_collisionLabel.style.display !== 'none') {
        kin3d_collisionLabel.style.right = currentRight + 'px';
        currentRight += (kin3d_collisionLabel.offsetWidth || 0) + gap;
    }
    if (kin3d_ikBackendLabel && kin3d_ikBackendLabel.style.display !== 'none') {
        kin3d_ikBackendLabel.style.right = currentRight + 'px';
    }
}

function kin3d_refreshIkBackendLabel() {
    if (!kin3d_ikBackendLabel) return;
    if (!kin3d_ikFallbackEnabled) {
        kin3d_ikBackendLabel.style.display = 'none';
        kin3d_layoutStatusOverlays();
        return;
    }

    kin3d_ikBackendLabel.style.display = 'flex';
    var mode = String(kin3d_ikBackendMode || 'WEB').toUpperCase();
    if (mode !== 'T41-QSPI' && mode !== 'T41-ESP-NOW' && mode !== 'WEB') {
        mode = 'WEB';
    }

    if (mode === 'T41-QSPI') {
        kin3d_ikBackendLabel.style.color = '#9BEB5D';
    } else if (mode === 'T41-ESP-NOW') {
        kin3d_ikBackendLabel.style.color = '#EAB96A';
    } else {
        kin3d_ikBackendLabel.style.color = '#4DB8FF';
    }
    kin3d_ikBackendLabel.innerText = 'IK: ' + mode;
    kin3d_layoutStatusOverlays();
}

function kin3d_createCadVersionOverlay(container) {
    if (!container || kin3d_cadVersionLabel) return;
    kin3d_cadVersionLabel = document.createElement('div');
    kin3d_cadVersionLabel.id = 'kin3d-cad-version-label';
    kin3d_cadVersionLabel.style.cssText = 'position:absolute;bottom:10px;left:10px;background:rgba(24,24,24,0.92);color:#F0F2F3;font-family:monospace;font-size:11px;padding:0 10px;border-radius:8px;pointer-events:none;z-index:22;border:none;height:30px;display:flex;align-items:center;';
    kin3d_cadVersionLabel.innerText = 'CAD v: -';
    container.appendChild(kin3d_cadVersionLabel);
    kin3d_updateCadVersionLabel();
    kin3d_createMechanicsOverlay(container);
}

function kin3d_updateCadVersionLabel() {
    if (!kin3d_cadVersionLabel) return;
    var tag = String(kin3d_cadVersion || 'unknown');
    kin3d_cadVersionLabel.innerText = 'CAD v: ' + tag;
}

function kin3d_createMechanicsOverlay(container) {
    if (!container || kin3d_mechanicsLabel) return;
    kin3d_mechanicsLabel = document.createElement('div');
    kin3d_mechanicsLabel.id = 'kin3d-mechanics-label';
    kin3d_mechanicsLabel.style.cssText = 'position:absolute;bottom:46px;left:10px;background:rgba(24,24,24,0.92);color:#EAB96A;font-family:monospace;font-size:11px;padding:0 10px;border-radius:8px;pointer-events:none;z-index:22;border:none;min-height:30px;max-width:440px;display:flex;align-items:center;';
    kin3d_mechanicsLabel.innerText = 'MECH: yukleniyor';
    container.appendChild(kin3d_mechanicsLabel);
    kin3d_updateMechanicsLabel();
}

function kin3d_updateMechanicsLabel() {
    if (!kin3d_mechanicsLabel) return;
    var snap = kin3d_getMechanicsSnapshot();
    if (snap.error) {
        kin3d_mechanicsLabel.style.color = '#FF7A7A';
        kin3d_mechanicsLabel.innerText = 'MECH: hata - ' + snap.error;
        return;
    }
    if (snap.loading && !snap.loaded) {
        kin3d_mechanicsLabel.style.color = '#EAB96A';
        kin3d_mechanicsLabel.innerText = 'MECH: yukleniyor';
        return;
    }
    if (!snap.loaded) {
        kin3d_mechanicsLabel.style.color = '#EAB96A';
        kin3d_mechanicsLabel.innerText = 'MECH: beklemede';
        return;
    }
    kin3d_mechanicsLabel.style.color = '#9BEB5D';
    var todoCount = snap.todo.length;
    kin3d_mechanicsLabel.innerText = 'MECH: ' + snap.materialCount + ' malzeme, ' +
        snap.assignedPartCount + ' parca, ' + snap.bearingMountCount +
        ' rulman noktasi, ' + snap.actuatorDeratingProfileCount + ' akt derate' +
        ', ' + snap.prrAxisCount + ' PRR aks, ' + snap.processModeCount + ' proses' +
        ', ' + snap.structuralPolicyCount + ' str policy' +
        (snap.totalMassKg > 0 ? ' | ' + snap.totalMassKg.toFixed(2) + ' kg' : '') +
        ' | PROC ' + (snap.activeProcessModeId || 'none') +
        ' | STR ' + (snap.worstStructuralStatus || 'UNKNOWN') +
        (todoCount ? ' | ' + todoCount + ' uyari' : '');
}

function kin3d_versionedCadUrl(url) {
    if (!url) return url;
    var ver = encodeURIComponent(String(kin3d_cadVersion || 'unknown'));
    var out = url;
    if (out.indexOf('cv=') < 0) {
        out += (out.indexOf('?') >= 0 ? '&' : '?') + 'cv=' + ver;
    }
    if (kin3d_settings && kin3d_settings.cadCacheVersionOnly === false) {
        out += '&rt=' + encodeURIComponent(kin3d_cadRuntimeTag);
    }
    return out;
}

function kin3d_bootCadVersionAndLoad() {
    try {
        var cached = localStorage.getItem('mros_cad_version');
        if (cached && String(cached).length > 0) kin3d_cadVersion = String(cached);
    } catch (e) {}
    kin3d_updateCadVersionLabel();

    var started = false;
    var startLoad = function() {
        if (started) return;
        started = true;
        kin3d_loadCadModels();
    };

    var guard = setTimeout(startLoad, 1200);
    fetch('/api/cad/version', { cache: 'no-store' })
        .then(function(resp) {
            if (!resp || !resp.ok) throw new Error('cad version fetch failed');
            return resp.json();
        })
        .then(function(data) {
            if (!data || !data.version) return;
            kin3d_cadVersion = String(data.version);
            try { localStorage.setItem('mros_cad_version', kin3d_cadVersion); } catch (e) {}
            kin3d_updateCadVersionLabel();
        })
        .catch(function() {})
        .finally(function() {
            clearTimeout(guard);
            startLoad();
        });
}

function setIkComputationMode(mode, fallbackEnabled) {
    if (mode !== undefined && mode !== null) {
        kin3d_ikBackendMode = String(mode);
    }
    if (typeof fallbackEnabled === 'boolean') {
        kin3d_ikFallbackEnabled = fallbackEnabled;
    }
    kin3d_refreshIkBackendLabel();
}

function kin3d_ensureCollisionMarkers(count) {
    while (kin3d_collisionMarkers.length < count) {
        var geo = new THREE.SphereGeometry(6, 12, 12);
        var mat = new THREE.MeshBasicMaterial({ color: 0xFF4D4D });
        var m = new THREE.Mesh(geo, mat);
        m.visible = false;
        kin3d_scene.add(m);
        kin3d_collisionMarkers.push(m);
    }
    while (kin3d_collisionMarkers.length > count) {
        var dead = kin3d_collisionMarkers.pop();
        if (dead) {
            kin3d_scene.remove(dead);
            if (dead.geometry) dead.geometry.dispose();
            if (dead.material) dead.material.dispose();
        }
    }
}

function kin3d_hideCollisionMarkers() {
    for (var i = 0; i < kin3d_collisionMarkers.length; i++) {
        kin3d_collisionMarkers[i].visible = false;
    }
}

function kin3d_refreshCollisionLabel() {
    if (!kin3d_collisionLabel) return;
    if (!kin3d_settings.showCollisionAlerts) {
        kin3d_collisionLabel.style.display = 'none';
        kin3d_layoutStatusOverlays();
        return;
    }
    kin3d_collisionLabel.style.display = 'flex';

    if (!kin3d_settings.useCAD) {
        kin3d_collisionLabel.style.color = '#A6ADB4';
        kin3d_collisionLabel.style.borderColor = '#5A5A5A';
        kin3d_collisionLabel.innerText = 'CAD CAKISMA: CAD kapali';
        kin3d_layoutStatusOverlays();
        return;
    }

    if (!kin3d_collisionPairs || kin3d_collisionPairs.length === 0) {
        kin3d_collisionLabel.style.color = '#9BEB5D';
        kin3d_collisionLabel.style.borderColor = '#4B5E3C';
        kin3d_collisionLabel.innerText = 'CAD CAKISMA: Yok';
        kin3d_layoutStatusOverlays();
        return;
    }

    var preview = kin3d_collisionPairs.slice(0, 3).map(function(p) {
        return p.a + ' x ' + p.b;
    }).join(' | ');
    var suffix = kin3d_collisionPairs.length > 3 ? ' ...' : '';
    kin3d_collisionLabel.style.color = '#FF8A8A';
    kin3d_collisionLabel.style.borderColor = '#7A3D3D';
    kin3d_collisionLabel.innerText = 'CAD CAKISMA: ' + kin3d_collisionPairs.length + '  [' + preview + suffix + ']';
    kin3d_layoutStatusOverlays();
}

function kin3d_updateCollisions(force) {
    if (!kin3d_scene) return;
    var now = (typeof performance !== 'undefined' && performance.now) ? performance.now() : Date.now();
    if (!force && (now - kin3d_collisionLastCheckMs) < kin3d_collisionCheckIntervalMs) return;
    kin3d_collisionLastCheckMs = now;

    if (!kin3d_settings.showCollisionAlerts || !kin3d_settings.useCAD) {
        kin3d_collisionPairs = [];
        kin3d_hideCollisionMarkers();
        kin3d_refreshCollisionLabel();
        return;
    }

    var candidates = [];
    for (var i = 0; i < kin3d_cadParts.length; i++) {
        var part = kin3d_cadParts[i];
        if (!part || String(part.id || '').indexOf('arm') !== 0) continue;
        var root = kin3d_cadRoots[part.id];
        if (!root || !root.visible) continue;
        root.updateMatrixWorld(true);
        var box = new THREE.Box3().setFromObject(root);
        if (!isFinite(box.min.x) || !isFinite(box.max.x)) continue;
        candidates.push({ id: part.id, box: box });
    }

    var pairs = [];
    for (var a = 0; a < candidates.length; a++) {
        for (var b = a + 1; b < candidates.length; b++) {
            var ca = candidates[a];
            var cb = candidates[b];
            if (!kin3d_shouldCheckCollisionPair(ca.id, cb.id)) continue;
            if (!ca.box.intersectsBox(cb.box)) continue;

            var overlap = ca.box.clone().intersect(cb.box);
            var sz = new THREE.Vector3();
            overlap.getSize(sz);
            var vol = sz.x * sz.y * sz.z;
            if (sz.x < kin3d_collisionMinAxisMm || sz.y < kin3d_collisionMinAxisMm || sz.z < kin3d_collisionMinAxisMm) continue;
            if (vol < kin3d_collisionMinVolumeMm3) continue;

            var center = new THREE.Vector3();
            overlap.getCenter(center);
            pairs.push({ a: ca.id, b: cb.id, center: center, volume: vol });
        }
    }

    kin3d_collisionPairs = pairs;
    kin3d_ensureCollisionMarkers(pairs.length);
    for (var k = 0; k < kin3d_collisionMarkers.length; k++) {
        var mk = kin3d_collisionMarkers[k];
        if (k < pairs.length) {
            mk.position.copy(pairs[k].center);
            mk.visible = true;
        } else {
            mk.visible = false;
        }
    }
    kin3d_refreshCollisionLabel();
}

function kin3d_loadCadModels() {
    if (typeof THREE.GLTFLoader !== 'function') {
        console.warn('GLTFLoader bulunamadi; CAD yuklenemedi.');
        return;
    }
    if (!kin3d_cadLoadingManager && typeof THREE.LoadingManager === 'function') {
        kin3d_cadLoadingManager = new THREE.LoadingManager();
        kin3d_cadLoadingManager.setURLModifier(function(url) {
            if (!url) return url;
            if (url.indexOf('data:') === 0 || url.indexOf('blob:') === 0) return url;
            if (url.indexOf('cv=') >= 0) return url;
            var isCadPath = (url.indexOf('/cad/') >= 0);
            var isCadResource = isCadPath || url === 'data.bin' || url.endsWith('.bin') || url.endsWith('.gltf');
            return isCadResource ? kin3d_versionedCadUrl(url) : url;
        });
    }
    kin3d_gltfLoader = kin3d_gltfLoader || new THREE.GLTFLoader(kin3d_cadLoadingManager);
    if (typeof THREE.DRACOLoader === 'function') {
        kin3d_dracoLoader = kin3d_dracoLoader || new THREE.DRACOLoader();
        kin3d_dracoLoader.setDecoderPath('https://www.gstatic.com/draco/versioned/decoders/1.5.6/');
        kin3d_gltfLoader.setDRACOLoader(kin3d_dracoLoader);
    } else {
        console.warn('DRACOLoader bulunamadi. Draco sikistirmali CAD dosyalari acilamayabilir.');
    }

    for (var i = 0; i < kin3d_cadParts.length; i++) {
        (function(part){
            kin3d_gltfLoader.load(kin3d_versionedCadUrl(part.url), function(gltf) {
                var model = (gltf && gltf.scene) ? gltf.scene : null;
                if (!kin3d_attachCadModel(model, part)) {
                    console.warn('CAD attach edilemedi:', part.id);
                }
            }, undefined, function(err) {
                console.warn('CAD yukleme hatasi [' + part.id + ']:', err);
            });
        })(kin3d_cadParts[i]);
    }
}

function kin3d_updateAuxCamProjection(aspect) {
    var a = (isFinite(aspect) && aspect > 0.01) ? aspect : (16 / 9);
    var halfH = kin3d_auxOrthoHalfHeight;
    var halfW = halfH * a;
    var cams = [kin3d_camTop, kin3d_camSide, kin3d_camFront];
    for (var i = 0; i < cams.length; i++) {
        var cam = cams[i];
        if (!cam) continue;
        // Force all auxiliary cameras to stay orthographic.
        if (!cam.isOrthographicCamera) {
            var ortho = new THREE.OrthographicCamera(-halfW, halfW, halfH, -halfH, 1, 5000);
            ortho.position.copy(cam.position);
            ortho.up.copy(cam.up);
            ortho.quaternion.copy(cam.quaternion);
            cam = ortho;
            if (i === 0) kin3d_camTop = cam;
            if (i === 1) kin3d_camSide = cam;
            if (i === 2) kin3d_camFront = cam;
        }
        cam.left = -halfW;
        cam.right = halfW;
        cam.top = halfH;
        cam.bottom = -halfH;
        cam.updateProjectionMatrix();
    }
}

function kin3d_init(containerId = 'canvas-container') {
    var container = document.getElementById(containerId);
    if (!container) return;

    kin3d_loadSettings();
    kin3d_loadMechanicalData();

    kin3d_scene = new THREE.Scene();
    kin3d_scene.background = new THREE.Color(kin3d_settings.bgColor);

    kin3d_camera = new THREE.PerspectiveCamera(
        kin3d_num(kin3d_settings.cameraFov, 45, 20, 100),
        container.clientWidth / container.clientHeight,
        kin3d_num(kin3d_settings.cameraNear, 1, 0.01, 500),
        kin3d_num(kin3d_settings.cameraFar, 5000, 100, 60000)
    );
    kin3d_camera.position.set(800, 600, 800);
    kin3d_camera.lookAt(0, 300, 0);

    kin3d_renderer = new THREE.WebGLRenderer({ antialias: (kin3d_settings.perfAntialias !== false), alpha: false });
    kin3d_renderer.setSize(container.clientWidth, container.clientHeight);
    kin3d_renderer.autoClear = false;
    // Better CAD readability on dark materials
    if (THREE.sRGBEncoding !== undefined) kin3d_renderer.outputEncoding = THREE.sRGBEncoding;
    if (THREE.ACESFilmicToneMapping !== undefined) kin3d_renderer.toneMapping = THREE.ACESFilmicToneMapping;
    kin3d_renderer.toneMappingExposure = kin3d_settings.lightExposure;
    container.appendChild(kin3d_renderer.domElement);

    kin3d_controls = new THREE.OrbitControls(kin3d_camera, kin3d_renderer.domElement);
    kin3d_controls.target.set(0, 300, 0);
    kin3d_controls.enableDamping = true;
    kin3d_controls.dampingFactor = kin3d_num(kin3d_settings.cameraDamping, 0.08, 0.01, 0.35);
    kin3d_controls.autoRotate = !!kin3d_settings.cameraAutoRotate;
    kin3d_controls.autoRotateSpeed = kin3d_num(kin3d_settings.cameraAutoRotateSpeed, 1.0, 0.1, 12.0);
    kin3d_controls.enablePan = (kin3d_settings.cameraEnablePan !== false);
    kin3d_controls.minDistance = kin3d_num(kin3d_settings.cameraMinDistance, 100, 20, 10000);
    kin3d_controls.maxDistance = kin3d_num(kin3d_settings.cameraMaxDistance, 2600, kin3d_controls.minDistance + 1, 20000);
    kin3d_installCameraNavHotkeys();
    kin3d_applyCameraMouseBindings();

    // ---- Lighting (Balanced Visibility) ----
    kin3d_ambientLight = new THREE.AmbientLight(0xffffff, kin3d_settings.lightAmbient);
    kin3d_scene.add(kin3d_ambientLight);

    kin3d_hemiLight = new THREE.HemisphereLight(0xffffff, 0x2c2c2c, kin3d_settings.lightHemi);
    kin3d_scene.add(kin3d_hemiLight);

    kin3d_dirLight = new THREE.DirectionalLight(0xffffff, kin3d_settings.lightKey);
    kin3d_dirLight.position.set(700, 1100, 500);
    kin3d_scene.add(kin3d_dirLight);

    kin3d_fillLight = new THREE.DirectionalLight(0xffffff, kin3d_settings.lightFill);
    kin3d_fillLight.position.set(-900, 500, 650);
    kin3d_scene.add(kin3d_fillLight);

    kin3d_backLight = new THREE.DirectionalLight(0xffffff, kin3d_settings.lightBack);
    kin3d_backLight.position.set(300, 600, -900);
    kin3d_scene.add(kin3d_backLight);

    // World Axes
    kin3d_worldAxes = new THREE.AxesHelper(kin3d_num(kin3d_settings.worldAxisSize, 200, 40, 2000));
    kin3d_scene.add(kin3d_worldAxes);

    // Grid on XZ plane
    kin3d_grid = new THREE.GridHelper(
        kin3d_num(kin3d_settings.gridSize, 1200, 200, 6000),
        Math.round(kin3d_num(kin3d_settings.gridDivisions, kin3d_settings.gridSize / 50, 2, 200)),
        kin3d_parseColorHex(kin3d_settings.gridCenterColor, 0x6A97EA),
        kin3d_parseColorHex(kin3d_settings.gridColor, 0x4D4646)
    );
    kin3d_scene.add(kin3d_grid);

    // ---- Robot Links (thick line through all joint positions) ----
    var linkGeo = new THREE.BufferGeometry();
    var positions = new Float32Array(9 * 3); // 9 points
    linkGeo.setAttribute('position', new THREE.BufferAttribute(positions, 3));
    var linkMat = new THREE.LineBasicMaterial({ color: 0xF0F2F3, linewidth: 3 });
    kin3d_linkLine = new THREE.Line(linkGeo, linkMat);
    kin3d_scene.add(kin3d_linkLine);

    // ---- Dashed line from base to end-effector ----
    var dashGeo = new THREE.BufferGeometry();
    var dashPos = new Float32Array(2 * 3);
    dashGeo.setAttribute('position', new THREE.BufferAttribute(dashPos, 3));
    kin3d_dashLine = new THREE.Line(dashGeo, new THREE.LineDashedMaterial({
        color: 0xEA6A6A, dashSize: 15, gapSize: 10, linewidth: 1
    }));
    kin3d_dashLine.computeLineDistances();
    kin3d_scene.add(kin3d_dashLine);

    // ---- Joint Spheres ----
    var jointColor = 0x6A97EA;
    for (var i = 0; i < 9; i++) {
        var radius = (i === 0) ? 20 : (i === 8 ? 10 : 14);
        var color = (i === 0) ? 0x888888 : (i === 8 ? 0xEA6A6A : jointColor);
        var geo = new THREE.SphereGeometry(radius, 24, 24);
        var mat = new THREE.MeshStandardMaterial({ color: color, roughness: 0.4, metalness: 0.3 });
        var sphere = new THREE.Mesh(geo, mat);
        kin3d_jointSpheres.push(sphere);
        kin3d_scene.add(sphere);
    }

    // ---- Link Cylinders (for visual thickness between joints) ----
    // We'll draw thick cylinders between consecutive points in the animate loop
    // using simple line for now (Three.js lines are thin on most GPUs)
    // Adding tube-like segments:
    kin3d_linkTubes = [];
    for (var i = 0; i < 8; i++) {
        var tubeGeo = new THREE.CylinderGeometry(kin3d_settings.linkRadius, kin3d_settings.linkRadius, 1, 12);
        var tubeMat = new THREE.MeshStandardMaterial({ color: 0xC0C0C0, roughness: 0.5, metalness: 0.2 });
        var tube = new THREE.Mesh(tubeGeo, tubeMat);
        kin3d_linkTubes.push(tube);
        kin3d_scene.add(tube);
    }

    // ---- End-effector coordinate frame ----
    kin3d_eeAxes = new THREE.AxesHelper(80);
    kin3d_scene.add(kin3d_eeAxes);

    // ---- Coordinate frames for each DH joint (Groups containing AxesHelper) ----
    for (var i = 0; i < 7; i++) {
        var group = new THREE.Group();
        var axes = new THREE.AxesHelper(40);
        axes.name = "axes";
        axes.visible = false;
        group.add(axes);
        kin3d_coordFrames.push(group);
        kin3d_scene.add(group);
    }
    kin3d_bootCadVersionAndLoad();

    // ---- Ghost Robot (0.25 opacity target pose) ----
    for (var i = 0; i < 9; i++) {
        var grad = (i === 0) ? 20 : (i === 8 ? 10 : 14);
        var gColor = (i === 8) ? 0x44FF88 : 0x88CCFF;
        var gGeo = new THREE.SphereGeometry(grad, 12, 12);
        var gMat = new THREE.MeshBasicMaterial({ color: gColor, transparent: true, opacity: 0.25 });
        var gSphere = new THREE.Mesh(gGeo, gMat);
        gSphere.visible = false;
        ghost_spheres.push(gSphere);
        kin3d_scene.add(gSphere);
    }
    for (var i = 0; i < 8; i++) {
        var gtGeo = new THREE.CylinderGeometry(5, 5, 1, 8);
        var gtMat = new THREE.MeshBasicMaterial({ color: 0x88CCFF, transparent: true, opacity: 0.2 });
        var gTube = new THREE.Mesh(gtGeo, gtMat);
        gTube.visible = false;
        ghost_tubes.push(gTube);
        kin3d_scene.add(gTube);
    }
    ghost_eeAxes = new THREE.AxesHelper(60);
    ghost_eeAxes.visible = false;
    kin3d_scene.add(ghost_eeAxes);

    // ---- Trajectory Path Line (planned path) ----
    var tpGeo = new THREE.BufferGeometry();
    var tpPos = new Float32Array(200 * 3);
    tpGeo.setAttribute('position', new THREE.BufferAttribute(tpPos, 3));
    traj_pathLine = new THREE.Line(tpGeo, kin3d_makeTrajectoryMaterial(kin3d_settings.trajColor, kin3d_settings.trajDashed !== false));
    traj_pathLine.visible = false;
    kin3d_scene.add(traj_pathLine);

    // Planned trajectory queue preview (dashed)
    var tqGeo = new THREE.BufferGeometry();
    var tqPos = new Float32Array(2 * 3);
    tqGeo.setAttribute('position', new THREE.BufferAttribute(tqPos, 3));
    traj_previewLine = new THREE.Line(tqGeo, kin3d_makeTrajectoryMaterial(kin3d_settings.trajColor, kin3d_settings.trajDashed !== false));
    traj_previewLine.visible = false;
    kin3d_scene.add(traj_previewLine);

    // ---- Trajectory Trace Line (actual traveled path) ----
    var ttGeo = new THREE.BufferGeometry();
    var ttPos = new Float32Array(traj_maxTrace * 3);
    ttGeo.setAttribute('position', new THREE.BufferAttribute(ttPos, 3));
    traj_traceLine = new THREE.Line(ttGeo, new THREE.LineBasicMaterial({
        color: 0x44DDDD, linewidth: 2
    }));
    traj_traceLine.geometry.setDrawRange(0, 0);
    kin3d_scene.add(traj_traceLine);

    // ---- EE Coordinate Labels (HTML overlay) ----
    // Actual (Solid Robot)
    kin3d_ee_label = document.createElement('div');
    kin3d_ee_label.id = 'ee-coord-label';
    kin3d_ee_label.style.cssText = 'position:absolute;bottom:8px;left:8px;background:rgb(30,60,40);color:#B5EA6A;font-family:monospace;font-size:12px;padding:5px 12px;border-radius:6px;pointer-events:none;z-index:20;border:none;';
    kin3d_ee_label.innerHTML = '<span style="font-weight:800;margin-right:8px;color:#fff;background:rgba(255,255,255,0.15);padding:1px 4px;border-radius:2px;">ANLIK</span> X: 0.0 &nbsp; Y: 0.0 &nbsp; Z: 0.0 &nbsp; &alpha;: 0.0&deg;';
    
    // Planned (Ghost Robot)
    kin3d_planned_label = document.createElement('div');
    kin3d_planned_label.id = 'planned-coord-label';
    kin3d_planned_label.style.cssText = 'position:absolute;bottom:42px;left:8px;background:rgb(50,50,50);color:#6A97EA;font-family:monospace;font-size:12px;padding:5px 12px;border-radius:6px;pointer-events:none;z-index:20;border:none;';
    kin3d_planned_label.innerHTML = '<span style="font-weight:800;margin-right:8px;color:#fff;background:rgba(255,255,255,0.15);padding:1px 4px;border-radius:2px;">PLANLANAN</span> X: 0.0 &nbsp; Y: 0.0 &nbsp; Z: 0.0 &nbsp; &alpha;: 0.0&deg;';

    container.style.position = 'relative';
    container.appendChild(kin3d_ee_label);
    container.appendChild(kin3d_planned_label);
    kin3d_createCollisionOverlay(container);
    kin3d_createIkBackendOverlay(container);
    kin3d_createCadVersionOverlay(container);

    // ---- Joint Angle Labels (HTML overlay) and Joint Name Labels (HTML overlay) ----
    for (var i = 0; i < 9; i++) {
        var div = document.createElement('div');
        div.className = 'joint-angle-label';
        div.style.position = 'absolute';
        div.style.color = '#FFD700'; // Gold
        div.style.backgroundColor = 'rgba(25, 25, 25, 0.8)';
        div.style.border = '1px solid rgba(255, 215, 0, 0.4)';
        div.style.padding = '3px 6px';
        div.style.fontSize = '11px';
        div.style.fontWeight = '800';
        div.style.borderRadius = '6px';
        div.style.pointerEvents = 'none';
        div.style.display = 'none';
        div.style.whiteSpace = 'nowrap';
        div.style.boxShadow = '0 2px 8px rgba(0,0,0,0.5)';
        div.innerText = 'J' + i + ': 0.0';
        container.appendChild(div);
        kin3d_jointAngleLabels.push(div);
        
        var nameDiv = document.createElement('div');
        nameDiv.className = 'joint-name-label';
        nameDiv.innerText = kin3d_jointNames[i];
        nameDiv.style.position = 'absolute';
        nameDiv.style.color = '#9BEB5D'; // Primary Green
        nameDiv.style.backgroundColor = 'rgba(25, 25, 25, 0.8)';
        nameDiv.style.border = '1px solid rgba(155, 235, 93, 0.5)';
        nameDiv.style.padding = '2px 6px';
        nameDiv.style.fontSize = '12px';
        nameDiv.style.fontWeight = '800';
        nameDiv.style.borderRadius = '6px';
        nameDiv.style.pointerEvents = 'none';
        nameDiv.style.display = 'none';
        nameDiv.style.whiteSpace = 'nowrap';
        nameDiv.style.boxShadow = '0 2px 8px rgba(0,0,0,0.5)';
        container.appendChild(nameDiv);
        kin3d_jointNameLabels.push(nameDiv);
    }

    // ---- Settings UI now lives under Ayarlar > 3D Gorumum ----
    kin3d_mountSettingsTabContent('settings-tab-3d-body');
    kin3d_createCadCalibrationPopup(container);
    kin3d_createCadContextMenu(container);
    kin3d_raycaster = new THREE.Raycaster();

    if (kin3d_renderer && kin3d_renderer.domElement) {
        kin3d_renderer.domElement.addEventListener('contextmenu', function(evt) {
            evt.preventDefault();
            evt.stopPropagation();
            kin3d_openCadContextMenuFromEvent(evt);
        });
    }

    window.addEventListener('resize', kin3d_onResize, false);

    // ---- Orientation Gizmo (SolidWorks-style corner axes) ----
    kin3d_initGizmo();

    // ---- Auxiliary Multi-View Cameras (16:9 Wide, Orthographic) ----
    var auxAspect = 16 / 9;
    var auxHalfH = kin3d_auxOrthoHalfHeight;
    var auxHalfW = auxHalfH * auxAspect;

    // Top View (looking down from high Y-up)
    kin3d_camTop = new THREE.OrthographicCamera(-auxHalfW, auxHalfW, auxHalfH, -auxHalfH, 1, 5000);
    kin3d_camTop.position.set(0, 1400, 0);
    kin3d_camTop.up.set(0, 0, -1);

    // Side View (looking from +X)
    kin3d_camSide = new THREE.OrthographicCamera(-auxHalfW, auxHalfW, auxHalfH, -auxHalfH, 1, 5000);
    kin3d_camSide.position.set(1300, 400, 0);

    // Front View (looking from +Z)
    kin3d_camFront = new THREE.OrthographicCamera(-auxHalfW, auxHalfW, auxHalfH, -auxHalfH, 1, 5000);
    kin3d_camFront.position.set(0, 400, 1300);
    kin3d_updateAuxCamProjection(auxAspect);

    // Initial pose: all zeros
    kin3d_updatePose([0, 0, 0, 0, 0, 0, 0]);
    kin3d_initialized = true;
    kin3d_applySettings(); // Apply loaded settings
    kin3d_animate();
}

var kin3d_linkTubes = [];

function kin3d_positionTube(tube, p1, p2) {
    var dir = new THREE.Vector3().subVectors(p2, p1);
    var len = dir.length();
    if (len < 0.01) { tube.visible = false; return; }
    tube.visible = true;
    tube.scale.set(1, len, 1);
    var mid = new THREE.Vector3().addVectors(p1, p2).multiplyScalar(0.5);
    tube.position.copy(mid);
    var up = new THREE.Vector3(0, 1, 0);
    var quat = new THREE.Quaternion();
    dir.normalize();
    quat.setFromUnitVectors(up, dir);
    tube.quaternion.copy(quat);
}

// Helper to project 3D point to 2D screen coordinates for DOM labels
function kin3d_projectToDOM(element, pos, offsetUp = 0) {
    if (!element || !pos) return;

    // Correct mapping for Three.js (X=X, Y=Z, Z=Y) + offset on vertical axis (Three.js Y)
    var vector = new THREE.Vector3(pos.x, pos.z + offsetUp, pos.y);
    vector.project(kin3d_camera);

    var container = document.getElementById('canvas-container');
    var width = container.clientWidth;
    var height = container.clientHeight;

    var x = (vector.x * 0.5 + 0.5) * width;
    var y = (-vector.y * 0.5 + 0.5) * height;

    element.style.left = `${x}px`;
    element.style.top = `${y}px`;
    element.style.transform = `translate(-50%, -50%)`; // Center the label
}

function kin3d_updatePose(angles_deg) {
    var T_all = compute_FK(angles_deg);
    var P = extract_positions(T_all);
    kin3d_jointPositions = P; // Store for animate update

    // Update link line
    var pos = kin3d_linkLine.geometry.attributes.position.array;
    for (var i = 0; i < 9; i++) {
        pos[i * 3 + 0] = P[i].x;
        pos[i * 3 + 1] = P[i].z; // swap Y/Z for Three.js (Y-up)
        pos[i * 3 + 2] = P[i].y;
    }
    kin3d_linkLine.geometry.attributes.position.needsUpdate = true;

    // Update dashed line (base to EE)
    var dpos = kin3d_dashLine.geometry.attributes.position.array;
    dpos[0] = 0; dpos[1] = 0; dpos[2] = 0;
    dpos[3] = P[8].x; dpos[4] = P[8].z; dpos[5] = P[8].y;
    kin3d_dashLine.geometry.attributes.position.needsUpdate = true;
    kin3d_dashLine.computeLineDistances();

    // Update DOM Label text (positions are updated in animate loop)
    for (var i = 0; i < 9; i++) {
        if (kin3d_jointAngleLabels[i]) {
            kin3d_jointAngleLabels[i].innerText = (i > 0 && i < 8) ? `J${i}: ${angles_deg[i-1].toFixed(1)}°` : '';
            kin3d_jointAngleLabels[i].style.display = (kin3d_settings.showAngleLabels && (i > 0 && i < 8)) ? 'block' : 'none';
        }
        if (kin3d_jointNameLabels[i]) {
            kin3d_jointNameLabels[i].style.display = kin3d_settings.showJointNames ? 'block' : 'none';
        }
    }

    // Update joint spheres
    for (var i = 0; i < 9; i++) {
        kin3d_jointSpheres[i].position.set(P[i].x, P[i].z, P[i].y);
    }

    // Update tube links between consecutive points
    for (var i = 0; i < 8; i++) {
        var p1 = new THREE.Vector3(P[i].x, P[i].z, P[i].y);
        var p2 = new THREE.Vector3(P[i+1].x, P[i+1].z, P[i+1].y);
        kin3d_positionTube(kin3d_linkTubes[i], p1, p2);
    }

    // Update end-effector axes
    var T7 = T_all[6];
    var eePos = mat4_get_pos(T_all[6]);
    kin3d_eeAxes.position.set(eePos.x, eePos.z, eePos.y);
    // Build rotation matrix for EE (swap Y/Z for Three.js)
    var m = new THREE.Matrix4();
    m.set(
        T7[0], T7[2], T7[1], eePos.x,
        T7[8], T7[10], T7[9], eePos.z,
        T7[4], T7[6], T7[5], eePos.y,
        0, 0, 0, 1
    );
    kin3d_eeAxes.matrix.copy(m);
    kin3d_eeAxes.matrixAutoUpdate = false;
    kin3d_eeAxes.matrixWorldNeedsUpdate = true;

    // Update DH coordinate frames and joint angle labels
    for (var i = 0; i < 7; i++) {
        var Ti = T_all[i];
        var fp = mat4_get_pos(Ti);
        kin3d_coordFrames[i].position.set(fp.x, fp.z, fp.y);
        
        // Update AxesHelper visibility within the group
        var axes = kin3d_coordFrames[i].getObjectByName("axes");
        if (axes) axes.visible = kin3d_settings.showLocalFrames;

        // Update local frame orientation for CAD attachment
        var m = new THREE.Matrix4();
        m.set(
            Ti[0], Ti[2], Ti[1], fp.x,
            Ti[8], Ti[10], Ti[9], fp.z,
            Ti[4], Ti[6], Ti[5], fp.y,
            0, 0, 0, 1
        );
        kin3d_coordFrames[i].matrix.copy(m);
        kin3d_coordFrames[i].matrixAutoUpdate = false;
        kin3d_coordFrames[i].matrixWorldNeedsUpdate = true;
    }

    // CAD modellerin kural bazli yerlesimi
    kin3d_updateCadPlacements(angles_deg, P, T_all);
    kin3d_updateMechanicalForceSnapshot(angles_deg, T_all);

    // Toggle visibility based on settings
    kin3d_setCadVisibility();
    kin3d_updateCollisions(false);
    var showKin = kin3d_settings.showKinematics;

    kin3d_linkLine.visible = showKin;
    kin3d_dashLine.visible = showKin && kin3d_settings.showDashLine;
    kin3d_eeAxes.visible = showKin && kin3d_settings.showLocalFrames;

    for (var i = 0; i < 8; i++) {
        // Stick        // Kinematics visibility (link tubes)
        kin3d_linkTubes[i].visible = showKin;
    }
    
    // Joint spheres match kinematics visibility
    for (var i = 0; i < 9; i++){
        kin3d_jointSpheres[i].visible = showKin && kin3d_settings.showJoints;
    }

    // Update auxiliary camera targets (Framing)
    var center = new THREE.Vector3(0, 0, 0);
    for (var i = 0; i < P.length; i++) {
        center.x += P[i].x; center.y += P[i].z; center.z += P[i].y;
    }
    center.divideScalar(P.length);
    
    // Offset center slightly upwards for better visual balance
    center.y += 50; 

    if (kin3d_camTop) {
        kin3d_camTop.position.set(center.x, 1400, center.z);
        kin3d_camTop.lookAt(center.x, center.y, center.z);
    }
    if (kin3d_camSide) kin3d_camSide.lookAt(center.x, center.y, center.z);
    if (kin3d_camFront) kin3d_camFront.lookAt(center.x, center.y, center.z);

    kin3d_refreshBboxHelpers();
    kin3d_lastAngles = angles_deg; // Store for trajectory path
}

// Public function called from WebSocket handler
function updateRobotAngles(angles) {
    if (!kin3d_initialized || !angles || angles.length < 7) return;
    kin3d_updatePose(angles);

    // Add to trajectory trace
    var T = compute_FK(angles);
    var ee = mat4_get_pos(T[6]);
    if (traj_tracePoints.length >= traj_maxTrace * 3) traj_tracePoints.splice(0, 3);
    traj_tracePoints.push(ee.x, ee.z, ee.y);

    if (traj_traceLine && traj_traceLine.geometry && traj_traceLine.geometry.attributes.position) {
        var tArr = traj_traceLine.geometry.attributes.position.array;
        for (var i = 0; i < traj_tracePoints.length && i < tArr.length; i++) tArr[i] = traj_tracePoints[i];
        traj_traceLine.geometry.attributes.position.needsUpdate = true;
        traj_traceLine.geometry.setDrawRange(0, Math.min(traj_maxTrace, traj_tracePoints.length / 3));
    }
}

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
    var ctx = ud.labelCtx;
    var canvas = ud.labelCanvas;
    ctx.clearRect(0, 0, canvas.width, canvas.height);
    ctx.font = "Bold 40px sans-serif";
    ctx.fillStyle = "white";
    ctx.textAlign = "center";
    ctx.shadowColor = "rgba(0,0,0,1.0)";
    ctx.shadowBlur = 6;
    ctx.fillText(nextText, canvas.width * 0.5, 45);
    ud.labelTex.needsUpdate = true;
}

function kin3d_setReusableLinePoints(line, points, dashed) {
    if (!line) return false;
    var count = Math.floor((points && points.length ? points.length : 0) / 3);
    if (count < 2) {
        line.visible = false;
        return false;
    }

    var geometry = line.geometry;
    var attr = geometry && geometry.attributes ? geometry.attributes.position : null;
    if (!geometry || !attr || !attr.array || attr.array.length < count * 3) {
        if (geometry && geometry.dispose) geometry.dispose();
        geometry = new THREE.BufferGeometry();
        geometry.setAttribute('position', new THREE.BufferAttribute(new Float32Array(count * 3), 3));
        line.geometry = geometry;
        attr = geometry.attributes.position;
    }

    attr.array.set(points, 0);
    attr.needsUpdate = true;
    geometry.setDrawRange(0, count);
    if (dashed && typeof line.computeLineDistances === 'function') line.computeLineDistances();
    line.visible = (kin3d_settings.showTrajectory !== false);
    return true;
}

// Draw sequence of planned coordinates as floating spheres
function updateTrajectory3D(trajArr) {
    if (!kin3d_initialized) return;
    var showTraj = (kin3d_settings.showTrajectory !== false);
    var src = Array.isArray(trajArr) ? trajArr : [];

    if (traj_previewLine) {
        if (src.length > 1) {
            var pathPts = [];
            for (var pi = 0; pi < src.length; pi++) {
                pathPts.push(src[pi].x, src[pi].z, src[pi].y);
            }
            if (!kin3d_setReusableLinePoints(traj_previewLine, pathPts, true)) {
                traj_previewLine.visible = false;
            }
        } else {
            traj_previewLine.visible = false;
        }
    }

    var density = kin3d_num(kin3d_settings.trajPointDensity, 1.0, 0.1, 3.0);
    var markerCap = Math.min(src.length, Math.max(2, Math.round(120 * density)));

    // Instantiate marker groups if necessary
    while (kin3d_trajSpheres.length < markerCap) {
        var group = new THREE.Group();

        var geo = new THREE.SphereGeometry(5, 16, 16);
        var mat = new THREE.MeshStandardMaterial({ color: 0xFFFFFF, roughness: 0.9, metalness: 0.0 }); // Matte white
        var mesh = new THREE.Mesh(geo, mat);
        group.add(mesh);

        // Label Sprite (H1, H2, H3...)
        var canvas = document.createElement('canvas');
        canvas.width = 128; canvas.height = 64;
        var ctx = canvas.getContext('2d');
        ctx.clearRect(0, 0, canvas.width, canvas.height);
        var tex = new THREE.CanvasTexture(canvas);
        var smat = new THREE.SpriteMaterial({ map: tex, transparent: true, depthTest: false });
        var sprite = new THREE.Sprite(smat);
        sprite.scale.set(40, 20, 1);
        sprite.position.y = 15; // Hover above sphere
        group.add(sprite);

        group.userData = {
            mesh: mesh,
            sprite: sprite,
            labelCanvas: canvas,
            labelCtx: ctx,
            labelTex: tex,
            labelText: ''
        };

        kin3d_scene.add(group);
        kin3d_trajSpheres.push(group);
    }

    var sampledIdx = [];
    for (var si = 0; si < markerCap; si++) {
        var sIdx = (markerCap <= 1) ? 0 : Math.round(si * (src.length - 1) / (markerCap - 1));
        sampledIdx.push(sIdx);
    }

    // Position and toggle visibility (sampled markers to avoid clutter)
    var anchorLabelIdx = 0;
    for (var i = 0; i < kin3d_trajSpheres.length; i++) {
        if (i < markerCap && showTraj) {
            var srcIdx = sampledIdx[i];
            var p = src[srcIdx];
            if (!p) {
                kin3d_trajSpheres[i].visible = false;
                continue;
            }
            var isInterp = kin3d_isInterpTrajectoryPoint(p);
            kin3d_trajSpheres[i].position.set(p.x, p.z, p.y); // Swapped Z/Y for three.js
            var ud = kin3d_trajSpheres[i].userData || {};
            if (ud.mesh) {
                var sc = isInterp ? 0.1 : 1.0; // interpolated markers are 0.1x of normal size
                ud.mesh.scale.set(sc, sc, sc);
            }
            if (!isInterp && kin3d_settings.showTrajectoryLabels !== false) {
                anchorLabelIdx++;
                kin3d_setTrajMarkerLabel(kin3d_trajSpheres[i], 'H' + anchorLabelIdx, true);
            } else {
                kin3d_setTrajMarkerLabel(kin3d_trajSpheres[i], '', false);
            }

            if (!isInterp && (i === 0 || i === markerCap - 1)) {
                var sty = String(kin3d_settings.startEndMarkerStyle || 'dot');
                if (ud.mesh) {
                    if (sty === 'ring') ud.mesh.scale.set(1.2, 0.35, 1.2);
                    else if (sty === 'cube') ud.mesh.scale.set(0.95, 0.95, 0.95);
                    else ud.mesh.scale.set(1.15, 1.15, 1.15);
                }
            }
            kin3d_trajSpheres[i].visible = true;
        } else {
            kin3d_setTrajMarkerLabel(kin3d_trajSpheres[i], '', false);
            kin3d_trajSpheres[i].visible = false;
        }
    }
}

// Update EE coordinate label from SPI data
function updateEELabel(cx, cy, cz, alpha) {
    kin3d_t41_coords.x = cx;
    kin3d_t41_coords.y = cy;
    kin3d_t41_coords.z = cz;
    kin3d_t41_coords.alpha = alpha;
    if (kin3d_ee_label) {
        kin3d_ee_label.innerHTML = '<span style="font-weight:800;margin-right:8px;color:#fff;background:rgba(255,255,255,0.15);padding:1px 4px;border-radius:2px;">ANLIK</span> X: ' + cx.toFixed(1) + ' &nbsp; Y: ' + cy.toFixed(1) + ' &nbsp; Z: ' + cz.toFixed(1) + ' &nbsp; &alpha;: ' + alpha.toFixed(1) + '&deg;';
    }
}

// Show ghost robot at target angles (0.25 opacity)
function showGhostPose(target_angles_deg, opts) {
    if (!kin3d_initialized) return;
    if (kin3d_settings.showGhost === false) {
        hideGhost();
        return;
    }
    var skipPath = !!(opts && opts.skipPath);
    var T_all = compute_FK(target_angles_deg);
    var P = extract_positions(T_all);
    var useCadGhost = (kin3d_settings.useCAD === true);

    // Update Planned Coordinate Label
    if(kin3d_planned_label) {
        var eePos = mat4_get_pos(T_all[6]);
        var r20 = T_all[6][8];
        var r00 = T_all[6][0];
        var r10 = T_all[6][4];
        var alpha = Math.atan2(-r20, Math.sqrt(r00*r00 + r10*r10)) * 180.0 / Math.PI;
        kin3d_planned_label.innerHTML = '<span style="font-weight:800;margin-right:8px;color:#fff;background:rgba(255,255,255,0.15);padding:1px 4px;border-radius:2px;">PLANLANAN</span> X: ' + eePos.x.toFixed(1) + ' &nbsp; Y: ' + eePos.y.toFixed(1) + ' &nbsp; Z: ' + eePos.z.toFixed(1) + ' &nbsp; &alpha;: ' + alpha.toFixed(1) + '&deg;';
    }

    if (useCadGhost) {
        for (var i = 0; i < ghost_spheres.length; i++) ghost_spheres[i].visible = false;
        for (var i = 0; i < ghost_tubes.length; i++) ghost_tubes[i].visible = false;
        if (ghost_eeAxes) ghost_eeAxes.visible = false;

        kin3d_updateCadPlacementsForRoots(kin3d_cadGhostRoots, target_angles_deg, P, T_all);
        kin3d_setCadGhostVisibility(true);
    } else {
        kin3d_setCadGhostVisibility(false);
        for (var i = 0; i < ghost_spheres.length; i++) {
            ghost_spheres[i].position.set(P[i].x, P[i].z, P[i].y);
            ghost_spheres[i].visible = true;
        }
        for (var i = 0; i < ghost_tubes.length; i++) {
            var p1 = new THREE.Vector3(P[i].x, P[i].z, P[i].y);
            var p2 = new THREE.Vector3(P[i+1].x, P[i+1].z, P[i+1].y);
            kin3d_positionTube(ghost_tubes[i], p1, p2);
            ghost_tubes[i].visible = true;
        }

        // Ghost EE axes
        var eeP = mat4_get_pos(T_all[6]);
        ghost_eeAxes.position.set(eeP.x, eeP.z, eeP.y);
        ghost_eeAxes.visible = true;
    }

    ghost_visible = true;

    // Build trajectory path from current to target for one-shot previews
    if (!skipPath) showTrajectoryPath(target_angles_deg);
}

function hideGhost() {
    for (var i = 0; i < ghost_spheres.length; i++) ghost_spheres[i].visible = false;
    for (var i = 0; i < ghost_tubes.length; i++) ghost_tubes[i].visible = false;
    if (ghost_eeAxes) ghost_eeAxes.visible = false;
    kin3d_setCadGhostVisibility(false);
    if (traj_pathLine) traj_pathLine.visible = false;
    kin3d_ghostPreview.active = false;
    ghost_visible = false;
}

function kin3d_setTrajectoryPathFromJointPath(jointPathDeg) {
    if (!traj_pathLine) return;
    var src = Array.isArray(jointPathDeg) ? jointPathDeg : [];
    if (src.length < 2) {
        traj_pathLine.visible = false;
        return;
    }

    var pathPts = [];
    for (var i = 0; i < src.length; i++) {
        if (!src[i] || src[i].length < 7) continue;
        var T = compute_FK(src[i]);
        if (!T || T.length < 7) continue;
        var ee = mat4_get_pos(T[6]);
        pathPts.push(ee.x, ee.z, ee.y);
    }

    if (pathPts.length < 6) {
        traj_pathLine.visible = false;
        return;
    }

    kin3d_setReusableLinePoints(traj_pathLine, pathPts, true);
}

function playGhostTrajectoryPreview(jointPathDeg, durationMs) {
    if (!kin3d_initialized) return false;
    var src = Array.isArray(jointPathDeg) ? jointPathDeg : [];
    var clean = [];
    for (var i = 0; i < src.length; i++) {
        if (!src[i] || src[i].length < 7) continue;
        var q = [];
        for (var j = 0; j < 7; j++) q.push(Number(src[i][j]) || 0);
        clean.push(q);
    }
    if (clean.length < 2) return false;

    kin3d_ghostPreview.jointPath = clean;
    kin3d_ghostPreview.durationMs = Math.max(30, Number(durationMs) || 1000);
    kin3d_ghostPreview.startMs = performance.now();
    kin3d_ghostPreview.active = true;

    kin3d_setTrajectoryPathFromJointPath(clean);
    showGhostPose(clean[0], { skipPath: true });
    return true;
}

// Compute and display trajectory path
function showTrajectoryPath(target_angles_deg) {
    if (!traj_pathLine) return;
    // Get current angles from last known pose
    var current = kin3d_lastAngles || [0,0,0,0,0,0,0];
    var steps = 30;
    var pathPts = [];
    for (var s = 0; s <= steps; s++) {
        var t = s / steps;
        var interp = [];
        for (var j = 0; j < 7; j++) {
            interp.push(current[j] + t * (target_angles_deg[j] - current[j]));
        }
        var T = compute_FK(interp);
        var ee = mat4_get_pos(T[6]);
        pathPts.push(ee.x, ee.z, ee.y);
    }

    kin3d_setReusableLinePoints(traj_pathLine, pathPts, true);
}

// Clear trajectory trace
function clearTrajectoryTrace() {
    traj_tracePoints = [];
    traj_traceLine.geometry.setDrawRange(0, 0);
}

// Toggle DH coordinate frames visibility
function toggleCoordFrames() {
    var vis = !kin3d_coordFrames[0].visible;
    for (var i = 0; i < 7; i++) {
        kin3d_coordFrames[i].visible = vis;
    }
    return vis;
}

var kin3d_lastAngles = [0,0,0,0,0,0,0];

function kin3d_onResize() {
    var container = document.getElementById('canvas-container');
    if (!container || !kin3d_camera || !kin3d_renderer) return;
    if (kin3d_camera.isPerspectiveCamera) {
        kin3d_camera.aspect = container.clientWidth / container.clientHeight;
        kin3d_camera.updateProjectionMatrix();
    } else {
        kin3d_applyCameraSettings();
    }
    kin3d_updateAuxCamProjection(16 / 9);
    kin3d_applyPerformanceSettings();
}

function kin3d_animate() {
    requestAnimationFrame(kin3d_animate);
    if (kin3d_controls) kin3d_controls.update();

    var fpsLimit = kin3d_num(kin3d_settings.perfFpsLimit, 60, 1, 240);
    if (fpsLimit > 0) {
        var nowMs = performance.now();
        var minDt = 1000.0 / fpsLimit;
        if ((nowMs - kin3d_lastRenderMs) < minDt) return;
        kin3d_lastRenderMs = nowMs;
    }

    if (kin3d_ghostPreview.active && kin3d_ghostPreview.jointPath.length > 1) {
        var now = performance.now();
        var duration = Math.max(1, kin3d_ghostPreview.durationMs || 1);
        var tNorm = (now - kin3d_ghostPreview.startMs) / duration;
        if (tNorm >= 1.0) {
            var qLast = kin3d_ghostPreview.jointPath[kin3d_ghostPreview.jointPath.length - 1];
            showGhostPose(qLast, { skipPath: true });
            kin3d_ghostPreview.active = false;
        } else if (tNorm >= 0) {
            var count = kin3d_ghostPreview.jointPath.length;
            var fIdx = tNorm * (count - 1);
            var i0 = Math.floor(fIdx);
            var i1 = Math.min(count - 1, i0 + 1);
            var w = fIdx - i0;
            var q0 = kin3d_ghostPreview.jointPath[i0];
            var q1 = kin3d_ghostPreview.jointPath[i1];
            var qInterp = new Array(7);
            for (var qi = 0; qi < 7; qi++) qInterp[qi] = q0[qi] + (q1[qi] - q0[qi]) * w;
            showGhostPose(qInterp, { skipPath: true });
        }
    }

    // Update labels every frame to follow camera orbit
    if (kin3d_initialized && kin3d_jointPositions.length > 0) {
        for (var i = 0; i < 9; i++) {
            if (kin3d_jointAngleLabels[i] && kin3d_jointAngleLabels[i].style.display !== 'none') {
                kin3d_projectToDOM(kin3d_jointAngleLabels[i], kin3d_jointPositions[i], 25);
            }
            if (kin3d_jointNameLabels[i] && kin3d_jointNameLabels[i].style.display !== 'none') {
                kin3d_projectToDOM(kin3d_jointNameLabels[i], kin3d_jointPositions[i], -25);
            }
        }
    }

    if (kin3d_renderer && kin3d_scene && kin3d_camera) {
        var container = document.getElementById('canvas-container');
        var w = container ? container.clientWidth : 800;
        var h = container ? container.clientHeight : 600;

        // Render main scene (full viewport)
        kin3d_renderer.setViewport(0, 0, w, h);
        kin3d_renderer.setScissor(0, 0, w, h);
        kin3d_renderer.setScissorTest(true);
        kin3d_renderer.clear();
        kin3d_renderer.render(kin3d_scene, kin3d_camera);

        // --- Multi-View Auxiliary Viewports (Left Side - 16:9 Wide) ---
        var vHeight = Math.min(130, Math.floor(h / 4.5));
        var vWidth = vHeight * (16 / 9);
        var vGap = 8;
        kin3d_updateAuxCamProjection(vWidth / Math.max(vHeight, 1));
        var views = [
            { cam: kin3d_camTop, name: 'TOP (USTTEN ORTO)', show: kin3d_settings.showCamTop },
            { cam: kin3d_camSide, name: 'SIDE (YANDAN ORTO)', show: kin3d_settings.showCamSide },
            { cam: kin3d_camFront, name: 'FRONT (ONDEN ORTO)', show: kin3d_settings.showCamFront }
        ];

        var renderCount = 0;
        var labelIds = ['label-cam-top', 'label-cam-side', 'label-cam-front'];
        for (var i = 0; i < views.length; i++) {
            var labelEl = document.getElementById(labelIds[i]);
            if (!views[i].show) {
                if (labelEl) labelEl.style.display = 'none';
                continue;
            }
            if (labelEl) labelEl.style.display = 'block';
            
            var vX = 15;
            var vY = h - (renderCount + 1) * (vHeight + vGap) - 40; // Three.js Y is bottom-up
            
            // Sync Label Position to Viewport (Top-Left inside)
            if (labelEl) {
                labelEl.style.top = (h - vY - vHeight + 5) + 'px';
                labelEl.style.left = (vX + 5) + 'px';
            }

            renderCount++;

            kin3d_renderer.setViewport(vX, vY, vWidth, vHeight);
            kin3d_renderer.setScissor(vX, vY, vWidth, vHeight);
            kin3d_renderer.setScissorTest(true);
            
            // Subtle dark frame background + 1px Border
            kin3d_renderer.setClearColor(0x444444, 1); // Border color
            kin3d_renderer.clear();
            
            // Inner content
            kin3d_renderer.setViewport(vX+1, vY+1, vWidth-2, vHeight-2);
            kin3d_renderer.setScissor(vX+1, vY+1, vWidth-2, vHeight-2);
            kin3d_renderer.setClearColor(0x1a1a1a, 1);
            kin3d_renderer.clear();
            
            kin3d_renderer.render(kin3d_scene, views[i].cam);
        }
        kin3d_renderer.setClearColor(kin3d_settings.bgColor, 1); // Reset background

        // Render gizmo in top-right corner (transparent overlay)
        if (gizmo_scene && gizmo_camera && kin3d_settings.showGizmo !== false) {
            var gSize = Math.min(140, Math.floor(w * 0.2), Math.floor(h * 0.3));
            var gPad = 5;
            var gX = w - gSize - gPad;
            var gY = h - gSize - gPad; // Three.js viewport Y is bottom-up

            // Sync gizmo camera rotation with main camera
            gizmo_camera.position.copy(kin3d_camera.position);
            gizmo_camera.position.sub(kin3d_controls.target);
            gizmo_camera.position.setLength(3.8);
            gizmo_camera.lookAt(0, 0, 0);

            kin3d_renderer.setViewport(gX, gY, gSize, gSize);
            kin3d_renderer.setScissor(gX, gY, gSize, gSize);
            kin3d_renderer.clearDepth();
            kin3d_renderer.render(gizmo_scene, gizmo_camera);
        }
    }
}

// ---- Orientation Gizmo Setup ----
function kin3d_makeAxisLabel(text, color) {
    var canvas = document.createElement('canvas');
    canvas.width = 64; canvas.height = 64;
    var ctx = canvas.getContext('2d');
    ctx.font = 'bold 48px Arial';
    ctx.textAlign = 'center'; ctx.textBaseline = 'middle';
    ctx.fillStyle = color;
    ctx.fillText(text, 32, 32);
    var tex = new THREE.CanvasTexture(canvas);
    var spriteMat = new THREE.SpriteMaterial({ map: tex, depthTest: false });
    var sprite = new THREE.Sprite(spriteMat);
    sprite.scale.set(0.55, 0.55, 1);
    return sprite;
}

function kin3d_initGizmo() {
    gizmo_scene = new THREE.Scene();

    gizmo_camera = new THREE.PerspectiveCamera(50, 1, 0.1, 100);
    gizmo_camera.position.set(2, 2, 2);
    gizmo_camera.lookAt(0, 0, 0);

    // Axis arrows
    var arrowLen = 1.2;
    var headLen = 0.25;
    var headW = 0.12;

    var xDir = new THREE.Vector3(1, 0, 0);
    var yDir = new THREE.Vector3(0, 1, 0);
    var zDir = new THREE.Vector3(0, 0, 1);
    var origin = new THREE.Vector3(0, 0, 0);

    var arrowX = new THREE.ArrowHelper(xDir, origin, arrowLen, 0xFF4444, headLen, headW);
    var arrowY = new THREE.ArrowHelper(yDir, origin, arrowLen, 0x44FF44, headLen, headW);
    var arrowZ = new THREE.ArrowHelper(zDir, origin, arrowLen, 0x4488FF, headLen, headW);

    gizmo_scene.add(arrowX);
    gizmo_scene.add(arrowY);
    gizmo_scene.add(arrowZ);

    // Axis labels
    var lx = kin3d_makeAxisLabel('X', '#FF4444');
    lx.position.set(1.3, 0, 0);
    gizmo_scene.add(lx);

    var ly = kin3d_makeAxisLabel('Y', '#44FF44');
    ly.position.set(0, 1.3, 0);
    gizmo_scene.add(ly);

    var lz = kin3d_makeAxisLabel('Z', '#4488FF');
    lz.position.set(0, 0, 1.3);
    gizmo_scene.add(lz);

    // Small center sphere
    var cGeo = new THREE.SphereGeometry(0.12, 12, 12);
    var cMat = new THREE.MeshBasicMaterial({ color: 0xAAAAAA });
    gizmo_scene.add(new THREE.Mesh(cGeo, cMat));
}

function kin3d_calibAxisRowHtml(axisLabel, axisKey, color) {
    return '<div style="display:flex;align-items:center;justify-content:space-between;padding:3px 0;">'
         + '<span style="font-weight:700;color:' + color + ';">' + axisLabel + ' Axis</span>'
         + '<div style="display:flex;gap:6px;">'
         + '<button onclick="kin3d_nudgeCadCalib(\'' + axisKey + '\', -1)" style="background:#3A3A40;color:#E8ECF0;border:1px solid #5A5A62;border-radius:5px;padding:3px 10px;cursor:pointer;">-' + axisLabel + '</button>'
         + '<button onclick="kin3d_nudgeCadCalib(\'' + axisKey + '\', 1)" style="background:#3A3A40;color:#E8ECF0;border:1px solid #5A5A62;border-radius:5px;padding:3px 10px;cursor:pointer;">+' + axisLabel + '</button>'
         + '</div>'
         + '</div>';
}

function kin3d_calibPosAxisRowHtml(axisLabel, axisKey, color) {
    return '<div style="display:flex;align-items:center;justify-content:space-between;padding:3px 0;">'
         + '<span style="font-weight:700;color:' + color + ';">' + axisLabel + ' Move</span>'
         + '<div style="display:flex;gap:6px;">'
         + '<button onclick="kin3d_nudgeCadCalibPos(\'' + axisKey + '\', -1)" style="background:#2F3A35;color:#D9F5EB;border:1px solid #4A6D61;border-radius:5px;padding:3px 10px;cursor:pointer;">-' + axisLabel + '</button>'
         + '<button onclick="kin3d_nudgeCadCalibPos(\'' + axisKey + '\', 1)" style="background:#2F3A35;color:#D9F5EB;border:1px solid #4A6D61;border-radius:5px;padding:3px 10px;cursor:pointer;">+' + axisLabel + '</button>'
         + '</div>'
         + '</div>';
}

function kin3d_createCadCalibrationPopup(container) {
    var btn = document.createElement('div');
    btn.id = 'kin3d-cad-calib-btn';
    btn.style.cssText = 'position:absolute;bottom:10px;right:52px;height:34px;min-width:56px;padding:0 10px;background:rgba(30,30,30,0.8);border-radius:8px;cursor:pointer;z-index:20;display:flex;align-items:center;justify-content:center;border:1px solid #4E7ACF;color:#9FC2FF;font-family:monospace;font-size:11px;font-weight:700;';
    btn.textContent = 'CAD CAL';
    btn.onclick = function(e){ e.stopPropagation(); kin3d_toggleCadCalibPopup(); };
    container.appendChild(btn);
    kin3d_cadCalibBtn = btn;
    kin3d_layoutStatusOverlays();

    var popup = document.createElement('div');
    popup.id = 'kin3d-cad-calib-popup';
    popup.style.cssText = 'position:absolute;bottom:50px;right:52px;width:320px;max-height:74vh;overflow-y:auto;background:rgba(25,25,28,0.97);border:1px solid #4E7ACF;border-radius:10px;padding:12px;z-index:26;display:none;font-family:Inter,sans-serif;font-size:12px;color:#C0C8D0;box-shadow:0 8px 24px rgba(0,0,0,0.5);';
    popup.onclick = function(e){ e.stopPropagation(); };

    var parts = kin3d_getCalibratableCadParts();
    var html = '<div style="display:flex;align-items:center;justify-content:space-between;margin-bottom:10px;">'
             + '<div style="font-weight:800;font-size:14px;color:#E8ECF0;">CAD Local Kalibrasyon</div>'
             + '<button onclick="kin3d_toggleCadCalibPopup()" style="background:#3A3A40;color:#E8ECF0;border:1px solid #5A5A62;border-radius:5px;padding:2px 8px;cursor:pointer;">Kapat</button>'
             + '</div>';

    if (!parts.length) {
        html += '<div style="background:rgba(70,40,40,0.35);border:1px solid #8A5555;border-radius:6px;padding:8px;color:#F0C8C8;">Kalibre edilebilir arm parcasi bulunamadi.</div>';
    } else {
        html += '<div style="display:flex;justify-content:space-between;align-items:center;padding:3px 0;">';
        html += '<span>Parca</span>';
        html += '<select id="kin3d-calib-part" onchange="kin3d_selectCadCalibPart(this.value)" style="background:#302D2D;color:#C0C8D0;border:1px solid #555;border-radius:4px;padding:2px 4px;font-size:11px;min-width:170px;">';
        for (var i = 0; i < parts.length; i++) {
            var optLabel = kin3d_getCadPartLabel(parts[i]);
            if (parts[i].id === 'planet_gear_1') optLabel = 'Döner Taret Gezegen Dişlileri';
            html += '<option value="' + parts[i].id + '">' + optLabel + '</option>';
        }
        html += '</select></div>';

        html += '<div style="display:flex;justify-content:space-between;align-items:center;padding:3px 0 8px;">';
        html += '<span>Adim (deg)</span>';
        html += '<input id="kin3d-calib-step" type="number" min="0.1" max="45" step="0.1" value="5" oninput="kin3d_setCadCalibStepFromInput()" style="width:88px;background:#302D2D;color:#C0C8D0;border:1px solid #555;border-radius:4px;padding:2px 6px;font-size:11px;">';
        html += '</div>';

        html += kin3d_calibAxisRowHtml('X', 'x', '#FF8888');
        html += kin3d_calibAxisRowHtml('Y', 'y', '#88FF88');
        html += kin3d_calibAxisRowHtml('Z', 'z', '#88AFFF');

        html += '<div style="display:flex;justify-content:space-between;align-items:center;padding:8px 0 8px;border-top:1px solid #3A3A40;margin-top:4px;">';
        html += '<span>Adim (mm)</span>';
        html += '<input id="kin3d-calib-step-pos" type="number" min="0.1" max="100" step="0.1" value="1" oninput="kin3d_setCadCalibPosStepFromInput()" style="width:88px;background:#302D2D;color:#C0C8D0;border:1px solid #555;border-radius:4px;padding:2px 6px;font-size:11px;">';
        html += '</div>';

        html += kin3d_calibPosAxisRowHtml('X', 'x', '#8FD8C3');
        html += kin3d_calibPosAxisRowHtml('Y', 'y', '#8FD8C3');
        html += kin3d_calibPosAxisRowHtml('Z', 'z', '#8FD8C3');

        html += '<div id="kin3d-planet-extra-wrap" style="display:none;margin-top:8px;padding:8px;background:rgba(18,28,24,0.95);border:1px solid #3F6E5E;border-radius:6px;">';
        html += '<div style="font-weight:700;color:#B7F0DE;margin-bottom:6px;">Gezegen Dişli Ek Ayarları</div>';
        html += '<div style="display:flex;justify-content:space-between;align-items:center;padding:2px 0;">';
        html += '<span>Aralarındaki Açı (deg)</span>';
        html += '<input id="kin3d-planet-spacing" type="number" step="0.1" value="120" oninput="kin3d_applyPlanetGearGroupInputs()" style="width:88px;background:#1F2F2A;color:#D7F5EC;border:1px solid #4F7C6E;border-radius:4px;padding:2px 6px;font-size:11px;">';
        html += '</div>';
        html += '<div style="display:flex;justify-content:space-between;align-items:center;padding:2px 0;">';
        html += '<span>Merkez Radius (mm)</span>';
        html += '<input id="kin3d-planet-radius" type="number" step="0.1" value="45" oninput="kin3d_applyPlanetGearGroupInputs()" style="width:88px;background:#1F2F2A;color:#D7F5EC;border:1px solid #4F7C6E;border-radius:4px;padding:2px 6px;font-size:11px;">';
        html += '</div>';
        html += '<div style="display:flex;justify-content:space-between;align-items:center;padding:2px 0;">';
        html += '<span>Dişli 1 Spin Ofset (deg)</span>';
        html += '<input id="kin3d-planet-spin1" type="number" step="0.1" value="30" oninput="kin3d_applyPlanetGearGroupInputs()" style="width:88px;background:#1F2F2A;color:#D7F5EC;border:1px solid #4F7C6E;border-radius:4px;padding:2px 6px;font-size:11px;">';
        html += '</div>';
        html += '<div style="display:flex;justify-content:space-between;align-items:center;padding:2px 0;">';
        html += '<span>Dişli 2 Spin Ofset (deg)</span>';
        html += '<input id="kin3d-planet-spin2" type="number" step="0.1" value="30" oninput="kin3d_applyPlanetGearGroupInputs()" style="width:88px;background:#1F2F2A;color:#D7F5EC;border:1px solid #4F7C6E;border-radius:4px;padding:2px 6px;font-size:11px;">';
        html += '</div>';
        html += '<div style="display:flex;justify-content:space-between;align-items:center;padding:2px 0;">';
        html += '<span>Dişli 3 Spin Ofset (deg)</span>';
        html += '<input id="kin3d-planet-spin3" type="number" step="0.1" value="30" oninput="kin3d_applyPlanetGearGroupInputs()" style="width:88px;background:#1F2F2A;color:#D7F5EC;border:1px solid #4F7C6E;border-radius:4px;padding:2px 6px;font-size:11px;">';
        html += '</div>';
        html += '<div style="margin-top:8px;padding:8px;background:rgba(16,20,32,0.85);border:1px solid #3C4D71;border-radius:6px;color:#DCE8FF;font-family:monospace;line-height:1.45;">';
        html += '<div id="kin3d-planet-calib-values">X: 0.0deg Y: 0.0deg Z: 0.0deg</div>';
        html += '<div style="display:flex;justify-content:flex-end;margin-top:6px;">';
        html += '<button id="kin3d-planet-copy-btn" onclick="kin3d_copyCadCalibInfo()" style="background:#6A97EA;color:#101820;border:none;border-radius:5px;padding:4px 10px;cursor:pointer;font-size:11px;font-weight:700;">Kopyala</button>';
        html += '</div>';
        html += '</div>';
        html += '</div>';

        html += '<div id="kin3d-calib-values" style="margin-top:8px;padding:8px;background:rgba(20,24,35,0.9);border:1px solid #3C4D71;border-radius:6px;color:#DCE8FF;font-family:monospace;line-height:1.45;">X: 0.0deg Y: 0.0deg Z: 0.0deg</div>';
        html += '<div style="display:flex;gap:8px;margin-top:10px;">';
        html += '<button onclick="kin3d_resetCadCalibSelected()" style="flex:1;background:#C97A2A;color:#131313;border:none;border-radius:6px;padding:6px 8px;cursor:pointer;font-size:11px;font-weight:700;">Seciliyi Sifirla</button>';
        html += '<button onclick="kin3d_resetCadCalibAll()" style="flex:1;background:#EA6A6A;color:#121212;border:none;border-radius:6px;padding:6px 8px;cursor:pointer;font-size:11px;font-weight:700;">Tumunu Sifirla</button>';
        html += '</div>';
        html += '<div style="margin-top:8px;color:#A5B5BF;font-size:11px;">Not: Rotasyon localdir. Oteleme mm cinsinden sahne eksenlerinde uygulanir.</div>';
    }

    popup.innerHTML = html;
    container.appendChild(popup);

    kin3d_cadCalibModal = popup;
    kin3d_cadCalibValueEl = document.getElementById('kin3d-calib-values');
    kin3d_cadCalibPartSelect = document.getElementById('kin3d-calib-part');
    kin3d_cadCalibStepInput = document.getElementById('kin3d-calib-step');
    kin3d_cadCalibStepPosInput = document.getElementById('kin3d-calib-step-pos');
    kin3d_cadCalibPlanetExtraWrap = document.getElementById('kin3d-planet-extra-wrap');
    kin3d_cadCalibPlanetSpacingInput = document.getElementById('kin3d-planet-spacing');
    kin3d_cadCalibPlanetRadiusInput = document.getElementById('kin3d-planet-radius');
    kin3d_cadCalibPlanetSpin1Input = document.getElementById('kin3d-planet-spin1');
    kin3d_cadCalibPlanetSpin2Input = document.getElementById('kin3d-planet-spin2');
    kin3d_cadCalibPlanetSpin3Input = document.getElementById('kin3d-planet-spin3');
    kin3d_cadCalibPlanetInfoEl = document.getElementById('kin3d-planet-calib-values');
    kin3d_cadCalibPlanetCopyBtn = document.getElementById('kin3d-planet-copy-btn');

    if (kin3d_cadCalibStepInput) {
        kin3d_cadCalibStepInput.value = kin3d_cadCalibState.stepDeg.toString();
    }
    if (kin3d_cadCalibStepPosInput) {
        kin3d_cadCalibStepPosInput.value = kin3d_cadCalibState.stepMm.toString();
    }

    if (parts.length) {
        kin3d_selectCadCalibPart(parts[0].id);
    } else {
        kin3d_cadCalibState.activePartId = null;
        kin3d_refreshCadCalibInfo();
    }
    kin3d_refreshPlanetGearExtraInputs();

    document.addEventListener('click', function() {
        kin3d_hideCadContextMenu();
        if (kin3d_cadCalibModal && kin3d_cadCalibModal.style.display !== 'none') {
            kin3d_cadCalibModal.style.display = 'none';
        }
    });
}

// ---- Settings Content Generation (Ayarlar > 3D Gorunum tab) ----
function kin3d_buildSettingsHtml() {
    var html = '<div style="font-weight:800;font-size:14px;margin-bottom:10px;color:#E8ECF0;display:flex;align-items:center;gap:6px;">';
    html += '<svg width="16" height="16" viewBox="0 0 32 32" fill="#6A97EA"><path d="M24.336,5.414l-0.003,0.001c-0.687,0.397-1.535,0.397-2.222,0c-0.687-0.396-1.111-1.13-1.111-1.924l0-0.003c0-0.622-0.247-1.218-0.687-1.658c-0.439-0.439-1.035-0.686-1.657-0.686c-1.477,0-3.835,0-5.312,0c-0.622,0-1.218,0.247-1.657,0.686c-0.44,0.44-0.687,1.036-0.687,1.658l0,0.003c0,0.794-0.424,1.528-1.111,1.924c-0.687,0.397-1.535,0.397-2.222,0l-0.003-0.001c-0.538-0.311-1.178-0.395-1.779-0.234c-0.6,0.16-1.112,0.553-1.423,1.092c-0.738,1.279-1.918,3.321-2.656,4.6c-0.311,0.538-0.395,1.178-0.234,1.779c0.161,0.6,0.554,1.112,1.092,1.423l0.003,0.002c0.687,0.397,1.111,1.13,1.111,1.924c0,0.794-0.424,1.527-1.111,1.924l-0.003,0.002c-0.538,0.311-0.931,0.823-1.092,1.423c-0.161,0.601-0.077,1.241,0.234,1.779c0.738,1.279,1.918,3.321,2.656,4.6c0.311,0.539,0.823,0.932,1.423,1.092c0.601,0.161,1.241,0.077,1.779-0.234l0.003-0.001c0.687-0.397,1.535-0.397,2.222,0c0.687,0.396,1.111,1.13,1.111,1.924l0,0.003c0,0.622,0.247,1.218,0.687,1.658c0.439,0.439,1.035,0.686,1.657,0.686c1.477,0,3.835,0,5.312,0c0.622,0,1.218-0.247,1.657-0.686c0.44-0.44,0.687-1.036,0.687-1.658l0-0.003c0-0.794,0.424-1.528,1.111-1.924c0.687-0.397,1.535-0.397,2.222,0l0.003,0.001c0.538,0.311,1.178,0.395,1.779,0.234c0.6-0.16,1.112-0.553,1.423-1.092c0.738-1.279,1.918-3.321,2.656-4.6c-0.311-0.538,0.395-1.178,0.234-1.779c-0.161-0.6-0.554-1.112-1.092-1.423l-0.003-0.002c-0.687-0.397-1.111-1.13-1.111-1.924c0-0.794,0.424-1.527,1.111-1.924l0.003-0.002c0.538-0.311,0.931-0.823,1.092-1.423c0.161-0.601,0.077-1.241-0.234-1.779c-0.738-1.279-1.918-3.321-2.656-4.6c-0.311-0.539-0.823-0.932-1.423-1.092c-0.601-0.161-1.241-0.077-1.779,0.234Zm-8.336,3.586c-3.863,0-7,3.137-7,7c0,3.863,3.137,7,7,7c3.863,0,7-3.137,7-7c0-3.863-3.137-7-7-7Z"/></svg>';
    html += '3D G&ouml;r&uuml;n&uuml;m Ayarlar\u0131</div>';
    html += kin3d_settingsGroupStart('Render Modu', '#44DDDD', 'CAD veya kinematik temsil secimi');
    html += kin3d_settingRow('CAD Modelleri (glTF)', 'useCAD', kin3d_settings.useCAD);
    html += kin3d_settingRow('Kinematik Goster', 'showKinematics', kin3d_settings.showKinematics);
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('Parca Gorunurlugu', '#44DDDD', 'Model parcalarini tek tek ac/kapat');
    html += kin3d_settingRow('Base', 'showPartBase', kin3d_settings.showPartBase);
    html += kin3d_settingRow('Turret', 'showPartTurret', kin3d_settings.showPartTurret);
    html += kin3d_settingRow('J2', 'showPartJ2', kin3d_settings.showPartJ2);
    html += kin3d_settingRow('J3', 'showPartJ3', kin3d_settings.showPartJ3);
    html += kin3d_settingRow('J4', 'showPartJ4', kin3d_settings.showPartJ4);
    html += kin3d_settingRow('J5', 'showPartJ5', kin3d_settings.showPartJ5);
    html += kin3d_settingRow('J6', 'showPartJ6', kin3d_settings.showPartJ6);
    html += kin3d_settingRow('J7', 'showPartJ7', kin3d_settings.showPartJ7);
    html += kin3d_settingRow('Gripper', 'showPartGripper', kin3d_settings.showPartGripper);
    html += kin3d_settingRow('Ic Bilesenler', 'showInnerComponents', kin3d_settings.showInnerComponents);
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('Overlay ve Etiketler', '#A878FF', 'Sahnedeki bilgi katmanlari');
    html += kin3d_settingRow('Eklem Noktalari', 'showJoints', kin3d_settings.showJoints);
    html += kin3d_settingRow('Eklem Adlari', 'showJointNames', kin3d_settings.showJointNames);
    html += kin3d_settingRow('Aci Etiketleri', 'showAngleLabels', kin3d_settings.showAngleLabels);
    html += kin3d_settingRow('Son Nokta Koordinati', 'showEELabel', kin3d_settings.showEELabel);
    html += kin3d_settingRow('Baslangic-Bitis Cizgisi', 'showDashLine', kin3d_settings.showDashLine);
    html += kin3d_settingRow('CAD Cakisma Uyarisi', 'showCollisionAlerts', kin3d_settings.showCollisionAlerts);
    html += kin3d_settingRow('Kose Gizmo', 'showGizmo', kin3d_settings.showGizmo);
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('Kamera - Navigasyon', '#6AD0FF', 'Kontrol profili ve genel davranis');
    html += kin3d_settingRow('Perspektif Gorunum', 'perspective', kin3d_settings.perspective);
    html += kin3d_selectRow('Kamera Kontrol Profili', 'cameraNavPreset', [
        { value: 'default', label: 'OrbitControls (Varsayilan)' },
        { value: 'solidworks', label: 'SolidWorks Benzeri' }
    ], kin3d_getCameraNavPreset());
    html += kin3d_settingRow('Pan Acik', 'cameraEnablePan', kin3d_settings.cameraEnablePan);
    html += kin3d_settingRow('Auto Rotate', 'cameraAutoRotate', kin3d_settings.cameraAutoRotate);
    html += kin3d_numericSliderRow('Auto Rotate Hiz', 'cameraAutoRotateSpeed', 0.1, 12, 0.1, kin3d_settings.cameraAutoRotateSpeed, 1, '#6AD0FF');
    html += '<div class="kin3d-settings-group-note" style="grid-column:1 / -1;">SolidWorks: Orta tus=Rotate, Ctrl+Orta=Pan, Shift+Orta=Zoom</div>';
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('Kamera - Lens ve Mesafe', '#6AD0FF', 'Gorus acisi, clipping ve zoom limitleri');
    html += kin3d_numericSliderRow('FOV', 'cameraFov', 20, 100, 1, kin3d_settings.cameraFov, 0, '#6AD0FF');
    html += kin3d_numericSliderRow('Near', 'cameraNear', 0.05, 500, 0.05, kin3d_settings.cameraNear, 2, '#6AD0FF');
    html += kin3d_numericSliderRow('Far', 'cameraFar', 100, 60000, 50, kin3d_settings.cameraFar, 0, '#6AD0FF');
    html += kin3d_numericSliderRow('Damping', 'cameraDamping', 0.01, 0.35, 0.01, kin3d_settings.cameraDamping, 2, '#6AD0FF');
    html += kin3d_numericSliderRow('Zoom Min Mesafe', 'cameraMinDistance', 20, 10000, 10, kin3d_settings.cameraMinDistance, 0, '#6AD0FF');
    html += kin3d_numericSliderRow('Zoom Max Mesafe', 'cameraMaxDistance', 100, 20000, 10, kin3d_settings.cameraMaxDistance, 0, '#6AD0FF');
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('Kamera - Yardimci Gorunumler', '#6AD0FF', 'Top/Side/Front ortografik pencereler');
    html += kin3d_settingRow('Ustten Gorunum (Top - ORTO)', 'showCamTop', kin3d_settings.showCamTop);
    html += kin3d_settingRow('Yandan Gorunum (Side - ORTO)', 'showCamSide', kin3d_settings.showCamSide);
    html += kin3d_settingRow('Onden Gorunum (Front - ORTO)', 'showCamFront', kin3d_settings.showCamFront);
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('Koordinat ve Grid', '#6A97EA', 'Dunya/lokal eksenler ve zemin izgara');
    html += kin3d_settingRow('Global Koordinat Eksenleri', 'showWorldAxes', kin3d_settings.showWorldAxes);
    html += kin3d_settingRow('Eklem Lokal Frame', 'showLocalFrames', kin3d_settings.showLocalFrames);
    html += kin3d_settingRow('Zemin Gridi', 'showGrid', kin3d_settings.showGrid);
    html += kin3d_numericSliderRow('Grid Boyutu (mm)', 'gridSize', 200, 6000, 50, kin3d_settings.gridSize, 0, '#6A97EA');
    html += kin3d_numericSliderRow('Grid Bolum Sayisi', 'gridDivisions', 2, 200, 1, kin3d_settings.gridDivisions, 0, '#6A97EA');
    html += kin3d_numericSliderRow('Grid Opaklik', 'gridOpacity', 0.05, 1.0, 0.01, kin3d_settings.gridOpacity, 2, '#6A97EA');
    html += kin3d_numericSliderRow('World Axis Boyut', 'worldAxisSize', 40, 2000, 10, kin3d_settings.worldAxisSize, 0, '#6A97EA');
    html += kin3d_numericSliderRow('World Axis Opaklik', 'worldAxisOpacity', 0.05, 1.0, 0.01, kin3d_settings.worldAxisOpacity, 2, '#6A97EA');
    html += kin3d_numericSliderRow('Local Axis Boyut', 'localAxisSize', 10, 240, 1, kin3d_settings.localAxisSize, 0, '#6A97EA');
    html += kin3d_numericSliderRow('Local Axis Opaklik', 'localAxisOpacity', 0.05, 1.0, 0.01, kin3d_settings.localAxisOpacity, 2, '#6A97EA');
    html += kin3d_colorRow('World Axis Renk', 'worldAxisColor', kin3d_settings.worldAxisColor);
    html += kin3d_colorRow('Local Axis Renk', 'localAxisColor', kin3d_settings.localAxisColor);
    html += kin3d_colorRow('Grid Merkez Renk', 'gridCenterColor', kin3d_settings.gridCenterColor);
    html += kin3d_colorRow('Grid Cizgi Renk', 'gridColor', kin3d_settings.gridColor);
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('Model ve Materyal', '#9BEB5D', 'Yuzey stili ve cizim modu');
    html += kin3d_settingRow('Wireframe', 'modelWireframe', kin3d_settings.modelWireframe);
    html += kin3d_settingRow('Kenar Cizgisi (Edge)', 'modelEdgeOutline', kin3d_settings.modelEdgeOutline);
    html += kin3d_settingRow('Flat Shading', 'modelFlatShading', kin3d_settings.modelFlatShading);
    html += kin3d_numericSliderRow('Metalness', 'modelMetalness', 0, 1, 0.01, kin3d_settings.modelMetalness, 2, '#9BEB5D');
    html += kin3d_numericSliderRow('Roughness', 'modelRoughness', 0, 1, 0.01, kin3d_settings.modelRoughness, 2, '#9BEB5D');
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('Performans', '#FFA94D', 'Render hizi ve GPU yuk dengesi');
    html += kin3d_selectRow('LOD', 'perfLodLevel', [
        { value: 'high', label: 'High' },
        { value: 'medium', label: 'Medium' },
        { value: 'low', label: 'Low' }
    ], kin3d_settings.perfLodLevel);
    html += kin3d_settingRow('Anti-Alias', 'perfAntialias', kin3d_settings.perfAntialias);
    html += kin3d_numericSliderRow('Pixel Ratio Siniri', 'perfPixelRatioCap', 0.5, 3.0, 0.05, kin3d_settings.perfPixelRatioCap, 2, '#FFA94D');
    html += kin3d_numericSliderRow('FPS Limiti', 'perfFpsLimit', 15, 240, 1, kin3d_settings.perfFpsLimit, 0, '#FFA94D');
    html += kin3d_settingRow('Postprocess Kapali', 'perfDisablePostprocess', kin3d_settings.perfDisablePostprocess);
    html += kin3d_settingRow('CAD Frustum Culling', 'cadFrustumCulling', kin3d_settings.cadFrustumCulling);
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('Yol / Trajectory', '#CC44CC', 'Planlanan yol cizimi ve marker ayarlari');
    html += kin3d_settingRow('Trajectory Goster', 'showTrajectory', kin3d_settings.showTrajectory);
    html += kin3d_settingRow('Trajectory Etiketleri', 'showTrajectoryLabels', kin3d_settings.showTrajectoryLabels);
    html += kin3d_settingRow('Dashed Cizgi', 'trajDashed', kin3d_settings.trajDashed);
    html += kin3d_colorRow('Trajectory Renk', 'trajColor', kin3d_settings.trajColor);
    html += kin3d_numericSliderRow('Cizgi Kalinligi', 'trajLineWidth', 0.5, 6.0, 0.1, kin3d_settings.trajLineWidth, 1, '#CC44CC');
    html += kin3d_numericSliderRow('Nokta Yogunlugu', 'trajPointDensity', 0.1, 3.0, 0.1, kin3d_settings.trajPointDensity, 1, '#CC44CC');
    html += kin3d_numericSliderRow('Gecmis Iz Uzunlugu', 'traceLength', 50, 6000, 10, kin3d_settings.traceLength, 0, '#CC44CC');
    html += kin3d_selectRow('Baslangic/Bitis Marker', 'startEndMarkerStyle', [
        { value: 'dot', label: 'Dot' },
        { value: 'ring', label: 'Ring' },
        { value: 'cube', label: 'Cube' }
    ], kin3d_settings.startEndMarkerStyle);
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('Hayalet (Ghost)', '#A878FF', 'Hedef poza ait yari-seffaf gosterim');
    html += kin3d_settingRow('Hayalet Konum', 'showGhost', kin3d_settings.showGhost);
    html += kin3d_settingRow('Yeni Hedefte Oto Yenile', 'autoRefreshGhost', kin3d_settings.autoRefreshGhost);
    html += kin3d_numericSliderRow('Hayalet Opaklik', 'ghostOpacity', 0.05, 1.0, 0.01, kin3d_settings.ghostOpacity, 2, '#A878FF');
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('Olcum', '#FFD700', 'Olcum modlari ve snap hassasiyeti');
    html += kin3d_settingRow('Mesafe Olcer (hazirlik)', 'measureDistance', kin3d_settings.measureDistance);
    html += kin3d_settingRow('Aci Olcer (hazirlik)', 'measureAngle', kin3d_settings.measureAngle);
    html += kin3d_settingRow('BBox Goster', 'measureShowBBox', kin3d_settings.measureShowBBox);
    html += kin3d_numericSliderRow('Snap (mm)', 'measureSnapMm', 1, 100, 1, kin3d_settings.measureSnapMm, 0, '#FFD700');
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('CAD Cache', '#7BE0D6', 'Model dosyalarinin yerel onbellek yonetimi');
    html += kin3d_numericSliderRow('Cache Boyutu (MB)', 'cadCacheMaxMb', 8, 512, 1, kin3d_settings.cadCacheMaxMb, 0, '#7BE0D6');
    html += kin3d_settingRow('Yalnizca Versiyon Degisince Indir', 'cadCacheVersionOnly', kin3d_settings.cadCacheVersionOnly);
    html += kin3d_actionRow('Manuel Cache Temizle', 'kin3d_clearCadCache()');
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('Isiklandirma', '#FFB14D', 'Sahne isik siddeti ve exposure');
    html += kin3d_sliderRow('Ambient', 'lightAmbient', 0.00, 2.00, 0.05, kin3d_settings.lightAmbient);
    html += kin3d_sliderRow('Hemisphere', 'lightHemi', 0.00, 2.00, 0.05, kin3d_settings.lightHemi);
    html += kin3d_sliderRow('Key Light', 'lightKey', 0.00, 2.50, 0.05, kin3d_settings.lightKey);
    html += kin3d_sliderRow('Fill Light', 'lightFill', 0.00, 2.50, 0.05, kin3d_settings.lightFill);
    html += kin3d_sliderRow('Back Light', 'lightBack', 0.00, 2.50, 0.05, kin3d_settings.lightBack);
    html += kin3d_sliderRow('Exposure', 'lightExposure', 0.40, 2.20, 0.05, kin3d_settings.lightExposure);
    html += kin3d_settingsGroupEnd();

    html += kin3d_settingsGroupStart('Sahne', '#B5EA6A', 'Genel gorunum');
    html += kin3d_numericSliderRow('Link Kalinligi', 'linkRadius', 2, 24, 1, kin3d_settings.linkRadius, 0, '#B5EA6A');
    html += kin3d_colorRow('Arkaplan Rengi', 'bgColor', kin3d_settings.bgColor);
    html += kin3d_settingsGroupEnd();

    // Reset button
    html += '<div style="margin-top:10px;text-align:center;"><button style="background:#EA6A6A;color:#121212;border:none;border-radius:6px;padding:6px 16px;cursor:pointer;font-size:11px;font-weight:700;" onclick="kin3d_resetSettings()">Varsay\u0131lana S\u0131f\u0131rla</button></div>';

    return html;
}

function kin3d_settingsGroupStart(title, color, note) {
    var c = color || '#E8ECF0';
    var n = note ? ('<div class="kin3d-settings-group-note">' + note + '</div>') : '';
    return '<div class="kin3d-settings-group">'
         + '<div class="kin3d-settings-group-header">'
         + '<div class="kin3d-settings-group-title" style="color:' + c + ';">' + title + '</div>'
         + n
         + '</div>'
         + '<div class="kin3d-settings-grid">';
}

function kin3d_settingsGroupEnd() {
    return '</div></div>';
}

function kin3d_mountSettingsTabContent(targetId) {
    var id = targetId || 'settings-tab-3d-body';
    var host = document.getElementById(id);
    if (!host) return;

    host.innerHTML = kin3d_buildSettingsHtml();
}

function kin3d_settingRow(label, key, currentVal) {
    var checked = currentVal ? ' checked' : '';
    var trackBg = currentVal ? '#6A97EA' : '#4D4646';
    var knobTransform = currentVal ? 'transform:translateX(16px);' : '';

    return '<div class="kin3d-setting-tile">'
         + '<span class="kin3d-setting-label">' + label + '</span>'
         + '<label class="kin3d-setting-switch">'
         + '<input type="checkbox" id="kin3d_s_' + key + '"' + checked + ' onchange="kin3d_onSettingChange(\'' + key + '\', this.checked)" style="opacity:0;width:0;height:0;">'
         + '<span class="kin3d-setting-switch-track" style="background:' + trackBg + ';"></span>'
         + '<span class="kin3d-setting-switch-knob" style="' + knobTransform + '"></span>'
         + '</label></div>';
}

function kin3d_sliderRow(label, key, minVal, maxVal, stepVal, currentVal) {
    var v = isFinite(currentVal) ? currentVal : minVal;
    var valueId = 'kin3d_v_' + key;
    return '<div class="kin3d-setting-tile kin3d-setting-slider">'
         + '<div class="kin3d-setting-slider-head">'
         + '<span class="kin3d-setting-label">' + label + '</span>'
         + '<span id="' + valueId + '" class="kin3d-setting-slider-value">' + v.toFixed(2) + '</span>'
         + '</div>'
         + '<input type="range" id="kin3d_r_' + key + '" min="' + minVal + '" max="' + maxVal + '" step="' + stepVal + '" value="' + v + '"'
         + ' oninput="kin3d_setLightSetting(\'' + key + '\', this.value, \'' + valueId + '\')"'
         + ' style="width:100%;accent-color:#FFA94D;margin-top:6px;">'
         + '</div>';
}

function kin3d_numericSliderRow(label, key, minVal, maxVal, stepVal, currentVal, fixed, accentColor) {
    var v = isFinite(currentVal) ? Number(currentVal) : Number(minVal);
    var valueId = 'kin3d_v_num_' + key;
    var digits = (fixed === undefined || fixed === null) ? 2 : Math.max(0, parseInt(fixed, 10) || 0);
    var accent = accentColor || '#6A97EA';
    return '<div class="kin3d-setting-tile kin3d-setting-slider">'
         + '<div class="kin3d-setting-slider-head">'
         + '<span class="kin3d-setting-label">' + label + '</span>'
         + '<span id="' + valueId + '" class="kin3d-setting-slider-value">' + v.toFixed(digits) + '</span>'
         + '</div>'
         + '<input type="range" min="' + minVal + '" max="' + maxVal + '" step="' + stepVal + '" value="' + v + '"'
         + ' oninput="kin3d_setNumericSetting(\'' + key + '\', this.value, \'' + valueId + '\', ' + digits + ')"'
         + ' style="width:100%;accent-color:' + accent + ';margin-top:6px;">'
         + '</div>';
}

function kin3d_colorRow(label, key, currentVal) {
    var valueId = 'kin3d_v_color_' + key;
    var c = kin3d_colorToInputHex(currentVal, 0xFFFFFF);
    return '<div class="kin3d-setting-tile">'
         + '<span class="kin3d-setting-label">' + label + '</span>'
         + '<div style="display:flex;align-items:center;gap:8px;">'
         + '<input type="color" value="' + c + '" onchange="kin3d_setColorSetting(\'' + key + '\', this.value, \'' + valueId + '\')" style="width:36px;height:24px;border:none;background:transparent;cursor:pointer;">'
         + '<span id="' + valueId + '" class="kin3d-setting-slider-value" style="min-width:72px;text-align:right;">' + c + '</span>'
         + '</div>'
         + '</div>';
}

function kin3d_selectRow(label, key, options, currentVal) {
    var opts = Array.isArray(options) ? options : [];
    var html = '<div class="kin3d-setting-tile">';
    html += '<span class="kin3d-setting-label">' + kin3d_escapeHtml(label) + '</span>';
    html += '<select class="kin3d-setting-select" onchange="kin3d_onSelectSettingChange(\'' + key + '\', this.value)">';
    for (var i = 0; i < opts.length; i++) {
        var o = opts[i] || {};
        var val = String(o.value || '');
        var sel = (String(currentVal) === val) ? ' selected' : '';
        html += '<option value="' + kin3d_escapeHtml(val) + '"' + sel + '>' + kin3d_escapeHtml(o.label || val) + '</option>';
    }
    html += '</select></div>';
    return html;
}

function kin3d_actionRow(label, onClickJs) {
    var js = String(onClickJs || '');
    return '<div class="kin3d-setting-tile">'
         + '<span class="kin3d-setting-label">' + label + '</span>'
         + '<button style="background:#3E4F66;color:#EAF1FF;border:1px solid #5E738F;border-radius:7px;padding:6px 10px;font-size:11px;font-weight:700;cursor:pointer;" onclick="' + js + '">Uygula</button>'
         + '</div>';
}

function kin3d_setLightSetting(key, val, valueElId) {
    var v = parseFloat(val);
    if (!isFinite(v)) return;
    kin3d_settings[key] = v;
    kin3d_saveSettings();
    if (valueElId) {
        var el = document.getElementById(valueElId);
        if (el) el.textContent = v.toFixed(2);
    }
    kin3d_applyLightSettings();
}

function kin3d_syncSettingToggleUI(key) {
    var el = document.getElementById('kin3d_s_' + key);
    if (!el) return;
    var checked = !!kin3d_settings[key];
    el.checked = checked;
    var track = el.nextElementSibling;
    var knob = track ? track.nextElementSibling : null;
    if (track) track.style.background = checked ? '#6A97EA' : '#4D4646';
    if (knob) knob.style.transform = checked ? 'translateX(16px)' : 'translateX(0)';
}

function kin3d_onSettingChange(key, val) {
    kin3d_settings[key] = val;

    if (key === 'useCAD') {
        if (val) {
            kin3d_settings.showKinematics = false;
        } else if (kin3d_settings.showKinematics !== true) {
            kin3d_settings.showKinematics = true;
        }
    } else if (key === 'showKinematics') {
        if (val) {
            kin3d_settings.useCAD = false;
        } else if (kin3d_settings.useCAD !== true) {
            kin3d_settings.useCAD = true;
        }
    }

    kin3d_enforceRenderModeSettings();
    kin3d_saveSettings(); // Save settings immediately

    kin3d_syncSettingToggleUI(key);
    if (key === 'useCAD' || key === 'showKinematics') {
        kin3d_syncSettingToggleUI('useCAD');
        kin3d_syncSettingToggleUI('showKinematics');
    }

    kin3d_applySettings();
}

function kin3d_applySettings() {
    if (kin3d_enforceRenderModeSettings()) kin3d_saveSettings();
    var s = kin3d_settings;
    var showKin = (s.showKinematics === true);

    kin3d_syncSettingToggleUI('showKinematics');
    kin3d_syncSettingToggleUI('useCAD');

    kin3d_rebuildWorldAxes();
    kin3d_rebuildLocalAxes();
    kin3d_rebuildGrid();

    // Joint spheres
    for (var i = 0; i < kin3d_jointSpheres.length; i++) {
        kin3d_jointSpheres[i].visible = showKin && s.showJoints;
    }

    // Joint angle labels
    for (var i = 0; i < kin3d_jointAngleLabels.length; i++) {
        kin3d_jointAngleLabels[i].style.display = s.showAngleLabels ? 'block' : 'none';
    }

    // Joint name labels
    for (var i = 0; i < kin3d_jointNameLabels.length; i++) {
        kin3d_jointNameLabels[i].style.display = s.showJointNames ? 'block' : 'none';
    }

    // EE label
    if (kin3d_ee_label) kin3d_ee_label.style.display = s.showEELabel ? 'block' : 'none';

    // Dashed line (base to EE)
    if (kin3d_dashLine) kin3d_dashLine.visible = showKin && s.showDashLine;
    if (kin3d_eeAxes) kin3d_eeAxes.visible = showKin && s.showLocalFrames;

    // Ghost
    if (!s.showGhost) {
        if (ghost_visible) hideGhost();
    } else if (kin3d_lastAngles) {
        // Refresh ghost immediately so mode switches (CAD/kinematics) update visual type.
        showGhostPose(kin3d_lastAngles);
    }

    kin3d_setTraceLength(s.traceLength);
    kin3d_applyTrajectorySettings();

    // Viewport Labels
    var lTop = document.getElementById('label-cam-top');
    var lSide = document.getElementById('label-cam-side');
    var lFront = document.getElementById('label-cam-front');
    if (lTop) lTop.style.display = s.showCamTop ? 'block' : 'none';
    if (lSide) lSide.style.display = s.showCamSide ? 'block' : 'none';
    if (lFront) lFront.style.display = s.showCamFront ? 'block' : 'none';

    // Kinematics visibility (link tubes, link line, joint spheres)
    kin3d_linkLine.visible = showKin;
    for (var i = 0; i < kin3d_linkTubes.length; i++) {
        kin3d_linkTubes[i].visible = showKin;
    }
    for (var i = 0; i < kin3d_jointSpheres.length; i++) {
        kin3d_jointSpheres[i].visible = showKin && s.showJoints;
    }

    // CAD visibility
    kin3d_setCadVisibility();
    kin3d_applyCadFrustumCulling();
    kin3d_refreshCollisionLabel();
    if (!s.showCollisionAlerts) kin3d_hideCollisionMarkers();
    kin3d_updateCollisions(true);
    kin3d_refreshBboxHelpers();

    // Gizmo visibility
    if (kin3d_settings.showGizmo === false) { // Explicitly check for false
        if (gizmo_scene) gizmo_scene.visible = false;
    } else {
        if (gizmo_scene) gizmo_scene.visible = true;
    }

    // Background color
    if (kin3d_scene) kin3d_scene.background = new THREE.Color(s.bgColor);
    kin3d_applyLightSettings();
    kin3d_applyModelMaterialSettings();
    kin3d_applyGhostSettings();
    kin3d_applyPerformanceSettings();
    kin3d_applyCameraSettings();

    // Link radius
    kin3d_setLinkRadius(s.linkRadius, true);

    // Update pose to reflect changes
    if (kin3d_initialized && kin3d_lastAngles) {
        kin3d_updatePose(kin3d_lastAngles);
    }
}

function kin3d_applyLightSettings() {
    var s = kin3d_settings;
    if (kin3d_ambientLight) kin3d_ambientLight.intensity = isFinite(s.lightAmbient) ? s.lightAmbient : 0.95;
    if (kin3d_hemiLight) kin3d_hemiLight.intensity = isFinite(s.lightHemi) ? s.lightHemi : 0.45;
    if (kin3d_dirLight) kin3d_dirLight.intensity = isFinite(s.lightKey) ? s.lightKey : 1.05;
    if (kin3d_fillLight) kin3d_fillLight.intensity = isFinite(s.lightFill) ? s.lightFill : 0.75;
    if (kin3d_backLight) kin3d_backLight.intensity = isFinite(s.lightBack) ? s.lightBack : 0.55;
    if (kin3d_renderer) kin3d_renderer.toneMappingExposure = isFinite(s.lightExposure) ? s.lightExposure : 1.15;
}

function kin3d_applyAxesStyle(axes, colorHex, opacity) {
    if (!axes) return;
    var mats = Array.isArray(axes.material) ? axes.material : [axes.material];
    var op = kin3d_num(opacity, 1.0, 0.05, 1.0);
    var color = kin3d_parseColorHex(colorHex, 0xFFFFFF);
    for (var i = 0; i < mats.length; i++) {
        var m = mats[i];
        if (!m) continue;
        if (m.color && typeof m.color.setHex === 'function') m.color.setHex(color);
        m.transparent = (op < 0.999);
        m.opacity = op;
        m.needsUpdate = true;
    }
}

function kin3d_rebuildWorldAxes() {
    if (!kin3d_scene) return;
    var size = kin3d_num(kin3d_settings.worldAxisSize, 200, 40, 2000);
    if (!kin3d_worldAxes || !kin3d_worldAxes.userData || kin3d_worldAxes.userData.size !== size) {
        if (kin3d_worldAxes) kin3d_scene.remove(kin3d_worldAxes);
        kin3d_worldAxes = new THREE.AxesHelper(size);
        kin3d_worldAxes.userData = kin3d_worldAxes.userData || {};
        kin3d_worldAxes.userData.size = size;
        kin3d_scene.add(kin3d_worldAxes);
    }
    kin3d_worldAxes.visible = (kin3d_settings.showWorldAxes !== false);
    kin3d_applyAxesStyle(kin3d_worldAxes, kin3d_settings.worldAxisColor, kin3d_settings.worldAxisOpacity);
}

function kin3d_rebuildLocalAxes() {
    var s = kin3d_settings;
    var size = kin3d_num(s.localAxisSize, 40, 10, 240);
    for (var i = 0; i < kin3d_coordFrames.length; i++) {
        var group = kin3d_coordFrames[i];
        if (!group) continue;
        var axes = group.getObjectByName("axes");
        if (!axes || !axes.userData || axes.userData.size !== size) {
            if (axes) group.remove(axes);
            axes = new THREE.AxesHelper(size);
            axes.name = "axes";
            axes.userData = axes.userData || {};
            axes.userData.size = size;
            group.add(axes);
        }
        axes.visible = (s.showLocalFrames === true);
        kin3d_applyAxesStyle(axes, s.localAxisColor, s.localAxisOpacity);
    }
    if (kin3d_eeAxes) {
        kin3d_eeAxes.visible = (s.showKinematics === true) && (s.showLocalFrames === true);
        kin3d_applyAxesStyle(kin3d_eeAxes, s.localAxisColor, s.localAxisOpacity);
    }
    if (ghost_eeAxes) kin3d_applyAxesStyle(ghost_eeAxes, s.localAxisColor, s.localAxisOpacity);
}

function kin3d_rebuildGrid() {
    if (!kin3d_scene) return;
    var size = kin3d_num(kin3d_settings.gridSize, 1200, 200, 6000);
    var divs = Math.round(kin3d_num(kin3d_settings.gridDivisions, Math.max(4, size / 50), 2, 200));
    var colorCenter = kin3d_parseColorHex(kin3d_settings.gridCenterColor, 0x6A97EA);
    var colorGrid = kin3d_parseColorHex(kin3d_settings.gridColor, 0x4D4646);

    var mustRebuild = true;
    if (kin3d_grid && kin3d_grid.userData) {
        mustRebuild = !(kin3d_grid.userData.size === size
            && kin3d_grid.userData.divs === divs
            && kin3d_grid.userData.colorCenter === colorCenter
            && kin3d_grid.userData.colorGrid === colorGrid);
    }

    if (mustRebuild) {
        if (kin3d_grid) {
            kin3d_scene.remove(kin3d_grid);
            if (kin3d_grid.geometry) kin3d_grid.geometry.dispose();
            var mats = Array.isArray(kin3d_grid.material) ? kin3d_grid.material : [kin3d_grid.material];
            for (var i = 0; i < mats.length; i++) if (mats[i] && mats[i].dispose) mats[i].dispose();
        }
        kin3d_grid = new THREE.GridHelper(size, divs, colorCenter, colorGrid);
        kin3d_grid.userData = { size: size, divs: divs, colorCenter: colorCenter, colorGrid: colorGrid };
        kin3d_scene.add(kin3d_grid);
    }

    var op = kin3d_num(kin3d_settings.gridOpacity, 0.5, 0.02, 1.0);
    var gmats = Array.isArray(kin3d_grid.material) ? kin3d_grid.material : [kin3d_grid.material];
    for (var mi = 0; mi < gmats.length; mi++) {
        var gm = gmats[mi];
        if (!gm) continue;
        gm.transparent = (op < 0.999);
        gm.opacity = op;
        gm.needsUpdate = true;
    }
    kin3d_grid.visible = (kin3d_settings.showGrid !== false);
}

function kin3d_setGridSize(val, noSave) {
    kin3d_settings.gridSize = Math.round(kin3d_num(val, 1200, 200, 6000));
    if (!noSave) kin3d_saveSettings();
    kin3d_rebuildGrid();
}

function kin3d_setLinkRadius(val, noSave) {
    var r = Math.round(kin3d_num(val, 6, 2, 24));
    kin3d_settings.linkRadius = r;
    if (!noSave) kin3d_saveSettings();
    for (var i = 0; i < kin3d_linkTubes.length; i++) {
        if (!kin3d_linkTubes[i] || !kin3d_linkTubes[i].geometry) continue;
        kin3d_linkTubes[i].geometry.dispose();
        kin3d_linkTubes[i].geometry = new THREE.CylinderGeometry(r, r, 1, 10);
    }
}

function kin3d_setBgColor(val) {
    var c = kin3d_parseColorHex(val, 0x302D2D);
    kin3d_settings.bgColor = c;
    kin3d_saveSettings();
    if (kin3d_scene) kin3d_scene.background = new THREE.Color(c);
}

function kin3d_applyMeshMaterialSettings(root) {
    if (!root || !root.traverse) return;
    var s = kin3d_settings;
    var metal = kin3d_num(s.modelMetalness, 0.30, 0.0, 1.0);
    var rough = kin3d_num(s.modelRoughness, 0.55, 0.0, 1.0);
    root.traverse(function(node) {
        if (!node || !node.isMesh || !node.material) return;
        var mats = Array.isArray(node.material) ? node.material : [node.material];
        for (var i = 0; i < mats.length; i++) {
            var m = mats[i];
            if (!m) continue;
            if ('wireframe' in m) m.wireframe = !!s.modelWireframe;
            if ('flatShading' in m) m.flatShading = !!s.modelFlatShading;
            if ('metalness' in m) m.metalness = metal;
            if ('roughness' in m) m.roughness = rough;
            m.needsUpdate = true;
        }

        var edge = node.userData ? node.userData.kin3dEdgeHelper : null;
        if (s.modelEdgeOutline) {
            if (!edge && node.geometry && typeof THREE.EdgesGeometry === 'function') {
                var edgeGeo = new THREE.EdgesGeometry(node.geometry, 35);
                var edgeMat = new THREE.LineBasicMaterial({ color: 0x101010, transparent: true, opacity: 0.68 });
                edge = new THREE.LineSegments(edgeGeo, edgeMat);
                edge.renderOrder = 4;
                edge.frustumCulled = false;
                edge.userData = edge.userData || {};
                edge.userData.kin3dForceNoFrustumCulling = true;
                node.add(edge);
                node.userData = node.userData || {};
                node.userData.kin3dEdgeHelper = edge;
            }
            if (edge) edge.visible = true;
        } else if (edge) {
            node.remove(edge);
            if (edge.geometry) edge.geometry.dispose();
            if (edge.material) edge.material.dispose();
            node.userData.kin3dEdgeHelper = null;
        }
    });
}

function kin3d_applyModelMaterialSettings() {
    for (var k in kin3d_cadRoots) kin3d_applyMeshMaterialSettings(kin3d_cadRoots[k]);
    for (var i = 0; i < kin3d_jointSpheres.length; i++) kin3d_applyMeshMaterialSettings(kin3d_jointSpheres[i]);
    for (var j = 0; j < kin3d_linkTubes.length; j++) kin3d_applyMeshMaterialSettings(kin3d_linkTubes[j]);
}

function kin3d_makeTrajectoryMaterial(colorHex, dashed) {
    var color = kin3d_parseColorHex(colorHex, 0xCC44CC);
    var width = kin3d_num(kin3d_settings.trajLineWidth, 1.0, 0.5, 6.0);
    if (dashed) return new THREE.LineDashedMaterial({ color: color, dashSize: 12, gapSize: 8, linewidth: width });
    return new THREE.LineBasicMaterial({ color: color, linewidth: width });
}

function kin3d_applyTrajectorySettings() {
    var s = kin3d_settings;
    var showTraj = (s.showTrajectory !== false);
    var color = kin3d_parseColorHex(s.trajColor, 0xCC44CC);
    var dashed = (s.trajDashed !== false);

    var lines = [traj_pathLine, traj_previewLine];
    for (var i = 0; i < lines.length; i++) {
        var ln = lines[i];
        if (!ln) continue;
        var mustDashed = dashed;
        var isDashed = ln.material && (ln.material.isLineDashedMaterial === true);
        if (isDashed !== mustDashed) {
            if (ln.material && ln.material.dispose) ln.material.dispose();
            ln.material = kin3d_makeTrajectoryMaterial(color, mustDashed);
            if (mustDashed && ln.computeLineDistances) ln.computeLineDistances();
        } else if (ln.material && ln.material.color) {
            ln.material.color.setHex(color);
        }
        ln.visible = showTraj && ln.visible;
    }
    if (traj_traceLine && traj_traceLine.material && traj_traceLine.material.color) {
        traj_traceLine.material.color.setHex(color);
        traj_traceLine.visible = showTraj;
    }
}

function kin3d_setTraceLength(v) {
    var limit = Math.round(kin3d_num(v, 500, 50, 6000));
    if (traj_maxTrace === limit) return;
    traj_maxTrace = limit;
    var keep = traj_tracePoints.slice(Math.max(0, traj_tracePoints.length - (traj_maxTrace * 3)));
    traj_tracePoints = keep;
    if (!traj_traceLine || !traj_traceLine.geometry) return;
    var geo = new THREE.BufferGeometry();
    var arr = new Float32Array(traj_maxTrace * 3);
    for (var i = 0; i < keep.length && i < arr.length; i++) arr[i] = keep[i];
    geo.setAttribute('position', new THREE.BufferAttribute(arr, 3));
    if (traj_traceLine.geometry) traj_traceLine.geometry.dispose();
    traj_traceLine.geometry = geo;
    traj_traceLine.geometry.setDrawRange(0, keep.length / 3);
}

function kin3d_applyGhostSettings() {
    var op = kin3d_num(kin3d_settings.ghostOpacity, 0.25, 0.05, 1.0);
    for (var i = 0; i < ghost_spheres.length; i++) {
        var m = ghost_spheres[i] ? ghost_spheres[i].material : null;
        if (!m) continue;
        m.transparent = (op < 0.999);
        m.opacity = op;
        m.needsUpdate = true;
    }
    for (var j = 0; j < ghost_tubes.length; j++) {
        var mt = ghost_tubes[j] ? ghost_tubes[j].material : null;
        if (!mt) continue;
        mt.transparent = (op < 0.999);
        mt.opacity = Math.max(0.05, op * 0.85);
        mt.needsUpdate = true;
    }
    for (var key in kin3d_cadGhostRoots) {
        var root = kin3d_cadGhostRoots[key];
        if (!root || !root.traverse) continue;
        root.traverse(function(node) {
            if (!node.isMesh || !node.material) return;
            var mats = Array.isArray(node.material) ? node.material : [node.material];
            for (var ii = 0; ii < mats.length; ii++) {
                var mm = mats[ii];
                if (!mm) continue;
                mm.transparent = true;
                mm.depthWrite = false;
                mm.opacity = op;
                mm.needsUpdate = true;
            }
        });
    }
}

function kin3d_refreshBboxHelpers() {
    var enabled = (kin3d_settings.measureShowBBox === true) && (kin3d_settings.useCAD !== false);
    for (var key in kin3d_bboxHelpers) {
        if (!enabled || !kin3d_cadRoots[key] || !kin3d_cadRoots[key].visible) {
            if (kin3d_bboxHelpers[key]) {
                kin3d_scene.remove(kin3d_bboxHelpers[key]);
                if (kin3d_bboxHelpers[key].geometry) kin3d_bboxHelpers[key].geometry.dispose();
                if (kin3d_bboxHelpers[key].material) kin3d_bboxHelpers[key].material.dispose();
                delete kin3d_bboxHelpers[key];
            }
            continue;
        }
        var helper = kin3d_bboxHelpers[key];
        if (!helper) {
            helper = new THREE.Box3Helper(new THREE.Box3(), 0xF6D365);
            kin3d_bboxHelpers[key] = helper;
            kin3d_scene.add(helper);
        }
        helper.box.setFromObject(kin3d_cadRoots[key]);
        helper.visible = true;
    }
}

function kin3d_applyPerformanceSettings() {
    if (!kin3d_renderer) return;
    var s = kin3d_settings;
    var cap = kin3d_num(s.perfPixelRatioCap, 1.5, 0.5, 3.0);
    var lod = String(s.perfLodLevel || 'high');
    var lodMul = 1.0;
    if (lod === 'medium') lodMul = 0.85;
    if (lod === 'low') lodMul = 0.70;
    var dpr = Math.max(0.5, Math.min((window.devicePixelRatio || 1), cap) * lodMul);
    kin3d_renderer.setPixelRatio(dpr);
    var c = document.getElementById('canvas-container');
    if (c) kin3d_renderer.setSize(c.clientWidth, c.clientHeight);
}

function kin3d_switchMainCamera(usePerspective) {
    var container = document.getElementById('canvas-container');
    if (!container) return;
    var prev = kin3d_camera;
    var aspect = Math.max(0.05, container.clientWidth / Math.max(1, container.clientHeight));
    var target = (kin3d_controls && kin3d_controls.target) ? kin3d_controls.target.clone() : new THREE.Vector3(0, 300, 0);
    var pos = prev ? prev.position.clone() : new THREE.Vector3(800, 600, 800);
    var up = prev ? prev.up.clone() : new THREE.Vector3(0, 1, 0);
    var fov = kin3d_num(kin3d_settings.cameraFov, 45, 20, 100);
    var near = kin3d_num(kin3d_settings.cameraNear, 1, 0.01, 500);
    var far = kin3d_num(kin3d_settings.cameraFar, 5000, 100, 60000);
    if (far <= near + 1) far = near + 1;

    if (usePerspective) {
        kin3d_camera = new THREE.PerspectiveCamera(fov, aspect, near, far);
    } else {
        var dist = Math.max(10, pos.distanceTo(target));
        var halfH = dist * Math.tan(THREE.MathUtils.degToRad(fov * 0.5));
        var halfW = halfH * aspect;
        kin3d_camera = new THREE.OrthographicCamera(-halfW, halfW, halfH, -halfH, near, far);
    }

    kin3d_camera.position.copy(pos);
    kin3d_camera.up.copy(up);
    kin3d_camera.lookAt(target);
    if (kin3d_controls) kin3d_controls.object = kin3d_camera;
}

function kin3d_applyCameraSettings() {
    var s = kin3d_settings;
    if (!kin3d_camera) return;

    var needPerspective = (s.perspective !== false);
    if ((needPerspective && !kin3d_camera.isPerspectiveCamera) || (!needPerspective && !kin3d_camera.isOrthographicCamera)) {
        kin3d_switchMainCamera(needPerspective);
    }

    var near = kin3d_num(s.cameraNear, 1, 0.01, 500);
    var far = kin3d_num(s.cameraFar, 5000, 100, 60000);
    if (far <= near + 1) far = near + 1;

    kin3d_camera.near = near;
    kin3d_camera.far = far;

    if (kin3d_camera.isPerspectiveCamera) {
        kin3d_camera.fov = kin3d_num(s.cameraFov, 45, 20, 100);
    } else if (kin3d_camera.isOrthographicCamera && kin3d_controls) {
        var c = document.getElementById('canvas-container');
        var aspect = c ? Math.max(0.05, c.clientWidth / Math.max(1, c.clientHeight)) : (16 / 9);
        var dist = Math.max(10, kin3d_camera.position.distanceTo(kin3d_controls.target));
        var halfH = dist * Math.tan(THREE.MathUtils.degToRad(kin3d_num(s.cameraFov, 45, 20, 100) * 0.5));
        var halfW = halfH * aspect;
        kin3d_camera.left = -halfW;
        kin3d_camera.right = halfW;
        kin3d_camera.top = halfH;
        kin3d_camera.bottom = -halfH;
    }
    kin3d_camera.updateProjectionMatrix();

    if (kin3d_controls) {
        kin3d_controls.enableDamping = true;
        kin3d_controls.dampingFactor = kin3d_num(s.cameraDamping, 0.08, 0.01, 0.35);
        kin3d_controls.autoRotate = !!s.cameraAutoRotate;
        kin3d_controls.autoRotateSpeed = kin3d_num(s.cameraAutoRotateSpeed, 1.0, 0.1, 12.0);
        kin3d_controls.enablePan = (s.cameraEnablePan !== false);
        kin3d_controls.minDistance = kin3d_num(s.cameraMinDistance, 100, 20, 10000);
        kin3d_controls.maxDistance = kin3d_num(s.cameraMaxDistance, 2600, kin3d_controls.minDistance + 1, 20000);
        kin3d_applyCameraMouseBindings();
    }
}

function kin3d_setNumericSetting(key, val, valueElId, fixed) {
    var prev = kin3d_settings[key];
    var num = Number(val);
    if (!isFinite(num)) return;
    kin3d_settings[key] = num;
    kin3d_saveSettings();
    if (valueElId) {
        var el = document.getElementById(valueElId);
        if (el) {
            var digits = (fixed === undefined || fixed === null) ? 2 : Math.max(0, parseInt(fixed, 10) || 0);
            el.textContent = num.toFixed(digits);
        }
    }
    if (prev !== num) kin3d_applySettings();
}

function kin3d_setColorSetting(key, val, valueElId) {
    var c = kin3d_parseColorHex(val, 0xFFFFFF);
    kin3d_settings[key] = c;
    kin3d_saveSettings();
    if (valueElId) {
        var el = document.getElementById(valueElId);
        if (el) el.textContent = kin3d_colorToInputHex(c, c);
    }
    kin3d_applySettings();
}

function kin3d_onSelectSettingChange(key, val) {
    kin3d_settings[key] = val;
    kin3d_saveSettings();
    kin3d_applySettings();
}

function kin3d_clearCadCache() {
    var done = function(msg) {
        if (window && typeof window.showToast === 'function') window.showToast(msg);
    };
    var complete = function() {
        try { localStorage.removeItem('mros_cad_version'); } catch (e) {}
        kin3d_cadRuntimeTag = String(Date.now());
        done('CAD cache temizlendi');
    };
    if (!window.caches || typeof caches.keys !== 'function') {
        complete();
        return;
    }
    caches.keys()
        .then(function(keys) {
            return Promise.all(keys.map(function(key) {
                return caches.open(key).then(function(cache) {
                    return cache.keys().then(function(reqs) {
                        var dels = [];
                        for (var i = 0; i < reqs.length; i++) {
                            if (String(reqs[i].url || '').indexOf('/cad/') >= 0) dels.push(cache.delete(reqs[i]));
                        }
                        return Promise.all(dels);
                    });
                });
            }));
        })
        .catch(function() {})
        .finally(complete);
}

function kin3d_resetSettings() {
    if (window && typeof window.mrosPopupSettingsSetSection === 'function') {
        window.mrosPopupSettingsSetSection('kin3d', null);
    }
    localStorage.removeItem('kin3d_settings');
    location.reload();
}

if (typeof window !== 'undefined') {
    window.kin3d_getMechanicsSnapshot = kin3d_getMechanicsSnapshot;
    window.kin3d_getMechanicalForceSnapshot = function() {
        return kin3d_mechanics.lastForceSnapshot;
    };
    window.kin3d_setActivePayload = kin3d_setActivePayload;
    window.kin3d_setActuatorTelemetry = kin3d_setActuatorTelemetry;
    window.kin3d_clearActuatorTelemetry = kin3d_clearActuatorTelemetry;
    window.kin3d_exportMaterialAssignmentsCsv = kin3d_exportMaterialAssignmentsCsv;
    window.kin3d_exportRobotRuntimeLoadsJson = kin3d_exportBearingLoadsJson;
    window.kin3d_downloadRobotRuntimeLoadsJson = kin3d_downloadRobotRuntimeLoadsJson;
    window.kin3d_exportBearingLoadsJson = kin3d_exportBearingLoadsJson;
}

document.addEventListener('DOMContentLoaded', function() {
    kin3d_init();
});
)rawliteral";
