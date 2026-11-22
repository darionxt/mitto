// Frames that a page makes without the network: an iframe's initial about:blank,
// the about:blank that it commits at once, the frame tree on the window,
// contentWindow and contentDocument, realms, writing into a frame's document,
// named and indexed access, and a frame that goes. It runs in a top-level page
// at an HTTP origin and gives a promise of what each case saw, which
// tests/test_page_frames.py compares with what Chrome 157 gave for it.
(async () => {
  const record = {};
  const caseOf = (name, fn) => {
    try {
      record[name] = fn();
    } catch (e) {
      record[name] = `threw ${e && e.name}: ${e && e.message}`;
    }
  };
  const frame = (attributes = {}) => {
    const element = document.createElement('iframe');
    for (const [name, value] of Object.entries(attributes)) {
      element.setAttribute(name, value);
    }
    return element;
  };

  caseOf('insertion', () => {
    const seen = [];
    const element = frame();
    element.addEventListener('load', (e) => seen.push(
        ['load', e.isTrusted, e.bubbles, element.contentDocument.readyState,
         element.contentDocument.URL].join(' ')));
    seen.push('before');
    document.body.appendChild(element);
    seen.push('after');
    const w = element.contentWindow;
    const result = {
      seen,
      length: frames.length,
      indexed: frames[0] === w,
      parent: w.parent === window,
      top: w.top === window,
      frameElement: w.frameElement === element,
      document: w.document === element.contentDocument,
      url: element.contentDocument.URL,
      markup: element.contentDocument.documentElement.outerHTML,
      readyState: element.contentDocument.readyState,
      origin: w.origin,
      locationOrigin: w.location.origin,
      href: w.location.href,
      compatMode: element.contentDocument.compatMode,
      characterSet: element.contentDocument.characterSet,
      contentType: element.contentDocument.contentType,
      referrer: element.contentDocument.referrer,
      cookieShared: (document.cookie = 'frames=1', element.contentDocument.cookie.includes('frames=1')),
      isSecureContext: w.isSecureContext,
      opener: w.opener,
      name: w.name,
    };
    element.remove();
    return result;
  });

  caseOf('realms', () => {
    const element = frame();
    document.body.appendChild(element);
    const w = element.contentWindow;
    const d = element.contentDocument;
    const result = [
      d instanceof Document,
      d instanceof w.Document,
      d.body instanceof w.HTMLBodyElement,
      d.createElement('p') instanceof w.HTMLParagraphElement,
      new w.Text('t') instanceof w.Text,
      new w.Text('t') instanceof Text,
      w.Array === Array,
      w.Document === Document,
      Object.getPrototypeOf(w) === Object.getPrototypeOf(window),
      (() => { try { w.eval('null.x'); } catch (e) { return [e instanceof w.TypeError, e instanceof TypeError]; } })(),
    ];
    element.remove();
    return result;
  });

  caseOf('tree', () => {
    const outer = frame();
    document.body.appendChild(outer);
    const w = outer.contentWindow;
    const inner = outer.contentDocument.createElement('iframe');
    outer.contentDocument.body.appendChild(inner);
    const second = frame();
    document.body.appendChild(second);
    const result = {
      length: frames.length,
      first: frames[0] === w,
      second: frames[1] === second.contentWindow,
      past: frames[2],
      innerLength: w.length,
      innerParent: inner.contentWindow.parent === w,
      innerTop: inner.contentWindow.top === window,
      nested: frames[0][0] === inner.contentWindow,
      frameElementOfTop: window.frameElement,
      selfFrames: [window.frames === window, window.self === window, w.frames === w],
    };
    // A frame removed from the tree comes out of the window's frames.
    outer.remove();
    result.afterRemoval = [frames.length, frames[0] === second.contentWindow];
    second.remove();
    return result;
  });

  caseOf('indexedProperties', () => {
    const element = frame();
    document.body.appendChild(element);
    const descriptor = Object.getOwnPropertyDescriptor(window, '0');
    const result = {
      descriptor: descriptor && [descriptor.value === element.contentWindow, descriptor.writable,
                                 descriptor.enumerable, descriptor.configurable, 'get' in descriptor],
      has: ['0' in window, '1' in window, window.hasOwnProperty('0')],
      keys: Object.keys(window).includes('0'),
      ownNames: Object.getOwnPropertyNames(window).indexOf('0'),
      sloppySet: (() => { window[0] = 5; return window[0] === element.contentWindow; })(),
      strictSet: (() => { 'use strict'; try { window[0] = 5; return 'set'; } catch (e) { return `${e.name}: ${e.message}`; } })(),
      define: (() => { try { Object.defineProperty(window, '0', {value: 1}); return 'defined'; } catch (e) { return `${e.name}: ${e.message}`; } })(),
      deleted: (() => { 'use strict'; try { return delete window[0]; } catch (e) { return `${e.name}: ${e.message}`; } })(),
      past: window[1],
    };
    element.remove();
    result.afterRemoval = ['0' in window, window[0]];
    return result;
  });

  caseOf('names', () => {
    const named = frame({name: 'kid', id: 'kid-id'});
    document.body.appendChild(named);
    const w = named.contentWindow;
    const result = {
      byName: window.kid === w,
      byId: window['kid-id'] === named,
      nameOfWindow: w.name,
      afterAttribute: (named.setAttribute('name', 'renamed'), [w.name, window.kid === w, window.renamed === w]),
      afterWindowName: (w.name = 'fromscript', [w.name, window.fromscript === w, named.getAttribute('name')]),
      emptyName: (w.name = '', [w.name, window[''] === undefined]),
    };
    named.remove();
    const unnamed = frame();
    document.body.appendChild(unnamed);
    result.unnamed = unnamed.contentWindow.name;
    unnamed.remove();
    return result;
  });

  caseOf('write', () => {
    const element = frame();
    const loads = [];
    element.addEventListener('load', () => loads.push(element.contentDocument.body &&
                                                      element.contentDocument.body.innerHTML));
    document.body.appendChild(element);
    const d = element.contentDocument;
    const states = [];
    d.addEventListener('readystatechange', () => states.push(d.readyState));
    d.open();
    states.push('opened ' + d.readyState);
    d.write('<!doctype html><p id=x>written</p>');
    d.close();
    const result = {
      loads,
      states,
      text: d.getElementById('x') && d.getElementById('x').textContent,
      compatMode: d.compatMode,
      url: d.URL,
      sameDocument: element.contentDocument === d,
    };
    element.remove();
    return result;
  });

  caseOf('detach', () => {
    const element = frame();
    document.body.appendChild(element);
    const w = element.contentWindow;
    const d = element.contentDocument;
    const seen = [];
    // Whether unload fires in a frame depends on the variations seed's unload
    // deprecation, which differs between the Persona's and Chrome for Testing's.
    w.addEventListener('pagehide', (e) => seen.push('pagehide ' + e.persisted));
    d.addEventListener('visibilitychange', () => seen.push('visibilitychange ' + d.visibilityState));
    element.remove();
    return {
      seen,
      contentWindow: element.contentWindow,
      contentDocument: element.contentDocument,
      parent: w.parent,
      top: w.top,
      frameElement: w.frameElement,
      length: w.length,
      name: w.name,
      documentKept: w.document === d,
      closed: w.closed,
      timer: w.setTimeout(() => {}, 0),
    };
  });

  caseOf('reinsertion', () => {
    const element = frame();
    document.body.appendChild(element);
    const first = element.contentWindow;
    element.remove();
    document.body.appendChild(element);
    const second = element.contentWindow;
    const result = [first === second, second && second.parent === window, frames.length];
    element.remove();
    return result;
  });

  caseOf('frameElement', () => {
    const element = document.createElement('frame');
    document.body.appendChild(element);
    const w = element.contentWindow;
    const result = [String(element), w && w.frameElement === element, element.contentDocument && element.contentDocument.URL];
    element.remove();
    return result;
  });

  return record;
})()
