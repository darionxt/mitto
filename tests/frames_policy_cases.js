// What an iframe's allow, allowfullscreen and allowpaymentrequest attributes, and its sandbox, let its documents inherit of the page's
// permissions policy. The page is at /app/page; /child runs tests/frames_policy_child.js. The value is a promise for a record of
// sections, which tests/data/frames_policy_cases.json holds as Chrome gave it.
(async () => {
  const OTHER = `http://localhost:${location.port}`;
  const plain = (value) => JSON.parse(JSON.stringify(value).replaceAll(OTHER, 'OTHER').replaceAll(location.origin, 'ORIGIN'));
  const settle = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
  const child = await (await fetch('/child.js')).text();
  const fill = (text) => text.replaceAll('OTHER', OTHER).replaceAll('ORIGIN', location.origin);
  const heard = (element) => new Promise((resolve) => {
    addEventListener('message', function listener({source, data}) {
      if (source !== element.contentWindow) return;
      removeEventListener('message', listener);
      resolve(data);
    });
  });
  const named = ['camera', 'fullscreen', 'gamepad', 'geolocation', 'payment', 'picture-in-picture'];
  const ofElement = ({featurePolicy}) => named.map((name) => [featurePolicy.allowsFeature(name), featurePolicy.getAllowlistForFeature(name)]);
  // What the frame's document says, only where it differs from `base`, and what the element's policy does.
  const show = async (attributes, after = () => {}) => {
    const element = document.createElement('iframe');
    for (const name in attributes) if (attributes[name] !== null) element.setAttribute(name, fill(attributes[name]));
    const said = heard(element);
    document.body.append(element);
    after(element);
    const value = await Promise.race([said, settle(5000).then(() => 'never')]);
    const result = [value, ofElement(element)];
    element.remove();
    return result;
  };
  const DOCUMENTS = {
    same: {src: '/child'}, other: {src: 'OTHER/child'}, srcdoc: {srcdoc: `<script>${child}</script>`}, data: {src: `data:text/html,${encodeURIComponent(`<script>${child}</script>`)}`},
    opaque: {src: '/child', sandbox: 'allow-scripts'},
  };
  const ALLOW = ['', 'camera', 'camera *', `camera 'none'`, `camera 'self'`, `camera 'SRC'`, 'camera OTHER', 'camera ORIGIN', 'camera ORIGIN OTHER', 'CAMERA; geolocation *', 'camera; camera *',
    `camera *; camera 'none'`, 'camera, geolocation', `unknown *; gamepad 'none'`, 'camera https://*.example.com', `fullscreen 'none'`, ' camera\t*;;', 'camera nonsense', 'camera "self"',
    'camera OTHER/path?x', 'camera http://localhost', 'camera HTTP://LOCALHOST:1', 'camera https:', 'camera http://localhost:x', `picture-in-picture 'none'`, 'camera \u00e9', 'web-share *'];
  const record = {};
  const section = async (name, run) => {
    try {
      record[name] = plain(await run());
    } catch (error) {
      record[name] = `threw ${error.name}: ${error.message}`;
    }
  };
  for (const kind in DOCUMENTS) {
    await section(`allow, ${kind}`, async () => {
      const results = {none: await show(DOCUMENTS[kind])};
      const [base] = results.none;
      for (const allow of ALLOW) {
        const [value, element] = await show({...DOCUMENTS[kind], allow});
        const {allowed, ...more} = value;
        results[allow] = [allowed.filter((name) => !base.allowed.includes(name)), base.allowed.filter((name) => !allowed.includes(name)), more.lists.camera, more.fullscreen, more.pictureInPicture, element];
      }
      return results;
    });
  }
  await section('other attributes', async () => {
    const results = {};
    for (const [name, attributes] of Object.entries({
      allowfullscreen: {allowfullscreen: ''}, 'allowfullscreen and none': {allowfullscreen: '', allow: `fullscreen 'none'`}, 'allowfullscreen and self': {allow: `fullscreen 'self'`, allowfullscreen: ''},
      allowpaymentrequest: {allowpaymentrequest: ''}, 'allowpaymentrequest and src': {allowpaymentrequest: '', allow: `payment 'src'`}, webkitallowfullscreen: {webkitallowfullscreen: ''},
    })) {
      for (const kind of ['other', 'opaque']) {
        const [{allowed, lists, fullscreen}, element] = await show({...DOCUMENTS[kind], ...attributes});
        results[`${name}, ${kind}`] = [allowed, lists.fullscreen, fullscreen, element];
      }
    }
    return results;
  });
  // Of the attribute as it was when the navigation started, and of its URL, whatever the document that comes.
  await section('changes', async () => {
    const results = {};
    const of = async (...args) => (await show(...args))[0].allowed;
    results['set at once'] = await of(DOCUMENTS.other, (element) => { element.allow = 'camera'; });
    results['redirected'] = await of({src: `/redirect?OTHER/child`, allow: 'camera'});
    results['redirected, for both'] = await of({src: `/redirect?OTHER/child`, allow: 'camera OTHER'});
    results['redirected back'] = await of({src: `OTHER/redirect?ORIGIN/child`, allow: 'camera'});
    const element = document.createElement('iframe');
    element.src = `${OTHER}/child`;
    let said = heard(element);
    document.body.append(element);
    results.first = (await said).allowed;
    element.allow = 'camera';
    results['the element, set'] = ofElement(element);
    said = heard(element);
    element.src = `${OTHER}/child?2`;
    results['navigated'] = (await said).allowed;
    said = heard(element);
    element.src = `${OTHER}/goes?${location.origin}/child`;
    results['elsewhere'] = (await said).allowed;
    element.remove();
    const nested = document.createElement('iframe');
    nested.allow = 'camera; geolocation *';
    nested.src = `${OTHER}/holds?${encodeURIComponent(location.origin)}`;
    said = heard(nested);
    document.body.append(nested);
    results.nested = (await said).allowed;
    nested.remove();
    // The object of a window that its frame's first document keeps.
    const kept = document.createElement('iframe');
    kept.src = '/child';
    said = heard(kept);
    document.body.append(kept);
    const policy = kept.contentDocument.featurePolicy;
    const keptDocument = kept.contentDocument;
    results['before'] = policy.allowsFeature('camera');
    await said;
    results['of the first document'] = keptDocument === kept.contentDocument || [keptDocument.createElement('iframe').featurePolicy === null, keptDocument.featurePolicy === null];
    results['the kept object'] = [policy.allowsFeature('camera'), policy === kept.contentDocument.featurePolicy];
    kept.remove();
    const frame = document.createElement('frame');
    frame.setAttribute('allow', 'camera');
    frame.src = '/child';
    said = heard(frame);
    document.body.append(frame);
    const {allowed, fullscreen} = await said;
    results['a frame element'] = [allowed, fullscreen];
    frame.remove();
    const parsed = new DOMParser().parseFromString(`<iframe allow="gamepad 'none'; unparsed"></iframe>`, 'text/html').querySelector('iframe').featurePolicy;
    results['in no document'] = [document.createElement('iframe').featurePolicy !== null, parsed.allowsFeature('gamepad'), parsed.allowsFeature('camera'), parsed.getAllowlistForFeature('camera')];
    const child = document.createElement('iframe');
    child.allow = `camera 'none'`;
    document.body.append(child);
    const made = new child.contentWindow.DOMParser().parseFromString('<iframe></iframe>', 'text/html').querySelector('iframe');
    const getter = Object.getOwnPropertyDescriptor(HTMLIFrameElement.prototype, 'featurePolicy').get;
    results['of another window'] = getter.call(made).allowsFeature('camera');
    const borrowed = DOMParser.prototype.parseFromString.call(new child.contentWindow.DOMParser(), '<iframe></iframe>', 'text/html');
    const implemented = DOMImplementation.prototype.createHTMLDocument.call(child.contentDocument.implementation);
    results['made by borrowed methods'] = [borrowed, implemented, document.implementation.createHTMLDocument()].map((from) => (from.querySelector('iframe') ?? from.createElement('iframe')).featurePolicy.allowsFeature('camera'));
    const based = new DOMParser().parseFromString('<base href="about:blank"><iframe src=child></iframe>', 'text/html').querySelector('iframe');
    results['a URL of nothing'] = based.featurePolicy.allowsFeature('camera');
    const keptPolicy = child.contentDocument.featurePolicy;
    child.remove();
    results['of a removed frame'] = [keptPolicy.allowsFeature('removed'), keptPolicy.allowsFeature('camera')];
    // A detached element's URL leaves the container policy as its allow made it, with 'src' of its URL then: Blink makes
    // the policy again as a navigation starts, and a detached element starts none.
    let detached = document.createElement('iframe');
    detached.allow = 'camera';
    detached.src = `${OTHER}/child`;
    results['detached, allow then src'] = [detached.featurePolicy.allowsFeature('camera'), detached.featurePolicy.allowsFeature('camera', OTHER),
      detached.featurePolicy.allowsFeature('camera', location.origin)];
    detached = document.createElement('iframe');
    detached.allow = 'camera';
    const before = detached.featurePolicy;
    detached.src = `${OTHER}/child`;
    results['detached, the object before src'] = [before.allowsFeature('camera'), before.allowsFeature('camera', OTHER), before.allowedFeatures().includes('camera'),
      before === detached.featurePolicy];
    detached = document.createElement('iframe');
    detached.src = `${OTHER}/child`;
    detached.allow = 'camera';
    results['detached, src then allow'] = [detached.featurePolicy.allowsFeature('camera'), detached.featurePolicy.allowsFeature('camera', OTHER)];
    detached = document.createElement('iframe');
    detached.allow = 'camera';
    detached.src = `${OTHER}/child`;
    said = heard(detached);
    document.body.append(detached);
    results['detached, then connected'] = [detached.featurePolicy.allowsFeature('camera'), detached.featurePolicy.allowsFeature('camera', OTHER)];
    await said;
    detached.remove();
    const about = document.createElement('iframe');
    about.src = 'about:other';
    results['another about: URL'] = ofElement(about);
    about.src = 'http://[::1]:1/';
    about.allow = 'camera';
    results['an IPv6 address'] = ofElement(about);
    about.src = 'http://invalid:port/path';
    about.allow = '';
    results['an invalid URL'] = ofElement(about);
    // A length limit in code units.
    about.src = '/child';
    about.allow = `gamepad 'none'; camera ${'\u00e9'.repeat(40000)}`;
    results['a long attribute'] = ofElement(about);
    about.allow = `gamepad 'none'; camera ${'x'.repeat(66000)}`;
    results['too long an attribute'] = ofElement(about);
    // A warning said before, again after others.
    for (let i = 0; i < 3; ++i) about.allow = `repeated${i % 2}`;
    for (let i = 0; i < 2; ++i) document.featurePolicy.allowsFeature('said');
    about.allow = 'said';
    const plainFrame = document.createElement('iframe');
    results['the same object'] = plainFrame.featurePolicy === plainFrame.featurePolicy;
    results['its interface'] = plainFrame.featurePolicy.constructor.name;
    results['unknown names'] = [plainFrame.featurePolicy.allowsFeature('unknown'), document.featurePolicy.allowsFeature('unknown'), document.featurePolicy.allowsFeature('camera', 'nonsense'),
      document.featurePolicy.getAllowlistForFeature('unknown')];
    return results;
  });
  return record;
})()
