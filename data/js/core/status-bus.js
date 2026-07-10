(function(global) {
  var root = global.MROS = global.MROS || {};
  root.core = root.core || {};

  var handlers = {};

  function on(topic, handler) {
    if (!topic || typeof handler !== 'function') return function() {};
    handlers[topic] = handlers[topic] || [];
    handlers[topic].push(handler);
    return function() {
      off(topic, handler);
    };
  }

  function off(topic, handler) {
    if (!handlers[topic]) return;
    handlers[topic] = handlers[topic].filter(function(fn) {
      return fn !== handler;
    });
  }

  function emit(topic, payload) {
    (handlers[topic] || []).slice().forEach(function(fn) {
      try { fn(payload); } catch (err) {}
    });
  }

  root.core.statusBus = {
    on: on,
    off: off,
    emit: emit
  };
})(window);
