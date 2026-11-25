// What a document of tests/frames_apis_cases.js says when its parent asks it: what it reads of each permission, then what the calls
// that a permissions policy decides give. A document of /nest has the frame that its parent asks instead.
(() => {
  const told = ({name, message}) => `${name}: ${message}`;
  const within = (promise, ms = 1500) => Promise.race([promise.then((value) => value, told), new Promise((resolve) => setTimeout(resolve, ms, 'pending'))]);
  const NAMES = ['geolocation', 'notifications', 'push', 'midi', 'camera', 'microphone', 'background-fetch', 'background-sync', 'persistent-storage', 'accelerometer', 'gyroscope', 'magnetometer', 'screen-wake-lock', 'display-capture', 'clipboard-read', 'clipboard-write',
    'payment-handler', 'idle-detection', 'periodic-background-sync', 'storage-access', 'window-management', 'local-fonts', 'top-level-storage-access', 'captured-surface-control', 'speaker-selection', 'keyboard-lock', 'pointer-lock', 'fullscreen', 'web-app-installation', 'local-network-access', 'local-network', 'loopback-network'];
  const DESCRIPTORS = [...NAMES.map((name) => ({name, userVisibleOnly: true})), {name: 'midi', sysex: true}, {name: 'camera', panTiltZoom: true}, {name: 'clipboard-write', allowWithoutSanitization: true}, {name: 'fullscreen', allowWithoutGesture: true}, {name: 'top-level-storage-access', requestedOrigin: 'https://a.example'}];
  const STEPS = {
    async state() {
      const permissions = {};
      for (const descriptor of DESCRIPTORS) permissions[JSON.stringify(descriptor)] = navigator.permissions ? await within(navigator.permissions.query(descriptor).then(({state}) => state)) : 'none';
      return {permissions, notification: typeof Notification === 'undefined' ? 'none' : Notification.permission, access: await within(document.hasStorageAccess())};
    },
    async calls() {
      const geolocation = navigator.geolocation ? await within(new Promise((resolve) => navigator.geolocation.getCurrentPosition(() => resolve('position'), ({code, message}) => resolve(code === 3 || message === 'User denied Geolocation' ? 'asked' : `${code} ${message}`), {timeout: 1}))) : 'none';
      const watch = navigator.geolocation ? await within(new Promise((resolve) => navigator.geolocation.watchPosition(() => resolve('position'), ({code, message}) => resolve(code === 3 || message === 'User denied Geolocation' ? 'asked' : `${code} ${message}`), {timeout: 1}))) : 'none';
      const keyboard = navigator.keyboard ? await within(navigator.keyboard.getLayoutMap().then((map) => map.constructor.name)) : 'none';
      const again = navigator.keyboard ? (() => { const one = navigator.keyboard.getLayoutMap(), two = navigator.keyboard.getLayoutMap(); one.catch(() => {}); return one === two; })() : 'none';
      const HINTS = ['architecture', 'bitness', 'model', 'platformVersion', 'uaFullVersion', 'wow64', 'fullVersionList', 'formFactors'];
      const hints = navigator.userAgentData ? Object.keys(await navigator.userAgentData.getHighEntropyValues(HINTS)) : 'none';
      const ASK = `(async () => {
        const said = [navigator.userAgentData ? Object.keys(await navigator.userAgentData.getHighEntropyValues(${JSON.stringify(HINTS)})) : 'none'];
        for (const name of ['microphone', 'camera', 'notifications', 'background-fetch', 'storage-access', 'accelerometer']) said.push(navigator.permissions ? (await navigator.permissions.query({name})).state : 'none');
        said.push(typeof Notification === 'undefined' ? 'none' : Notification.permission);
        return said;
      })()`;
      const work = (url) => within(new Promise((resolve) => {
        const worker = new Worker(url);
        worker.onmessage = ({data}) => resolve(data);
        worker.onerror = () => resolve('error');
      }));
      const worker = origin === 'null' ? 'opaque' : await work(URL.createObjectURL(new Blob([`${ASK}.then((said) => postMessage(said))`])));
      const opaque = await work(`data:text/javascript,${encodeURIComponent(`${ASK}.then((said) => postMessage(said))`)}`);
      let gamepads;
      try {
        gamepads = navigator.getGamepads().length;
      } catch (error) {
        gamepads = told(error);
      }
      const bytes = new Uint8Array(16);
      const credentials = {};
      const container = navigator.credentials;
      const allows = (feature) => document.featurePolicy.allowsFeature(feature);
      const attempts = !container ? {} : {
        password: () => container.get({password: true}),
        silent: () => container.get({password: true, mediation: 'silent'}),
        federated: () => container.get({federated: {providers: ['https://a.example']}}),
        conditional: () => container.get({password: true, mediation: 'conditional'}),
        publicKey: () => allows('publickey-credentials-get') ? 'asked' : container.get({publicKey: {challenge: bytes, rpId: 'a.example'}}),
        both: () => allows('publickey-credentials-get') ? 'asked' : container.get({password: true, publicKey: {challenge: bytes, rpId: 'a.example'}, mediation: 'conditional'}),
        otp: () => container.get({otp: {transport: []}}),
        identity: () => container.get({identity: {providers: []}}),
        identityOfOne: () => allows('identity-credentials-get') ? 'asked' : container.get({identity: {providers: [{configURL: 'https://a.example/', clientId: 'a'}]}}),
        identityOfTwo: () => credentials.password === null || credentials.password.startsWith('NotSupportedError') ? 'asked' : container.get({identity: {providers: [{configURL: 'https://a.example/', clientId: 'a'}, {configURL: 'https://b.example/', clientId: 'b'}]}}),
        digital: () => container.get({digital: {requests: []}}),
        nothing: () => container.get({}),
      };
      for (const name in attempts) credentials[name] = await within((async () => attempts[name]())());
      const request = typeof Notification === 'undefined' ? 'none' : Notification.permission === 'default' ? 'asked' : await within(Notification.requestPermission());
      return {geolocation, watch, keyboard, again, hints, worker, opaque, gamepads, credentials, request};
    },
  };
  globalThis.apis = (step) => STEPS[step]();
  if (parent === window) return;
  const inner = location.pathname === '/nest' ? document.createElement('iframe') : null;
  addEventListener('message', async ({source, data}) => {
    if (source === parent && data.step) {
      if (inner) postMessage.call(inner.contentWindow, data, '*');
      else postMessage.call(parent, {said: JSON.stringify(await STEPS[data.step]())}, '*');
    } else if (inner && source === inner.contentWindow) {
      postMessage.call(parent, data, '*');
    }
  });
  if (inner) {
    inner.allow = document.featurePolicy.features().join(';');
    inner.src = new URL(location.search.slice(1), location).href;
    document.documentElement.append(inner);
  } else {
    postMessage.call(parent, {ready: true}, '*');
  }
})()
