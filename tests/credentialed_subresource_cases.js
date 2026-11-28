// What the subresources of a document and of its workers do whose URLs have a user or a password, which
// tests/test_page_credentialed_subresources.py compares with Chrome 157's. The document runs documentCases(), and each
// worker workerCases(), which know each case by its name in the URLs that it asks for.
const query = new URL(location.href).searchParams;
const OWN = location.host;
const OTHER = query.get('other');

const urlOf = (name, type, user = 'u:p@', host = OWN) => `http://${user}${host}/a?case=${name}&type=${type}`;
const redirectTo = (url, host = OWN) => `http://${host}/redirect?to=${encodeURIComponent(url)}`;
const wait = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));

const outcomes = {};

async function run(name, act) {
  try {
    outcomes[name] = await Promise.race([act(name), wait(10000).then(() => 'none')]);
  } catch (error) {
    outcomes[name] = error.name;
  }
}

// What the timeline has of each case: an entry's URL, with what stands for the hosts in place of them, and what it
// says of the response.
function entriesOfCases() {
  const entries = {};
  for (const entry of performance.getEntriesByType('resource')) {
    let name = entry.name;
    while (name.includes('%')) {
      name = decodeURIComponent(name);
    }
    const found = /[?&]case=([^&]*)/.exec(name);
    // Nothing reads an opaque response to its end, which its entry comes with.
    if (found && !found[1].includes('no-cors-redirect-from-other')) {
      (entries[found[1]] ||= []).push([
        name.replaceAll(OWN, 'OWN').replaceAll(OTHER, 'OTHER'), entry.initiatorType, entry.responseStatus,
        entry.transferSize, entry.encodedBodySize, entry.decodedBodySize, entry.nextHopProtocol, entry.contentType,
        entry.renderBlockingStatus, entry.redirectStart > 0, entry.requestStart > 0]);
    }
  }
  return entries;
}

async function describe(response) {
  const text = await response.text();
  const url = response.url.replaceAll(OWN, 'OWN').replaceAll(OTHER, 'OTHER');
  return `${response.type} ${response.status} ${response.redirected} ${url} ${text}`;
}

async function fetchCases(run, prefix) {
  for (const mode of ['cors', 'no-cors', 'same-origin']) {
    for (const credentials of ['same-origin', 'include', 'omit']) {
      await run(`${prefix}fetch-${mode}-${credentials}-redirect`, (name) =>
        fetch(redirectTo(urlOf(name, 'txt')), {mode, credentials}).then(describe));
    }
    await run(`${prefix}fetch-${mode}-redirect-other`, (name) =>
      fetch(redirectTo(urlOf(name, 'txt', 'u:p@', OTHER)), {mode}).then(describe));
    await run(`${prefix}fetch-${mode}-redirect-from-other`, (name) =>
      fetch(redirectTo(urlOf(name, 'txt'), OTHER), {mode}).then(describe));
    await run(`${prefix}fetch-${mode}-redirect-keepalive`, (name) =>
      fetch(redirectTo(urlOf(name, 'txt')), {mode, keepalive: true}).then(describe));
  }
  for (const user of ['x:y@', 'u@', ':p@', 'u:P@']) {
    await run(`${prefix}fetch-redirect-${user.replace('@', '').replace(':', '-')}`, (name) =>
      fetch(redirectTo(urlOf(name, 'txt', user))).then(describe));
  }
  await run(`${prefix}fetch-redirect-twice`, (name) =>
    fetch(redirectTo(redirectTo(urlOf(name, 'txt')))).then(describe));
  await run(`${prefix}fetch-redirect-back`, (name) =>
    fetch(redirectTo(redirectTo(urlOf(name, 'txt', '')).replace('//', '//u:p@'))).then(describe));
}

async function documentCases() {
  performance.setResourceTimingBufferSize(1000);
  const element = (tag, properties) => new Promise((resolve) => {
    const made = Object.assign(document.createElement(tag), properties);
    let when = ' at once';
    made.onload = () => resolve(`load${when}`);
    made.onerror = () => resolve(`error${when}`);
    document.body.append(made);
    queueMicrotask(() => { when = ''; });
  });
  const kinds = {
    // First, so that each beacon, which goes on its own once sendBeacon() has returned, has come before the
    // server is asked what came.
    'beacon': async (url) => `${navigator.sendBeacon(url)}`,
    'img': (src) => element('img', {src}),
    'img-anonymous': (src) => element('img', {crossOrigin: 'anonymous', src}),
    'img-use-credentials': (src) => element('img', {crossOrigin: 'use-credentials', src}),
    'script': (src) => element('script', {src}),
    'script-sync': (src) => element('script', {async: false, src}),
    'script-anonymous': (src) => element('script', {crossOrigin: 'anonymous', src}),
    'module': (src) => element('script', {type: 'module', src}),
    'module-use-credentials': (src) => element('script', {type: 'module', crossOrigin: 'use-credentials', src}),
    'static-import': (src) => element('script', {type: 'module', src: `http://${OWN}/module?to=${encodeURIComponent(src)}`}),
    'import': (src) => import(src).then((module) => module.url.replaceAll(OWN, 'OWN').replaceAll(OTHER, 'OTHER')),
    'stylesheet': (href) => element('link', {rel: 'stylesheet', href}),
    'stylesheet-anonymous': (href) => element('link', {rel: 'stylesheet', crossOrigin: 'anonymous', href}),
    'import-rule': (href) => element('link', {rel: 'stylesheet', href: `http://${OWN}/import?to=${encodeURIComponent(href)}`}),
    'preload-script': (href) => element('link', {rel: 'preload', as: 'script', href}),
    'preload-script-anonymous': (href) =>
      element('link', {rel: 'preload', as: 'script', crossOrigin: 'anonymous', href}),
    'preload-style': (href) => element('link', {rel: 'preload', as: 'style', href}),
    'preload-image': (href) => element('link', {rel: 'preload', as: 'image', href}),
    'preload-font': (href) => element('link', {rel: 'preload', as: 'font', crossOrigin: 'anonymous', href}),
    'preload-fetch': (href) => element('link', {rel: 'preload', as: 'fetch', href}),
    'preload-fetch-anonymous': (href) =>
      element('link', {rel: 'preload', as: 'fetch', crossOrigin: 'anonymous', href}),
    'modulepreload': (href) => element('link', {rel: 'modulepreload', href}),
    'prefetch': (href) => element('link', {rel: 'prefetch', href}),
  };
  const typeOf = (kind) =>
    kind.includes('img') || kind.includes('image') ? 'png'
      : /style|rule/.test(kind) ? 'css' : /module|import|script/.test(kind) ? 'js' : 'txt';
  for (const [kind, load] of Object.entries(kinds)) {
    const type = typeOf(kind);
    await run(kind, (name) => load(urlOf(name, type)));
    await run(`${kind}-other-user`, (name) => load(urlOf(name, type, 'x:y@')));
    await run(`${kind}-other-origin`, (name) => load(urlOf(name, type, 'u:p@', OTHER)));
    await run(`${kind}-redirect`, (name) => load(redirectTo(urlOf(name, type))));
    await run(`${kind}-redirect-other-user`, (name) => load(redirectTo(urlOf(name, type, 'x:y@'))));
    await run(`${kind}-redirect-other-origin`, (name) => load(redirectTo(urlOf(name, type, 'u:p@', OTHER))));
    await run(`${kind}-redirect-from-other-origin`, (name) => load(redirectTo(urlOf(name, type), OTHER)));
  }
  for (const user of ['u@', ':p@', 'u:P@']) {
    await run(`img-${user.replace('@', '').replace(':', '-')}`, (name) => kinds['img'](urlOf(name, 'png', user)));
  }
  // A plain URL that is asked for again with a user and a password, and the other way round.
  await run('img-again', async (name) => {
    const plain = await kinds['img'](urlOf(name, 'png', ''));
    return `${plain} ${await kinds['img'](urlOf(name, 'png'))}`;
  });
  await run('img-anonymous-again', async (name) => {
    const plain = await kinds['img-anonymous'](urlOf(name, 'png'));
    return `${plain} ${await kinds['img-anonymous'](urlOf(name, 'png', ''))}`;
  });
  await run('preload-then-script', async (name) => {
    const preload = await kinds['preload-script-anonymous'](urlOf(name, 'js', ''));
    return `${preload} ${await kinds['script-anonymous'](urlOf(name, 'js'))}`;
  });
  await run('import-again', async (name) => {
    const first = await import(urlOf(name, 'js', ''));
    const again = await import(urlOf(name, 'js'));
    return `${first === again} ${again.url.replaceAll(OWN, 'OWN')}`;
  });
  // Imports of a sheet of another origin, which a document may not read unless the sheet came in mode cors.
  for (const [name, user] of [['plain', ''], ['user', 'x:y@']]) {
    for (const crossOrigin of [null, 'anonymous']) {
      await run(`import-rule-of-other-origin-${name}-${crossOrigin}`, (case_name) => element('link', {
        rel: 'stylesheet', crossOrigin,
        href: `http://${OTHER}/import?to=${encodeURIComponent(urlOf(case_name, 'css', user))}`}));
    }
  }
  // The URLs that are left of one that goes without its user and password.
  const withoutHosts = (text) => decodeURIComponent(text).replaceAll(OWN, 'OWN').replaceAll(OTHER, 'OTHER');
  await run('img-anonymous-url', (name) => new Promise((resolve) => {
    const image = Object.assign(new Image(), {crossOrigin: 'anonymous', src: urlOf(name, 'png')});
    image.onload = () => resolve(withoutHosts(`${image.src} ${image.currentSrc}`));
  }));
  // An image that an access check failed has its source as it was given.
  for (const [name, source] of [
      ['img-url', urlOf('img-url', 'png', 'x:y@')],
      ['img-redirect-url', redirectTo(urlOf('img-redirect-url', 'png', 'x:y@'))],
      ['img-missing-url', `http://${OWN}/missing?case=img-missing-url&type=png`]]) {
    await run(name, () => new Promise((resolve) => {
      const image = Object.assign(new Image(), {src: source.replace('http:', '')});
      image.onerror = () => resolve(withoutHosts(`${image.src} ${image.currentSrc} ${image.complete}`));
    }));
  }
  // A source that its picture picks again loads again, but for one that an access check of its own URL failed.
  for (const [name, source, crossOrigin] of [
      ['picture-again', urlOf('picture-again', 'png', 'x:y@'), null],
      ['picture-redirect-again', redirectTo(urlOf('picture-redirect-again', 'png', 'x:y@')), null],
      ['picture-anonymous-redirect-again', redirectTo(urlOf('picture-anonymous-redirect-again', 'png', 'x:y@')),
        'anonymous'],
      ['picture-missing-again', `http://${OWN}/missing?case=picture-missing-again&type=png`, null]]) {
    await run(name, async () => {
      const picture = document.createElement('picture');
      const candidate = Object.assign(document.createElement('source'), {srcset: source});
      const image = Object.assign(new Image(), {crossOrigin});
      let errors = 0;
      const failed = () => new Promise((resolve) => {
        image.onerror = () => {
          ++errors;
          resolve();
        };
      });
      const first = failed();
      picture.append(candidate, image);
      document.body.append(picture);
      await first;
      const second = failed();
      candidate.media = 'all';
      await (name === 'picture-again' ? wait(500) : second);
      return `${errors} ${withoutHosts(image.currentSrc)}`;
    });
  }
  for (const [name, properties] of [['script-anonymous-url', {crossOrigin: 'anonymous'}], ['module-url', {type: 'module'}]]) {
    await run(name, () => new Promise((resolve) => {
      self.onerror = (message, file) => {
        self.onerror = null;
        resolve(withoutHosts(`${message} ${file}`));
        return true;
      };
      document.body.append(
        Object.assign(document.createElement('script'), {...properties, src: urlOf(name, 'throws')}));
    }));
  }
  await run('location', () => withoutHosts(`${location.href} ${location} ${document.URL} ${document.baseURI}`));
  // A frame's document with no URL of its own.
  for (const [name, properties] of [['about-blank', {}], ['srcdoc', {srcdoc: '<body></body>'}]]) {
    await run(`${name}-img`, async (case_name) => {
      const frame = Object.assign(document.createElement('iframe'), properties);
      const loaded = new Promise((resolve) => { frame.onload = resolve; });
      document.body.append(frame);
      await loaded;
      return new Promise((resolve) => {
        const image = frame.contentDocument.createElement('img');
        image.onload = () => resolve('load');
        image.onerror = () => resolve('error');
        image.src = urlOf(case_name, 'png');
        frame.contentDocument.body.append(image);
      });
    });
  }
  await fetchCases(run, '');
  if (self.caches) {
    const cache = await caches.open('credentials');
    await run('cache-add-redirect', (name) => cache.add(redirectTo(urlOf(name, 'txt'))).then(() => 'added'));
  }
  // What each worker says of its own cases.
  const worker = (url, options) => new Promise((resolve) => {
    const made = new Worker(url, options);
    made.onmessage = ({data}) => {
      made.terminate();
      resolve(data);
    };
    made.onerror = () => resolve('error');
  });
  const workers = {};
  for (const type of ['classic', 'module']) {
    // What a worker refuses follows its own URL, whatever its creator's, so one document starts them.
    for (const [name, user] of document.URL.includes('@') ? [] : [['plain', ''], ['user', 'u:p@']]) {
      const id = `${type}-${name}`;
      const path = `/worker?prefix=${id}-&other=${OTHER}`;
      workers[id] = await worker(`http://${user}${OWN}${path}`, {type});
      workers[`${id}-redirect`] = await worker(
        redirectTo(`http://${user}${OWN}${path.replace(id, `${id}-redirect`)}`), {type});
    }
    const url = async (...args) => withoutHosts(await worker(...args));
    await run(`${type}-worker`, (name) => url(urlOf(name, 'worker'), {type}));
    await run(`${type}-worker-other-user`, (name) => url(urlOf(name, 'worker', 'x:y@'), {type}));
    await run(`${type}-worker-redirect`, (name) => url(redirectTo(urlOf(name, 'worker')), {type}));
    await run(`${type}-worker-relative`, (name) => url(urlOf(name, 'worker').replace('/a?', '/relative?'), {type}));
  }
  await run('module-worker-static-import', (name) =>
    worker(`http://${OWN}/module?worker=1&to=${encodeURIComponent(urlOf(name, 'js'))}`, {type: 'module'}));
  await run('module-worker-static-import-redirect', (name) =>
    worker(`http://${OWN}/module?worker=1&to=${encodeURIComponent(redirectTo(urlOf(name, 'js')))}`, {type: 'module'}));
  await wait(200);
  return {outcomes, parsed, entries: entriesOfCases(), workers};
}

async function workerCases() {
  const prefix = query.get('prefix');
  await run(`${prefix}url`, () => location.href.replaceAll(OWN, 'OWN').replaceAll(OTHER, 'OTHER'));
  const sources = {
    'own': (name) => urlOf(name, 'js'),
    'other-user': (name) => urlOf(name, 'js', 'x:y@'),
    'other-origin': (name) => urlOf(name, 'js', 'u:p@', OTHER),
    'redirect': (name) => redirectTo(urlOf(name, 'js')),
    'redirect-other-user': (name) => redirectTo(urlOf(name, 'js', 'x:y@')),
    'redirect-other-origin': (name) => redirectTo(urlOf(name, 'js', 'u:p@', OTHER)),
  };
  for (const [source, urlOfCase] of Object.entries(sources)) {
    await run(`${prefix}importScripts-${source}`, (name) => {
      importScripts(urlOfCase(name));
      return 'ran';
    });
    await run(`${prefix}import-${source}`, (name) =>
      import(urlOfCase(name)).then((module) => module.url.replaceAll(OWN, 'OWN').replaceAll(OTHER, 'OTHER')));
  }
  await fetchCases(run, prefix);
  await wait(200);
  return {outcomes, entries: entriesOfCases()};
}

if (self.document) {
  self.cases = documentCases();
} else {
  workerCases().then((results) => postMessage(results));
}
