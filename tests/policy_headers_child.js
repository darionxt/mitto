// What a document of tests/policy_headers_cases.js says of its permissions policy, and with ?with of the frames that it makes, to its
// parent, by its own postMessage().
(async () => {
  const OTHER = `http://localhost:${location.port}`;
  const names = ['camera', 'gamepad', 'geolocation', 'fullscreen', 'microphone', 'usb', 'cross-origin-isolated', 'local-network-access', 'local-network', 'loopback-network'];
  const of = (policy) => Object.fromEntries(names.map((name) => [name, [policy.allowsFeature(name), policy.allowsFeature(name, OTHER), policy.allowsFeature(name, 'https://a.example'),
    policy.allowsFeature(name, 'https://sub.b.example'), policy.allowsFeature(name, 'http://c.example:8080'), policy.getAllowlistForFeature(name)]]));
  const policy = document.featurePolicy;
  const say = {names: of(policy), count: policy.allowedFeatures().filter((name) => name !== 'unload').length, fullscreen: document.fullscreenEnabled, pip: document.pictureInPictureEnabled};
  if (location.pathname === '/with') {
    say.frames = {};
    for (const [kind, src, allow] of [['same', '/child', null], ['other', `${OTHER}/child`, null], ['other, allowed', `${OTHER}/child`, `camera; gamepad 'none'; geolocation *; usb`], ['srcdoc', null, 'camera']]) {
      const element = document.createElement('iframe');
      if (allow !== null) element.allow = allow;
      if (src === null) element.srcdoc = '<script src="/child.js"><' + '/script>';
      else element.src = src;
      const heard = new Promise((resolve) => addEventListener('message', function listener({source, data}) {
        if (source !== element.contentWindow) return;
        removeEventListener('message', listener);
        resolve(data);
      }));
      document.documentElement.append(element);
      const {names: inherited, count} = await heard;
      say.frames[kind] = [Object.fromEntries(Object.entries(inherited).map(([name, [own, other, , , , list]]) => [name, [own, other, list]])), count,
        Object.fromEntries(names.map((name) => [name, [element.featurePolicy.allowsFeature(name), element.featurePolicy.getAllowlistForFeature(name)]]))];
      element.remove();
    }
  }
  if (location.pathname === '/with' && JSON.parse(decodeURIComponent(location.search.slice(1))).unload) {
    const listens = (view) => {
      let ran = false;
      view.addEventListener('unload', () => { ran = true; });
      view.dispatchEvent(new view.Event('unload'));
      return ran;
    };
    const unload = [policy.allowsFeature('unload'), listens(window), policy.getAllowlistForFeature('unload')];
    for (const allow of [null, 'unload', `unload 'none'`, 'unload *', `unload 'self'`]) {
      for (const [kind, attributes] of Object.entries({same: {src: '/child'}, 'same of its own': {src: `/with?${encodeURIComponent(JSON.stringify({pp: ['unload=*']}))}`}, srcdoc: {srcdoc: 'x'}, blank: {}, data: {src: 'data:text/html,x'}})) {
        const element = document.createElement('iframe');
        if (allow !== null) element.allow = allow;
        for (const name in attributes) element.setAttribute(name, attributes[name]);
        const done = new Promise((resolve) => { element.onload = resolve; });
        document.documentElement.append(element);
        await done;
        const inner = element.contentDocument;
        unload.push([allow, kind, element.featurePolicy.allowsFeature('unload'), inner && [inner.featurePolicy.allowsFeature('unload'), listens(element.contentWindow), inner.featurePolicy.getAllowlistForFeature('unload')]]);
        element.remove();
      }
    }
    postMessage.call(parent, unload, '*');
    return;
  }
  postMessage.call(parent, say, '*');
})()
