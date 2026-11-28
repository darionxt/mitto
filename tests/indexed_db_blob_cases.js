// What a page sees of the blobs that it keeps in IndexedDB, which
// tests/test_page_indexed_db.py runs in a Page and in a dedicated worker, and
// compares with what Chrome 157 gave for the same script on the same kind of
// page, at http://127.0.0.1:<port>/. It settles with an object of outcomes.
(async () => {
  const out = {};
  const error = (e) => (e ? `${e.constructor?.name} ${e.name}: ${e.message}` : null);
  const caseOf = async (name, f) => {
    try {
      out[name] = await f();
    } catch (e) {
      out[name] = error(e);
    }
  };
  // A blob or a file, with its bytes as text if it is short.
  const show = async (v) => {
    if (v === undefined) return 'undefined';
    if (v === null || typeof v !== 'object') return v;
    if (v instanceof Blob) {
      const seen = {is: Object.prototype.toString.call(v), type: v.type, size: v.size};
      if (v instanceof File) {
        Object.assign(seen, {name: v.name, lastModified: v.lastModified, path: v.webkitRelativePath});
      }
      const text = await v.text();
      seen.text = text.length > 40 ? text.length : text;
      return seen;
    }
    if (v instanceof Map) return {map: await Promise.all([...v].map(async ([k, x]) => [await show(k), await show(x)]))};
    if (v instanceof Set) return {set: await Promise.all([...v].map(show))};
    if (Array.isArray(v)) return Promise.all(v.map(show));
    return Object.fromEntries(await Promise.all(Object.entries(v).map(async ([k, x]) => [k, await show(x)])));
  };
  const result = (r) => new Promise((resolve, reject) => {
    r.onsuccess = () => resolve(r.result);
    r.onerror = (e) => {
      e.preventDefault();
      reject(r.error);
    };
  });
  const finished = (tx) => new Promise((resolve) => {
    tx.oncomplete = () => resolve('complete');
    tx.onabort = () => resolve(`abort ${error(tx.error)}`);
  });
  const open = (name, upgrade, version) => new Promise((resolve, reject) => {
    const r = indexedDB.open(name, version);
    r.onupgradeneeded = () => upgrade(r.result, r.transaction);
    r.onsuccess = () => resolve(r.result);
    r.onerror = () => reject(r.error);
  });
  const write = async (db, f) => {
    const tx = db.transaction('s', 'readwrite');
    f(tx.objectStore('s'), tx);
    return finished(tx);
  };
  const read = (db, f) => result(f(db.transaction('s').objectStore('s')));
  // A database of its own for a case, with a store 's'.
  const withStore = async (name, f, options) => {
    const db = await open(name, (d) => d.createObjectStore('s', options));
    try {
      return await f(db);
    } finally {
      db.close();
      await result(indexedDB.deleteDatabase(name));
    }
  };

  await caseOf('blobs and files of every kind', () => withStore('kinds', async (db) => {
    const values = {
      empty: new Blob(),
      text: new Blob(['hello ', 'wörld'], {type: 'TEXT/Plain;Charset=UTF-8'}),
      badType: new Blob(['x'], {type: 'té/xt'}),
      bytes: new Blob([new Uint8Array([0xff, 0, 0xfe])], {type: 'application/octet-stream'}),
      slice: new Blob(['0123456789'], {type: 'a/b'}).slice(2, 5, 'c/d'),
      sliceOfNone: new Blob(['0123456789']).slice(4, 4),
      composite: new Blob([new Blob(['ab']), new Uint8Array([99]), new Blob(['defg']).slice(0, 2)]),
      file: new File(['contents'], 'a name.txt', {type: 'text/plain', lastModified: 1234567890123}),
      oldFile: new File([], '', {lastModified: -1}),
      nameOfAll: new File(['x'], '文件 \ud800\u0001/..\\x', {lastModified: 0}),
      farFile: new File(['x'], 'far', {lastModified: 8.64e15}),
    };
    const done = await write(db, (s) => {
      for (const [k, v] of Object.entries(values)) s.put(v, k);
    });
    const seen = {};
    for (const k of Object.keys(values)) seen[k] = await show(await read(db, (s) => s.get(k)));
    return {done, seen};
  }));

  await caseOf('a file with no time is of when it was made', () => withStore('now', async (db) => {
    const file = new File(['x'], 'now');
    await write(db, (s) => s.put(file, 1));
    const kept = await read(db, (s) => s.get(1));
    return kept.lastModified === file.lastModified;
  }));

  await caseOf('a value of many blobs', () => withStore('nested', async (db) => {
    const blob = new Blob(['shared'], {type: 'x/y'});
    const file = new File(['f'], 'f', {lastModified: 5});
    await write(db, (s) => s.put({
      a: blob, b: blob, copy: structuredClone(blob), list: [blob, file], map: new Map([[blob, file]]),
      set: new Set([file]), buffer: new Uint8Array(70000), text: 'z'.repeat(70000),
    }, 1));
    const kept = await read(db, (s) => s.get(1));
    const buffer = kept.buffer.length;
    const text = kept.text.length;
    delete kept.buffer;
    delete kept.text;
    return {
      buffer, text, seen: await show(kept),
      same: [kept.a === kept.b, kept.a === kept.copy, kept.a === kept.list[0], kept.list[1] === [...kept.set][0],
        kept.map.has(kept.a)],
    };
  }));

  await caseOf('each read gives a blob of its own', () => withStore('each', async (db) => {
    await write(db, (s) => s.put(new Blob(['x']), 1));
    const request = db.transaction('s').objectStore('s').get(1);
    const first = await result(request);
    const second = await read(db, (s) => s.get(1));
    return [first === request.result, first === second, await second.text()];
  }));

  await caseOf('read before the commit, and after an abort', () => withStore('abort', async (db) => {
    let seen;
    const done = await write(db, (s, tx) => {
      s.put({b: new Blob(['early'])}, 1);
      s.get(1).onsuccess = (e) => {
        seen = e.target.result;
        tx.abort();
      };
    });
    return {done, seen: await show(seen), left: await read(db, (s) => s.count())};
  }));

  await caseOf('read after the record, the store and the database went', async () => {
    const db = await open('gone', (d) => d.createObjectStore('s'));
    await write(db, (s) => {
      s.put(new Blob(['one']), 1);
      s.put(new Blob(['two']), 2);
      s.put(new Blob(['three']), 3);
    });
    const [one, two, three] = [await read(db, (s) => s.get(1)), await read(db, (s) => s.get(2)),
      await read(db, (s) => s.get(3))];
    await write(db, (s) => s.delete(1));
    await write(db, (s) => s.put('other', 2));
    await write(db, (s) => s.clear());
    db.close();
    await result(indexedDB.deleteDatabase('gone'));
    return show([one, two, three]);
  });

  await caseOf('getAll, getAllRecords and cursors', async () => {
    const db = await open('all', (d) => {
      const s = d.createObjectStore('s', {keyPath: 'id', autoIncrement: true});
      s.createIndex('type', 'blob.type');
      s.createIndex('size', 'blob.size');
    });
    await write(db, (s) => {
      s.put({blob: new Blob(['b'], {type: 'b/b'})});
      s.put({blob: new Blob(['aa'], {type: 'a/a'})});
      s.put({id: 10, blob: new File(['ccc'], 'c', {type: 'c/c', lastModified: 3})});
      s.put({blob: 'none'});
    });
    const cursorOver = (source) => new Promise((resolve, reject) => {
      const seen = [];
      const r = source(db.transaction('s').objectStore('s'));
      r.onerror = () => reject(r.error);
      r.onsuccess = () => {
        if (!r.result) return resolve(seen);
        seen.push([r.result.key, r.result.primaryKey, r.result.value]);
        r.result.continue();
      };
    });
    const records = await read(db, (s) => s.getAllRecords({direction: 'prev'}));
    const seen = {
      all: await show(await read(db, (s) => s.getAll())),
      records: await show(records.map((r) => [r.key, r.primaryKey, r.value])),
      byType: await show(await read(db, (s) => s.index('type').getAll())),
      ofSize: await show(await read(db, (s) => s.index('size').get(2))),
      key: await read(db, (s) => s.index('type').getKey('c/c')),
      cursor: await show(await cursorOver((s) => s.openCursor())),
      byIndex: await show(await cursorOver((s) => s.index('size').openCursor(null, 'prev'))),
      advanced: await show(await new Promise((resolve, reject) => {
        const seen = [];
        const r = db.transaction('s').objectStore('s').index('size').openCursor();
        r.onerror = () => reject(r.error);
        r.onsuccess = () => {
          if (!r.result) return resolve(seen);
          seen.push([r.result.key, r.result.value]);
          r.result.advance(2);
        };
      })),
    };
    db.close();
    // An index over what is there takes its keys from the blobs that it reads.
    const again = await open('all', (d, tx) => {
      const s = tx.objectStore('s');
      s.createIndex('name', 'blob.name');
      s.createIndex('modified', ['blob.lastModified', 'blob.size']);
    }, 2);
    seen.byName = await show(await read(again, (s) => s.index('name').getAllKeys()));
    seen.modified = await show(await result(again.transaction('s').objectStore('s').index('modified').getAllRecords())
      .then((r) => r.map((x) => [x.key, x.primaryKey])));
    again.close();
    await result(indexedDB.deleteDatabase('all'));
    return seen;
  });

  await caseOf('a cursor updates a blob', () => withStore('update', async (db) => {
    await write(db, (s) => {
      s.put({n: 1, blob: new Blob(['old'])}, 1);
      s.openCursor(1).onsuccess = (e) => {
        const cursor = e.target.result;
        cursor.update({...cursor.value, blob: new Blob([cursor.value.blob, '+new'], {type: 't/t'})});
      };
    });
    return show(await read(db, (s) => s.get(1)));
  }));

  await caseOf('what fails keeps the blob before', () => withStore('fails', async (db) => {
    await write(db, (s) => s.put(new Blob(['kept']), 1));
    let failed;
    const done = await write(db, (s) => {
      const r = s.add(new Blob(['not']), 1);
      r.onerror = (e) => {
        e.preventDefault();
        failed = error(r.error);
      };
    });
    let thrown;
    await write(db, (s) => {
      try {
        s.put({blob: new Blob(['x']), f() {}}, 2);
      } catch (e) {
        thrown = error(e);
      }
    });
    return {done, failed, thrown, kept: await show(await read(db, (s) => s.getAll()))};
  }));

  await caseOf('what reads a kept blob', () => withStore('readers', async (db) => {
    await write(db, (s) => s.put(new File(['0123456789'], 'n', {type: 'text/x', lastModified: 7}), 1));
    const file = await read(db, (s) => s.get(1));
    const url = URL.createObjectURL(file);
    const response = await fetch(url);
    const seen = {
      type: response.headers.get('content-type'),
      fetched: await response.text(),
      slice: await show(file.slice(3, 6, 'a/b')),
      buffer: (await file.arrayBuffer()).byteLength,
      stream: (await file.stream().getReader().read()).value.length,
      reader: await new Promise((resolve) => {
        const r = new FileReader();
        r.onloadend = () => resolve(r.result);
        r.readAsDataURL(file);
      }),
      clone: await show(structuredClone(file)),
      form: await new Response(file).text(),
    };
    URL.revokeObjectURL(url);
    const {port1, port2} = new MessageChannel();
    const got = new Promise((resolve) => {
      port2.onmessage = (e) => resolve(e.data);
    });
    port1.postMessage(file);
    seen.message = await show(await got);
    port1.close();
    // Kept again, beside what a blob of it makes.
    await write(db, (s) => s.put([file, new Blob([file, '!'])], 2));
    seen.again = await show(await read(db, (s) => s.get(2)));
    return seen;
  }));

  await caseOf('blobs of more than a message', () => withStore('larger', async (db) => {
    const mib = 1024 * 1024;
    // Bytes that say where they are.
    const bytes = (size) => {
      const view = new Uint32Array(size / 4);
      for (let i = 0; i < view.length; i += 1024) view[i] = i;
      return view;
    };
    const value = {
      n: 1, one: new Blob([bytes(mib)]), empty: new Blob(), two: new Blob([bytes(2 * mib)]),
      most: new Blob([bytes(18 * mib)]), slice: new Blob([bytes(3 * mib)]).slice(mib, 2 * mib + 4096),
    };
    const done = await write(db, (s) => {
      s.put(value, 1);
      s.put('kept', 2);
    });
    const kept = await read(db, (s) => s.get(1));
    const where = async (blob, from) => new Uint32Array(await blob.slice(from, from + 4).arrayBuffer())[0] * 4;
    return {
      done, sizes: Object.entries(kept).map(([k, v]) => [k, v.size]), all: (await read(db, (s) => s.getAll())).length,
      one: await where(kept.one, mib - 4096), two: await where(kept.two, mib),
      most: [await where(kept.most, 0), await where(kept.most, 17 * mib), await where(kept.most, 18 * mib - 4096)],
      slice: [await where(kept.slice, 0), await where(kept.slice, mib)],
    };
  }));

  await caseOf('more than one message of blobs', () => withStore('large', async (db) => {
    const size = 3 * 1024 * 1024;
    await write(db, (s) => {
      for (let i = 0; i < 8; ++i) s.put(new Blob([new Uint8Array(size).fill(i)]), i);
    });
    const all = await read(db, (s) => s.getAll());
    const bytes = await Promise.all(all.map(async (b) => [b.size, new Uint8Array(await b.slice(-1).arrayBuffer())[0]]));
    const walked = await new Promise((resolve) => {
      let total = 0;
      const r = db.transaction('s').objectStore('s').openCursor();
      r.onsuccess = () => {
        if (!r.result) return resolve(total);
        total += r.result.value.size;
        r.result.continue();
      };
    });
    return {bytes, walked};
  }));

  return out;
})();
