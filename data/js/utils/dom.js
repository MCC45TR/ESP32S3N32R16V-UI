(function(global) {
  var root = global.MROS = global.MROS || {};
  root.utils = root.utils || {};

  function byId(id) {
    return document.getElementById(id);
  }

  function setText(id, text) {
    var el = byId(id);
    if (el) el.textContent = text;
    return el;
  }

  function setVisible(id, visible, displayValue) {
    var el = byId(id);
    if (!el) return null;
    if (visible) {
      el.style.removeProperty('display');
      if (displayValue) el.style.setProperty('display', displayValue);
    } else {
      el.style.setProperty('display', 'none', 'important');
    }
    return el;
  }

  root.utils.dom = {
    byId: byId,
    setText: setText,
    setVisible: setVisible
  };
})(window);
