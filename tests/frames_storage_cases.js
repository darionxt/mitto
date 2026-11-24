// What the documents of a page and its frames share: localStorage, sessionStorage and their events, BroadcastChannels and Web
// Locks, which go by a document's storage key, its origin with the site of the top-level document and whether a document from
// its own up to that one is of another site. The page is at /app/page?PORT, where PORT is a second server's, of another origin
// but the same site. The value is a promise for a record of sections, which tests/data/frames_storage_cases.json holds as
// Chrome gave it.
(async () => {
  const OTHER = `http://localhost:${location.port}`;
  const SITE = `http://127.0.0.1:${location.search.slice(1)}`;
  const plain = (value) => JSON.parse(JSON.stringify(value).replaceAll(OTHER, 'OTHER').replaceAll(SITE, 'SITE')
    .replaceAll(location.href, 'PAGE').replaceAll(location.origin, 'ORIGIN'));
  const settle = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
  const until = async (done) => {
    for (const end = performance.now() + 15000; !(await done()) && performance.now() < end;) await settle(10);
  };
  const loaded = (element) => new Promise((resolve) => element.addEventListener('load', resolve, {once: true}));
  const add = async (attributes) => {
    const element = document.createElement('iframe');
    const done = loaded(element);
    Object.assign(element, attributes);
    document.body.append(element);
    await done;
    return element;
  };
  const record = {};
  const section = async (name, run) => {
    try {
      record[name] = plain(await run());
    } catch (error) {
      record[name] = `threw ${error.name}: ${plain(error.message)}`;
    }
  };
  const child = await (await fetch('/child.js')).text();
  (0, eval)(child);
  const markup = `<script>${child}</script>`;

  const blank = document.createElement('iframe');
  document.body.append(blank);
  const script = blank.contentDocument.createElement('script');
  script.textContent = child;
  blank.contentDocument.body.append(script);
  const data = (text) => `data:text/html,${encodeURIComponent(text)}`;
  const inSrcdoc = `${markup}<iframe srcdoc="${markup.replaceAll('&', '&amp;').replaceAll('"', '&quot;')}"></iframe>`;
  const elements = {
    same: await add({src: '/child'}), blank, srcdoc: await add({srcdoc: markup}), data: await add({src: data(markup)}),
    data2: await add({src: data(markup)}), site: await add({src: `${SITE}/child`}), other: await add({src: `${OTHER}/child`}),
    other2: await add({src: `${OTHER}/child`}), same_same: await add({src: '/child?nest=/child'}),
    site_same: await add({src: `${SITE}/child?nest=${location.origin}/child`}),
    other_same: await add({src: `${OTHER}/child?nest=${location.origin}/child`}),
    other_other: await add({src: `${OTHER}/child?nest=/child`}), data_srcdoc: await add({src: data(inSrcdoc)}),
  };
  // Of a name with two parts, the frame within the frame of the first.
  const names = ['page', ...Object.keys(elements), 'data_top'];
  const answerOf = (name, message) => {
    if (name === 'page') return act(message);
    if (name === 'data_top') return ask(elements.data_srcdoc.contentWindow, message);
    const {contentWindow} = elements[name];
    return ask(contentWindow, name.includes('_') ? {inner: message} : message);
  };
  const all = async (message) => Object.fromEntries(await Promise.all(names.map(async (name) => [name, await answerOf(name, message)])));
  // Those that share a storage key, so that a case waits for what they hear.
  const shared = [['page', 'same', 'blank', 'srcdoc', 'same_same', 'site_same'], ['other', 'other2', 'other_other'], ['data_srcdoc', 'data_top']];
  const hearers = async (name, index, act) => {
    await all({act: 'heard'});
    const state = await act();
    const expected = (shared.find((group) => group.includes(name)) ?? [name]).length - 1;
    const counted = async () => Object.values(await all({act: 'count'})).filter((counts) => counts[index]).length;
    if (/^(set|posted)/.test(state)) await until(async () => await counted() >= expected);
    await settle(60);
    const heard = Object.entries(await all({act: 'heard'})).filter(([, lists]) => lists[index].length);
    const distinct = [...new Set(heard.map(([, lists]) => JSON.stringify(lists[index])))];
    return [state, heard.map(([name]) => name).join(' '), ...distinct.map((text) => JSON.parse(text))];
  };
  await section('listening', () => all({act: 'listen'}));
  await section('BroadcastChannel', async () => {
    const results = {};
    for (const name of names) results[name] = await hearers(name, 0, () => answerOf(name, {act: 'post', data: name}));
    return results;
  });
  for (const kind of ['localStorage', 'sessionStorage']) {
    await section(kind, async () => {
      const results = {};
      for (const name of names) results[name] = await hearers(name, 1, () => answerOf(name, {act: 'set', kind, key: 'k', value: name}));
      results.values = await all({act: 'get', kind, key: 'k'});
      results.same_value = await hearers('data', 1, () => answerOf('same', {act: 'set', kind, key: 'k', value: 'site_same'}));
      results.removed = await hearers('data', 1, async () => {
        const state = await answerOf('blank', {act: 'remove', kind, key: 'k'});
        await until(async () => (await answerOf('page', {act: 'count'}))[1]);
        return state;
      });
      results.cleared = await hearers('data', 1, async () => {
        await answerOf('blank', {act: 'set', kind, key: 'l', value: '1'});
        const state = await answerOf('srcdoc', {act: 'clear', kind});
        await until(async () => (await answerOf('page', {act: 'count'}))[1] >= 2);
        return state;
      });
      return results;
    });
  }
  await section('locks', async () => {
    // A navigator's locks are its window's, even where a document of another storage key was the first to read them.
    record.borrowed = await answerOf('other_same', {act: 'borrow'});
    await answerOf('same', {act: 'lock'});
    await until(async () => (await act({act: 'locks'})).length);
    return all({act: 'locks'});
  });
  for (const name in elements) elements[name].remove();

  // What a script still holds of a document that has gone.
  for (const way of ['removed', 'navigated']) {
    await section(`a document whose frame was ${way}`, async () => {
      const results = {};
      const told = (run) => {
        try {
          return String(run());
        } catch (error) {
          return `${error.name}: ${error.message}`;
        }
      };
      for (const kind of ['localStorage', 'sessionStorage']) {
        const element = await add({src: '/child'});
        const {contentWindow} = element;
        const storage = contentWindow[kind];
        storage.setItem('a', 'b');
        storage.k = 'v';
        if (way === 'removed') element.remove();
        else await Promise.all([loaded(element), element.src = '/child?next']);
        // Another window's change, whose event the gone one's would be.
        window[kind].setItem('z', '1');
        await settle(60);
        results[kind] = {
          length: told(() => storage.length), key: told(() => storage.key(0)), getItem: told(() => storage.getItem('a')),
          setItem: told(() => storage.setItem('a', 'c')), removeItem: told(() => storage.removeItem('a')), clear: told(() => storage.clear()),
          get: told(() => storage.k), set: told(() => { storage.k = 'w'; }), delete: told(() => delete storage.k), in: told(() => 'k' in storage),
          keys: told(() => Object.keys(storage)), own: told(() => Object.hasOwn(storage, 'a')),
          again: told(() => Object.getOwnPropertyDescriptor(window, kind).get.call(contentWindow) === storage),
          kept: window[kind].getItem('a') + window[kind].getItem('k'),
        };
        element.remove();
        window[kind].clear();
      }
      const element = await add({src: '/child'});
      const {contentWindow} = element;
      const {navigator: goneNavigator} = contentWindow;
      const held = new contentWindow.BroadcastChannel('gone');
      const channel = new BroadcastChannel('gone');
      const heard = [];
      held.onmessage = (event) => heard.push(`gone heard ${event.data}`);
      channel.onmessage = (event) => heard.push(`page heard ${event.data}`);
      channel.postMessage('1');
      await until(() => heard.length);
      if (way === 'removed') element.remove();
      else await Promise.all([loaded(element), element.src = '/child?next']);
      channel.postMessage('2');
      results.post = told(() => held.postMessage('3'));
      held.close();
      results.closed = told(() => held.postMessage('4'));
      const query = LockManager.prototype.query.call(goneNavigator.locks);
      results.locks = await Promise.race([query.then(() => 'answered', String), settle(2000).then(() => 'nothing')]);
      if (way === 'removed') results.made = told(() => new contentWindow.BroadcastChannel('gone').postMessage('5'));
      await settle(60);
      results.heard = heard;
      channel.close();
      element.remove();
      return results;
    });
  }

  // A window hears of changes once it listens, whichever way, or has reached its storage.
  await section('ways to listen', async () => {
    const results = {};
    const ways = {
      listener: (window, listener) => window.addEventListener('storage', listener),
      handler: (window, listener) => { window.onstorage = listener; },
      'body\'s handler': ({document}, listener) => { document.body.onstorage = listener; },
      'body\'s attribute': (window, listener) => { window.listener = listener; window.document.body.setAttribute('onstorage', 'listener(event)'); },
      'removed listener': (window, listener) => { window.addEventListener('storage', listener); window.removeEventListener('storage', listener); },
      'on the document': ({document}, listener) => document.addEventListener('storage', listener),
      none: () => {},
    };
    for (const way in ways) {
      const element = await add({src: '/child'});
      const last = await add({src: '/child'});
      const heard = [];
      let done = 0;
      last.contentWindow.addEventListener('storage', () => ++done);
      ways[way](element.contentWindow, (event) => heard.push([event.key, event.newValue, event.target === element.contentWindow, event.bubbles].join(' ')));
      for (const kind of ['localStorage', 'sessionStorage']) window[kind].setItem(kind, way);
      await until(() => done === 2);
      await settle(20);
      results[way] = heard.sort();
      element.remove();
      last.remove();
      for (const kind of ['localStorage', 'sessionStorage']) window[kind].clear();
    }
    // The window of a frame's initial about:blank, which its first document takes over.
    const element = document.createElement('iframe');
    document.body.append(element);
    const heard = [];
    // But for the clear() before, which its window may still hear of.
    element.contentWindow.addEventListener('storage', ({key}) => { if (key) heard.push(key); });
    localStorage.setItem('before', '1');
    await until(() => heard.length);
    await Promise.all([loaded(element), element.src = '/child']);
    localStorage.setItem('after', '1');
    await settle(100);
    results['an initial about:blank'] = heard;
    element.remove();
    localStorage.clear();
    return results;
  });
  await section('locks of frames that go', async () => {
    const order = [];
    const first = await add({src: '/child'});
    const second = document.createElement('iframe');
    document.body.append(second);
    let free;
    const pending = async (count) => until(async () => (await navigator.locks.query()).pending.length === count);
    navigator.locks.request('l', () => { order.push('page'); return new Promise((resolve) => { free = resolve; }); });
    await until(() => order.length);
    first.contentWindow.navigator.locks.request('l', () => { order.push('frame'); return new Promise(() => {}); });
    await pending(1);
    second.contentWindow.navigator.locks.request('l', () => { order.push('about:blank'); });
    await pending(2);
    free();
    await until(() => order.length === 2);
    first.remove();
    await until(() => order.length === 3);
    second.remove();
    return order;
  });
  return record;
})()
