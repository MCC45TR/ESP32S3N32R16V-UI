(function(global) {
  var root = global.MROS = global.MROS || {};
  root.utils = root.utils || {};

  function fixedOrDash(value, digits, suffix) {
    var n = Number(value);
    if (!isFinite(n)) return '--' + (suffix || '');
    return n.toFixed(Number(digits) || 0) + (suffix || '');
  }

  function bytesHuman(bytes) {
    var value = Number(bytes);
    if (!isFinite(value) || value < 0) return '--';
    if (value >= 1048576) return (value / 1048576).toFixed(2) + ' MB';
    if (value >= 1024) return (value / 1024).toFixed(1) + ' KB';
    return String(Math.round(value)) + ' B';
  }

  function percentOrDash(value, digits) {
    return fixedOrDash(value, digits === undefined ? 1 : digits, ' %');
  }

  root.utils.format = {
    fixedOrDash: fixedOrDash,
    bytesHuman: bytesHuman,
    percentOrDash: percentOrDash
  };
})(window);
