(function(global) {
  var root = global.MROS = global.MROS || {};
  root.bootstrap = root.bootstrap || {};

  root.bootstrap.onDomReady = function(callback) {
    if (typeof callback !== 'function') return;
    if (document.readyState === 'loading') {
      document.addEventListener('DOMContentLoaded', callback, { once: true });
      return;
    }
    callback();
  };
})(window);
