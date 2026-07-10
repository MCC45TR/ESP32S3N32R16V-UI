(function(global) {
  var root = global.MROS = global.MROS || {};
  root.panels = root.panels || {};

  function mountMotionBlocksModal() {
    var modal = document.getElementById('motionBlocksModal');
    if (!modal || modal.getAttribute('data-mounted') === '1') return modal;
    modal.innerHTML = [
      '<div class="modal-content motion-blocks-modal">',
      '<span class="close-btn" onclick="closeMotionBlocksModal()">&times;</span>',
      '<div class="card-title">Hareket Planla (Blok Arayuz)</div>',
      '<p style="font-size:12px; color:#b7c0c8; margin-top:0; text-align:left;">',
      'Yeni baslayanlar icin bloklari sirayla ekleyip program akisi kurabilirsin. Onizle ile 3D yolu kontrol et, Kayit Et ile yerelde sakla.',
      '</p>',
      '<div class="motion-block-palette">',
      '<button class="sidebar-btn btn-doc" onclick="mpAddBlock(\'goto\')">Noktaya Git</button>',
      '<button class="sidebar-btn btn-neutral" onclick="mpAddBlock(\'line\')">Cizgisel Gecis</button>',
      '<button class="sidebar-btn btn-neutral" onclick="mpAddBlock(\'wait\')">Bekle</button>',
      '<button class="sidebar-btn btn-neutral" onclick="mpAddBlock(\'pen\')">Kalem Seviyesi</button>',
      '<button class="sidebar-btn btn-neutral" onclick="mpAddBlock(\'svg\')">SVG Yolu Ekle</button>',
      '</div>',
      '<div id="motion_blocks_canvas" class="motion-block-canvas"></div>',
      '<div class="motion-block-toolbar">',
      '<button class="sidebar-btn btn-out" onclick="clearMotionBlocks()">Temizle</button>',
      '<button class="sidebar-btn btn-doc" onclick="previewMotionBlocks()">Onizle</button>',
      '<button class="sidebar-btn btn-pri" onclick="saveMotionBlocks()">Kayit Et</button>',
      '</div>',
      '<div class="motion-block-toolbar" style="margin-top:8px;">',
      '<button class="sidebar-btn btn-neutral" onclick="applyMotionBlocks(false)">Kuyrugu Degistir</button>',
      '<button class="sidebar-btn btn-pri" onclick="applyMotionBlocks(true)">Kuyruga Ekle</button>',
      '</div>',
      '<div id="motion_blocks_status" style="margin-top:8px; font-size:12px; color:#9fb0bf; text-align:left;">Blok ekleyerek plan olustur.</div>',
      '</div>'
    ].join('');
    modal.setAttribute('data-mounted', '1');
    return modal;
  }

  root.panels.templates = root.panels.templates || {};
  root.panels.templates.mountMotionBlocksModal = mountMotionBlocksModal;
  global.mountMotionBlocksModal = mountMotionBlocksModal;
})(window);
