// A top-level document with frames of its own origin, of another site, and of its own origin within one of another site, each
// of which runs tests/frames_storage_child.js. It is given the other site's origin, and answerOf() has one of them act.
(async (other) => {
  (0, eval)(await (await fetch('/child.js')).text());
  const add = async (src) => {
    const element = document.createElement('iframe');
    const done = new Promise((resolve) => element.addEventListener('load', resolve, {once: true}));
    element.src = src;
    document.body.append(element);
    await done;
    return element;
  };
  const elements = {same: await add('/child'), other: await add(`${other}/child`), other_same: await add(`${other}/child?nest=${location.origin}/child`)};
  globalThis.answerOf = (name, message) =>
    name === 'top' ? act(message) : ask(elements[name].contentWindow, name === 'other_same' ? {inner: message} : message);
  const names = ['top', ...Object.keys(elements)];
  for (const name of names) await answerOf(name, {act: 'listen'});
  return names;
})
