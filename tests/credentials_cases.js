// What navigator.credentials.get() gives for each of these options, which
// tests/test_page_program.py runs in a Page at http://127.0.0.1:8722/, a secure
// page on an IP address, and at http://localhost:8722/, a secure page on a
// domain, in order, and compares with what Chrome 157.0.8103.51 gave for the
// same script from a new profile, in tests/data/credentials_cases.json. Each
// case's value is what the request gave within a second or so: "pending" for
// one that waits. A WebAuthn request that waits holds the browser's one slot,
// so on localhost the requests after the first are refused until an abort
// cancels it. Its value is the record as JSON.

(async () => {
  const wait = (p, ms = 1000) => Promise.race([p.then(v => 'resolved ' + v, e => 'rejected ' + e), new Promise(r => setTimeout(() => r('pending'), ms))]);
  const get = (o, ms) => wait(navigator.credentials.get(o), ms);
  const pk = (extra = {}, publicKey = {}) => Object.assign({publicKey: Object.assign({challenge: new Uint8Array(16)}, publicKey)}, extra);
  const order = async (a, b) => { const seen = []; a.then(() => seen.push('a ok'), e => seen.push('a ' + e.name)); b.then(() => seen.push('b ok'), e => seen.push('b ' + e.name)); await new Promise(r => setTimeout(r, 1000)); return seen.join(); };
  const CASES = {
    "no type": "return await get()",
    "silent alone": "return await get({mediation: 'silent'})",
    "password": "return await get({password: true})",
    "password required": "return await get({password: true, mediation: 'required'})",
    "federated": "return await get({federated: {providers: ['https://a.test']}})",
    "identity without providers": "return await get({identity: {}})",
    "password twice": "return await order(navigator.credentials.get({password: true}), navigator.credentials.get({password: true}))",
    "password after password": "return [await get({password: true}), await get({password: true})].join()",
    "password settles in a later task": "const seen = []; navigator.credentials.get({password: true}).then(() => seen.push('settled')); setTimeout(() => seen.push('task'), 0); await new Promise(r => setTimeout(r, 300)); return seen.join()",
    "rejection is at once": "let s = 'pending'; navigator.credentials.get().catch(() => s = 'rejected'); await Promise.resolve(); await Promise.resolve(); return s",
    "conditional without publicKey": "return await get({password: true, mediation: 'conditional'})",
    "immediate password": "return await get({password: true, uiMode: 'immediate'})",
    "immediate federated": "return await get({federated: {}, uiMode: 'immediate'})",
    "aborted signal": "return await get({password: true, signal: AbortSignal.abort('why')})",
    "otp without transport": "return await get({otp: {}})",
    "otp sms": "return await get({otp: {transport: ['sms']}})",
    "otp sms twice": "return await order(navigator.credentials.get({otp: {transport: ['sms']}}), navigator.credentials.get({otp: {transport: ['sms']}}))",
    "otp abort": "const c = new AbortController(); const p = get({otp: {transport: ['sms']}, signal: c.signal}); c.abort(); return await p",
    "publicKey": "return await get(pk(), 1500)",
    "publicKey conditional": "return await get(pk({mediation: 'conditional'}), 1500)",
    "publicKey twice": "return await order(navigator.credentials.get(pk()), navigator.credentials.get(pk()))",
    "publicKey abort": "const c = new AbortController(); const p = get(pk({signal: c.signal}), 1500); await new Promise(r => setTimeout(r, 300)); c.abort('stop'); return await p",
    "publicKey abort then again": "const c = new AbortController(); const a = get(pk({signal: c.signal}), 1500); c.abort(); const b = get(pk(), 1500); return [await a, await b].join(' | ')",
    "publicKey rpId not a host": "return await get(pk({}, {rpId: 'a b'}), 1500)",
    "publicKey rpId IP": "return await get(pk({}, {rpId: '127.0.0.1'}), 1500)",
    "publicKey rpId suffix": "return await get(pk({}, {rpId: location.hostname}), 1500)",
    "challenge missing": "return await get({publicKey: {}})",
    "challenge string": "return await get({publicKey: {challenge: 'x'}})",
    "challenge shared buffer": "return await get({publicKey: {challenge: new WebAssembly.Memory({initial: 1, maximum: 1, shared: true}).buffer}})",
    "challenge shared view": "return await get({publicKey: {challenge: new Uint8Array(new WebAssembly.Memory({initial: 1, maximum: 1, shared: true}).buffer)}})",
    "challenge resizable buffer": "return await get({publicKey: {challenge: new ArrayBuffer(8, {maxByteLength: 16})}})",
    "challenge view on a resizable buffer": "return await get({publicKey: {challenge: new Uint8Array(new ArrayBuffer(8, {maxByteLength: 16}))}})",
    "challenge buffer": "return await get({publicKey: {challenge: new ArrayBuffer(8)}}, 1500)",
    "allowCredentials over 64": "const d = []; for (let i = 0; i < 65; i++) d.push({id: new Uint8Array([i]), type: 'public-key'}); try { await navigator.credentials.get(pk({}, {allowCredentials: d})); return 'resolved'; } catch (e) { return [e.name, e.message, e instanceof DOMException, e.code].join('|'); }",
    "allowCredentials id missing": "return await get(pk({}, {allowCredentials: [{type: 'public-key'}]}))",
    "credProps": "return await get(pk({}, {extensions: {credProps: true}}))",
    "largeBlob support": "return await get(pk({}, {extensions: {largeBlob: {support: 'required'}}}))",
    "largeBlob write": "return await get(pk({}, {extensions: {largeBlob: {write: new Uint8Array(2049)}}}))",
    "prf with an empty allow list": "return await get(pk({}, {extensions: {prf: {evalByCredential: {AQ: {first: new Uint8Array(1)}}}}}))",
    "prf bad base64": "return await get(pk({}, {allowCredentials: [{id: new Uint8Array([1]), type: 'public-key'}], extensions: {prf: {evalByCredential: {'+': {first: new Uint8Array(1)}}}}}))",
    "prf unknown id": "return await get(pk({}, {allowCredentials: [{id: new Uint8Array([1]), type: 'public-key'}], extensions: {prf: {evalByCredential: {Ag: {first: new Uint8Array(1)}}}}}))",
    "prf large input": "return await get(pk({}, {extensions: {prf: {eval: {first: new Uint8Array(257)}}}}))",
    "appid not a URL": "return await get(pk({}, {extensions: {appid: 'not a url'}}))",
    "appid of another site": "return await get(pk({}, {extensions: {appid: 'https://example.com/'}}), 1500)",
    "conditional immediate": "return await get(pk({mediation: 'conditional', uiMode: 'immediate'}))",
    "immediate publicKey": "return await get(pk({uiMode: 'immediate'}))",
    "immediate allowCredentials": "return await get(pk({uiMode: 'immediate'}, {allowCredentials: [{id: new Uint8Array([1]), type: 'public-key'}]}))",
    "digital no requests": "return await get({digital: {requests: []}})",
    "digital": "return await get({digital: {requests: [{protocol: 'openid4vp-v1-unsigned', data: {}}]}})",
    "digital unknown protocol": "return await get({digital: {requests: [{protocol: 'x', data: {}}]}})",
    "identity no providers": "return await get({identity: {providers: []}})",
    "identity eleven providers": "const p = []; for (let i = 0; i < 11; i++) p.push({configURL: 'https://idp' + i + '.test/c.json', clientId: 'x'}); return await get({identity: {providers: p}})",
    "identity no clientId": "return await get({identity: {providers: [{configURL: 'https://idp.test/c.json'}]}})",
    "identity empty clientId": "return await get({identity: {providers: [{configURL: 'https://idp.test/c.json', clientId: ''}]}})",
    "identity relative config": "return await get({identity: {providers: [{configURL: 'c.json', clientId: 'x'}]}})",
    "identity conditional": "return await get({identity: {providers: [{configURL: 'https://idp.test/c.json', clientId: 'x'}]}, mediation: 'conditional'})",
    "identity active silent": "return await get({identity: {mode: 'active', providers: [{configURL: 'https://idp.test/c.json', clientId: 'x'}]}, mediation: 'silent'})",
    "identity active two": "return await get({identity: {mode: 'active', providers: [{configURL: 'https://idp.test/c.json', clientId: 'x'}, {configURL: 'https://idp2.test/c.json', clientId: 'x'}]}})",
    "identity active": "return await get({identity: {mode: 'active', providers: [{configURL: 'https://idp.test/c.json', clientId: 'x'}]}}, 1500)",
    "identity duplicate": "return await get({identity: {providers: [{configURL: 'https://idp.test/c.json', clientId: 'x'}, {configURL: 'https://idp.test/c.json', clientId: 'y'}]}}, 1500)",
    "identity http provider": "return await get({identity: {providers: [{configURL: 'http://idp.test/c.json', clientId: 'x'}]}}, 1500)",
    "identity params throws": "return await get({identity: {providers: [{configURL: 'https://idp.test/c.json', clientId: 'x', params: {get a() { throw new Error('boom'); }}}]}})",
    "identity passive": "return await get({identity: {providers: [{configURL: 'https://idp.test/c.json', clientId: 'x'}]}}, 1500)",
    "same object": "return [navigator.credentials === navigator.credentials, Object.prototype.toString.call(navigator.credentials)].join()",
    "get on another receiver": "try { await CredentialsContainer.prototype.get.call({}, {password: true}); return 'resolved'; } catch (e) { return 'rejected ' + e; }",
    "options not an object": "return await get(5)",
    "isSecureContext and origin": "return [isSecureContext, location.protocol, location.hostname].join()",
  };
  const run = async (script) => {
    try {
      const value = await eval("(async () => {" + script + "})()");
      return "value: " + (typeof value === "object" && value !== null
          ? Object.prototype.toString.call(value) : String(value));
    } catch (error) {
      return "threw: " + error;
    }
  };
  const out = {};
  for (const [name, script] of Object.entries(CASES)) {
    out[name] = await run(script);
  }
  return JSON.stringify(out);
})()
