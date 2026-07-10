(function(global) {
  var DOC = global.document;
  if (!DOC) return;
  var VERSION = '20260510_ui_auth_wifi_r1';
  if (global.MROS && global.MROS.bootstrap && global.MROS.bootstrap.runtimeRequested) return;
  var root = global.MROS = global.MROS || {};
  root.bootstrap = root.bootstrap || {};
  root.bootstrap.runtimeRequested = true;
  function loadRuntime() {
    if (DOC.querySelector('script[data-mros-app-runtime="1"]')) return;
    var script = DOC.createElement('script');
    script.async = false;
    script.src = '/js/core/app-runtime.js?v=' + VERSION;
    script.setAttribute('data-mros-app-runtime', '1');
    DOC.head.appendChild(script);
  }
  loadRuntime();
})(window);
