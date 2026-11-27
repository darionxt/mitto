// XMLHttpRequest, asynchronous and synchronous, in a window and in a worker:
// its events in their order, its errors, what it sends of each kind of body and
// of the lines that a script sets, and what it gives of each kind of response.
// It runs in a top-level page at an HTTP origin, on the server of
// tests/test_page_xhr.py, and gives a promise of what each case saw, which that
// test compares with what Chrome 157 gave for it.
(async () => {
  const record = {};
  const here = location.origin;
  const cross = here.replace('127.0.0.1', 'localhost');
  const withUser = here.replace('//', '//a:b@');
  const crossUser = cross.replace('//', '//a:b@');
  const clean = (value) => JSON.parse(JSON.stringify(value === undefined ? null : value)
      .replaceAll(withUser, 'USER').replaceAll(crossUser, 'CROSSUSER').replaceAll(here, 'HERE').replaceAll(cross, 'CROSS')
      .replace(/blob:HERE\/[0-9a-f-]+/g, 'blob:HERE/U').replace(/boundary=[^ "]+/g, 'boundary=B'));
  const caseOf = async (name, fn) => {
    try {
      record[name] = clean(await fn());
    } catch (e) {
      record[name] = `threw ${e && e.name}: ${e && e.message}`;
    }
  };
  const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const EVENTS = ['loadstart', 'progress', 'abort', 'error', 'load', 'timeout', 'loadend'];
  // Logs each event of `xhr`, and of its upload if `upload`.
  const watch = (xhr, upload = false, log = []) => {
    xhr.addEventListener('readystatechange', () => log.push(`readystatechange ${xhr.readyState}`));
    const listen = (target, prefix) => {
      for (const type of EVENTS) {
        target.addEventListener(type, (e) => log.push(`${prefix}${type} ${e.lengthComputable}/${e.loaded}/${e.total}`));
      }
    };
    listen(xhr, '');
    if (upload) listen(xhr.upload, 'upload ');
    return log;
  };
  const ended = (xhr) => new Promise((resolve) => xhr.addEventListener('loadend', () => setTimeout(resolve, 0)));
  const tried = (f) => {
    try {
      const value = f();
      return value === undefined ? 'ok' : 'ok ' + value;
    } catch (e) {
      return `${e.name}: ${e.message}`;
    }
  };
  const show = (xhr) => [
    xhr.readyState, xhr.status, xhr.statusText, xhr.responseURL, tried(() => xhr.responseText),
    xhr.getAllResponseHeaders().split('\r\n').filter((line) => line && !/^(date|server|connection)/.test(line)),
  ];
  // Sends `body`, and gives the events and what the server saw.
  let paths = 0;
  const send = async (body, {method = 'POST', headers = [], upload = true, async = true} = {}) => {
    const path = `/log/${++paths}`;
    const xhr = new XMLHttpRequest();
    const log = watch(xhr, upload);
    xhr.open(method, path, async);
    for (const [name, value] of headers) xhr.setRequestHeader(name, value);
    const end = ended(xhr);
    xhr.send(body);
    await end;
    const seen = await (await fetch(path.replace('/log', '/seen'))).json();
    return {seen, upload: log.filter((line) => line.startsWith('upload')), status: xhr.status};
  };
  const echo = async (body, type, responseType = '') => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/echo');
    xhr.overrideMimeType(type);
    xhr.responseType = responseType;
    const end = ended(xhr);
    xhr.send(body);
    await end;
    return xhr;
  };
  const inWorker = (script) => new Promise((resolve) => {
    const source = `
      const here = '${here}'; const cross = '${cross}'; const EVENTS = ${JSON.stringify(EVENTS)};
      const wait = ${wait}; const watch = ${watch}; const ended = ${ended}; const tried = ${tried}; const show = ${show};
      (async () => {
        try { postMessage(await (async () => { ${script} })()); } catch (e) { postMessage('threw ' + e); }
      })();`;
    const worker = new Worker(URL.createObjectURL(new Blob([source])));
    worker.onmessage = (e) => resolve(e.data);
    worker.onerror = (e) => resolve('error ' + e.message);
  });

  await caseOf('events', async () => {
    const out = {};
    for (const path of ['/bytes/5', '/bytes/0', '/redirect', '/status/404']) {
      const xhr = new XMLHttpRequest();
      const log = watch(xhr);
      xhr.open('GET', path);
      const end = ended(xhr);
      xhr.send();
      log.push('sent');
      await end;
      out[path] = [log, show(xhr)];
    }
    return out;
  });

  await caseOf('abort', async () => {
    const at = {};
    for (const when of ['sent', 2, 3, 4]) {
      const xhr = new XMLHttpRequest();
      const log = watch(xhr, true);
      xhr.open('POST', '/echo');
      const end = ended(xhr);
      if (when !== 'sent') {
        xhr.onreadystatechange = () => {
          if (xhr.readyState !== when) return;
          xhr.onreadystatechange = null;
          log.push('aborting');
          xhr.abort();
          log.push('aborted ' + xhr.readyState);
        };
      }
      xhr.send('body');
      if (when === 'sent') xhr.abort();
      // A request that is aborted once it is done fires no more events.
      await Promise.race([end, wait(300)]);
      await wait(20);
      at[when] = [log, show(xhr)];
    }
    return at;
  });

  await caseOf('failures', async () => {
    const out = {};
    for (const url of [cross + '/bytes/5', cross + '/cors/5', 'http://127.0.0.1:1/', 'foo:bar', 'about:blank', 'file:///x',
      'javascript:1', 'ftp://127.0.0.1/', 'blob:' + here + '/none', 'data:', withUser + '/bytes/1']) {
      const xhr = new XMLHttpRequest();
      const log = watch(xhr);
      xhr.open('GET', url);
      const end = ended(xhr);
      log.push(tried(() => xhr.send()));
      await end;
      out[url] = [log, show(xhr)];
    }
    return out;
  });

  await caseOf('timeout', async () => {
    const out = {};
    for (const late of [false, true]) {
      const xhr = new XMLHttpRequest();
      const log = watch(xhr);
      xhr.open('GET', '/never');
      if (!late) xhr.timeout = 50;
      const end = ended(xhr);
      xhr.send();
      if (late) {
        await wait(50);
        xhr.timeout = 10;
        log.push('set');
      }
      await end;
      out[late ? 'set late' : 'set first'] = [log, show(xhr)];
    }
    return out;
  });

  await caseOf('reopening', async () => {
    const xhr = new XMLHttpRequest();
    const log = watch(xhr);
    xhr.open('GET', '/bytes/5');
    xhr.onloadstart = () => {
      xhr.onloadstart = null;
      log.push('again');
      xhr.open('GET', '/bytes/6');
      xhr.send();
    };
    xhr.send();
    xhr.open('GET', '/bytes/7');
    log.push('reopened ' + xhr.readyState);
    const end = ended(xhr);
    xhr.send();
    await end;
    return [log, show(xhr)];
  });

  await caseOf('what a request leaves to the next', async () => {
    const out = [];
    // A progress event that waits as another request starts: the second piece of the body comes within 50 ms of the
    // first, or else the request is tried again.
    for (const sends of [false, true]) {
      for (let tries = 0; tries < 50; ++tries) {
        const xhr = new XMLHttpRequest();
        const log = watch(xhr);
        const first = new Promise((resolve) => xhr.addEventListener('progress', resolve, {once: true}));
        xhr.open('GET', '/pieces');
        xhr.send();
        await first;
        while (xhr.responseText.length === 10) await wait(1);
        const waits = log.at(-1) === 'progress true/10/2000';
        log.push('open');
        xhr.open('GET', '/never/' + sends);
        if (sends) xhr.send();
        await wait(200);
        xhr.abort();
        if (!waits) continue;
        out.push(log);
        break;
      }
    }
    // An upload that ends before its first report, after another.
    const xhr = new XMLHttpRequest();
    const log = watch(xhr, true);
    xhr.open('POST', '/echo');
    let end = ended(xhr);
    xhr.send('0123456789');
    await end;
    for (const body of ['abc', null]) {
      xhr.open('POST', '/echo');
      xhr.send(body);
      xhr.abort();
      log.push('next');
    }
    xhr.open('POST', 'http://127.0.0.1:1/');
    end = ended(xhr);
    xhr.send('abcd');
    await end;
    out.push(log.filter((line) => line === 'next' || line.startsWith('upload')));
    return out;
  });

  await caseOf('reopening from listeners', async () => {
    const out = {};
    const moments = {
      'readystatechange 2': ['readystatechange', (xhr) => xhr.readyState === 2],
      'readystatechange 3': ['readystatechange', (xhr) => xhr.readyState === 3],
      'readystatechange 4': ['readystatechange', (xhr) => xhr.readyState === 4],
      progress: ['progress', () => true], load: ['load', () => true], loadend: ['loadend', () => true],
      'upload load': ['upload load', () => true], 'upload loadstart': ['upload loadstart', () => true],
    };
    const actions = {
      open: (xhr) => xhr.open('GET', '/bytes/7'),
      'open and send': (xhr) => { xhr.open('GET', '/bytes/7'); xhr.send(); },
      abort: (xhr) => xhr.abort(),
      'abort and open': (xhr) => { xhr.abort(); xhr.open('GET', '/bytes/7'); },
    };
    const one = async (type, is, act, url, responseType) => {
      const xhr = new XMLHttpRequest();
      const log = [];
      let acted = false;
      let done;
      const acts = new Promise((resolve) => done = resolve);
      const listener = () => {
        if (acted || !is(xhr)) return;
        acted = true;
        log.push('acts');
        log.push(tried(() => act(xhr)));
        done();
      };
      if (type.startsWith('upload ')) xhr.upload.addEventListener(type.slice(7), listener);
      else xhr.addEventListener(type, listener);
      watch(xhr, true, log);
      xhr.open('POST', url);
      xhr.responseType = responseType;
      if (responseType === 'document') xhr.overrideMimeType('text/html');
      const finish = ended(xhr);
      xhr.send('<p>hello');
      // Until the listener has acted, or the request has ended without, and nothing has come of it for a while.
      await Promise.race([acts, finish]);
      for (let length = -1; length !== log.length; await wait(300)) length = log.length;
      const response = responseType === 'arraybuffer' ? xhr.response && xhr.response.byteLength
                       : responseType === 'document' ? xhr.response && xhr.response.body.innerHTML : xhr.responseText;
      const d = responseType === 'arraybuffer' ? null : xhr.responseXML;
      return [log, xhr.readyState, xhr.status, xhr.responseURL, response, d && [d.URL, d.contentType, d.characterSet]];
    };
    for (const [moment, [type, is]] of Object.entries(moments)) {
      await Promise.all(Object.entries(actions).map(async ([action, act]) => {
        const kinds = [['/echo', ''], ['/empty', ''], ['/echo', 'arraybuffer'], ['/echo', 'document'], ['/empty', 'document']];
        await Promise.all(kinds.map(async ([url, responseType]) => {
          out[`${moment}, ${action}, ${url} ${responseType}`] = await one(type, is, act, url, responseType);
        }));
      }));
    }
    return Object.fromEntries(Object.entries(out).sort());
  });

  await caseOf('checks', async () => {
    const xhr = new XMLHttpRequest();
    const out = [
      tried(() => xhr.open('GE T', '/')), tried(() => xhr.open('TRACE', '/')), tried(() => xhr.open('connect', '/')),
      tried(() => xhr.open('GET', 'http://[')), tried(() => xhr.open('GET')), tried(() => xhr.open('Ā', '/')),
      tried(() => xhr.send()), tried(() => xhr.setRequestHeader('a', 'b')), xhr.readyState,
      tried(() => xhr.responseType = 'nonsense'), xhr.responseType,
    ];
    xhr.open('get', '/bytes/1');
    out.push(
        tried(() => xhr.setRequestHeader('a b', 'c')), tried(() => xhr.setRequestHeader('a', 'b\n c')),
        tried(() => xhr.setRequestHeader('Host', 'x')), tried(() => xhr.withCredentials = true));
    const end = ended(xhr);
    xhr.send();
    out.push(
        tried(() => xhr.send()), tried(() => xhr.withCredentials = false), tried(() => xhr.setRequestHeader('a', 'b')),
        tried(() => xhr.overrideMimeType('text/plain')));
    xhr.onreadystatechange = () => {
      if (xhr.readyState === 3) {
        out.push(tried(() => xhr.responseType = 'json'), tried(() => xhr.overrideMimeType('a/b')));
      }
    };
    await end;
    xhr.responseType;
    out.push(tried(() => new XMLHttpRequestUpload()), tried(() => new XMLHttpRequestEventTarget()));
    const json = await echo('{}', 'text/plain', 'json');
    out.push(tried(() => json.responseText), tried(() => json.responseXML));
    return out;
  });

  await caseOf('bodies', async () => {
    const form = new FormData();
    form.append('a', 'b');
    const html = new DOMParser().parseFromString('<p>é', 'text/html');
    const xml = new DOMParser().parseFromString('<?xml version="1.0" encoding="utf-16"?><a>é</a>', 'text/xml');
    const plain = new Document();
    plain.append(plain.createElement('root'));
    const out = {};
    for (const [name, body] of [
      ['string', 'é\ud800'], ['empty', ''], ['null', null], ['undefined', undefined], ['number', 5],
      ['buffer', new Uint8Array([97, 98, 99]).buffer], ['view', new Uint8Array([97, 98, 99]).subarray(1)],
      ['data view', new DataView(new ArrayBuffer(4))], ['blob', new Blob(['xyz'])],
      ['typed blob', new Blob(['xyz'], {type: 'a/b;c=d'})], ['badly typed blob', new Blob(['xyz'], {type: 'nonsense'})],
      ['params', new URLSearchParams('a=1&b=é')], ['html', html], ['xml', xml], ['plain document', plain],
      ['created document', document.implementation.createDocument(null, 'root')],
      ['object', {toString: () => 'text'}],
    ]) {
      out[name] = await send(body);
    }
    const {seen} = await send(form);
    out.form = seen.headers['content-type'];
    return out;
  });

  await caseOf('content types', async () => {
    const out = {};
    const html = new DOMParser().parseFromString('', 'text/html');
    for (const type of ['text/plain', 'text/plain;charset=latin1', 'text/plain; charset = \'latin1\' ; a=b',
      'a/b;charset=x;charset=y', 'charset=latin1', 'text/plain;xcharset=latin1', 'text/x;CHARSET="UTF-8"']) {
      out[type] = (await send('x', {headers: [['Content-Type', type]]})).seen.headers['content-type'];
    }
    out.params = (await send(new URLSearchParams('a=1'), {headers: [['Content-Type', 'a/b;charset=c']]})).seen.headers['content-type'];
    out.document = (await send(html, {headers: [['Content-Type', 'a/b;charset=c']]})).seen.headers['content-type'];
    out.blob = (await send(new Blob(['x'], {type: 'x/y'}), {headers: [['Content-Type', 'a/b;charset=c']]})).seen.headers['content-type'];
    return out;
  });

  await caseOf('methods and lines', async () => {
    const out = {};
    for (const method of ['get', 'head', 'post', 'PUT', 'patch', 'delete', 'Custom']) {
      out[method] = await send('b', {method, headers: [['X-A', '1']]});
    }
    out.lines = (await send(null, {method: 'GET', headers: [
      ['X-A', '1'], ['x-a', ' 2 '], ['X-B', ''], ['X-B', 'x\tx'], ['X-B', ''], ['Accept', 'a/b'], ['Cookie', 'a=b'],
      ['Origin', 'http://x'], ['Referer', 'y'], ['Sec-A', '1'], ['Proxy-A', '1'], ['Content-Length', '3'],
    ]})).seen;
    const fetched = `/log/${++paths}`;
    await fetch(fetched, {headers: [['X-A', '1'], ['x-a', ''], ['X-B', ''], ['x-b', '2']]});
    out.fetchLines = (await (await fetch(fetched.replace('/log', '/seen'))).json());
    return out;
  });

  await caseOf('users and passwords', async () => {
    const out = [];
    // What the server saw of a request to its own origin or to another one.
    for (const [origin, async] of [[withUser, true], [crossUser, true], [withUser, false], [crossUser, false]]) {
      const path = `/log/${++paths}`;
      const xhr = new XMLHttpRequest();
      const log = watch(xhr);
      xhr.open('GET', origin + path, async);
      const end = ended(xhr);
      log.push(tried(() => xhr.send()));
      if (async) await end;
      const seen = await (await fetch(path.replace('/log', '/seen'))).json();
      out.push([log.at(-1), xhr.status, xhr.responseURL, seen.headers]);
    }
    // Redirects to a URL with a user, of this origin and of another one.
    for (const from of [here, cross]) {
      for (const to of [withUser + '/bytes/5', crossUser + '/cors/5', cross + '/cors/5']) {
        const url = `${from}/to/${encodeURIComponent(to)}`;
        const xhr = new XMLHttpRequest();
        xhr.open('GET', url);
        const end = ended(xhr);
        xhr.send();
        await end;
        out.push([from, to, xhr.status, xhr.responseURL]);
      }
    }
    return out;
  });

  await caseOf('exposed names', async () => {
    const out = [];
    for (const list of ['x-shown', 'x-shown, x-hidden', 'x-shown,, x-hidden,', ',x-shown', 'x-shown x-hidden', 'x-shown, not a name',
                        '"x-shown"', '*', '*, x-shown', 'x-shown, *, (']) {
      const url = `${cross}/expose/${encodeURIComponent(list)}`;
      const xhr = new XMLHttpRequest();
      xhr.open('GET', url);
      const end = ended(xhr);
      xhr.send();
      await end;
      out.push([list, show(xhr).at(-1), [...(await fetch(url)).headers.keys()]]);
    }
    return out;
  });

  await caseOf('names and values beyond ASCII', async () => {
    const xhr = new XMLHttpRequest();
    xhr.open('GET', cross + '/cors/5');
    const end = ended(xhr);
    xhr.send();
    await end;
    const out = [tried(() => xhr.getResponseHeader('\u00e9')), tried(() => xhr.setRequestHeader('\u00e9', '1')),
                 tried(() => xhr.open('\u00e9', '/')), tried(() => xhr.open('GET', '/')),
                 tried(() => xhr.setRequestHeader('\u00e9', '1')), tried(() => xhr.setRequestHeader('x', '\u00e9\n\u00e9')),
                 tried(() => xhr.setRequestHeader('cookie', '\u00e9'))];
    // A value goes as its bytes.
    const path = `/log/${++paths}`;
    const sent = new XMLHttpRequest();
    sent.open('GET', path);
    sent.setRequestHeader('X-A', '\u00e9');
    const sentEnd = ended(sent);
    sent.send();
    await sentEnd;
    out.push((await (await fetch(path.replace('/log', '/seen'))).json()).headers['x-a']);
    return out;
  });

  await caseOf('encoded bodies', async () => {
    const out = [];
    // Two lines of the size, and bodies of unusual sizes.
    for (const path of ['/lengths/2', '/lengths/3', '/empty', '/status/204']) {
      const xhr = new XMLHttpRequest();
      const log = watch(xhr);
      xhr.open('GET', path);
      const end = ended(xhr);
      xhr.send();
      await end;
      out.push([path, log.filter((line) => /^(progress|load) /.test(line)), xhr.getResponseHeader('content-length')]);
    }
    for (const encoding of ['gzip', 'x-gzip', 'GZIP', 'deflate', 'identity', 'foo', 'gzip, identity', 'foo, gzip', '']) {
      const xhr = new XMLHttpRequest();
      const log = watch(xhr);
      xhr.open('GET', '/encoded/' + encodeURIComponent(encoding));
      const end = ended(xhr);
      xhr.send();
      await end;
      out.push([encoding, log.filter((line) => /^(progress|load) /.test(line)), xhr.responseText.length]);
    }
    return out;
  });

  await caseOf('responses held back while they are sniffed', async () => {
    const out = [];
    let id = 0;
    for (const kind of ['text/plain', 'text/plain;charset=utf-8', 'application/octet-stream', 'text/xml', 'application/xml', '',
                        'text/html', 'application/json', 'text/javascript', 'x/y', 'application/msword',
                        'text/plain+application/json', 'application/json+text/plain']) {
      for (const options of kind === 'text/plain' ? ['', 'nosniff', 'long'] : ['']) {
        ++id;
        const path = `/held/${id}/${encodeURIComponent(`${kind}|${options}`)}`;
        const xhr = new XMLHttpRequest();
        xhr.open('GET', path);
        const end = ended(xhr);
        xhr.send();
        // A fetch() is not sniffed, so the server has sent the first piece by the time that it has one.
        const response = await fetch(path);
        const piece = await response.body.getReader().read();
        for (let waited = 0; waited < 300 && xhr.readyState < 3; waited += 10) await wait(10);
        const before = [xhr.readyState, xhr.status, xhr.responseText.length, xhr.getResponseHeader('content-type')];
        await fetch('/release/' + id);
        await end;
        out.push([kind, options, before, xhr.responseText.length, piece.value.length > 0]);
      }
    }
    return out;
  });

  await caseOf('response lines', async () => {
    const xhr = new XMLHttpRequest();
    xhr.open('GET', '/twice');
    const out = [xhr.getResponseHeader('x-twice'), xhr.getAllResponseHeaders()];
    const end = ended(xhr);
    xhr.send();
    await end;
    out.push(xhr.getResponseHeader('X-TWICE'), xhr.getResponseHeader('none'), xhr.getResponseHeader('set-cookie'),
             tried(() => xhr.getResponseHeader('Ā')), show(xhr)[5]);
    const other = new XMLHttpRequest();
    other.open('GET', cross + '/cors/5');
    const otherEnd = ended(other);
    other.send();
    await otherEnd;
    out.push(other.getResponseHeader('content-type'), other.getResponseHeader('x-hidden'), other.getResponseHeader('x-shown'),
             show(other)[5]);
    return out;
  });

  await caseOf('responses', async () => {
    const out = {};
    for (const [type, bodies] of Object.entries({
      'arraybuffer': ['abc', ''], 'blob': ['abc', ''], 'json': ['{"a":1}', '{', '', '﻿[1]'], 'text': ['é'],
    })) {
      out[type] = [];
      for (const body of bodies) {
        const xhr = await echo(body, 'x/y', type);
        const value = xhr.response;
        out[type].push(
            value instanceof Blob ? ['Blob', value.size, value.type] :
            value instanceof ArrayBuffer ? ['ArrayBuffer', value.byteLength] : value,
            value === xhr.response);
      }
    }
    return out;
  });

  await caseOf('documents', async () => {
    const out = [];
    for (const [body, type, responseType] of [
      ['<a><b/></a>', 'text/xml', ''], ['<a><b></a>', 'text/xml', ''], ['', 'text/xml', ''],
      ['<p>é<script>window.ran = 1</' + 'script>', 'text/html', 'document'], ['<p>x', 'text/html', ''],
      ['<svg xmlns="http://www.w3.org/2000/svg"/>', 'image/svg+xml', 'document'], ['<a/>', 'application/octet-stream', ''],
      ['<a/>', 'x/y+xml', ''], ['<!doctype html><div><template shadowrootmode="open">x</template></div>', 'text/html', 'document'],
    ]) {
      const xhr = await echo(body, type, responseType);
      const d = xhr.responseXML;
      out.push(d && [
        d.constructor.name, d.contentType, d.URL, d.baseURI, d.characterSet, d.compatMode, d.readyState, d.defaultView,
        d.location, d.cookie, d.documentElement && d.documentElement.outerHTML, d === xhr.responseXML,
      ]);
    }
    out.push(String(window.ran));
    // An HTML one's encoding is its decoder's, which its links' queries go by.
    for (const [body, type] of [
      ['<meta charset=windows-1251><title>\u00e5</title><a href="?\u00e5">', 'text/html'], ['<a href="?\u00e5">', 'text/html;charset=iso-8859-2'],
      ['<a href="?\u00e5">', 'text/html'], ['<?xml version="1.0" encoding="windows-1251"?><a>\u00e5</a>', 'text/xml'],
      ['<html xmlns="http://www.w3.org/1999/xhtml"><body/></html>', 'application/xhtml+xml;charset=windows-1252'],
    ]) {
      const d = (await echo(new Uint8Array([...body].map((c) => c.charCodeAt(0))), type, 'document')).responseXML;
      const a = d && d.querySelector('a');
      out.push(d && [d.characterSet, d.charset, d.inputEncoding, d.title, a && a.href && new URL(a.href).search]);
    }
    return out;
  });

  await caseOf('charsets', async () => {
    const out = [];
    const bytes = (...parts) => btoa(parts.map((part) => typeof part === 'string' ? part : String.fromCharCode(part)).join(''));
    for (const [data, type, responseType, override] of [
      [bytes(0xe9), 'text/plain', ''], [bytes(0xe9), 'text/plain;charset=latin1', ''],
      [bytes(0xef, 0xbb, 0xbf, 'a'), 'text/plain;charset=latin1', ''], [bytes(0xfe, 0xff, 0, 'a'), 'text/plain', ''],
      [bytes(0xff, 0xfe, 'a', 0), 'application/json', 'json'], [bytes(0xef, 0xbb, 0xbf, '1'), 'application/json', 'json'],
      [bytes('<?xml version="1.0" encoding="iso-8859-1"?><a>', 0xe9, '</a>'), 'text/xml', ''],
      [bytes('<?xml version="1.0" encoding="iso-8859-1"?><a>', 0xe9, '</a>'), 'text/xml', 'document'],
      [bytes('<a>', 0xff, 'b</a>'), 'text/xml', 'document'],
      [bytes('<meta charset=windows-1251><p>', 0xe9), 'text/html', 'document'],
      [bytes('<meta charset=windows-1251><p>', 0xe9), 'text/html', 'text'],
      [bytes(0xe9), 'text/plain', '', 'text/plain;charset=windows-1251'], [bytes(0xe9), 'text/plain', '', ';charset=latin1'],
      [bytes(0xe9), 'text/plain', '', 'x/y;charset=bogus'],
    ]) {
      const xhr = new XMLHttpRequest();
      xhr.open('GET', `data:${type};base64,${data}`);
      if (override) xhr.overrideMimeType(override);
      xhr.responseType = responseType;
      const end = ended(xhr);
      xhr.send();
      await end;
      out.push(xhr.response instanceof Document ? xhr.response.documentElement.textContent : xhr.response);
    }
    return out;
  });

  await caseOf('local schemes', async () => {
    const out = [];
    const url = URL.createObjectURL(new Blob(['hello'], {type: 'text/x'}));
    for (const [method, address, range] of [
      ['GET', 'data:text/plain,abc'], ['HEAD', 'data:text/plain,abc'], ['GET', url], ['GET', url, 'bytes=1-2'], ['HEAD', url], ['POST', url],
    ]) {
      const xhr = new XMLHttpRequest();
      const log = watch(xhr);
      xhr.open(method, address);
      if (range) xhr.setRequestHeader('Range', range);
      const end = ended(xhr);
      xhr.send();
      await end;
      out.push([method, log, show(xhr)]);
    }
    const revoked = new XMLHttpRequest();
    revoked.open('GET', url);
    URL.revokeObjectURL(url);
    const end = ended(revoked);
    revoked.send();
    await end;
    out.push(show(revoked));
    return out;
  });

  await caseOf('preloads', async () => {
    // The number of the request whose response each XMLHttpRequest got, after a preload that was the first.
    const preload = async (href, crossorigin, waits = true) => {
      const link = document.createElement('link');
      Object.assign(link, {rel: 'preload', as: 'fetch', href});
      if (crossorigin !== null) link.crossOrigin = crossorigin;
      const loaded = new Promise((resolve) => link.onload = link.onerror = resolve);
      document.head.append(link);
      if (waits) await loaded;
    };
    const ask = async (href, {method = 'GET', headers = [], async = true, credentials = false, responseType = ''} = {}) => {
      const xhr = new XMLHttpRequest();
      const log = watch(xhr);
      xhr.open(method, href, async);
      if (async) xhr.responseType = responseType;
      xhr.withCredentials = credentials;
      for (const [name, value] of headers) xhr.setRequestHeader(name, value);
      const end = ended(xhr);
      xhr.send();
      if (async) await end;
      const body = responseType === 'blob' ? await xhr.response.text() : responseType ? String(xhr.response) : xhr.responseText;
      return [body, xhr.getResponseHeader('x-count'), xhr.status, log.at(-1)];
    };
    const out = {};
    const cases = {
      plain: {}, again: {}, sync: {async: false}, post: {method: 'POST'}, blob: {responseType: 'blob'},
      buffer: {responseType: 'arraybuffer'}, header: {headers: [['X-A', '1']]}, conditional: {headers: [['If-None-Match', 'x']]},
      credentials: {credentials: true},
    };
    for (const [name, init] of Object.entries(cases)) {
      const href = '/count/' + name;
      await preload(href, 'anonymous');
      out[name] = [await ask(href, init)];
      if (name === 'again') out[name].push(await ask(href, init));
    }
    await preload('/count/noCors', null);
    out.noCors = await ask('/count/noCors');
    await preload('/count/include', 'use-credentials');
    out.include = [await ask('/count/include'), await ask('/count/include', {credentials: true})];
    await preload('/count/slow', 'anonymous', false);
    out.slow = await ask('/count/slow');
    await preload(cross + '/count/cross', 'anonymous');
    out.cross = await ask(cross + '/count/cross');
    await preload(cross + '/bytes/5', 'anonymous');
    const refused = new XMLHttpRequest();
    const log = watch(refused);
    refused.open('GET', cross + '/bytes/5');
    const end = ended(refused);
    refused.send();
    await end;
    out.refused = log;
    const taken = new XMLHttpRequest();
    await preload('/count/slow-aborted', 'anonymous', false);
    taken.open('GET', '/count/slow-aborted');
    taken.send();
    taken.abort();
    out.aborted = await ask('/count/slow-aborted');
    return out;
  });

  await caseOf('uploads', async () => {
    const out = {};
    for (const [name, url, when] of [
      ['watched', '/echo', 'before'], ['late', '/echo', 'after'], ['read first', '/echo', 'read'],
      ['other origin unwatched', cross + '/cors/x', 'none'], ['other origin watched', cross + '/cors/x', 'before'],
    ]) {
      const xhr = new XMLHttpRequest();
      const log = watch(xhr, when === 'before');
      if (when === 'read') xhr.upload;
      xhr.open('POST', url);
      const end = ended(xhr);
      xhr.send('body');
      if (when === 'after' || when === 'read') {
        for (const type of EVENTS) xhr.upload.addEventListener(type, () => log.push('upload ' + type));
      }
      await end;
      out[name] = log;
    }
    return out;
  });

  await caseOf('late upload listeners', async () => {
    // A listener of the upload that comes after send() hears nothing of a request of another origin that went with no
    // preflight.
    const out = [];
    const digits = ['Device-Memory', 'DPR', 'Width', 'Viewport-Width', 'RTT', 'Downlink'].map((name) => [name, '1'.repeat(128)]);
    const texts = ['Accept', 'Accept-Language', 'Content-Language'].map((name) => [name, 'a'.repeat(128)]);
    for (const [origin, headers] of [
      [here, []], [cross, []], [cross, [['Range', 'bytes=0-5']]], [cross, [['Range', 'bytes=-5']]],
      [cross, [['Range', 'bytes=0-5,7-8']]], [cross, [['X-A', '1']]], [cross, [['Save-Data', 'on']]],
      [cross, [['Save-Data', 'off']]], [cross, [['ECT', '4g']]], [cross, [['DPR', '1.5']]], [cross, [['DPR', '1.']]],
      [cross, [['Content-Type', 'text/plain']]], [cross, [['Content-Type', 'text/xml']]],
      [cross, digits], [cross, [...digits, ...texts]],
    ]) {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', origin + '/cors/5');
      for (const [name, value] of headers) xhr.setRequestHeader(name, value);
      const log = [];
      xhr.upload;
      const end = ended(xhr);
      xhr.send('body');
      for (const type of EVENTS) xhr.upload.addEventListener(type, () => log.push(type));
      await end;
      out.push([xhr.status, log]);
    }
    return out;
  });

  await caseOf('frames that go as a request starts', async () => {
    const out = [];
    for (const type of ['loadstart', 'readystatechange', 'upload loadstart', 'abort']) {
      for (const async of type === 'readystatechange' ? [true, false] : [true]) {
        const frame = document.createElement('iframe');
        document.body.append(frame);
        const xhr = new frame.contentWindow.XMLHttpRequest();
        const log = watch(xhr, true);
        const target = type.startsWith('upload ') ? xhr.upload : xhr;
        target.addEventListener(type.replace('upload ', ''), () => {
          if (xhr.readyState < 1 || !frame.isConnected) return;
          log.push('goes');
          frame.remove();
        });
        log.push(tried(() => xhr.open('POST', here + '/echo', async)), tried(() => xhr.send('body')));
        if (type === 'abort') xhr.abort();
        await wait(100);
        out.push([type, log, xhr.readyState, xhr.status, tried(() => xhr.send('again')), tried(() => xhr.abort()), xhr.readyState]);
        frame.remove();
      }
    }
    return out;
  });

  await caseOf('members of another realm', async () => {
    // A request is its XMLHttpRequest's frame's, whichever realm's members a script calls on it.
    const frame = document.createElement('iframe');
    frame.src = '/frame/page';
    const loaded = new Promise((resolve) => frame.onload = resolve);
    document.body.append(frame);
    await loaded;
    const w = frame.contentWindow;
    const mine = XMLHttpRequest.prototype;
    const get = (xhr, name) => Object.getOwnPropertyDescriptor(mine, name).get.call(xhr);
    const out = [];
    for (const [async, type, body] of [
      [true, '', 'x'], [true, 'blob', 'x'], [true, 'arraybuffer', 'x'], [true, 'document', '<p>'], [false, '', 'x'],
    ]) {
      const path = `/log/${++paths}`;
      const xhr = new w.XMLHttpRequest();
      mine.open.call(xhr, 'POST', '..' + path, async);
      mine.overrideMimeType.call(xhr, 'text/html');
      if (async) xhr.responseType = type;
      const end = ended(xhr);
      mine.send.call(xhr, body);
      if (async) await end;
      const response = get(xhr, 'response');
      const realm = Object(response) !== response ? null
          : [window, w].findIndex((of) => Object.getPrototypeOf(response) === of[response.constructor.name].prototype);
      const seen = await (await fetch(path.replace('/log', '/seen'))).json();
      const entries = [window, w].map((of) => of.performance.getEntriesByName(xhr.responseURL).map((entry) => entry.initiatorType));
      out.push([seen.headers.referer, entries, realm, get(xhr, 'status'), xhr.responseURL]);
    }
    const json = new w.XMLHttpRequest();
    mine.open.call(json, 'POST', '/echo');
    json.responseType = 'json';
    const end = ended(json);
    mine.send.call(json, '{}');
    await end;
    out.push([window, w].findIndex((of) => Object.getPrototypeOf(get(json, 'response')) === of.Object.prototype));
    frame.remove();
    return out;
  });

  await caseOf('frames that go', async () => {
    const out = [];
    const frame = document.createElement('iframe');
    document.body.append(frame);
    const w = frame.contentWindow;
    const opened = new w.XMLHttpRequest();
    const loading = new w.XMLHttpRequest();
    const done = new w.XMLHttpRequest();
    done.open('GET', here + '/bytes/5');
    const end = ended(done);
    done.send();
    await end;
    out.push(show(done));
    const log = watch(opened);
    const loadingLog = watch(loading);
    opened.open('GET', here + '/bytes/5');
    loading.open('GET', here + '/never');
    loading.send();
    await wait(20);
    frame.remove();
    out.push(show(done), done.response, done.getResponseHeader('content-type'));
    out.push(tried(() => opened.send()), opened.readyState, tried(() => opened.open('GET', '/')), opened.readyState, log);
    const late = new w.XMLHttpRequest();
    out.push(tried(() => late.open('GET', here + '/')), late.readyState, tried(() => late.send()));
    await wait(50);
    out.push(loading.readyState, loading.status, loadingLog);
    return out;
  });

  await caseOf('synchronous', async () => {
    const out = [];
    for (const [method, url, body] of [
      ['GET', '/bytes/5'], ['GET', '/bytes/0'], ['POST', '/echo', new Blob(['abc'])], ['GET', '/redirect'], ['GET', '/status/404'],
      ['GET', cross + '/cors/5'], ['GET', cross + '/bytes/5'], ['GET', 'http://127.0.0.1:1/'], ['GET', 'foo:bar'],
      ['GET', 'ftp://127.0.0.1/'], ['GET', 'data:text/plain,abc'], ['GET', 'data:'],
      ['GET', URL.createObjectURL(new Blob(['hello']))], ['POST', URL.createObjectURL(new Blob(['hello']))],
    ]) {
      const xhr = new XMLHttpRequest();
      const log = watch(xhr, true);
      xhr.open(method, url, false);
      log.push(tried(() => xhr.send(body)));
      out.push([log, show(xhr)]);
    }
    const xml = new XMLHttpRequest();
    xml.open('POST', '/echo', false);
    xml.overrideMimeType('text/xml');
    xml.send('<a>b</a>');
    out.push(xml.responseXML.documentElement.outerHTML);
    return out;
  });

  await caseOf('limits of synchronous requests', async () => {
    const xhr = new XMLHttpRequest();
    const out = [];
    xhr.responseType = 'text';
    out.push(tried(() => xhr.open('GET', '/bytes/1', false)), xhr.readyState);
    xhr.responseType = '';
    xhr.timeout = 5;
    out.push(tried(() => xhr.open('GET', '/bytes/1', false)), xhr.readyState);
    xhr.timeout = 0;
    out.push(tried(() => xhr.open('GET', '/bytes/1', false)), xhr.readyState, tried(() => xhr.timeout = 5),
             tried(() => xhr.responseType = 'json'), tried(() => xhr.withCredentials = true), tried(() => xhr.send()),
             xhr.responseText, tried(() => xhr.send()));
    const aborted = new XMLHttpRequest();
    aborted.open('GET', '/bytes/1', false);
    aborted.onreadystatechange = () => aborted.abort();
    out.push(tried(() => aborted.send()), aborted.readyState);
    return out;
  });

  await caseOf('what runs during a synchronous request', async () => {
    const log = [];
    setTimeout(() => log.push('timer'), 0);
    Promise.resolve().then(() => log.push('microtask'));
    const channel = new MessageChannel();
    channel.port1.onmessage = () => log.push('message');
    channel.port2.postMessage(1);
    const other = new XMLHttpRequest();
    other.open('GET', '/bytes/1');
    const end = ended(other);
    other.onload = () => log.push('load');
    other.send();
    const xhr = new XMLHttpRequest();
    xhr.open('GET', '/slow', false);
    xhr.send();
    log.push('done ' + xhr.responseText.length);
    await end;
    await wait(20);
    return [log.slice(0, 2), log.slice(2).sort()];
  });

  await caseOf('synchronous requests as a page is left', async () => {
    const out = [];
    const frame = document.createElement('iframe');
    frame.src = '/bytes/1';
    const loaded = () => new Promise((resolve) => frame.addEventListener('load', resolve, {once: true}));
    document.body.append(frame);
    await loaded();
    const w = frame.contentWindow;
    for (const type of ['beforeunload', 'pagehide']) {
      w.addEventListener(type, () => {
        for (const of of [w, window]) {
          const xhr = new of.XMLHttpRequest();
          xhr.open('GET', here + '/bytes/1', false);
          out.push(type + ' ' + tried(() => xhr.send()) + ' ' + xhr.status);
        }
      });
    }
    frame.src = '/bytes/2';
    await loaded();
    return out;
  });

  await caseOf('synchronous requests past the limit of connections', async () => {
    const controller = new AbortController();
    for (let i = 0; i < 8; ++i) fetch('/never/' + i, {signal: controller.signal}).catch(() => {});
    await wait(200);
    const xhr = new XMLHttpRequest();
    xhr.open('GET', '/bytes/5', false);
    xhr.send();
    const worker = await inWorker(`
      const xhr = new XMLHttpRequest(); xhr.open('GET', here + '/bytes/6', false); xhr.send(); return xhr.responseText;`);
    controller.abort();
    return [xhr.responseText, worker];
  });

  await caseOf('worker', async () => ({
    async: await inWorker(`
      const xhr = new XMLHttpRequest(); const log = watch(xhr, true); xhr.open('POST', here + '/echo');
      const end = ended(xhr); xhr.send(new Blob(['abc'])); await end; return [log, show(xhr)];`),
    document: await inWorker(`
      const xhr = new XMLHttpRequest(); xhr.responseType = 'document';
      return [xhr.responseType, 'responseXML' in xhr, typeof XMLHttpRequestUpload];`),
    sync: await inWorker(`
      const out = [];
      for (const type of ['', 'text', 'json', 'arraybuffer', 'blob']) {
        const xhr = new XMLHttpRequest(); const log = watch(xhr); xhr.open('POST', here + '/echo', false); xhr.responseType = type;
        log.push(tried(() => xhr.send('[1]'))); out.push([log, Object.prototype.toString.call(xhr.response)]);
      }
      return out;`),
    timeout: await inWorker(`
      const xhr = new XMLHttpRequest(); xhr.open('GET', here + '/never', false); xhr.timeout = 50;
      return [tried(() => xhr.send()), show(xhr)];`),
    refused: await inWorker(`
      const xhr = new XMLHttpRequest(); xhr.open('GET', 'http://127.0.0.1:1/', false);
      return [tried(() => xhr.send()), show(xhr)];`),
    relative: await inWorker(`return tried(() => new XMLHttpRequest().open('GET', '/relative'));`),
  }));

  return record;
})();
