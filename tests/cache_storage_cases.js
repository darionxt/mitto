// What a page sees of the Cache API, which tests/test_page_cache_storage.py
// runs in a Page and compares with what Chrome 157 gave for the same script on
// the same kind of page. The page is at http://127.0.0.1:<port>/, a secure
// context, whose server answers /res/<name> as tests/test_page_cache_storage.py
// says. The script settles with an object of outcomes.
(async () => {
  const out = {};
  const outcome = async (f) => {
    try {
      const value = await f();
      return ['ok', value === undefined ? 'undefined' : value];
    } catch (e) {
      return `${e?.constructor?.name} ${e?.name}: ${e?.message}`;
    }
  };
  // The header lines that the page or the test's server chose, not those of
  // the server's software and clock, read one name at a time.
  const chosen = (headers) => ['content-type', 'set-cookie', 'vary', 'x-a', 'x-one']
      .map((name) => [name, headers.get(name)]).filter(([, value]) => value !== null);
  const describe = async (r) => r === undefined ? 'undefined' : {
    type: r.type, url: r.url.replace(location.origin, ''), redirected: r.redirected, status: r.status,
    ok: r.ok, statusText: r.statusText, headers: chosen(r.headers),
    bodyUsed: r.bodyUsed, text: await r.text(),
  };
  const urls = (requests) => requests.map((r) => r.url.replace(location.origin, ''));

  out.shape = [
    Object.prototype.toString.call(caches), caches === window.caches,
    Object.getOwnPropertyNames(CacheStorage.prototype).sort(),
    Object.getOwnPropertyNames(Cache.prototype).sort(),
  ];
  for (const name of await caches.keys()) await caches.delete(name);

  out.empty = [await caches.keys(), await caches.has('a'), await caches.delete('a'),
    await caches.match('/res/a'), await outcome(() => caches.match('/res/a', {cacheName: 'a'}))];

  const a = await caches.open('a');
  const b = await caches.open('b');
  const a2 = await caches.open('a');
  out.open = [Object.prototype.toString.call(a), a === a2, await caches.keys(), await caches.has('a')];

  // put() and match() of a response that a page made.
  out.putMade = await outcome(() => a.put('/res/made?x=1', new Response('made body', {
    status: 201, statusText: 'Made', headers: {'X-One': '1', 'Content-Type': 'text/plain', 'Set-Cookie': 'a=b'},
  })));
  out.matchMade = await describe(await a.match('/res/made?x=1'));
  out.matchMadeSearch = [
    await a.match('/res/made'), (await a.match('/res/made', {ignoreSearch: true}))?.status,
    (await a.match('/res/made?x=1#frag'))?.status,
  ];
  out.matchPost = [
    await a.match(new Request('/res/made?x=1', {method: 'POST'})),
    (await a.match(new Request('/res/made?x=1', {method: 'POST'}), {ignoreMethod: true}))?.status,
    await a.matchAll(new Request('/res/made?x=1', {method: 'POST'})),
    await a.keys(new Request('/res/made?x=1', {method: 'POST'})),
    await a.delete(new Request('/res/made?x=1', {method: 'POST'})),
  ];
  // An empty body comes back null.
  await a.put('/res/empty', new Response(''));
  await a.put('/res/none', new Response(null, {status: 204}));
  out.empty_bodies = [await describe(await a.match('/res/empty')), await describe(await a.match('/res/none'))];

  // A fetched response, and its fragment.
  const fetched = await fetch('/res/a');
  await a.put('/res/a#frag', fetched);
  out.fetchedUsed = fetched.bodyUsed;
  out.matchFetched = await describe(await a.match('/res/a'));
  out.keys = urls(await a.keys());
  out.keysWithFragment = (await a.keys('/res/a')).map((r) => [r.url.replace(location.origin, ''), r.method]);

  // Order: by the time each entry was put, and a put of the same URL replaces it.
  await a.put('/res/made?x=1', new Response('again'));
  out.keysAfterReplace = urls(await a.keys());
  out.matchAll = (await Promise.all((await a.matchAll()).map((r) => r.text())));
  out.matchAllSearch = (await a.matchAll('/res/made', {ignoreSearch: true})).length;

  // Vary.
  await b.put(new Request('/res/vary', {headers: {'X-Kind': 'one'}}),
              new Response('vary', {headers: {Vary: 'X-Kind'}}));
  out.vary = [
    (await b.match(new Request('/res/vary', {headers: {'X-Kind': 'one'}})))?.status,
    await b.match(new Request('/res/vary', {headers: {'X-Kind': 'two'}})),
    await b.match('/res/vary'),
    (await b.match('/res/vary', {ignoreVary: true}))?.status,
    (await b.keys())[0].headers.get('x-kind'),
  ];

  // What put() refuses.
  out.putErrors = {
    scheme: await outcome(() => a.put('data:text/plain,x', new Response('x'))),
    method: await outcome(() => a.put(new Request('/res/p', {method: 'POST'}), new Response('x'))),
    varyStar: await outcome(() => a.put('/res/p', new Response('x', {headers: {Vary: 'X, *'}}))),
    partial: await outcome(() => a.put('/res/p', new Response('x', {status: 206}))),
    used: await outcome(async () => { const r = new Response('x'); await r.text(); return a.put('/res/p', r); }),
    badUrl: await outcome(() => a.put('http://[', new Response('x'))),
    noResponse: await outcome(() => a.put('/res/p')),
    notResponse: await outcome(() => a.put('/res/p', {})),
  };
  out.afterPutErrors = urls(await a.keys());

  // add() and addAll().
  out.add = [
    await outcome(() => a.add('/res/b')),
    await outcome(() => a.add('/res/missing')),
    await outcome(() => a.add('/res/vary-star')),
    await outcome(() => a.addAll([])),
    await outcome(() => a.addAll(['/res/c', '/res/d'])),
    (await outcome(() => a.addAll(['/res/e', '/res/e']))).replace(location.origin, ''),
    await outcome(() => a.addAll(['/res/f', '/res/missing'])),
    await outcome(() => a.add(new Request('/res/g', {method: 'POST'}))),
  ];
  out.afterAdd = urls(await a.keys());
  out.addedText = await (await a.match('/res/c')).text();

  // delete().
  out.delete = [await a.delete('/res/c'), await a.delete('/res/c'),
    await a.delete('/res/MADE', {ignoreSearch: true}), await a.delete('/res/made', {ignoreSearch: true}),
    urls(await a.keys())];

  // caches.match() across caches, in the order they were made.
  await b.put('/res/b', new Response('from b'));
  out.cachesMatch = [
    await (await caches.match('/res/b')).text(),
    await (await caches.match('/res/b', {cacheName: 'b'})).text(),
    await caches.match('/res/b', {cacheName: 'none'}),
    await caches.match('/res/nothing'),
  ];

  // A large body, in parts both ways.
  const big = 'x'.repeat(3 * 1024 * 1024 + 17);
  await a.put('/res/big', new Response(big));
  const bigBack = await (await a.match('/res/big')).text();
  out.big = [bigBack.length, bigBack === big];
  const bytes = new Uint8Array(300000).map((_, i) => i % 251);
  await a.put('/res/bytes', new Response(bytes));
  const bytesBack = new Uint8Array(await (await a.match('/res/bytes')).arrayBuffer());
  out.bytes = [bytesBack.length, bytesBack.every((v, i) => v === i % 251)];

  // A deleted cache still answers through its object, and is no longer listed.
  out.deleteCache = [await caches.delete('b'), await caches.keys(), await caches.has('b'),
    await (await b.match('/res/b'))?.text(), await caches.delete('b')];
  const b3 = await caches.open('b');
  out.reopened = [await b3.keys(), await caches.keys()];

  // Names that are not ASCII, or not even well-formed UTF-16.
  await caches.open('é😀');
  await caches.open('lone \ud800');
  out.names = (await caches.keys()).map((n) => [...n].map((c) => c.codePointAt(0)));
  out.hasLone = await caches.has('lone \ud800');

  out.estimate = await (async () => {
    const e = await navigator.storage.estimate();
    return [e.usage > 3 * 1024 * 1024, Object.keys(e.usageDetails ?? {}), e.quota - e.usage];
  })();

  for (const name of await caches.keys()) await caches.delete(name);
  out.cleared = await caches.keys();
  return out;
})()
