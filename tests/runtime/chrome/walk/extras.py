"""What a walk finds that the generator's listing does not have. Throwaway, for darionxt/mitto#207, never merged.

    python extras.py web_idl_listing.txt WALK.json

Prints the globals that are neither in the listing nor V8's builtins, and the own properties of each interface object
and prototype that the listing does not name.
"""
import json, re, sys
listing, walkfile = sys.argv[1], sys.argv[2]
ifaces = {}
cur = None
for line in open(listing, encoding='utf-8'):
    m = re.match(r'^(interface|namespace) (\w+)(?: : (\w+))?(.*)$', line)
    if m:
        cur = m.group(2)
        ifaces[cur] = {'kind': m.group(1), 'rest': m.group(4), 'members': set(), 'lines': []}
        continue
    m = re.match(r'^  (\w+)(\.prototype| instance)?\.([^:]+):', line)
    if m and cur:
        ifaces[cur]['members'].add(((m.group(2) or '').strip(), m.group(3)))
    m2 = re.match(r'^  (\w+)\.length', line)
window = {n for n, i in ifaces.items() if 'exposed on' in i['rest'] and ('Window' in i['rest'].split('[')[0] or 'exposed on *' in i['rest'])}
walk = json.load(open(walkfile))
nodes = {n['p']: n for n in walk['nodes']}
g = nodes['globalThis']
win_members = {name for target, name in ifaces['Window']['members']}
v8 = set('''Object Function Array Number parseFloat parseInt Infinity NaN undefined Boolean String Symbol Date Promise RegExp Error AggregateError EvalError RangeError ReferenceError SyntaxError TypeError URIError globalThis JSON Math Intl ArrayBuffer Atomics Uint8Array Int8Array Uint16Array Int16Array Uint32Array Int32Array BigUint64Array BigInt64Array Uint8ClampedArray Float32Array Float64Array Float16Array DataView Map BigInt Set WeakMap WeakSet Proxy Reflect FinalizationRegistry WeakRef decodeURI decodeURIComponent encodeURI encodeURIComponent escape unescape eval isFinite isNaN console SharedArrayBuffer WebAssembly Iterator SuppressedError DisposableStack AsyncDisposableStack Temporal'''.split())
extras = []
for key, attrs, *vals in g['own']:
    if key in window or key in v8 or key in win_members or key.startswith('@@'):
        continue
    extras.append(('global', key, attrs))
print('globals not in the listing and not V8 builtins:', extras)
print('V8-builtin names present:', sorted(k for k, *_ in g['own'] if k in v8))
std = {'length', 'name', 'prototype', 'constructor', 'arguments', 'caller'}
for name in sorted(window):
    for path, target in ((f'globalThis.{name}', ''), (f'globalThis.{name}.prototype', '.prototype')):
        n = nodes.get(path)
        if not n:
            continue
        listed = {m for t, m in ifaces[name]['members'] if t == target}
        extra = [o[0] for o in n['own'] if o[0].replace('@@Symbol.', '@@') not in listed and o[0] not in std]
        if extra:
            print(path, 'extra', extra)
# instances: window's own accessors not in Window members
own = [o[0] for o in g['own'] if o[1].startswith('-') or len(o) == 4]
print('namespaces/objects by path:')
for p in ('globalThis.console', 'globalThis.chrome', 'globalThis.WebAssembly', 'globalThis.Intl', 'globalThis.CSS', 'globalThis.Atomics', 'globalThis.Math', 'globalThis.JSON', 'globalThis.Reflect'):
    n = nodes.get(p)
    if n: print(' ', p, [o[0] for o in n['own']])
