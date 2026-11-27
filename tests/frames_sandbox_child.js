// What a document of tests/frames_sandbox_cases.js says of itself to its parent, by its own postMessage().
(async () => {
  const key = new URLSearchParams(location.search).get('key') ?? window.name;
  const text = (run) => {
    try {
      const value = run();
      return value === null || typeof value !== 'object' ? String(value) : typeof value;
    } catch (error) {
      return `${error.name}: ${error.message}`;
    }
  };
  const later = async (run) => {
    try {
      const value = await Promise.race([run(), new Promise((resolve, reject) => setTimeout(reject, 5000, new Error('never')))]);
      return value === null || typeof value !== 'object' ? String(value) : typeof value;
    } catch (error) {
      return `${error.name}: ${error.message}`;
    }
  };
  const loading = (element, set) => new Promise((resolve) => {
    element.onload = () => resolve('load');
    element.onerror = () => resolve('error');
    set(element);
    document.documentElement.append(element);
  });
  const said = {
    origin: text(() => origin), location: text(() => location.origin), domain: text(() => document.domain), referrer: text(() => document.referrer),
    baseURI: text(() => document.baseURI.slice(0, 60)), secure: text(() => isSecureContext), frameElement: text(() => frameElement),
    cookie: text(() => document.cookie), setCookie: text(() => { document.cookie = 'set=1; path=/'; return document.cookie; }),
    localStorage: text(() => localStorage.length), sessionStorage: text(() => sessionStorage.length),
    open: await later(() => new Promise((resolve, reject) => {
      const request = indexedDB.open('sandbox');
      request.onsuccess = () => { request.result.close(); resolve('opened'); };
      request.onerror = () => reject(request.error);
    })),
    databases: await later(() => indexedDB.databases()), caches: await later(() => caches.keys()), locks: await later(() => navigator.locks.query()),
    cookieStore: await later(() => cookieStore.getAll()), storageAccess: await later(() => document.hasStorageAccess()), estimate: await later(() => navigator.storage.estimate().then(({quota}) => typeof quota)),
    persisted: await later(() => navigator.storage.persisted()), buckets: await later(() => navigator.storageBuckets.keys()),
    channel: text(() => { new BroadcastChannel('sandbox').close(); return 'made'; }), blob: text(() => URL.createObjectURL(new Blob(['x'])).slice(0, 10)),
    worker: await later(() => new Promise((resolve) => {
      const worker = new Worker(`/worker.js?worker&${encodeURIComponent(key)}`);
      worker.onmessage = () => resolve('ran');
      worker.onerror = () => resolve('error');
    })),
    fetch: await later(() => fetch(`/seen?fetch&${encodeURIComponent(key)}`).then(({type}) => type)),
    cors: await later(() => fetch(`/cors?cors&${encodeURIComponent(key)}`, {credentials: 'include'}).then(({type}) => type)),
    noCors: await later(() => fetch(`/seen?no-cors&${encodeURIComponent(key)}`, {mode: 'no-cors', credentials: 'include'}).then(({type}) => type)),
    sameOrigin: await later(() => fetch(`/seen?same-origin&${encodeURIComponent(key)}`, {mode: 'same-origin'}).then(({type}) => type)),
    image: await later(() => loading(new Image(), (image) => { image.src = `/seen.svg?image&${encodeURIComponent(key)}`; })),
    script: await later(() => loading(document.createElement('script'), (script) => { script.src = `/seen?script&${encodeURIComponent(key)}`; })),
    crossorigin: await later(() => loading(document.createElement('script'), (script) => { script.crossOrigin = ''; script.src = `/seen?crossorigin&${encodeURIComponent(key)}`; })),
    module: await later(() => import(`/cors?module&${encodeURIComponent(key)}`)),
    timing: text(() => performance.getEntriesByType('resource').filter(({name}) => /[?](image|script)&/.test(name))
      .map(({responseStart, transferSize, decodedBodySize}) => [responseStart > 0, transferSize > 0, decodedBodySize]).join(' ')),
    navigation: text(() => performance.getEntriesByType('navigation').map(({responseStart, decodedBodySize}) => [responseStart > 0, decodedBodySize > 0]).join(' ')),
    pushed: text(() => { history.replaceState(1, '', '#hash'); return 'replaced'; }),
  };
  postMessage.call(parent, {key, said}, '*');
})()
