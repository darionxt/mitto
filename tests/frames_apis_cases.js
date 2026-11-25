// What the calls that a permissions policy, or a document's ancestors, decide give in the frames of a page of a secure context, whose
// /child runs tests/frames_apis_child.js, as /nest does, which holds the frame that its query names, /without, whose header takes
// each feature from it, /late, the same once /release is asked for, /self, whose header keeps high-entropy hints to its own origin, and
// /neither and /one, whose headers take both of the local networks and one. The value is a promise for a record by the frame, which tests/data/frames_apis_cases.json holds as Chrome gave it.
(async () => {
  const OTHER = `http://localhost:${location.port}`;
  const names = document.featurePolicy.features();
  const all = names.join(';'), every = names.map((name) => `${name} *`).join(';'), none = names.map((name) => `${name} 'none'`).join(';');
  const CHILD = `<script src="${location.origin}/child.js"><` + '/script>';
  const kinds = {
    'same': {src: '/child'}, 'same, none': {src: '/child', allow: none}, 'same, without': {src: '/without'}, 'same, self': {src: '/self'}, 'same, neither network': {src: '/neither'}, 'same, one network': {src: '/one'}, 'same, sandbox': {src: '/child', sandbox: 'allow-scripts'}, 'same, sandbox, all': {src: '/child', sandbox: 'allow-scripts', allow: all},
    'same, sandbox with its origin': {src: '/child', sandbox: 'allow-scripts allow-same-origin'}, 'other': {src: `${OTHER}/child`}, 'other, all': {src: `${OTHER}/child`, allow: all}, 'other, every': {src: `${OTHER}/child`, allow: every}, 'other, sandbox, every': {src: `${OTHER}/child`, sandbox: 'allow-scripts', allow: every},
    'other, without': {src: `${OTHER}/without`, allow: all}, 'srcdoc': {srcdoc: CHILD}, 'srcdoc, none': {srcdoc: CHILD, allow: none}, 'data': {src: `data:text/html,${encodeURIComponent(CHILD)}`}, 'data, every': {src: `data:text/html,${encodeURIComponent(CHILD)}`, allow: every},
    'same in other': {src: `${OTHER}/nest?${location.origin}/child`}, 'same in other, all': {src: `${OTHER}/nest?${location.origin}/child`, allow: all}, 'other in same': {src: `/nest?${OTHER}/child`}, 'other in same, all': {src: `/nest?${OTHER}/child`, allow: all}, 'third in other, all': {src: `${OTHER}/nest?${OTHER.replace('localhost', 'a.localhost')}/child`, allow: every}, 'same in same': {src: '/nest?/child'}, 'same in data': {src: `data:text/html,${encodeURIComponent(`<script>NEST = '${location.origin}/child';<` + `/script>${CHILD}`)}`},
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
  // Whose an object is.
  const FrameObject = frames['same, none'].contentWindow.Object;
  const of = (object) => (object instanceof Object ? 'the page' : object instanceof FrameObject ? 'the frame' : 'neither');
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
      let promise;
      try {
        promise = calls[name]();
        given[name] = await promise;
      } catch (error) {
        given[name] = [told(error), of(error)];
      }
      if (promise && promise.then) given[`${name}'s promise`] = of(promise);
    }
    return given;
  };
  const inner = frames['same, none'].contentWindow;
  record['the page on a frame'] = await borrow(window, inner);
  record['a frame on the page'] = await borrow(inner, window);
  // A document that takes its initial about:blank's window has its navigator and the statuses that it gave, which a later query of
  // the same kind brings up to date.
  const reused = document.createElement('iframe');
  reused.src = '/late';
  const loaded = new Promise((resolve) => { reused.onload = resolve; });
  document.body.append(reused);
  const {navigator: {permissions: early}} = reused.contentWindow;
  const first = await early.query({name: 'camera'}), states = [first.state];
  await fetch('/release');
  await loaded;
  // Chrome now and then never answers a query that the window makes while its document changes over in the browser.
  await new Promise((resolve) => setTimeout(resolve, 300));
  states.push(reused.contentWindow.navigator.permissions === early, first.state);
  const second = await early.query({name: 'camera'});
  record['a reused window'] = [...states, second.state, first.state, first === second];
  // An error goes to its own callback's realm, whichever the other callback is of.
  const doomed = document.createElement('iframe');
  document.body.append(doomed);
  const {Function: DoomedFunction} = doomed.contentWindow;
  const deny = (success) => new Promise((resolve) => inner.navigator.geolocation.getCurrentPosition(success, (error) => resolve([error.message, of(error)]), {timeout: 1}));
  const live = await deny(new DoomedFunction(''));
  const late = deny(new DoomedFunction(''));
  doomed.remove();
  record['callbacks of two realms'] = {live, late: await Promise.race([late, new Promise((resolve) => setTimeout(resolve, 300, 'never'))])};
  // A navigator whose frame has gone asks no policy of its gamepads, and its geolocation calls nothing back, even what it was to.
  const {navigator: gone, Notification: GoneNotification, Permissions, Keyboard, NavigatorUAData, CredentialsContainer} = inner, {geolocation} = gone;
  const kept = {permissions: gone.permissions, keyboard: gone.keyboard, userAgentData: gone.userAgentData, credentials: gone.credentials}, realm = {Permissions, Keyboard, NavigatorUAData, CredentialsContainer};
  const heard = [];
  const locate = (name) => geolocation.getCurrentPosition(() => heard.push(name), () => heard.push(name), {timeout: 1});
  locate('before');
  frames['same, none'].remove();
  locate('after');
  const watch = geolocation.watchPosition(() => heard.push('watch'), () => heard.push('watch'), {timeout: 1});
  await new Promise((resolve) => setTimeout(resolve, 300));
  record.gone = {gamepads: gone.getGamepads().length, heard, watch, notification: GoneNotification.permission};
  // Its promises reject at once, but for what waits for a task of its own, and an error is of the function's realm.
  for (const [name, {Permissions, Keyboard, NavigatorUAData, CredentialsContainer}] of [['the page on the gone', window], ['the gone', realm]]) {
    record[name] = {};
    const calls = {
      query: () => Permissions.prototype.query.call(kept.permissions, {name: 'camera'}),
      unknown: () => Permissions.prototype.query.call(kept.permissions, {name: 'unknown'}),
      keyboard: () => Keyboard.prototype.getLayoutMap.call(kept.keyboard),
      hints: () => NavigatorUAData.prototype.getHighEntropyValues.call(kept.userAgentData, ['model']),
      password: () => CredentialsContainer.prototype.get.call(kept.credentials, {password: true}),
      nothing: () => CredentialsContainer.prototype.get.call(kept.credentials, {}),
    };
    for (const call in calls) {
      record[name][call] = await Promise.race([calls[call]().then(() => 'resolved', (error) => [told(error), of(error)]), new Promise((resolve) => setTimeout(resolve, 300, 'pending'))]);
    }
  }
  return record;
})()
