(function(global) {
  var DOC = global.document;
  if (!DOC) return;
  if (DOC.querySelector('script[data-mros-app-entry="1"]')) return;
  var script = DOC.createElement('script');
  script.async = false;
  script.src = '/js/bootstrap/app-entry.js?v=20260422_transport_r1';
  script.setAttribute('data-mros-app-entry', '1');
  DOC.head.appendChild(script);
})(window);
