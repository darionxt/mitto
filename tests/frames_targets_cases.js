// Which frame may navigate which other, by location, window.open() and links, what its call gives or throws, which frame moves, and what
// its request says. /top?actor=ACTOR makes the frames anew for each act and has the actor do it, and goes on from /moved where that took the
// page away. Each document logs to the server what it sees, which tests/data/frames_targets_cases.json holds as Chrome gave it.
const PORT = location.port || document.currentScript.src.split(':')[2].split('/')[0];
const A = `http://127.0.0.1:${PORT}`, B = `http://localhost:${PORT}`;
// The frame that acts: its document's URL, its element's attributes, and what is between it and the page.
const ACTORS = {
  'same': {src: `${A}/frame`}, 'other': {src: `${B}/frame`}, 'same in other': {src: `${B}/nest?of=${A}/frame`}, 'other in same': {src: `${A}/nest?of=${B}/frame`}, 'same in same': {src: `${A}/nest?of=${A}/frame`},
  'sandbox': {src: `${A}/frame`, sandbox: 'allow-scripts'}, 'sandbox origin': {src: `${A}/frame`, sandbox: 'allow-scripts allow-same-origin'}, 'sandbox top': {src: `${A}/frame`, sandbox: 'allow-scripts allow-top-navigation'}, 'sandbox origin top': {src: `${A}/frame`, sandbox: 'allow-scripts allow-same-origin allow-top-navigation'},
  'sandbox origin activation': {src: `${A}/frame`, sandbox: 'allow-scripts allow-same-origin allow-top-navigation-by-user-activation'}, 'sandbox popups': {src: `${A}/frame`, sandbox: 'allow-scripts allow-popups'}, 'sandbox other top': {src: `${B}/frame`, sandbox: 'allow-scripts allow-same-origin allow-top-navigation'}, 'srcdoc': {srcdoc: true},
  'sandbox locks': {src: `${A}/frame`, sandbox: 'allow-scripts allow-same-origin allow-orientation-lock allow-presentation'},
  'other in sandbox top': {src: `${A}/nest?of=${B}/frame`, sandbox: 'allow-scripts allow-same-origin allow-top-navigation'},
};
// What it does: T is a URL of the page's origin, O of the other, and both land on /moved.
const ACTS = {
  'top.location': 'top.location = T', 'top.location other': 'top.location = O', 'top.location.href': 'top.location.href = T', 'top.location.replace': 'top.location.replace(T)', 'top.location hash': 'top.location = TOP + "#x"', 'top.location javascript': 'top.location = "javascript:void(0)"',
  'sibling.location': 'top.frames.sibling.location = T', 'sibling.location other': 'top.frames.sibling.location = O', 'sibling.location.replace': 'top.frames.sibling.location.replace(T)', 'sibling.location javascript': 'top.frames.sibling.location.replace("javascript:void(0)")',
  'child.location': 'frames.child.location = T', 'child.location other': 'frames.child.location = O', 'own location': 'location = T',
  'open _top': 'open(T, "_top")', 'open _top other': 'open(O, "_top")', 'open _parent': 'open(T, "_parent")', 'open _self': 'open(T, "_self")', 'open sibling': 'open(T, "sibling")', 'open foreign': 'open(T, "foreign")', 'open child': 'open(T, "child")', 'open new': 'open(T, "unknown")', 'open _blank': 'open(T, "_blank")', 'open nothing sibling': 'open("", "sibling")', 'open sibling noopener': 'open(T, "sibling", "noopener")', 'open SIBLING': 'open(T, "SIBLING")', 'open _TOP': 'open(T, "_TOP")', 'open sibling noreferrer': 'open(T, "sibling", "popup=1, NOREFERRER")', 'open sibling noopener=0': 'open(T, "sibling", "noopener=0")', 'open _top noopener': 'open(T, "_top", "noopener")', 'open nothing _top': 'open("", "_top")',
  'open own name': 'open(T, name)', 'open empty': 'open(T, "")', 'open invalid': 'open("http://[", "sibling")', 'child.open _self': 'frames.child.open(T, "_self")', 'child.open _parent': 'frames.child.open(T, "_parent")', 'child.open sibling': 'frames.child.open(T, "sibling")', 'open sibling hash': 'open(A + "/plain#x", "sibling")', 'open sibling javascript': 'open("javascript:parent.postMessage(String(1), String.fromCharCode(42))", "sibling")', 'open foreign javascript': 'open("javascript:void(0)", "foreign")',
  'link _top': 'link("_top", T)', 'link _parent': 'link("_parent", T)', 'link sibling': 'link("sibling", T)', 'link foreign': 'link("foreign", T)', 'link child': 'link("child", T)', 'link new': 'link("unknown", T)', 'link base': 'link(null, T)', 'link _self': 'link("_self", T)', 'open renamed': '(frames.child.name = "renamed", open(T, "renamed"))', 'opener': 'open("", "child").opener', 'opener disowned': '(open("", "child").opener = null, frames.child.opener)', 'opener shadowed': '(frames.child.opener = 5, frames.child.opener)', 'opener none': 'frames.child.opener',
  'top.location removed': 'top.location = T; frameElement.remove()', 'sibling.location removed': 'top.frames.sibling.location = T; frameElement.remove()', 'link _top removed': 'link("_top", T); frameElement.remove()',
  'async orientation': 'screen.orientation.lock("any").then(() => "locked", told)', 'presentation': 'new PresentationRequest("https://a.example/") && "made"',
  'link noreferrer': 'link("sibling", T, "noreferrer")', 'link noopener': 'link("sibling", T, "noopener")', 'link newline': 'link("sib\\n<ling", T)', 'link SIBLING': 'link("SIBLING", T)', 'link _self noreferrer': 'link("_self", T, "noreferrer")',
};
// The actors that do everything, and what the rest do: the calls that reach no window of another origin.
const FULL = ['same', 'sandbox origin', 'srcdoc', 'same in other'];
const PAGE = Object.keys(ACTS).filter((name) => !/^(top|own|link _parent)|removed/.test(name));
const SOME = Object.keys(ACTS).filter((name) => /^(open|link|async|presentation)/.test(name) && !/(noopener=0|nothing _top|own name|empty|invalid|hash|SIBLING|newline|noreferrer|noopener|opener|renamed|removed)/.test(name));
const SAME = /^(same|page|srcdoc|sandbox origin)/;
const actsOf = (actor) => actor === 'page' ? PAGE : FULL.includes(actor) ? Object.keys(ACTS) : actor === 'other' ? Object.keys(ACTS).filter((name) => /^(own|open|link|async|presentation)/.test(name) && !/^opener|renamed|removed/.test(name)) : SAME.test(actor) ? [...SOME, 'top.location', 'top.location other', 'sibling.location', 'child.location'] : SOME;
const query = new URLSearchParams(location.search);
const actor = query.get('actor') ?? globalThis.ACTOR, at = Number(query.get('at'));
const rest = (index) => `actor=${encodeURIComponent(actor)}&at=${index}`;
// The top-level window's fetch() where it can be reached, since a frame that has gone has none.
let LOGGER = window;
try {
  if (top.document) LOGGER = top;
} catch {
  // Of another origin.
}
const log = (entry) => LOGGER.fetch(`${A}/log`, {method: 'POST', mode: 'no-cors', keepalive: true, body: JSON.stringify(entry)});
const plain = (text) => text.replaceAll(A, 'A').replaceAll(B, 'B').replace(/[?][^']*/, '');
const told = ({name, message}) => plain(`${name}: ${message}`);
const make = (attributes, name) => {
  const element = document.createElement('iframe');
  element.name = name;
  for (const key in attributes) element.setAttribute(key, attributes[key]);
  const loaded = new Promise((resolve) => { element.onload = resolve; });
  document.body.append(element);
  return loaded.then(() => element);
};
const link = (target, url, rel) => {
  const anchor = document.createElement('a');
  anchor.href = url;
  if (rel) anchor.rel = rel;
  if (target === null) {
    const base = document.createElement('base');
    base.target = 'sibling';
    document.head.append(base);
  } else {
    anchor.target = target;
  }
  document.body.append(anchor);
  anchor.click();
};
// Acts as the case named `name` at `index`, and logs what it gave.
const perform = async (name, index) => {
  const T = `${A}/moved?${rest(index)}`, O = `${B}/moved?${rest(index)}`, TOP = `${A}/top?${rest(index)}`;
  const which = (value) => value === null ? 'null' : value === undefined ? 'nothing' : typeof value !== 'object' ? 'value' : value === top ? 'top' : value === window ? 'self' : value === parent ? 'parent' : value === frames.child ? 'child' : SAME.test(actor) && value === top.frames.sibling ? 'sibling' : SAME.test(actor) && value === top.frames.foreign ? 'foreign' : 'a window';
  let result;
  try {
    const given = (0, eval)(`(T, O, TOP, link, told) => { ${/^\S*location(\.href)? = |; /.test(ACTS[name]) ? '' : 'return '}${ACTS[name]}; }`)(T, O, TOP, link, told);
    result = name.startsWith('async') ? await given : which(given);
  } catch (error) {
    result = told(error);
  }
  await log({at: index, result});
};
const run = async () => {
  if (location.pathname === '/top') {
    const acts = actsOf(actor);
    addEventListener('message', ({data}) => data === '1' && log({at: current, navigate: 'a javascript: URL ran'}));
    navigation.addEventListener('navigate', (event) => log({at: current, navigate: event.hashChange ? 'hash' : plain(event.destination.url)}));
    let current = at;
    for (; current < acts.length; ++current) {
      document.body.replaceChildren();
      document.querySelector('base')?.remove();
      await make({src: `${A}/plain`}, 'sibling');
      await make({src: `${B}/plain`}, 'foreign');
      if (actor === 'page') {
        await make({src: `${A}/plain`}, 'child');
        await perform(acts[current], current);
      } else {
        const {src, srcdoc, ...more} = ACTORS[actor];
        const attributes = srcdoc ? {srcdoc: `<!doctype html><body><script>ACTOR = '${actor}';<` + `/script><script src="${A}/cases.js"><` + `/script><script>run()<` + `/script>`} : {src: `${src}${src.includes('?') ? '&' : '?'}${rest(current)}`};
        const ready = new Promise((resolve) => addEventListener('message', function listener({data}) {
          if (data !== 'ready') return;
          removeEventListener('message', listener);
          resolve();
        }));
        const element = await make({...attributes, ...more}, 'actor');
        await ready;
        postMessage.call(element.contentWindow, {act: acts[current], index: current}, '*');
      }
      // Answered once what the act does is done.
      await fetch(`/settle?at=${current}`);
    }
    await fetch('/finished', {method: 'POST'});
  } else if (location.pathname === '/moved') {
    await log({at, moved: window === top ? 'top' : name, referrer: plain(document.referrer), opener: opener === null ? 'null' : opener === top ? 'top' : opener === window ? 'self' : 'a window'});
    if (window === top) location.replace(`${A}/top?${rest(at + 1)}`);
  } else if (location.pathname === '/nest') {
    // Holds the frame that its query names, and passes on what its parent and that say.
    const inner = document.createElement('iframe');
    addEventListener('message', ({source, data}) => postMessage.call(source === parent ? inner.contentWindow : parent, data, '*'));
    inner.name = 'actor';
    inner.src = `${query.get('of')}?${rest(at)}`;
    document.body.append(inner);
  } else if (location.pathname === '/frame' || location.href === 'about:srcdoc') {
    addEventListener('message', ({data}) => data.act && perform(data.act, data.index));
    await make({src: `${A}/plain`}, 'child');
    postMessage.call(parent, 'ready', '*');
  }
};
