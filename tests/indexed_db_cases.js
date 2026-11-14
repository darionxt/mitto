// What a page sees of IndexedDB, which tests/test_page_indexed_db.py runs in a
// Page and compares with what Chrome 157 gave for the same script on the same
// kind of page, at http://127.0.0.1:<port>/. The script settles with an object
// of outcomes: values, the events in the order they came, and errors as their
// class, name and message.
(async () => {
  const out = {};
  const error = (e) => (e ? `${e.constructor?.name} ${e.name}: ${e.message}` : null);
  const outcome = async (f) => {
    try {
      const value = await f();
      return ['ok', value === undefined ? 'undefined' : value];
    } catch (e) {
      return error(e);
    }
  };
  const sync = (f) => {
    try {
      const value = f();
      return ['ok', value === undefined ? 'undefined' : value];
    } catch (e) {
      return error(e);
    }
  };
  // Keys and values as JSON can hold them.
  const show = (v) => {
    if (v === undefined) return 'undefined';
    if (v === null) return null;
    if (v instanceof Date) return {date: v.getTime()};
    if (v instanceof ArrayBuffer) return {binary: [...new Uint8Array(v)]};
    if (ArrayBuffer.isView(v)) return {view: v.constructor.name, bytes: [...new Uint8Array(v.buffer)]};
    if (v instanceof Map) return {map: [...v].map(([k, x]) => [show(k), show(x)])};
    if (v instanceof Set) return {set: [...v].map(show)};
    if (Array.isArray(v)) return v.map(show);
    if (typeof v === 'number' && !Number.isFinite(v)) return {number: String(v)};
    if (typeof v === 'object') {
      return Object.fromEntries(Object.entries(v).map(([k, x]) => [k, show(x)]));
    }
    return v;
  };
  // What never settles shows as such, instead of stopping the script.
  const within = (promise) => Promise.race([promise,
    new Promise((resolve) => setTimeout(() => resolve('timed out'), 10000))]);
  const request = (r) => within(new Promise((resolve) => {
    r.onsuccess = () => resolve(['success', show(r.result)]);
    r.onerror = (e) => {
      e.preventDefault();
      resolve(['error', error(r.error)]);
    };
  }));
  const finished = (tx) => within(new Promise((resolve) => {
    tx.oncomplete = () => resolve('complete');
    tx.onabort = () => resolve(`abort ${error(tx.error)}`);
  }));
  const open = (name, version, upgrade) => within(new Promise((resolve) => {
    const log = [];
    const r = version === undefined ? indexedDB.open(name) : indexedDB.open(name, version);
    r.onupgradeneeded = (e) => {
      log.push(['upgradeneeded', e.oldVersion, e.newVersion, r.transaction.mode, r.result.version,
        r.readyState, Object.prototype.toString.call(e)]);
      if (upgrade) {
        try {
          upgrade(r.result, r.transaction, log);
        } catch (thrown) {
          log.push(['threw', error(thrown)]);
        }
      }
    };
    r.onblocked = (e) => log.push(['blocked', e.oldVersion, e.newVersion]);
    r.onsuccess = () => {
      log.push(['success', r.result.version, [...r.result.objectStoreNames]]);
      resolve({db: r.result, log});
    };
    r.onerror = (e) => {
      log.push(['error', error(r.error)]);
      e.preventDefault();
      resolve({db: null, log});
    };
  }));
  const remove = (name) => within(new Promise((resolve) => {
    const log = [];
    const r = indexedDB.deleteDatabase(name);
    r.onblocked = (e) => log.push(['blocked', e.oldVersion, e.newVersion]);
    r.onsuccess = (e) => {
      log.push(['success', e.oldVersion, e.newVersion, show(r.result)]);
      resolve(log);
    };
    r.onerror = () => {
      log.push(['error', error(r.error)]);
      resolve(log);
    };
  }));
  const names = (list) => [...list];

  out.shape = [
    Object.prototype.toString.call(indexedDB), indexedDB === window.indexedDB,
    ...['IDBFactory', 'IDBOpenDBRequest', 'IDBRequest', 'IDBDatabase', 'IDBTransaction', 'IDBObjectStore',
      'IDBIndex', 'IDBCursor', 'IDBCursorWithValue', 'IDBKeyRange', 'IDBVersionChangeEvent', 'IDBRecord',
      'DOMStringList'].map((name) => [name, typeof window[name],
      window[name] ? Object.getOwnPropertyNames(window[name].prototype).sort() : null]),
  ];

  for (const {name} of await indexedDB.databases()) await remove(name);
  out.emptyDatabases = await indexedDB.databases();

  // cmp() and the order of keys.
  out.cmp = [
    [1, 2], [2, 1], [1, 1], ['a', 1], [1, 'a'], [new Date(5), 5], [[1], 'z'], [new Uint8Array([1]), 'z'],
    [[1, 2], [1]], [[], [1]], ['ab', 'b'], [-Infinity, -1e300], [new Uint8Array([1, 2]), new Uint8Array([1, 3])],
  ].map(([a, b]) => sync(() => indexedDB.cmp(a, b)));
  out.cmpErrors = [
    sync(() => indexedDB.cmp(null, 1)), sync(() => indexedDB.cmp(1, {})), sync(() => indexedDB.cmp(NaN, 1)),
    sync(() => indexedDB.cmp(true, 1)), sync(() => indexedDB.cmp(new Date(NaN), 1)),
    sync(() => { const a = []; a.push(a); return indexedDB.cmp(a, 1); }), sync(() => indexedDB.cmp(1)),
  ];

  // IDBKeyRange.
  out.keyRange = [
    sync(() => { const r = IDBKeyRange.only(3); return [r.lower, r.upper, r.lowerOpen, r.upperOpen]; }),
    sync(() => { const r = IDBKeyRange.lowerBound('a', true); return [r.lower, r.upper, r.lowerOpen, r.upperOpen]; }),
    sync(() => { const r = IDBKeyRange.upperBound([1, 2]); return [r.lower, show(r.upper), r.lowerOpen, r.upperOpen]; }),
    sync(() => { const r = IDBKeyRange.bound(1, 5, false, true); return [r.includes(1), r.includes(5), r.includes(3), r.includes('a')]; }),
    sync(() => IDBKeyRange.bound(5, 1)), sync(() => IDBKeyRange.bound(1, 1, true, false)),
    sync(() => IDBKeyRange.only({})), sync(() => IDBKeyRange.lowerBound(NaN)), sync(() => IDBKeyRange.only(3).includes({})),
    sync(() => new IDBKeyRange()),
  ];

  // Open errors.
  out.openErrors = [
    sync(() => indexedDB.open('x', 0)), sync(() => indexedDB.open('x', -1)), sync(() => indexedDB.open('x', 2 ** 53)),
    sync(() => indexedDB.open('x', 'a')), sync(() => indexedDB.deleteDatabase()),
  ];

  // A new database and its upgrade.
  const first = await open('a', 1, (db, tx, log) => {
    const s = db.createObjectStore('s', {keyPath: 'id'});
    log.push(['store', s.name, s.keyPath, s.autoIncrement, names(s.indexNames), names(db.objectStoreNames),
      names(tx.objectStoreNames), tx.durability]);
    s.createIndex('by_name', 'name');
    s.createIndex('by_tag', 'tags', {multiEntry: true});
    s.createIndex('unique_mail', 'mail', {unique: true});
    s.createIndex('compound', ['name', 'id']);
    db.createObjectStore('auto', {autoIncrement: true});
    db.createObjectStore('kp', {keyPath: 'k.x', autoIncrement: true});
    db.createObjectStore('plain');
    log.push(['indexes', names(s.indexNames), s.index('by_tag').multiEntry, s.index('unique_mail').unique,
      s.index('compound').keyPath]);
    log.push(['createErrors',
      sync(() => db.createObjectStore('s')),
      sync(() => db.createObjectStore('bad', {keyPath: 'a b'})),
      sync(() => db.createObjectStore('bad', {keyPath: '', autoIncrement: true})),
      sync(() => db.createObjectStore('bad', {keyPath: ['a'], autoIncrement: true})),
      sync(() => s.createIndex('by_name', 'x')),
      sync(() => s.createIndex('bad', 'a..b')),
      sync(() => s.createIndex('bad', ['a', 'b'], {multiEntry: true})),
      sync(() => s.index('none')),
      sync(() => db.deleteObjectStore('none')),
    ]);
  });
  out.firstOpen = first.log;
  const db = first.db;
  out.db = [db.name, db.version, names(db.objectStoreNames), Object.prototype.toString.call(db.objectStoreNames),
    db.objectStoreNames.contains('s'), db.objectStoreNames.item(0), db.objectStoreNames.item(9),
    db.objectStoreNames.length];
  out.databases = await indexedDB.databases();

  // Transactions' errors.
  out.transactionErrors = [
    sync(() => db.transaction('none')), sync(() => db.transaction([])), sync(() => db.transaction('s', 'versionchange')),
    sync(() => db.transaction('s', 'bad')), sync(() => db.transaction('s').objectStore('auto')),
    sync(() => db.createObjectStore('x')), sync(() => db.deleteObjectStore('s')),
    sync(() => { const tx = db.transaction(['s', 'auto', 's'], 'readwrite', {durability: 'strict'});
      return [names(tx.objectStoreNames), tx.mode, tx.durability, tx.db === db, tx.error]; }),
    sync(() => db.transaction('s', 'readonly', {durability: 'bad'})),
  ];

  // Writes and reads.
  {
    const tx = db.transaction(['s', 'auto', 'kp', 'plain'], 'readwrite');
    const done = finished(tx);
    const s = tx.objectStore('s');
    const results = [];
    results.push(await request(s.put({id: 1, name: 'one', tags: ['x', 'y', 'x'], mail: 'a@b'})));
    results.push(await request(s.add({id: 2, name: 'two', tags: 'y', mail: 'b@b'})));
    results.push(await request(s.put({id: 3, name: 'one', tags: [], mail: 'c@b'})));
    results.push(await request(s.add({id: 1, name: 'dup'})));
    results.push(await request(s.put({id: 4, name: 'four', mail: 'a@b'})));
    results.push(sync(() => s.put({name: 'no id'})));
    results.push(sync(() => s.put({id: {}})));
    results.push(sync(() => s.put({id: 5, f() {}})));
    results.push(sync(() => s.put({id: 5}, 5)));
    const auto = tx.objectStore('auto');
    results.push(await request(auto.add('a')));
    results.push(await request(auto.add('b')));
    results.push(await request(auto.put('c', 10)));
    results.push(await request(auto.add('d')));
    results.push(await request(auto.put('e', 2.5)));
    results.push(await request(auto.add('f')));
    results.push(await request(auto.put('g', 'str')));
    results.push(await request(auto.add('h')));
    const kp = tx.objectStore('kp');
    results.push(await request(kp.put({k: {}, v: 1})));
    results.push(await request(kp.put({k: {x: 7}, v: 2})));
    results.push(await request(kp.put({v: 3})));
    results.push(await request(kp.get(1)));
    results.push(await request(kp.getAll()));
    results.push(sync(() => kp.put({k: 5})));
    const plain = tx.objectStore('plain');
    const keys = [3, -1, 0, -0, Infinity, -Infinity, 'b', 'a', '', new Date(10), new Date(-5), [1], [1, 'a'], [],
      [[]], new Uint8Array([2]), new Uint8Array([1, 255]).buffer, new Uint8Array([]), 1e-300];
    for (const key of keys) plain.put(show(key), key);
    results.push(await request(plain.getAllKeys()));
    results.push(await request(plain.getAll(IDBKeyRange.bound(0, 'b'), 4)));
    results.push(await request(plain.count()));
    results.push(await request(plain.count(IDBKeyRange.lowerBound('a'))));
    results.push(await request(plain.getKey(IDBKeyRange.lowerBound(1, true))));
    results.push(await request(plain.get([1, 'a'])));
    results.push(await request(plain.get(IDBKeyRange.upperBound(0, true))));
    results.push(await request(plain.delete(IDBKeyRange.bound('a', 'z'))));
    results.push(await request(plain.getAllKeys(null, 3)));
    results.push(sync(() => plain.getAll(null, -1)));
    results.push(sync(() => plain.get()));
    results.push(sync(() => plain.get({})));
    results.push(await request(plain.put({d: new Date(1), m: new Map([[1, 'a']]), s: new Set([2]),
      b: new Uint16Array([1, 2]), n: NaN, nested: {a: [1, {b: 2}]}}, 'values')));
    results.push(await request(plain.get('values')));
    results.push(await request(plain.put('x'.repeat(200000), 'large')));
    results.push(await request(plain.get('large')).then(([kind, v]) => [kind, v.length]));
    out.writes = results;
    out.writesDone = await done;
  }

  // Indexes and cursors.
  {
    const tx = db.transaction('s', 'readonly');
    const done = finished(tx);
    const s = tx.objectStore('s');
    const results = [];
    results.push(await request(s.index('by_name').get('one')));
    results.push(await request(s.index('by_name').getKey('one')));
    results.push(await request(s.index('by_name').getAll('one')));
    results.push(await request(s.index('by_name').getAllKeys()));
    results.push(await request(s.index('by_tag').getAllKeys()));
    results.push(await request(s.index('by_tag').count('y')));
    results.push(await request(s.index('compound').getAll()));
    results.push(await request(s.index('unique_mail').getAllKeys()));
    for (const direction of ['next', 'prev', 'nextunique', 'prevunique']) {
      results.push(await within(new Promise((resolve) => {
        const seen = [];
        const r = s.index('by_name').openCursor(null, direction);
        r.onsuccess = () => {
          const cursor = r.result;
          if (!cursor) return resolve([direction, seen]);
          seen.push([cursor.key, cursor.primaryKey, cursor.value.id, cursor.direction,
            Object.prototype.toString.call(cursor), cursor.source === s.index('by_name'), cursor.request === r]);
          cursor.continue();
        };
      })));
    }
    results.push(await within(new Promise((resolve) => {
      const seen = [];
      const r = s.openKeyCursor(IDBKeyRange.lowerBound(2), 'prev');
      r.onsuccess = () => {
        const cursor = r.result;
        if (!cursor) return resolve(seen);
        seen.push([cursor.key, cursor.primaryKey, 'value' in cursor]);
        cursor.continue();
      };
    })));
    results.push(await within(new Promise((resolve) => {
      const seen = [];
      let step = 0;
      const r = s.index('by_name').openCursor();
      r.onsuccess = () => {
        const cursor = r.result;
        if (!cursor) return resolve(seen);
        seen.push([cursor.key, cursor.primaryKey]);
        step++;
        if (step === 1) {
          seen.push(sync(() => cursor.continue('a')));
          seen.push(sync(() => { cursor.continue(); cursor.continue(); }));
        } else if (step === 2) {
          seen.push(sync(() => cursor.continuePrimaryKey('one', 3)));
          cursor.continuePrimaryKey('two', 2);
        } else {
          seen.push(sync(() => cursor.update({id: 9})));
          seen.push(sync(() => cursor.delete()));
          seen.push(sync(() => cursor.advance(0)));
          cursor.advance(2);
        }
      };
    })));
    out.reads = results;
    out.readsDone = await done;
  }

  // Cursor writes.
  {
    const tx = db.transaction('s', 'readwrite');
    const done = finished(tx);
    const s = tx.objectStore('s');
    const results = await within(new Promise((resolve) => {
      const seen = [];
      const r = s.openCursor();
      r.onsuccess = () => {
        const cursor = r.result;
        if (!cursor) return resolve(seen);
        if (cursor.key === 1) {
          seen.push(sync(() => cursor.update({id: 99})));
          const u = cursor.update({...cursor.value, name: 'uno'});
          u.onsuccess = () => seen.push(['updated', show(u.result)]);
        } else if (cursor.key === 2) {
          const d = cursor.delete();
          d.onsuccess = () => seen.push(['deleted', show(d.result)]);
        }
        cursor.continue();
      };
    }));
    out.cursorWrites = [results, await request(s.getAll())];
    out.cursorWritesDone = await done;
  }

  // Aborts, errors that abort, and commit().
  {
    const tx = db.transaction('plain', 'readwrite');
    const done = finished(tx);
    const store = tx.objectStore('plain');
    const r1 = store.put(1, 'aborted');
    const r2 = store.get('aborted');
    const events = [];
    r1.onerror = () => events.push(['r1 error', error(r1.error)]);
    r2.onerror = () => events.push(['r2 error', error(r2.error)]);
    tx.abort();
    events.push(sync(() => tx.abort()));
    events.push(sync(() => store.get(1)));
    out.abort = [await done, events, r1.readyState, sync(() => r1.result), error(r1.error)];
  }
  {
    const tx = db.transaction('s', 'readwrite');
    const done = finished(tx);
    const r = tx.objectStore('s').add({id: 3, name: 'dup'});
    const events = [];
    r.onerror = (e) => events.push(['request error', error(r.error), e.bubbles, e.cancelable]);
    tx.onerror = (e) => events.push(['transaction error', e.target === r, error(tx.error)]);
    db.onerror = (e) => events.push(['database error', e.target === r]);
    out.errorAborts = [await done, events];
    db.onerror = null;
  }
  {
    const tx = db.transaction('plain', 'readwrite');
    const done = finished(tx);
    const store = tx.objectStore('plain');
    store.put('c', 'committed');
    tx.commit();
    out.commit = [sync(() => store.put('d', 'late')), sync(() => tx.commit()), await done,
      await request(db.transaction('plain').objectStore('plain').get('committed'))];
  }
  {
    const tx = db.transaction('plain', 'readonly');
    out.readOnly = [sync(() => tx.objectStore('plain').put(1, 1)), sync(() => tx.objectStore('plain').clear()),
      await finished(tx)];
  }
  {
    const tx = db.transaction('plain', 'readwrite');
    const done = finished(tx);
    await new Promise((resolve) => setTimeout(resolve, 0));
    out.inactive = [sync(() => tx.objectStore('plain').put(1, 1)), await done];
  }
  {
    // A handler that throws aborts its transaction.
    const tx = db.transaction('plain', 'readwrite');
    const done = finished(tx);
    const r = tx.objectStore('plain').get('committed');
    window.addEventListener('error', (e) => e.preventDefault(), {once: true});
    r.onsuccess = () => {
      throw new Error('handler threw');
    };
    out.handlerThrew = await done;
  }

  // Another connection, versionchange and blocked.
  {
    const log = [];
    db.onversionchange = (e) => log.push(['versionchange', e.oldVersion, e.newVersion]);
    const second = open('a', 2, (db2, tx, upgradeLog) => {
      upgradeLog.push(['stores in upgrade', names(db2.objectStoreNames)]);
      db2.deleteObjectStore('plain');
      tx.objectStore('s').name = 'renamed';
      tx.objectStore('renamed').index('by_name').name = 'by_name2';
      tx.objectStore('renamed').deleteIndex('by_tag');
    });
    await new Promise((resolve) => setTimeout(resolve, 50));
    log.push(['closing']);
    db.close();
    log.push(sync(() => db.transaction('s')));
    const result = await second;
    const renamed = result.db.transaction('renamed');
    out.versionChange = [log, result.log, names(result.db.objectStoreNames),
      names(renamed.objectStore('renamed').indexNames)];
    // A connection with a transaction closes once it ends: whether the next
    // open hears blocked would depend on which comes first, in Chrome too.
    await finished(renamed);
    // An upgrade that aborts leaves the version as it was.
    const closeEvents = [];
    result.db.onclose = () => closeEvents.push('close');
    result.db.close();
    const aborted = await open('a', 3, (db3, tx) => {
      db3.createObjectStore('new');
      tx.abort();
    });
    out.abortedUpgrade = [aborted.log, closeEvents];
    const lower = await open('a', 1);
    out.lowerVersion = lower.log;
    const again = await open('a');
    out.reopen = [again.log, again.db.version];
    again.db.close();
  }

  // Delete, with a connection that does not close.
  {
    const keep = await open('b', 5);
    const log = [];
    keep.db.onversionchange = (e) => log.push(['versionchange', e.oldVersion, e.newVersion]);
    const deleting = remove('b');
    await new Promise((resolve) => setTimeout(resolve, 50));
    log.push('closing');
    keep.db.close();
    out.deleteBlocked = [log, await deleting, await remove('none'), await indexedDB.databases()];
  }

  // Events constructed by a page.
  out.events = [
    sync(() => { const e = new IDBVersionChangeEvent('versionchange', {oldVersion: 2, newVersion: 3});
      return [e.type, e.oldVersion, e.newVersion, e.bubbles]; }),
    sync(() => { const e = new IDBVersionChangeEvent('x'); return [e.oldVersion, e.newVersion]; }),
  ];
  return out;
})()
