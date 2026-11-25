// A frame's srcdoc and its later about:blank, which commit without the
// network from a later task: when they load, the window they keep or get, their
// URL, origin, referrer and base URL, and the frame's load events. It runs in a
// top-level page at an HTTP origin and gives a promise of what each case saw,
// which tests/test_page_frames.py compares with what Chrome 157 gave for it.
(async () => {
  const record = {};
  // A load that does not come within 5 seconds is recorded as missing.
  const loaded = (element) => new Promise((resolve) => {
    element.addEventListener('load', () => resolve('load'), {once: true});
    setTimeout(() => resolve('no load'), 5000);
  });
  const frame = (srcdoc) => {
    const element = document.createElement('iframe');
    if (srcdoc !== undefined) {
      element.srcdoc = srcdoc;
    }
    return element;
  };

  {
    const element = frame('<!doctype html><p id=s>srcdoc</p><script>parent.ranIn = document.URL;</script>');
    let loads = 0;
    element.addEventListener('load', () => ++loads);
    const load = loaded(element);
    document.body.appendChild(element);
    const before = {
      url: element.contentDocument.URL,
      readyState: element.contentDocument.readyState,
      loads,
    };
    const initialWindow = element.contentWindow;
    const initialDocument = element.contentDocument;
    const firstLoad = await load;
    const d = element.contentDocument;
    record.srcdoc = {
      load: firstLoad,
      before,
      url: d.URL,
      text: d.getElementById('s') && d.getElementById('s').textContent,
      ranIn: window.ranIn,
      sameWindow: element.contentWindow === initialWindow,
      newDocument: d !== initialDocument,
      origin: element.contentWindow.origin,
      locationHref: element.contentWindow.location.href,
      referrer: d.referrer,
      baseURI: d.baseURI,
      compatMode: d.compatMode,
      readyState: d.readyState,
      loads,
      isSecureContext: element.contentWindow.isSecureContext,
      characterSet: d.characterSet,
    };
    // A new srcdoc navigates again, to a new window.
    const window1 = element.contentWindow;
    const again = loaded(element);
    element.srcdoc = '<p id=s>again</p>';
    const stillOld = element.contentDocument.getElementById('s').textContent;
    const againLoad = await again;
    record.srcdocAgain = {
      load: againLoad,
      stillOld,
      text: element.contentDocument.getElementById('s').textContent,
      newWindow: element.contentWindow !== window1,
      oldWindowClosed: window1.closed,
      loads,
    };
    // Without srcdoc or src, the frame goes to about:blank, in a new window.
    const window2 = element.contentWindow;
    const blank = loaded(element);
    element.removeAttribute('srcdoc');
    const blankLoad = await blank;
    record.blankAfterSrcdoc = {
      load: blankLoad,
      url: element.contentDocument.URL,
      newWindow: element.contentWindow !== window2,
      origin: element.contentWindow.origin,
      body: element.contentDocument.body && element.contentDocument.body.outerHTML,
      referrer: element.contentDocument.referrer,
      loads,
    };
    element.remove();
  }

  {
    // An about:blank that the element asks for after its first one, which
    // Chrome commits from a later task in a new window.
    const element = frame();
    document.body.appendChild(element);
    const first = element.contentWindow;
    let loads = 0;
    element.addEventListener('load', () => ++loads);
    const load = loaded(element);
    element.src = 'about:blank#next';
    const syncUrl = element.contentDocument.URL;
    const secondLoad = await load;
    record.secondBlank = {
      load: secondLoad,
      syncUrl,
      url: element.contentDocument.URL,
      newWindow: element.contentWindow !== first,
      loads,
    };
    element.remove();
  }

  {
    // srcdoc wins over src, and a frame element has it too.
    const element = document.createElement('frame');
    element.src = 'about:blank';
    element.setAttribute('srcdoc', '<p id=s>frame srcdoc</p>');
    const load = loaded(element);
    document.body.appendChild(element);
    record.frameSrcdoc = [await load, element.contentDocument.URL,
                          element.contentDocument.getElementById('s') &&
                              element.contentDocument.getElementById('s').textContent];
    element.remove();
  }

  {
    // A srcdoc with a fragment.
    const element = frame('<p id=foo>anchor</p>');
    element.src = 'about:srcdoc#foo';
    const load = loaded(element);
    document.body.appendChild(element);
    record.srcdocUrlIsTheAttribute = [await load, element.contentDocument.URL];
    element.remove();
  }

  return record;
})()
