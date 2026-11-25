// A page's fetches with header lines that its script sets, in a window and in a
// worker, whose order on the wire is Blink's HTTPHeaderMap's. Each goes to a
// path of its own, /NAME, whose lines the server of
// tests/test_page_header_order.py records, which that test compares with what
// Chrome 157 sent. The promise settles once all have ended.
(async () => {
  const sets = {
    none: [],
    one: [['X-Alpha', '1']],
    two: [['Authorization', 'Bearer x'], ['X-Requested-With', 'XMLHttpRequest']],
    three: [['X-Alpha', '1'], ['X-Beta', '2'], ['X-Gamma', '3']],
    reversed: [['X-Gamma', '3'], ['X-Beta', '2'], ['X-Alpha', '1']],
    accept: [['Accept', 'application/json'], ['X-CSRF-Token', 'z'], ['Accept-Language', 'de']],
    cache: [['Cache-Control', 'no-cache'], ['Pragma', 'no-cache']],
    lower: [['x-lower', '1'], ['accept', 'a/b'], ['content-type', 'c/d'], ['user-agent', 'mine'], ['UPPER', '2']],
    hints: [['DPR', '2'], ['Save-Data', 'on'], ['Width', '100']],
    twice: [['x-one', '1'], ['X-Two', '2'], ['X-ONE', '3']],
    many: 'abcdefgh'.split('').map((letter) => ['x-' + letter, letter]),
    more: 'abcdefghijklmnopqrstuvwxyz'.split('').map((letter) => [letter + '-header', letter]),
  };
  const ask = async (place) => {
    for (const [name, headers] of Object.entries(sets)) {
      await fetch(`${origin}/${place}-get-${name}`, {headers});
      await fetch(`${origin}/${place}-post-${name}`, {method: 'POST', body: '{}', headers});
      await fetch(`${origin}/${place}-typed-${name}`, {method: 'PUT', body: '{}', headers: [...headers, ['Content-Type', 'application/json']]});
    }
    await fetch(`${origin}/${place}-no-cors`, {mode: 'no-cors', headers: [['Accept', 'a/b'], ['Content-Language', 'de'], ['Accept-Language', 'de']]});
    await fetch(`${origin}/${place}-blob`, {method: 'POST', body: new Blob(['x'], {type: 'a/b'}), headers: [['X-Alpha', '1']]});
    await fetch(new Request(`${origin}/${place}-request`, {headers: {'X-Beta': '2', 'X-Alpha': '1'}}));
  };
  const origin = location.origin;
  await ask('window');
  const source = `const origin = ${JSON.stringify(origin)}; const sets = ${JSON.stringify(sets)}; const ask = ${ask};
                  ask('worker').then(() => postMessage('done'), (e) => postMessage(String(e)));`;
  const worker = new Worker(URL.createObjectURL(new Blob([source])));
  return await new Promise((resolve) => worker.onmessage = (e) => resolve(e.data));
})();
