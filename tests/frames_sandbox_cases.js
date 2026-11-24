// What an iframe's sandbox attribute withholds from its documents: their origin, and their scripts. The page is at /app/page;
// /child?key=... runs tests/frames_sandbox_child.js. The value is a promise for a record of sections, which
// tests/data/frames_sandbox_cases.json holds as Chrome gave it.
(async () => {
  const OTHER = `http://localhost:${location.port}`;
  const plain = (value) => JSON.parse(JSON.stringify(value).replaceAll(OTHER, 'OTHER').replaceAll(location.href, 'PAGE')
    .replaceAll(location.origin, 'ORIGIN'));
  const settle = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
  const until = async (done) => {
    for (const end = performance.now() + 15000; !done() && performance.now() < end;) await settle(10);
  };
  const loaded = (element) => new Promise((resolve) => element.addEventListener('load', resolve, {once: true}));
  const child = await (await fetch('/child.js')).text();
  const said = {};
  const heard = [];
  addEventListener('message', ({data, origin}) => {
    if (data && data.said) said[data.key] = {...data.said, from: origin};
    else heard.push([data, origin]);
  });
  const make = (sandbox, attributes = {}) => {
    const element = document.createElement('iframe');
    if (sandbox !== null) element.setAttribute('sandbox', sandbox);
    Object.assign(element, attributes);
    return element;
  };
  const show = async (element) => {
    const done = loaded(element);
    document.body.append(element);
    await done;
    return element;
  };
  const seen = ({contentDocument}) => contentDocument === null ? 'null' : contentDocument.URL;
  const record = {};
  const section = async (name, run) => {
    try {
      record[name] = plain(await run());
    } catch (error) {
      record[name] = `threw ${error.name}: ${error.message}`;
    }
  };
  const SANDBOXES = {none: null, all: '', scripts: 'allow-scripts', origin: 'allow-same-origin', both: 'allow-same-origin allow-scripts'};
  const DOCUMENTS = {
    initial: () => ({}), blank: () => ({src: 'about:blank'}), srcdoc: () => ({srcdoc: '<p>x'}), same: () => ({src: '/empty'}),
    other: () => ({src: `${OTHER}/empty`}), data: () => ({src: 'data:text/html,x'}),
  };
  // What the page reaches of a frame, before its element's load event and after it.
  await section('reach', async () => {
    const results = {};
    for (const kind in DOCUMENTS) {
      results[kind] = {};
      for (const sandbox in SANDBOXES) {
        const element = make(SANDBOXES[sandbox], DOCUMENTS[kind]());
        const done = loaded(element);
        document.body.append(element);
        const first = seen(element);
        await done;
        results[kind][sandbox] = [first, seen(element)];
        element.remove();
      }
    }
    return results;
  });
  // What a document says of itself, and what it may do.
  for (const sandbox of ['none', 'scripts', 'both']) {
    await section(`within ${sandbox}`, async () => {
      const results = {};
      const documents = {
        srcdoc: {srcdoc: `<body><script>${child}</script>`}, same: {src: `/child?key=same ${sandbox}`}, other: {src: `${OTHER}/child?key=other ${sandbox}`},
      };
      for (const kind in documents) {
        const key = `${kind} ${sandbox}`;
        const element = make(SANDBOXES[sandbox], {name: key, ...documents[kind]});
        document.body.append(element);
        await until(() => said[key]);
        results[kind] = said[key];
        element.remove();
      }
      return results;
    });
  }
  // The attribute holds from the next navigation on, and with what the parent's document has.
  await section('changes', async () => {
    const results = {};
    const element = await show(make('allow-same-origin', {src: '/empty'}));
    results.before = seen(element);
    element.sandbox = '';
    results['set, not navigated'] = seen(element);
    await Promise.all([loaded(element), element.src = '/empty?2']);
    results['navigated'] = seen(element);
    element.removeAttribute('sandbox');
    results['removed, not navigated'] = seen(element);
    await Promise.all([loaded(element), element.src = '/empty?3']);
    results['removed, navigated'] = seen(element);
    element.remove();
    // Set while a navigation waits: what it was as that started.
    const waiting = make(null, {src: '/empty?slow'});
    const done = loaded(waiting);
    document.body.append(waiting);
    waiting.sandbox = '';
    await done;
    results['set while it waits'] = seen(waiting);
    waiting.remove();
    const tokens = {};
    for (const value of ['ALLOW-SAME-ORIGIN', ' allow-same-origin\n', 'allow-same-origin,', 'allow-everything allow-same-origin', 'allow-forms allow-same-origin']) {
      const shown = await show(make(value, {srcdoc: ''}));
      tokens[value] = [seen(shown), shown.sandbox.length, shown.sandbox.value];
      shown.remove();
    }
    const frame = document.createElement('frame');
    frame.setAttribute('sandbox', '');
    const framed = await show(Object.assign(frame, {src: '/empty'}));
    results['a frame element'] = seen(framed);
    framed.remove();
    return {...results, tokens};
  });
  await section('nested', async () => {
    const results = {};
    const inner = (sandbox) => `<iframe${sandbox === null ? '' : ` sandbox="${sandbox}"`} srcdoc="<script>
      let reached; try { reached = !!parent.document; } catch { reached = false; }
      postMessage.call(top, ['inner', origin, reached], '*')</script>"></iframe>`;
    for (const outer of ['scripts', 'both']) {
      for (const within of ['none', 'scripts', 'both']) {
        heard.length = 0;
        const element = make(SANDBOXES[outer], {srcdoc: inner(SANDBOXES[within])});
        await show(element);
        await until(() => heard.length);
        results[`${within} in ${outer}`] = heard.slice();
        element.remove();
      }
    }
    // The documents of one opaque origin: a sandboxed document's frames that have its origin.
    heard.length = 0;
    const element = make('allow-scripts', {srcdoc: `<script>
      const frame = document.createElement('iframe');
      document.documentElement.append(frame);
      postMessage.call(top, [frame.contentDocument === null, origin], '*')</script>`});
    await show(element);
    await until(() => heard.length);
    results['a frame of its own'] = heard.slice();
    element.remove();
    return results;
  });
  // What the page sends to a document of an opaque origin, and what runs in it.
  await section('from the page', async () => {
    const results = {};
    const heardOf = {};
    for (const kind of ['scripts', 'both']) {
      heard.length = 0;
      const element = make(SANDBOXES[kind], {src: '/hear'});
      await show(element);
      for (const target of ['*', '/', location.origin, OTHER]) postMessage.call(element.contentWindow, target, target);
      postMessage.call(element.contentWindow, 'last', '*');
      await until(() => heard.some(([data]) => data === 'heard last'));
      results[kind] = heard.map(([data, origin]) => `${data} from ${origin}`);
      heard.length = 0;
      element.src = `javascript:postMessage.call(parent, 'javascript: ran', '*')`;
      await settle(100);
      heardOf[kind] = heard.map(([data]) => data);
      element.remove();
    }
    return {...results, javascript: heardOf};
  });
  // A document without scripts, of the page's origin.
  await section('no scripts', async () => {
    const results = {};
    const ran = window.ran = [];
    const element = make('allow-same-origin', {srcdoc: `<noscript><p id=noscript>n</p></noscript><body onclick="parent.ran.push('handler')">
      <script>parent.ran.push('inline')</script><script type=module>parent.ran.push('module')</script><script src=/ran.js></script>
      <img src=x: onerror="parent.ran.push('onerror')"><p id=p>x</p>`});
    await show(element);
    const {contentWindow: w, contentDocument: d} = element;
    results.noscript = [d.getElementById('noscript') !== null, d.querySelector('noscript').childNodes.length];
    const script = d.createElement('script');
    script.textContent = 'parent.ran.push("inserted")';
    d.body.append(script);
    const here = document.createElement('script');
    here.textContent = 'ran.push("inserted by the page")';
    d.body.append(here);
    d.body.setAttribute('onclick', 'parent.ran.push("set handler")');
    d.body.click();
    results.onclick = String(d.body.onclick);
    d.body.onclick = () => ran.push('the page\'s handler');
    d.body.click();
    d.getElementById('p').addEventListener('x', () => ran.push('the page\'s listener'));
    d.getElementById('p').dispatchEvent(new Event('x'));
    w.setTimeout(() => ran.push('timer of its window'), 0);
    w.setTimeout('parent.ran.push("string timer")', 0);
    results.eval = w.eval('1 + 1');
    w.eval('parent.ran.push("eval")');
    new w.Function('parent.ran.push("Function")')();
    w.Promise.resolve().then(() => ran.push('promise of its window'));
    w.queueMicrotask(() => ran.push('microtask of its window'));
    setTimeout(w.eval('() => parent.ran.push("its function in the page\'s timer")'), 0);
    d.body.addEventListener('y', w.eval('() => parent.ran.push("its function as a listener")'));
    d.body.dispatchEvent(new Event('y'));
    element.src = 'javascript:parent.ran.push("javascript:")';
    w.location = 'javascript:parent.ran.push("its location")';
    const link = d.createElement('a');
    link.href = 'javascript:parent.ran.push("link")';
    d.body.append(link);
    link.click();
    await settle(250);
    results.ran = ran.slice().sort();
    const parsed = new DOMParser().parseFromString('<noscript><p></p></noscript>', 'text/html');
    results.parsed = parsed.querySelector('noscript').children.length;
    element.remove();
    return results;
  });
  return record;
})()
