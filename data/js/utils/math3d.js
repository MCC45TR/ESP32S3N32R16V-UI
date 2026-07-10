(function(global) {
  var root = global.MROS = global.MROS || {};
  root.utils = root.utils || {};

  function clamp(value, minValue, maxValue) {
    var n = Number(value);
    if (!isFinite(n)) n = Number(minValue) || 0;
    if (isFinite(minValue) && n < minValue) return minValue;
    if (isFinite(maxValue) && n > maxValue) return maxValue;
    return n;
  }

  function degToRad(deg) {
    return (Number(deg) || 0) * Math.PI / 180;
  }

  function radToDeg(rad) {
    return (Number(rad) || 0) * 180 / Math.PI;
  }

  function finiteOr(value, fallback) {
    var n = Number(value);
    return isFinite(n) ? n : fallback;
  }

  root.utils.math3d = {
    clamp: clamp,
    degToRad: degToRad,
    radToDeg: radToDeg,
    finiteOr: finiteOr
  };
})(window);
