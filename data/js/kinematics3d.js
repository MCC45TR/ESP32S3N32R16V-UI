(function(global) {
  var DOC = global.document;
  if (!DOC) return;
  if (DOC.querySelector('script[data-mros-robot-scene="1"]')) return;
  var script = DOC.createElement('script');
  script.async = false;
  script.src = '/js/scene/robot-scene.js?v=20260619_prr_process_r1';
  script.setAttribute('data-mros-robot-scene', '1');
  DOC.head.appendChild(script);
})(window);
