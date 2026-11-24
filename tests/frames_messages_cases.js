// What window.postMessage() gives between a page and its frames: who sent a message, from which origin, whom it is for, and
// what it carries. The page is at /app/page; a frame's document of /child?... does what its query says. A frame of another
// origin calls its own postMessage() on its parent's window, since a Page's windows of another origin give no property yet.
// The value is a promise for a record of sections, which tests/data/frames_messages_cases.json holds as Chrome gave it.
(async () => {
  const OTHER = `http://localhost:${location.port}`;
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
    return order;
  });
  return record;
})()
