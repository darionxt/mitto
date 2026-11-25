// The requests whose header lines a page's script sets, by fetch() and by
// XMLHttpRequest, in a window and in a worker. tests/header_order.py gives it
// KINDS, the ways to ask, and SETS, the lines by name. Each request goes to a
// path of its own, /order/NAME, whose lines the server records. The promise
// settles with "done" once all have ended.
(async () => {
  const origin = location.origin;
  const ask = async (place, kinds, sets) => {
    const asks = {
      fetch: (url, init) => fetch(url, init),
      xhr: (url, {method = 'GET', headers = [], body = null}) => new Promise((resolve) => {
        const xhr = new XMLHttpRequest();
        xhr.open(method, url);
        for (const [name, value] of headers) xhr.setRequestHeader(name, value);
        xhr.onloadend = resolve;
        xhr.send(body);
      }),
    };
    for (const kind of kinds) {
      const url = (verb, name) => `${origin}/order/${place}-${kind}-${verb}-${name}`;
      for (const [name, headers] of Object.entries(sets)) {
        await asks[kind](url('get', name), {headers});
        await asks[kind](url('post', name), {method: 'POST', body: '{}', headers});
        await asks[kind](url('put', name), {method: 'PUT', body: '{}', headers: [...headers, ['Content-Type', 'application/json']]});
      }
    }
    if (kinds.includes('fetch')) {
      const url = (name) => `${origin}/order/${place}-fetch-${name}`;
      await fetch(url('no-cors'), {mode: 'no-cors', headers: [['Accept', 'a/b'], ['Content-Language', 'de'], ['Accept-Language', 'de']]});
      await fetch(url('blob'), {method: 'POST', body: new Blob(['x'], {type: 'a/b'}), headers: [['X-Alpha', '1']]});
      await fetch(new Request(url('request'), {headers: {'X-Beta': '2', 'X-Alpha': '1'}}));
      await fetch(url('twice'), {headers: [['x-one', '1'], ['X-Two', '2'], ['X-ONE', '3']]});
      await fetch(url('lower'), {method: 'POST', body: 'x', headers: [['x-lower', '1'], ['accept', 'a/b'], ['content-type', 'c/d'], ['user-agent', 'mine'], ['UPPER', '2']]});
    }
  };
  const kinds = KINDS;
  const sets = SETS;
  await ask('window', kinds, sets);
  const source = `const origin = ${JSON.stringify(origin)}; const ask = ${ask};
                  ask('worker', ${JSON.stringify(kinds)}, ${JSON.stringify(sets)})
                      .then(() => postMessage('done'), (e) => postMessage(String(e)));`;
  const worker = new Worker(URL.createObjectURL(new Blob([source], {type: 'text/javascript'})));
  return await new Promise((resolve) => worker.onmessage = (e) => resolve(e.data));
})();
