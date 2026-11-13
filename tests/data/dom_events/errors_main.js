document.body.addEventListener('ping', () => { throw new RangeError('from a listener'); });
document.body.dispatchEvent(new Event('ping'));
log.push('after ping');
setTimeout(() => { throw new SyntaxError('from a timer'); }, 0);
reportError(new Error('reported'));
reportError(42);
log.push('after reportError');
queueMicrotask(() => { throw new EvalError('from a microtask'); });
// An error listener that throws: its error goes to the console only.
window.addEventListener('error', function nested(e) {
  if (e.message.includes('nest')) { window.removeEventListener('error', nested); throw new Error('thrown in an error listener'); }
});
setTimeout(() => { throw new Error('nest'); }, 5);
Promise.reject(new Error('rejected'));
Promise.reject(new Error('cancel me'));
Promise.reject(new Error('handle later'));
Promise.reject(7);
Promise.reject(new Error('handled by the listener'));
const p = Promise.reject(new Error('caught in time'));
setTimeout(() => {}, 0);
p.catch(() => {});
const e1 = new ErrorEvent('error', {message: 'm', filename: 'f', lineno: 1, colno: 2});
const e2 = new ErrorEvent('custom');
const e3 = new ErrorEvent('custom', {error: null});
const e4 = new ErrorEvent('custom', {error: undefined});
log.push(['ErrorEvent', describe(e1.error), describe(e2.error), describe(e3.error), describe(e4.error), e1.message, e1.filename, e1.lineno, e1.colno, e1.bubbles, e1.cancelable, e2.message, e2.lineno]);
const pr = Promise.resolve(3);
const r1 = new PromiseRejectionEvent('x', {promise: pr, reason: 'why'});
const r2 = new PromiseRejectionEvent('x', {promise: 5});
let r3;
try { new PromiseRejectionEvent('x', {}); } catch (x) { r3 = x.name + ': ' + x.message; }
log.push(['PromiseRejectionEvent', r1.promise === pr, describe(r1.reason), r2.promise instanceof Promise, describe(r2.reason), r1.cancelable, r3]);
// A dispatched ErrorEvent at the window runs onerror with five arguments too.
window.dispatchEvent(new ErrorEvent('error', {message: 'dispatched', filename: 'here', lineno: 7, colno: 8, error: 9}));
// But not one at another target.
document.body.onerror = function () { log.push(['body onerror', arguments.length]); };
setTimeout(() => done(log), 100);
