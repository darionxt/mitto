function done(result) { window.result = result; fetch('/record', {method: 'POST', body: JSON.stringify(result)}); }
const base = location.href.replace(/[^/]*$/, '');
const strip = s => typeof s === 'string' ? s.split(base).join('BASE/') : s;
const describe = v => v === null ? 'null' : v === undefined ? 'undefined' : v instanceof Error ? v.constructor.name + ':' + v.message : typeof v === 'object' ? Object.prototype.toString.call(v) : typeof v + ':' + String(v);
window.log = [];
window.onerror = function (message, filename, lineno, colno, error) {
  log.push(['onerror', this === window, arguments.length, strip(message), strip(filename), lineno, colno, describe(error)]);
  return message.includes('cancel me');
};
window.addEventListener('error', e => {
  log.push(['error listener', e.constructor.name, e.bubbles, e.cancelable, e.isTrusted, e.eventPhase, e.target === window, strip(e.message), strip(e.filename), e.lineno, e.colno, describe(e.error), e.defaultPrevented]);
});
window.addEventListener('error', e => log.push(['error capture', e.eventPhase]), true);
window.addEventListener('unhandledrejection', e => {
  log.push(['unhandledrejection', e.constructor.name, e.cancelable, e.isTrusted, describe(e.reason), e.promise instanceof Promise]);
  if (String(e.reason).includes('cancel me')) e.preventDefault();
  // A listener that handles the promise without canceling the event: the console still hears of it.
  if (String(e.reason).includes('handled by the listener')) e.promise.catch(() => {});
  if (String(e.reason).includes('handle later')) setTimeout(() => { e.promise.catch(() => {}); log.push('handler added'); }, 0);
});
window.addEventListener('rejectionhandled', e => log.push(['rejectionhandled', e.cancelable, e.isTrusted, describe(e.reason)]));
