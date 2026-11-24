// What window.postMessage() gives between a page and its frames: who sent a message, from which origin, whom it is for, and
// what it carries. The page is at /app/page?PORT, where PORT is a second server's, of another origin but the same site; a
// frame's document of /child?... does what its query says. A frame of another origin calls its own postMessage() on its
// parent's window, since a Page's windows of another origin give no property yet.
// The value is a promise for a record of sections, which tests/data/frames_messages_cases.json holds as Chrome gave it.
(async () => {
  const OTHER = `http://localhost:${location.port}`;
  const THIRD = `http://third.localhost:${location.port}`;
  const SAME_SITE = `http://127.0.0.1:${location.search.slice(1)}`;
  const settle = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
  const loaded = (element) => new Promise((resolve) => element.addEventListener('load', resolve, {once: true}));
  const add = async (src) => {
    const element = document.createElement('iframe');
    if (src) element.src = src;
    document.body.appendChild(element);
    if (src) await loaded(element);
    return element;
  };
  // The messages that come to the page while `run` runs and 150 ms after.
  const heard = async (run) => {
    const events = [];
    const listener = (event) => events.push(event);
    addEventListener('message', listener);
    addEventListener('messageerror', listener);
    await run();
    await settle(150);
    removeEventListener('message', listener);
    removeEventListener('messageerror', listener);
    return events;
  };
  const plain = (text) => String(text).replaceAll(OTHER, 'OTHER').replaceAll(location.origin, 'ORIGIN');
  const named = (source, element) =>
    source === null ? 'null' : source === window ? 'page' : element && source === element.contentWindow ? 'frame' : 'other';
  const told = (event, element) => [
    event.type, event.data, plain(event.origin), named(event.source, element), event.isTrusted, event.ports.length,
    event.lastEventId, event.bubbles, event.cancelable,
  ];
  const record = {};
  const section = async (name, run) => {
    try {
      record[name] = await run();
    } catch (error) {
      record[name] = `threw ${error.name}: ${plain(error.message)}`;
    }
  };

  // A message names the window that sent it, and that window's origin.
  await section('from a frame', async () => {
    const results = {};
    for (const [name, src] of [['same', '/child?send'], ['other', `${OTHER}/child?send`]]) {
      let element;
      const events = await heard(async () => { element = await add(src); });
      results[name] = events.map((event) => told(event, element));
      results[`${name} data`] = events.map((event) => event.data && event.data instanceof Object);
      element.remove();
    }
    const opaque = document.createElement('iframe');
    opaque.src = `data:text/html,<script>postMessage.call(parent, 'data:', '*')</script>`;
    results.opaque = (await heard(() => { document.body.appendChild(opaque); })).map((event) => told(event, opaque));
    opaque.remove();
    return results;
  });

  // A frame's answer says what it heard of the page's message.
  await section('to a frame', async () => {
    const results = {};
    for (const [name, src] of [['same', '/child?answer'], ['other', `${OTHER}/child?answer`]]) {
      const element = await add(src);
      const w = element.contentWindow;
      const targets = {
        any: '*', own: '/', page: location.origin, frame: name === 'same' ? location.origin : OTHER,
        path: (name === 'same' ? location.origin : OTHER) + '/some/path?query#fragment', elsewhere: 'https://example.com',
      };
      results[name] = {};
      for (const target in targets) {
        const events = await heard(() => postMessage.call(w, {target}, targets[target]));
        results[name][target] = events.map((event) => event.data);
      }
      results[name].options = (await heard(() => postMessage.call(w, 'o', {targetOrigin: '*'}))).map((event) => event.data);
      results[name].none = (await heard(() => postMessage.call(w, 'n'))).map((event) => event.data);
      element.remove();
    }
    const thrown = [];
    const element = await add('/child?answer');
    for (const target of ['nowhere', 'about:blank', 'data:text/html,x', '']) {
      try {
        element.contentWindow.postMessage('x', target);
        thrown.push('sent');
      } catch (error) {
        thrown.push(`${error.name}: ${error.message}`);
      }
    }
    element.remove();
    return {...results, thrown};
  });

  // The sender is the incumbent window, whose script made the call, whichever window's function it calls.
  await section('incumbent', async () => {
    const element = await add('/child');
    const w = element.contentWindow;
    window.sendFromPage = (data) => window.postMessage(data, '*');
    window.sendToFrame = (data) => w.postMessage(data, '*');
    const results = {};
    const ways = {
      'the frame calls its parent': () => w.eval(`parent.postMessage('a', '*')`),
      'the frame applies': () => w.eval(`Reflect.apply(postMessage, parent, ['b', '*'])`),
      'the frame applies its parent\'s': () => w.eval(`parent.postMessage.call(parent, 'c', '*')`),
      'the frame calls the page\'s function': () => w.eval(`parent.sendFromPage('d')`),
      'the page calls the frame\'s method': () => w.postMessage.call(window, 'e', '*'),
      'the page calls the frame\'s function': () => w.eval(`(data) => parent.postMessage(data, '*')`)('f'),
      'a bound method in the frame\'s timer': () => w.eval(`setTimeout(parent.postMessage.bind(parent, 'g', '*'))`),
      'a bound method in the frame\'s microtask': () => w.eval(`Promise.resolve().then(parent.postMessage.bind(parent, 'h', '*'))`),
      'itself': () => window.postMessage('j', '*'),
    };
    for (const way in ways) results[way] = (await heard(ways[way])).map((event) => [event.data, named(event.source, element)]);
    w.eval(`onmessage = (event) => parent.heardIn = [event.data, event.source === parent, event.source === window]`);
    for (const [way, run] of [['to the frame', () => w.postMessage('k', '*')], ['the frame to itself', () => w.eval(`postMessage('l', '*')`)],
                              ['the frame calls the page\'s function to it', () => w.eval(`parent.sendToFrame('m')`)]]) {
      window.heardIn = null;
      run();
      await settle(80);
      results[way] = window.heardIn;
    }
    element.remove();
    return results;
  });

  // What a message's source is once its frame shows another document or has gone, and where a message then goes.
  await section('a sender that goes', async () => {
    const results = {};
    for (const way of ['removed', 'navigated', 'removed later']) {
      const element = await add('/child');
      const proxy = element.contentWindow;
      let kept = null;
      const events = await heard(async () => {
        proxy.eval(`parent.postMessage('x', '*')`);
        if (way === 'removed') element.remove();
        if (way === 'navigated') {
          element.src = '/child?next';
          await loaded(element);
        }
        if (way === 'removed later') {
          await settle(50);
          element.remove();
        }
      });
      kept = events[0];
      results[way] = [events.length, kept && kept.source === null, kept && kept.source === proxy, kept && plain(kept.origin)];
      element.remove();
    }
    const element = await add('/child');
    const w = element.contentWindow;
    window.got = [];
    w.eval(`onmessage = (event) => top.got.push(event.data)`);
    w.postMessage('to one that goes', '*');
    element.remove();
    await settle(80);
    results['to a frame that goes'] = window.got.slice();
    const again = await add('/child');
    again.contentWindow.eval(`onmessage = (event) => top.got.push(event.data)`);
    again.contentWindow.postMessage('to a document that goes', '*');
    again.src = '/child?collect';
    await loaded(again);
    await settle(80);
    results['to a document that goes'] = window.got;
    again.remove();
    return results;
  });

  // What a message carries: clones of the recipient's realm, ports and buffers, and a module that another origin may not have.
  await section('carried', async () => {
    const element = await add('/child');
    const w = element.contentWindow;
    const results = {};
    w.eval(`onmessage = (event) => {
      const {data} = event;
      top.seen = [data instanceof Object, data.map instanceof Map, data.date instanceof Date, data.error instanceof Error,
                  data.list && Array.isArray(data.list) && data.list instanceof Array, data.buffer instanceof ArrayBuffer,
                  data.buffer && data.buffer.byteLength, event.ports.length, event.ports[0] instanceof MessagePort,
                  event.ports === event.ports, Object.isFrozen(event.ports)];
      if (event.ports[0]) event.ports[0].postMessage('through the port');
    }`);
    const channel = new MessageChannel();
    const buffer = new ArrayBuffer(8);
    const answered = new Promise((resolve) => { channel.port1.onmessage = (event) => resolve([event.data, event.source, event.origin]); });
    w.postMessage({map: new Map(), date: new Date(0), error: new Error('e'), list: [1], buffer}, '*', [channel.port2, buffer]);
    results.port = await answered;
    results.seen = window.seen;
    results.detached = buffer.byteLength;
    element.remove();

    const module = new WebAssembly.Module(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
    for (const [name, src] of [['same', '/child?modules'], ['other', `${OTHER}/child?modules`]]) {
      const frame = await add(src);
      results[`module ${name}`] = (await heard(() => postMessage.call(frame.contentWindow, module, '*'))).map((event) => event.data);
      frame.remove();
    }
    let uncloneable;
    const frame = await add('/child');
    try {
      frame.contentWindow.postMessage(() => 1, '*');
    } catch (error) {
      uncloneable = [error.name, error instanceof DOMException];
    }
    frame.remove();
    return {...results, uncloneable};
  });

  // Messages keep their order, within a window and with its other tasks, and a message comes in a task of its own.
  await section('order', async () => {
    const element = await add('/child');
    const w = element.contentWindow;
    const order = [];
    addEventListener('message', (event) => order.push(event.data), {signal: AbortSignal.timeout(200)});
    postMessage('1', '*');
    w.eval(`parent.postMessage('2', '*')`);
    postMessage('3', '*');
    Promise.resolve().then(() => order.push('microtask'));
    order.push('task');
    await settle(100);
    element.remove();
    return order.slice();
  });

  // Chrome has a frame of another site in another process. The page's window of such a frame stays the frame's from document
  // to document, until one of the page's site comes, where any other frame's window is its document's.
  const CHILD_OF = {'same origin': '/child', 'same site': `${SAME_SITE}/child`, 'another site': `${OTHER}/child`};
  const DATA = `data:text/html,<script>postMessage.call(parent, 'hello', '*')</script>`;
  const load = (element, src) => {
    const done = loaded(element);
    element.src = src;
    return done;
  };
  const hello = async (src) => {
    const element = document.createElement('iframe');
    const said = new Promise((resolve) => addEventListener('message', function listener(event) {
      if (event.data !== 'hello') return;
      removeEventListener('message', listener);
      resolve(event);
    }));
    await Promise.all([(() => { const done = loaded(element); element.src = src; document.body.appendChild(element); return done; })(), said]);
    return [element, await said];
  };
  await section('a source whose frame goes', async () => {
    const results = {};
    const ways = {
      'its own origin': ['own'], 'the page\'s origin': ['/child'], 'the page\'s origin and back': ['/child', 'own'],
      'the page\'s site': [`${SAME_SITE}/child`], 'another site': [`${OTHER}/child`], 'a third site and back': [`${THIRD}/child`, 'own'],
      'a data: URL': ['data:text/html,x'], 'about:blank': ['about:blank'], 'removed': ['removed'], 'a third site, removed': [`${THIRD}/child`, 'removed'],
    };
    for (const [kind, src] of [...Object.entries(CHILD_OF).map(([kind, url]) => [kind, `${url}?hello`]), ['data:', DATA]]) {
      results[kind] = {};
      for (const way in ways) {
        const [element, event] = await hello(src);
        const proxy = element.contentWindow;
        const seen = [named(event.source, element)];
        for (const step of ways[way]) {
          if (step === 'removed') element.remove();
          else await load(element, step !== 'own' ? step : src === DATA ? 'data:text/html,y' : src.replace('?hello', '?own'));
          seen.push(event.source === null ? 'null' : event.source === proxy ? 'frame' : 'other');
        }
        results[kind][way] = seen.join(' ');
        element.remove();
      }
    }
    return results;
  });
  await section('a frame that goes while its messages wait', async () => {
    const results = {};
    for (const kind in CHILD_OF) {
      results[kind] = {};
      for (const way of ['removed', 'navigated by the page', 'navigated by itself']) {
        const element = document.createElement('iframe');
        const events = [];
        let proxy;
        const listener = (event) => {
          if (!/^number/.test(event.data)) return;
          proxy ??= element.contentWindow;
          const source = event.source === null ? 'null' : event.source === proxy ? 'frame' : 'other';
          events.push(`${event.data} ${source}`);
          if (events.length === 1 && way === 'removed') element.remove();
          if (events.length === 1 && way === 'navigated by the page') element.src = `${CHILD_OF[kind]}?next`;
        };
        addEventListener('message', listener);
        element.src = `${CHILD_OF[kind]}?${way === 'navigated by itself' ? 'threeAndGo' : 'three'}`;
        document.body.appendChild(element);
        await settle(250);
        removeEventListener('message', listener);
        element.remove();
        results[kind][way] = events;
      }
    }
    return results;
  });
  await section('an event that a page makes', async () => {
    const results = {};
    for (const kind in CHILD_OF) {
      const element = await add(CHILD_OF[kind]);
      const made = new MessageEvent('x', {source: element.contentWindow});
      const initialized = document.createEvent('MessageEvent');
      initialized.initMessageEvent('x', false, false, null, '', '', element.contentWindow, []);
      const seen = () => [made, initialized].map((event) => named(event.source, element)).join(' ');
      results[kind] = [seen()];
      for (const url of [`${CHILD_OF[kind]}?next`, '/child?page', `${CHILD_OF[kind]}?back`]) {
        await load(element, url);
        results[kind].push(seen());
      }
      element.remove();
    }
    return results;
  });

  // A port's messages are checked where they come, whichever context the port was in when they were sent.
  await section('through a port', async () => {
    const results = {};
    const bytes = new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]);
    const module = new WebAssembly.Module(bytes);
    const kinds = {...CHILD_OF, 'data:': `data:text/html,<script>(${portOf})()</script>`};
    for (const kind in kinds) {
      const element = await add(kinds[kind].startsWith('data:') ? kinds[kind].replaceAll('%', '%25').replaceAll('#', '%23') : `${kinds[kind]}?port`);
      const channel = new MessageChannel();
      const back = [];
      for (const type of ['message', 'messageerror']) {
        channel.port1.addEventListener(type, (event) => {
          back.push([event.type, event.data instanceof WebAssembly.Module ? 'module' : event.data, event.origin, event.source, event.ports.length]);
        });
      }
      results[kind] = [];
      const events = await heard(async () => {
        channel.port1.postMessage(module);
        channel.port1.postMessage('before');
        channel.port1.postMessage({module}, [new MessageChannel().port1]);
        postMessage.call(element.contentWindow, 'port', '*', [channel.port2]);
        channel.port1.start();
        await settle(100);
        channel.port1.postMessage(module);
        channel.port1.postMessage('after');
        postMessage.call(element.contentWindow, 'send back', '*');
      });
      results[kind] = {there: events.map((event) => event.data), back};
      element.remove();
    }
    const worker = (url) => new Promise((resolve) => {
      const made = new Worker(url);
      const channel = new MessageChannel();
      const got = [];
      const say = (event) => got.push(event.data instanceof WebAssembly.Module ? `${event.type} module` : event.data);
      made.onmessage = made.onmessageerror = say;
      channel.port1.onmessage = channel.port1.onmessageerror = (event) => got.push(`port back ${event.type}`);
      made.postMessage('port', [channel.port2]);
      made.postMessage(module);
      channel.port1.postMessage(module);
      setTimeout(() => { made.terminate(); resolve(got.sort()); }, 400);
    });
    const source = `const bytes = new Uint8Array([${bytes}]);
      onmessage = (event) => {
        const port = event.ports[0];
        if (!port) { postMessage('worker ' + event.type + (event.data instanceof WebAssembly.Module ? ' module' : '')); return; }
        port.onmessage = port.onmessageerror = (event) => postMessage('port ' + event.type + (event.data instanceof WebAssembly.Module ? ' module' : ''));
        port.postMessage(new WebAssembly.Module(bytes));
        postMessage(new WebAssembly.Module(bytes));
      };`;
    results['blob: worker'] = await worker(URL.createObjectURL(new Blob([source])));
    results['data: worker'] = await worker(`data:text/javascript,${encodeURIComponent(source)}`);
    return results;
  });

  // What a call on a window throws is of the realm of the function, and what a script makes within the call is of its own.
  await section('realms', async () => {
    const results = {};
    const realm = (value) => {
      let prototype = value;
      while (Object.getPrototypeOf(prototype)) prototype = Object.getPrototypeOf(prototype);
      return prototype === Object.prototype ? 'page' : 'another';
    };
    const thrown = (run) => {
      try {
        run();
        return 'nothing';
      } catch (error) {
        return `${error.name} of ${realm(error)}`;
      }
    };
    for (const kind in CHILD_OF) {
      const element = await add(CHILD_OF[kind]);
      const w = element.contentWindow;
      const channel = new MessageChannel();
      results[kind] = {
        clone: thrown(() => postMessage.call(w, () => 1, '*')),
        target: thrown(() => postMessage.call(w, 1, 'nowhere')),
        'twice': thrown(() => postMessage.call(w, 1, '*', [channel.port1, channel.port1])),
        'not transferable': thrown(() => postMessage.call(w, 1, '*', [{}])),
        delegate: thrown(() => postMessage.call(w, 1, {targetOrigin: '*', delegate: 'x'})),
        arguments: thrown(() => postMessage.call(w)),
      };
      element.remove();
    }
    const element = await add(CHILD_OF['another site']);
    const makers = {
      signal: () => AbortSignal.abort(), timeout: () => AbortSignal.timeout(1e6), response: () => Response.json({}), rect: () => DOMRect.fromRect({}),
      controller: () => { let made; new ReadableStream({start(controller) { made = controller; }}); return made; }, blob: () => new Blob(),
      event: () => { let made; const target = new EventTarget(); target.onx = 0; target.addEventListener('x', (event) => { made = event; }); target.dispatchEvent(new Event('x')); return made; },
      element: () => document.createElement('p').classList, named: () => document.namedForMessages, exception: () => { try { document.querySelector('<'); } catch (error) { return error; } },
      clone: () => structuredClone(new Blob()), activation: () => navigator.userActivation, channel: () => new MessageChannel().port1,
    };
    document.body.insertAdjacentHTML('beforeend', '<form name=namedForMessages></form>');
    const ways = {
      message: (run) => postMessage.call(element.contentWindow, {get x() { return run(); }}, '*'),
      option: (run) => postMessage.call(element.contentWindow, 1, {get targetOrigin() { run(); return '*'; }}),
      transfer: (run) => postMessage.call(element.contentWindow, 1, '*', {[Symbol.iterator]: function* () { run(); }}),
    };
    results.within = {};
    for (const way in ways) {
      results.within[way] = Object.entries(makers).map(([name, make]) => {
        let made;
        ways[way](() => { made = make(); });
        return `${name} of ${realm(made)}`;
      });
    }
    element.remove();
    const own = await add('/child');
    results.borrowed = {
      atob: thrown(() => atob.call(own.contentWindow, '!')),
      querySelector: thrown(() => Document.prototype.querySelector.call(own.contentDocument, '<')),
      appendChild: thrown(() => Node.prototype.appendChild.call(own.contentDocument.body, document)),
      clone: thrown(() => structuredClone.call(own.contentWindow, () => 1)),
      'the frame\'s querySelector': thrown(() => own.contentWindow.Document.prototype.querySelector.call(document, '<')),
      'the frame\'s postMessage': thrown(() => own.contentWindow.postMessage.call(window, () => 1, '*')),
      'the frame\'s own': thrown(() => own.contentDocument.querySelector('<')),
      'what the clone is': realm(structuredClone.call(own.contentWindow, {})),
      'what the frame\'s clone is': realm(own.contentWindow.structuredClone.call(window, {})),
      'what a method returns': realm(Document.prototype.createRange.call(own.contentDocument)),
    };
    const {contentWindow} = own;
    const post = contentWindow.postMessage;
    own.remove();
    results.borrowed['on a removed window'] = thrown(() => postMessage.call(contentWindow, () => 1, '*'));
    results.borrowed['a removed window\'s'] = thrown(() => post.call(window, () => 1, '*'));
    return results;
  });
  return record;

  // A frame's document that takes a port, as /child?port does.
  function portOf() {
    const told = (data) => postMessage.call(parent, data, '*');
    const module = () => new WebAssembly.Module(new Uint8Array([0, 97, 115, 109, 1, 0, 0, 0]));
    let port;
    onmessage = (event) => {
      if (event.data === 'send back') {
        port.postMessage(module());
        port.postMessage('back');
        return;
      }
      port = event.ports[0];
      port.onmessage = port.onmessageerror = ({type, data, origin, source, ports}) => {
        told([type, data instanceof WebAssembly.Module ? 'module' : data && data.module ? 'with a module' : data, origin, source, ports.length]);
      };
    };
  }
})()
