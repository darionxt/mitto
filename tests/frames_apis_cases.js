// What the calls that a permissions policy, or a document's ancestors, decide give in the frames of a page of a secure context, whose
// /child runs tests/frames_apis_child.js, as /nest does, which holds the frame that its query names, and /without, whose header takes
// each feature from it. The value is a promise for a record by the frame, which tests/data/frames_apis_cases.json holds as Chrome gave it.
(async () => {
  const OTHER = `http://localhost:${location.port}`;
  const names = document.featurePolicy.features();
  const all = names.join(';'), every = names.map((name) => `${name} *`).join(';'), none = names.map((name) => `${name} 'none'`).join(';');
  const CHILD = `<script src="${location.origin}/child.js"><` + '/script>';
  const kinds = {
    'same': {src: '/child'}, 'same, none': {src: '/child', allow: none}, 'same, without': {src: '/without'}, 'same, sandbox': {src: '/child', sandbox: 'allow-scripts'}, 'same, sandbox, all': {src: '/child', sandbox: 'allow-scripts', allow: all},
    'same, sandbox with its origin': {src: '/child', sandbox: 'allow-scripts allow-same-origin'}, 'other': {src: `${OTHER}/child`}, 'other, all': {src: `${OTHER}/child`, allow: all}, 'other, every': {src: `${OTHER}/child`, allow: every}, 'other, sandbox, every': {src: `${OTHER}/child`, sandbox: 'allow-scripts', allow: every},
    'other, without': {src: `${OTHER}/without`, allow: all}, 'srcdoc': {srcdoc: CHILD}, 'srcdoc, none': {srcdoc: CHILD, allow: none}, 'data': {src: `data:text/html,${encodeURIComponent(CHILD)}`}, 'data, every': {src: `data:text/html,${encodeURIComponent(CHILD)}`, allow: every},
    'same in other': {src: `${OTHER}/nest?${location.origin}/child`}, 'same in other, all': {src: `${OTHER}/nest?${location.origin}/child`, allow: all}, 'other in same': {src: `/nest?${OTHER}/child`}, 'other in same, all': {src: `/nest?${OTHER}/child`, allow: all}, 'third in other, all': {src: `${OTHER}/nest?${OTHER.replace('localhost', 'a.localhost')}/child`, allow: every}, 'same in same': {src: '/nest?/child'},
  };
  const frames = {};
  for (const kind in kinds) {
    const element = document.createElement('iframe');
    for (const name in kinds[kind]) element.setAttribute(name, kinds[kind][name]);
    const ready = new Promise((resolve) => addEventListener('message', function listener({source}) {
      if (source !== element.contentWindow) return;
      removeEventListener('message', listener);
      resolve();
    }));
    document.body.append(element);
    await Promise.race([ready, new Promise((resolve) => setTimeout(resolve, 5000))]);
    frames[kind] = element;
  }
  const ask = (element, step) => {
    const said = new Promise((resolve) => addEventListener('message', function listener({source, data}) {
      if (source !== element.contentWindow) return;
      removeEventListener('message', listener);
      resolve(JSON.parse(data.said));
    }));
    postMessage.call(element.contentWindow, {step}, '*');
    return Promise.race([said, new Promise((resolve) => setTimeout(resolve, 30000, 'never'))]);
  };
  // Only what differs from the page's own.
  const differing = (now, base) => {
    if (now === null || base === null || typeof now !== 'object' || typeof base !== 'object' || Array.isArray(now) || Array.isArray(base)) return now;
    const names = [...new Set([...Object.keys(now), ...Object.keys(base)])].filter((name) => JSON.stringify(now[name]) !== JSON.stringify(base[name]));
    return Object.fromEntries(names.map((name) => [name, now[name] === undefined ? 'nothing' : differing(now[name], base[name])]));
  };
  // Each frame's states before any frame's calls, some of which Chrome's headless answers then change.
  const page = {state: await apis('state')};
  const record = {page};
  for (const kind in frames) record[kind] = {state: differing(await ask(frames[kind], 'state'), page.state)};
  page.calls = await apis('calls');
  for (const kind in frames) record[kind].calls = differing(await ask(frames[kind], 'calls'), page.calls);
  // Each call goes by its receiver's document, whichever realm its function is of.
  const told = ({name, message}) => `${name}: ${message}`;
  const borrow = async ({Permissions, Geolocation, Keyboard, NavigatorUAData, Navigator, CredentialsContainer}, {navigator}) => {
    const calls = {
      query: () => Permissions.prototype.query.call(navigator.permissions, {name: 'camera'}).then(({state}) => state),
      background: () => Permissions.prototype.query.call(navigator.permissions, {name: 'background-fetch'}).then(({state}) => state),
      geolocation: () => new Promise((resolve) => Geolocation.prototype.getCurrentPosition.call(navigator.geolocation, () => resolve('position'), ({code, message}) => resolve(code === 3 || message === 'User denied Geolocation' ? 'asked' : `${code} ${message}`), {timeout: 1})),
      keyboard: () => Keyboard.prototype.getLayoutMap.call(navigator.keyboard).then((map) => map.constructor.name),
      hints: () => NavigatorUAData.prototype.getHighEntropyValues.call(navigator.userAgentData, ['model']).then((values) => Object.keys(values)),
      gamepads: () => Navigator.prototype.getGamepads.call(navigator).length,
      password: () => CredentialsContainer.prototype.get.call(navigator.credentials, {password: true}),
      otp: () => CredentialsContainer.prototype.get.call(navigator.credentials, {otp: {transport: []}}),
      digital: () => CredentialsContainer.prototype.get.call(navigator.credentials, {digital: {requests: []}}),
    };
    const given = {};
    for (const name in calls) {
      try {
        given[name] = await calls[name]();
      } catch (error) {
        given[name] = told(error);
      }
    }
    return given;
  };
  const inner = frames['same, none'].contentWindow;
  record['the page on a frame'] = await borrow(window, inner);
  record['a frame on the page'] = await borrow(inner, window);
  return record;
})()
