(function(global) {
  var root = global.MROS = global.MROS || {};
  root.ik = root.ik || {};

  function pushUniqueSeed(seeds, seed) {
    if (typeof global.plannerPushUniqueSeed === 'function') return global.plannerPushUniqueSeed(seeds, seed);
    return Array.isArray(seeds) ? seeds : [];
  }

  function variantsForTarget(target, seedAnglesDeg, options) {
    return (typeof global.plannerSeedVariantsForTarget === 'function')
      ? global.plannerSeedVariantsForTarget(target, seedAnglesDeg, options)
      : [];
  }

  function transitionCost(prevAnglesDeg, candidate, options) {
    return (typeof global.plannerTransitionCost === 'function')
      ? global.plannerTransitionCost(prevAnglesDeg, candidate, options)
      : Number.POSITIVE_INFINITY;
  }

  root.ik.seedStrategy = {
    pushUniqueSeed: pushUniqueSeed,
    variantsForTarget: variantsForTarget,
    transitionCost: transitionCost
  };
})(window);
