// What a document of tests/frames_policy_cases.js says of its permissions policy to its parent, by its own postMessage().
(() => {
  const policy = document.featurePolicy;
  // All but unload, which the Persona's variations seed may take away.
  const allowed = policy.allowedFeatures().filter((name) => name !== 'unload');
  const lists = {};
  for (const name of ['camera', 'fullscreen', 'gamepad', 'geolocation']) lists[name] = policy.getAllowlistForFeature(name);
  const others = ['camera', 'gamepad', 'fullscreen'].map((name) => [policy.allowsFeature(name), policy.allowsFeature(name, location.origin === 'null' ? 'https://example.com' : location.origin),
    policy.allowsFeature(name, 'https://example.com')]);
  postMessage.call(parent, {allowed, features: policy.features().length, lists, others, fullscreen: document.fullscreenEnabled, pictureInPicture: document.pictureInPictureEnabled}, '*');
})()
