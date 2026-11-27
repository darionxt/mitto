// What a dedicated worker asks for, whose Referer tests/test_page_worker_referrers.py compares with Chrome 157's. A
// classic worker imports a script, and either kind fetches, imports a module and starts a worker of its own, which
// asks for the same but for a worker.
const query = new URL(location.href).searchParams;
const urlOf = (what) => `/ask?case=${encodeURIComponent(query.get('case'))}&what=${query.get('nested') ? 'nested ' : ''}${what}`;
(async () => {
  try {
    importScripts(`${urlOf('importScripts')}&script=1`);
  } catch {
    // A module worker's throws.
  }
  await fetch(urlOf('fetch'));
  await fetch(urlOf('unsafe-url fetch'), {referrerPolicy: 'unsafe-url'});
  await fetch(urlOf('no-referrer fetch'), {referrerPolicy: 'no-referrer'});
  await fetch(urlOf('fetch with a referrer'), {referrer: '/other'});
  await fetch(new Request(urlOf('request')));
  await import(`${urlOf('import()')}&script=1`);
  if (!query.get('nested')) {
    const worker = new Worker(`${urlOf('worker')}&script=1&worker=1&nested=1`);
    await new Promise((resolve) => {
      worker.onmessage = resolve;
      worker.onerror = resolve;
    });
  }
  postMessage('done');
})();
