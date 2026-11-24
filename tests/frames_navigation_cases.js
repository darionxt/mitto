// Frames that navigate through the network: when a frame's document loads and
// whose window it gets, what its request sends, a 204, a redirect, a failure, a
// Location that another frame's script navigates, a frame whose src changes,
// a parent whose load waits for its frames, a frame element and a frame of
// another origin. It runs in a top-level page at an HTTP origin, on the server
// of tests/test_page_frames.py, and gives a promise of what each case saw,
// which that test compares with what Chrome 157 gave for it.
(async () => {
  const record = {};
  const caseOf = async (name, fn) => {
    try {
      record[name] = await fn();
    } catch (e) {
      record[name] = `threw ${e && e.name}: ${e && e.message}`;
    }
  };
  const loaded = (element) => new Promise((resolve) => {
    element.addEventListener('load', resolve, {once: true});
  });
  const settle = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const frame = (src, tag = 'iframe') => {
    const element = document.createElement(tag);
    element.src = src;
    return element;
  };

  await caseOf('load', async () => {
    const element = frame('/child?load');
    const seen = [];
    element.addEventListener('load', () => seen.push(
        ['load', element.contentDocument.URL, element.contentDocument.readyState].join(' ')));
    document.body.appendChild(element);
    const w = element.contentWindow;
    w.marker = 'kept';
    seen.push('inserted ' + element.contentDocument.URL);
    const timeOrigin = w.performance.timeOrigin;
    await loaded(element);
    const d = element.contentDocument;
    const result = {
      seen,
      sameProxy: element.contentWindow === w,
      marker: element.contentWindow.marker,
      url: d.URL,
      text: d.getElementById('c').textContent,
      referrer: d.referrer,
      origin: element.contentWindow.origin,
      readyState: d.readyState,
      compatMode: d.compatMode,
      contentType: d.contentType,
      characterSet: d.characterSet,
      timeOriginKept: element.contentWindow.performance.timeOrigin === timeOrigin,
      navigationEntries: element.contentWindow.performance.getEntriesByType('navigation')
          .map((e) => [e.name, e.type]),
      frameElement: element.contentWindow.frameElement === element,
      parent: element.contentWindow.parent === window,
      length: frames.length,
      cookie: (document.cookie = 'navigation=1', d.cookie.includes('navigation=1')),
    };
    element.remove();
    return result;
  });

  await caseOf('timingAtParse', async () => {
    // What /timing's script reads of performance.timing while it parses.
    const element = frame('/timing');
    document.body.appendChild(element);
    await loaded(element);
    const result = window.timingAtParse;
    element.remove();
    return result;
  });

  await caseOf('reuseAfterBlank', async () => {
    // A frame that has committed its about:blank at once, and fired load,
    // navigates to a document of its origin.
    const element = document.createElement('iframe');
    document.body.appendChild(element);
    element.contentWindow.marker = 'kept';
    element.src = '/timing';
    await loaded(element);
    const result = {marker: element.contentWindow.marker, timing: window.timingAtParse};
    element.remove();
    return result;
  });

  await caseOf('headers', async () => {
    const element = frame('/echo');
    document.body.appendChild(element);
    await loaded(element);
    const headers = JSON.parse(element.contentDocument.getElementById('h').textContent);
    element.remove();
    return headers;
  });

  await caseOf('parentWaits', async () => {
    window.order = [];
    const element = frame('/nested');
    element.addEventListener('load', () => window.order.push(
        'outer ' + element.contentDocument.readyState));
    document.body.appendChild(element);
    await loaded(element);
    const result = {
      order: window.order,
      innerUrl: element.contentWindow[0].document.URL,
      innerParent: element.contentWindow[0].parent === element.contentWindow,
      innerTop: element.contentWindow[0].top === window,
    };
    element.remove();
    return result;
  });

  await caseOf('noContent', async () => {
    const fresh = frame('/204');
    let freshLoads = 0;
    fresh.addEventListener('load', () => freshLoads++);
    document.body.appendChild(fresh);
    const later = frame('/child?first');
    document.body.appendChild(later);
    await loaded(later);
    let laterLoads = 0;
    later.addEventListener('load', () => laterLoads++);
    later.src = '/204';
    await settle(500);
    const result = {
      freshLoads,
      freshUrl: fresh.contentDocument.URL,
      laterLoads,
      laterUrl: later.contentDocument.URL,
    };
    fresh.remove();
    later.remove();
    return result;
  });

  await caseOf('redirect', async () => {
    const element = frame('/redirect');
    document.body.appendChild(element);
    await loaded(element);
    const d = element.contentDocument;
    const result = [d.URL, d.referrer, element.contentWindow.location.href];
    element.remove();
    return result;
  });

  await caseOf('location', async () => {
    const element = frame('/dir/page');
    document.body.appendChild(element);
    await loaded(element);
    // The parent's script completes the URL against its own document, and the
    // frame's against the frame's.
    element.contentWindow.location = 'child?fromparent';
    await loaded(element);
    const fromParent = [element.contentDocument.URL, element.contentDocument.referrer];
    element.contentWindow.eval("location.href = 'child?fromframe'");
    await loaded(element);
    const fromFrame = [element.contentDocument.URL, element.contentDocument.referrer];
    element.remove();
    return {fromParent, fromFrame};
  });

  await caseOf('srcChange', async () => {
    const element = frame('/child?a');
    document.body.appendChild(element);
    await loaded(element);
    const w = element.contentWindow;
    const firstDocument = element.contentDocument;
    w.marker = 'first';
    const seen = [];
    w.addEventListener('pagehide', (e) => seen.push('pagehide ' + e.persisted));
    element.src = '/child?b';
    seen.push('set ' + element.contentDocument.URL);
    await loaded(element);
    const result = {
      seen,
      url: element.contentDocument.URL,
      sameProxy: element.contentWindow === w,
      marker: element.contentWindow.marker,
      newDocument: element.contentDocument !== firstDocument,
      referrer: element.contentDocument.referrer,
    };
    element.remove();
    return result;
  });

  await caseOf('removedWhileLoading', async () => {
    const element = frame('/slow');
    let loads = 0;
    element.addEventListener('load', () => loads++);
    document.body.appendChild(element);
    element.remove();
    await settle(600);
    return [loads, frames.length, element.contentWindow];
  });

  await caseOf('frameElement', async () => {
    const element = frame('/child?frame', 'frame');
    document.body.appendChild(element);
    await loaded(element);
    const result = [element.contentDocument.URL, element.contentWindow.frameElement === element];
    element.remove();
    return result;
  });

  await caseOf('failed', async () => {
    // A navigation that fails without a response commits Chrome's error page,
    // of an opaque origin.
    const fresh = frame('/drop');
    let freshLoads = 0;
    fresh.addEventListener('load', () => freshLoads++);
    document.body.appendChild(fresh);
    const later = frame('/child?before');
    document.body.appendChild(later);
    await loaded(later);
    let laterLoads = 0;
    later.addEventListener('load', () => laterLoads++);
    later.src = '/drop';
    await Promise.race([loaded(later), settle(3000)]);
    await settle(200);
    const result = {
      freshLoads,
      freshDocument: fresh.contentDocument,
      laterLoads,
      laterDocument: later.contentDocument,
      length: frames.length,
    };
    fresh.remove();
    later.remove();
    return result;
  });

  await caseOf('otherOrigin', async () => {
    const element = frame(`http://localhost:${location.port}/child?other`);
    document.body.appendChild(element);
    await loaded(element);
    const result = [element.contentDocument, frames.length, frames[0] === element.contentWindow];
    element.remove();
    return result;
  });

  return record;
})()
