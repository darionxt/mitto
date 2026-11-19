// What a page sees of the Storage Buckets API, which
// tests/test_page_storage_buckets.py runs in a Page and compares with what
// Chrome 157 gave for the same script on the same kind of page, at
// http://127.0.0.1:<port>/ in a profile of its own. The script settles with an
// object of outcomes: values, the events in the order they came, and errors as
// their class, name and message. Usage is shown only as whether it is 0, since
// Chrome counts the bytes of its own files.
(async () => {
  const out = {};
  const error = (e) => (e ? `${e.constructor?.name} ${e.name}: ${e.message}` : null);
  // Numbers JSON cannot hold, as strings.
  const show = (v) => {
    if (v === undefined) return 'undefined';
    if (typeof v === 'number' && !Number.isFinite(v)) return {number: String(v)};
    if (Array.isArray(v)) return v.map(show);
    if (v && typeof v === 'object' && Object.getPrototypeOf(v) === Object.prototype) {
      return Object.fromEntries(Object.entries(v).map(([k, x]) => [k, show(x)]));
    }
    if (v && typeof v === 'object') return Object.prototype.toString.call(v);
    return v;
  };
  // What never settles shows as such, instead of stopping the script.
  const outcome = async (f) => {
    try {
      const v = await Promise.race([f(), new Promise((r) => setTimeout(() => r('timed out'), 5000))]);
      return ['ok', show(v)];
    } catch (e) {
      return error(e);
    }
  };
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const m = navigator.storageBuckets;
  const openDb = (factory, name, upgrade) => new Promise((resolve, reject) => {
    const r = factory.open(name);
    r.onupgradeneeded = () => upgrade && upgrade(r.result);
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
  const finished = (tx) => new Promise((resolve) => {
    tx.oncomplete = () => resolve('complete');
    tx.onabort = () => resolve(`abort ${error(tx.error)}`);
  });

  const methods = (proto) => Object.getOwnPropertyNames(proto)
    .filter((k) => typeof Object.getOwnPropertyDescriptor(proto, k).value === 'function')
    .map((k) => [k, proto[k].length]);
  out.shape = {
    manager: Object.prototype.toString.call(m),
    sameManager: m === navigator.storageBuckets,
    managerProperties: Object.getOwnPropertyNames(StorageBucketManager.prototype),
    managerMethods: methods(StorageBucketManager.prototype),
    bucketProperties: Object.getOwnPropertyNames(StorageBucket.prototype),
    bucketMethods: methods(StorageBucket.prototype),
    durability: 'durability' in StorageBucket.prototype,
    locks: 'locks' in StorageBucket.prototype,
  };
  out.keysAtFirst = await outcome(() => m.keys());

  // Names.
  out.openNames = {};
  for (const name of ['', 'a', 'A', 'a-b', 'a_b', '-a', '_a', '0', '0a', 'a'.repeat(63), 'a'.repeat(64),
    'é', 'a b', 'default', '_default', 'a.b', 'a\u0000']) {
    out.openNames[JSON.stringify(name)] = await outcome(async () => (await m.open(name)).name);
  }
  out.keysAfterNames = await outcome(() => m.keys());
  out.deleteNames = {};
  for (const name of ['', 'A', '-a', 'nope', 'a', 'default']) {
    out.deleteNames[JSON.stringify(name)] = await outcome(() => m.delete(name));
  }
  out.keysAfterDeletes = await outcome(() => m.keys());

  // Options.
  out.options = {
    quotaZero: await outcome(() => m.open('q', {quota: 0})),
    quotaNegative: await outcome(() => m.open('q', {quota: -1})),
    quotaTooLarge: await outcome(() => m.open('q', {quota: 2 ** 53})),
    quotaLargest: await outcome(async () => (await (await m.open('qm', {quota: Number.MAX_SAFE_INTEGER})).estimate()).quota),
    expiresPast: await outcome(() => m.open('e', {expires: Date.now() - 1000})),
    expiresEpoch: await outcome(() => m.open('e', {expires: 0})),
    expiresNaN: await outcome(() => m.open('e', {expires: NaN})),
    durabilityIgnored: await outcome(async () => (await m.open('dur', {durability: 'bogus'})).name),
    noArguments: await outcome(() => m.open()),
    nullName: await outcome(async () => (await m.open(null)).name),
    undefinedOptions: await outcome(async () => (await m.open('uo', undefined)).name),
    notOptions: await outcome(() => m.open('no', 5)),
    deleteNoArguments: await outcome(() => m.delete()),
  };
  const order = [];
  await outcome(() => m.open('ord', {
    get persisted() { order.push('persisted'); return true; },
    get durability() { order.push('durability'); return 'strict'; },
    get quota() { order.push('quota'); return 1000; },
    get expires() { order.push('expires'); return Date.now() + 1e7; },
  }));
  out.optionOrder = order;

  // A bucket and its policies.
  const b = await m.open('b1', {quota: 12345, persisted: true, expires: Date.now() + 86400000});
  out.bucket = {
    name: b.name,
    tag: Object.prototype.toString.call(b),
    indexedDB: Object.prototype.toString.call(b.indexedDB),
    sameIndexedDB: b.indexedDB === b.indexedDB,
    notWindowIndexedDB: b.indexedDB !== indexedDB,
    caches: Object.prototype.toString.call(b.caches),
    sameCaches: b.caches === b.caches,
    notWindowCaches: b.caches !== caches,
    persisted: await outcome(() => b.persisted()),
    persist: await outcome(() => b.persist()),
    estimate: await outcome(() => b.estimate()),
    expiresType: await outcome(async () => typeof (await b.expires())),
    setExpiresPast: await outcome(() => b.setExpires(Date.now() - 1000)),
    expiresAfterPast: await outcome(async () => typeof (await b.expires())),
    setExpires: await outcome(() => b.setExpires(1e15)),
    expires: await outcome(() => b.expires()),
    setExpiresFraction: await outcome(() => b.setExpires(1e15 + 0.123456)),
    expiresFraction: await outcome(() => b.expires()),
    setExpiresHuge: await outcome(() => b.setExpires(1e300)),
    expiresHuge: await outcome(() => b.expires()),
    setExpiresNegative: await outcome(() => b.setExpires(-1e300)),
    expiresNegative: await outcome(() => b.expires()),
    setExpiresNoArguments: await outcome(() => b.setExpires()),
    setExpiresNaN: await outcome(() => b.setExpires(NaN)),
    getDirectory: await outcome(async () => Object.prototype.toString.call(await b.getDirectory())),
  };
  await b.setExpires(Date.now() + 86400000);
  const b2 = await m.open('b1', {quota: 99, persisted: false, expires: 2e15});
  out.reopened = {
    sameObject: b2 === b,
    quotaKept: await outcome(async () => (await b2.estimate()).quota),
    expiresChanged: await outcome(() => b2.expires()),
    expiresOfOld: await outcome(() => b.expires()),
  };
  out.plain = await outcome(async () => {
    const p = await m.open('plain');
    return [await p.expires(), await p.persisted(), await p.estimate()];
  });
  out.openWithExpiresHuge = await outcome(async () => (await m.open('eh', {expires: 1e300})).expires());
  out.estimateOfKey = await outcome(() => navigator.storage.estimate());
  out.concurrentOpens = await outcome(async () => {
    const [x, y] = await Promise.all([m.open('cc'), m.open('cc')]);
    return [x === y, x.name, y.name];
  });
  out.persistedOption = await outcome(async () => {
    const p = await m.open('p', {persisted: true});
    return [await p.persisted(), await p.persist(), await navigator.storage.persisted()];
  });
  out.receivers = {
    keys: await outcome(() => StorageBucketManager.prototype.keys.call({})),
    estimate: await outcome(() => StorageBucket.prototype.estimate.call({})),
    name: await outcome(() => Object.getOwnPropertyDescriptor(StorageBucket.prototype, 'name').get.call({})),
    newBucket: await outcome(() => new StorageBucket()),
    newManager: await outcome(() => new StorageBucketManager()),
  };
  out.keysBeforeClearing = await outcome(() => m.keys());
  for (const k of await m.keys()) await m.delete(k);

  // The storage of buckets, apart from each other's and the default bucket's.
  out.apart = await outcome(async () => {
    const inbox = await m.open('inbox');
    const outbox = await m.open('outbox');
    const write = async (bucket, value) => {
      const db = await openDb(bucket.indexedDB, 'messages', (d) => d.createObjectStore('s'));
      const tx = db.transaction('s', 'readwrite');
      tx.objectStore('s').put(value, 'k');
      await finished(tx);
      return db;
    };
    const read = (db) => new Promise((resolve) => {
      const r = db.transaction('s').objectStore('s').get('k');
      r.onsuccess = () => resolve(r.result);
    });
    const inboxDb = await write(inbox, 'in');
    const outboxDb = await write(outbox, 'out');
    const inboxCache = await inbox.caches.open('c');
    await inboxCache.put('/a', new Response('bread'));
    const outboxCache = await outbox.caches.open('c');
    const result = {
      read: [await read(inboxDb), await read(outboxDb)],
      databases: [await inbox.indexedDB.databases(), await indexedDB.databases()],
      cacheKeys: [await inbox.caches.keys(), await outbox.caches.keys(), await caches.keys()],
      matches: [await (await inboxCache.match('/a')).text(), String(await outboxCache.match('/a')),
        String(await caches.match('/a'))],
    };
    inboxDb.close();
    outboxDb.close();
    return result;
  });
  out.usage = await outcome(async () => {
    const bucket = await m.open('inbox');
    const before = await navigator.storage.estimate();
    const estimate = await bucket.estimate();
    const key = await navigator.storage.estimate();
    await m.delete('inbox');
    await m.delete('outbox');
    const after = await navigator.storage.estimate();
    return {
      bucketUsed: estimate.usage > 0,
      bucketQuotaOverUsage: estimate.quota - estimate.usage,
      bucketDetails: estimate.usageDetails,
      keyCountsBuckets: key.usage >= estimate.usage && before.usage === key.usage,
      keyDetails: Object.keys(key.usageDetails),
      keyQuotaOverUsage: key.quota - key.usage,
      afterDelete: [after.usage, after.usageDetails],
    };
  });

  // A bucket's own quota.
  out.bucketQuota = await outcome(async () => {
    const bucket = await m.open('small', {quota: 1000});
    const db = await openDb(bucket.indexedDB, 'db', (d) => d.createObjectStore('s'));
    const tx = db.transaction('s', 'readwrite');
    tx.objectStore('s').put('x'.repeat(5000), 'k');
    const put = await finished(tx);
    const e = tx.error;
    const cache = await bucket.caches.open('c');
    let cacheError = null;
    try {
      await cache.put('/a', new Response('z'.repeat(5000)));
    } catch (thrown) {
      cacheError = [thrown.constructor.name, thrown.name, thrown.message, thrown.quota, thrown.requested];
    }
    const estimate = await bucket.estimate();
    db.close();
    return {
      put,
      error: [e.constructor.name, e.name, e.message, e.quota, e.requested],
      cacheError,
      quota: estimate.quota,
    };
  });

  // A deleted bucket, each call of a bucket of its own.
  let deadCount = 0;
  const deadBucket = async () => {
    const name = `dead${++deadCount}`;
    const bucket = await m.open(name);
    await m.delete(name);
    return bucket;
  };
  const idbOpen = (factory) => new Promise((resolve, reject) => {
    const r = factory.open('db');
    r.onsuccess = () => {
      r.result.close();
      resolve('success');
    };
    r.onerror = () => reject(r.error);
  });
  out.deleted = {
    idbOpen: await outcome(async () => idbOpen((await deadBucket()).indexedDB)),
    idbDatabases: await outcome(async () => (await deadBucket()).indexedDB.databases()),
    idbDeleteDatabase: await outcome(async () => {
      const bucket = await deadBucket();
      return new Promise((resolve, reject) => {
        const r = bucket.indexedDB.deleteDatabase('db');
        r.onsuccess = () => resolve('success');
        r.onerror = () => reject(r.error);
      });
    }),
    cachesOpen: await outcome(async () => (await deadBucket()).caches.open('x')),
    cachesKeys: await outcome(async () => (await deadBucket()).caches.keys()),
    cachesHas: await outcome(async () => (await deadBucket()).caches.has('x')),
    cachesDelete: await outcome(async () => (await deadBucket()).caches.delete('x')),
    cachesMatch: await outcome(async () => (await deadBucket()).caches.match('/x')),
    persisted: await outcome(async () => (await deadBucket()).persisted()),
    persist: await outcome(async () => (await deadBucket()).persist()),
    estimate: await outcome(async () => (await deadBucket()).estimate()),
    expires: await outcome(async () => (await deadBucket()).expires()),
    setExpires: await outcome(async () => (await deadBucket()).setExpires(Date.now() + 1e6)),
    inTurn: await outcome(async () => {
      const bucket = await deadBucket();
      const calls = [];
      for (const k of ['persist', 'estimate', 'persist', 'persisted', 'persist', 'expires']) {
        calls.push([k, await outcome(() => bucket[k]())]);
      }
      return calls;
    }),
  };
  out.reopenedAfterDelete = await outcome(async () => {
    const old = await deadBucket();
    const fresh = await m.open(`dead${deadCount}`);
    return {
      oldIndexedDB: await outcome(() => idbOpen(old.indexedDB)),
      oldEstimate: await outcome(() => old.estimate()),
      freshEstimate: await outcome(() => fresh.estimate()),
      oldPersisted: await outcome(() => old.persisted()),
      freshIndexedDB: await outcome(() => idbOpen(fresh.indexedDB)),
      keys: await m.keys(),
    };
  });
  // Storage that a page used before its bucket went.
  out.usedThenDeleted = await outcome(async () => {
    const bucket = await m.open('used');
    const db = await openDb(bucket.indexedDB, 'db', (d) => d.createObjectStore('s'));
    db.close();
    const c = bucket.caches;
    const cache = await c.open('x');
    await cache.put('/a', new Response('A'));
    await m.delete('used');
    return {
      idbOpen: await outcome(() => idbOpen(bucket.indexedDB)),
      idbDatabases: await outcome(() => bucket.indexedDB.databases()),
      cachesOpen: await outcome(() => c.open('x')),
      cachesKeys: await outcome(() => c.keys()),
      cachesHas: await outcome(() => c.has('x')),
      cachesDelete: await outcome(() => c.delete('x')),
      cachesMatch: await outcome(() => c.match('/a')),
      cacheMatch: await outcome(() => cache.match('/a')),
      cacheMatchAll: await outcome(() => cache.matchAll('/a')),
      cacheKeys: await outcome(() => cache.keys()),
      cachePut: await outcome(() => cache.put('/b', new Response('B'))),
      cacheDelete: await outcome(() => cache.delete('/a')),
      reopened: await outcome(async () => {
        const fresh = await m.open('used');
        return [await fresh.indexedDB.databases(), await fresh.caches.keys()];
      }),
    };
  });
  // A connection that is open while its bucket goes.
  out.openWhileDeleted = await outcome(async () => {
    const bucket = await m.open('open');
    const db = await openDb(bucket.indexedDB, 'db', (d) => d.createObjectStore('s'));
    const log = [];
    db.onclose = (e) => log.push(['close', e.constructor.name, e.bubbles, e.cancelable]);
    db.onversionchange = () => log.push('versionchange');
    const tx = db.transaction('s', 'readwrite');
    tx.onabort = () => log.push(['abort', error(tx.error)]);
    tx.oncomplete = () => log.push('complete');
    await m.delete('open');
    await sleep(500);
    log.push(['transaction', await outcome(() => {
      db.transaction('s');
      return 'made';
    })]);
    return log;
  });
  out.idleWhileDeleted = await outcome(async () => {
    const bucket = await m.open('idle');
    const db = await openDb(bucket.indexedDB, 'db', (d) => d.createObjectStore('s'));
    const log = [];
    db.onclose = () => log.push('close');
    await m.delete('idle');
    await sleep(500);
    return log;
  });

  // Expiration.
  out.expiry = await outcome(async () => {
    const bucket = await m.open('exp', {expires: Date.now() + 1e6});
    await bucket.setExpires(Date.now() - 1);
    const usable = await outcome(async () => [typeof (await bucket.expires()), (await bucket.estimate()).usage]);
    const keys = await m.keys();
    const afterKeys = await outcome(() => bucket.estimate());
    const other = await m.open('exp2', {expires: Date.now() + 1e6});
    await other.setExpires(Date.now() - 1);
    const reopened = await m.open('exp2');
    return {
      usable,
      listed: keys.includes('exp'),
      afterKeys,
      oldAfterReopen: await outcome(() => other.estimate()),
      reopenedExpires: await outcome(() => reopened.expires()),
    };
  });

  for (const k of await m.keys()) await m.delete(k);
  out.keysAtLast = await outcome(() => m.keys());
  return out;
})()
