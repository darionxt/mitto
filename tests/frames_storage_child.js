// What each document of tests/frames_storage_cases.js and tests/frames_storage_tab.js runs: it does what a message asks of it,
// or asks its first frame, and answers. A message is of any origin, and goes by the sender's own postMessage().
(() => {
  let channel;
  let heard = [];
  let events = [];
  const text = (run) => {
    try {
      return run();
    } catch (error) {
      return `${error.name}: ${error.message}`;
    }
  };
  const area = (event) => text(() => event.storageArea === localStorage ? 'local' : event.storageArea === sessionStorage ? 'session' : 'other');
  const acts = {
    listen: () => text(() => {
      addEventListener('storage', (event) => events.push([event.key, event.oldValue, event.newValue, event.url, area(event), event.isTrusted]));
      channel = new BroadcastChannel('frames');
      channel.onmessage = channel.onmessageerror = (event) => heard.push([event.type, event.data, event.origin, event.source]);
      return 'listens';
    }),
    post: ({data}) => text(() => { channel.postMessage(data); return 'posted'; }),
    set: ({kind, key, value}) => text(() => { window[kind].setItem(key, value); return 'set'; }),
    remove: ({kind, key}) => text(() => { window[kind].removeItem(key); return 'removed'; }),
    clear: ({kind}) => text(() => { window[kind].clear(); return 'cleared'; }),
    get: ({kind, key}) => text(() => window[kind].getItem(key)),
    heard: () => { const all = [heard, events]; heard = []; events = []; return all; },
    count: () => [heard.length, events.length],
    // The getter of this document's realm, on the top-level document's navigator.
    borrow: () => text(() => typeof Object.getOwnPropertyDescriptor(Navigator.prototype, 'locks').get.call(top.navigator).query),
    lock: () => text(() => { navigator.locks.request('frames', () => new Promise(() => {})); return 'asked'; }),
    locks: async () => {
      try {
        return (await navigator.locks.query()).held.map((lock) => lock.name);
      } catch (error) {
        return `${error.name}: ${error.message}`;
      }
    },
  };
  globalThis.act = (message) => acts[message.act](message);
  const answers = new Map();
  let asked = 0;
  // Asks `window` to act, which answers in its own time.
  globalThis.ask = (window, message) => new Promise((resolve) => {
    const ask = `${Math.random()} ${++asked}`;
    answers.set(ask, resolve);
    postMessage.call(window, {...message, ask}, '*');
  });
  addEventListener('message', async ({data, source}) => {
    if (data && answers.has(data.answer)) {
      answers.get(data.answer)(data.value);
      answers.delete(data.answer);
    } else if (data && data.ask) {
      const value = data.inner ? await ask(frames[0], data.inner) : await act(data);
      postMessage.call(source, {answer: data.ask, value}, '*');
    }
  });
  const nest = window !== top && new URLSearchParams(location.search).get('nest');
  if (nest) {
    const element = document.createElement('iframe');
    element.src = nest;
    document.documentElement.append(element);
  }
})()
