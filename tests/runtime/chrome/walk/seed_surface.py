"""Studies of a variations seed that switch a feature a page can see. Throwaway, for darionxt/mitto#207, never merged.

    python seed_surface.py CHROMIUM_SRC web_idl_listing.txt SEED.pb...

A feature counts when it is a runtime-enabled feature named in the generator's listing, or the base feature behind one
(runtime_enabled_features.json5 and content/child/runtime_features.cc), or a V8Flag_ feature. SEED.pb is
VariationsSafeSeedV2 from a user data directory, through zstd -d.
"""
import re, sys, collections
sys.path.insert(0, sys.argv[0].rsplit('/', 1)[0])
from seed_read_lib import fields
S, listing, *seeds = sys.argv[1:]
lst = open(listing, encoding='utf-8').read()
surface = set(re.findall(r'RuntimeEnabled=(\w+)', lst))
# Runtime features and their base features.
j = open(S + '/third_party/blink/renderer/platform/runtime_enabled_features.json5', encoding='utf-8').read()
base_of = {}
for m in re.finditer(r'\{\s*\n\s*name: "(\w+)",(.*?)\n    \},', j, re.S):
    name, body = m.group(1), m.group(2)
    bf = re.search(r'base_feature: "(\w+)"', body)
    if bf and bf.group(1) != 'none': base_of.setdefault(bf.group(1), set()).add(name)
    elif not bf: base_of.setdefault(name, set()).add(name)
    # implied_by / depends_on chains
    for dep in re.findall(r'depends_on: \[([^\]]*)\]', body) + re.findall(r'implied_by: \[([^\]]*)\]', body):
        for d in re.findall(r'"(\w+)"', dep):
            base_of.setdefault(d, set()).add(name)
rf = open(S + '/content/child/runtime_features.cc', encoding='utf-8').read()
for m in re.finditer(r'\{\s*w(?:r)?f::Enable(\w+),\s*(?:raw_ref\()?(?:[\w:]+::)?k(\w+)', rf):
    base_of.setdefault(m.group(2), set()).add(m.group(1))
for m in re.finditer(r'\{\s*"(\w+)",\s*(?:raw_ref\()?(?:[\w:]+::)?k(\w+)', rf):
    base_of.setdefault(m.group(2), set()).add(m.group(1))
def page_runtime(base):
    out = set()
    stack = [base]
    seen = set()
    while stack:
        b = stack.pop()
        if b in seen: continue
        seen.add(b)
        for r in base_of.get(b, ()):
            out.add(r); stack.append(r)
    return out & surface
for seed in seeds:
    print('=====', seed)
    outer = fields(open(seed, 'rb').read())
    for f, v in fields(dict(outer)[1]):
        if f != 2: continue
        st = fields(v); sd = {}
        for k, x in st: sd.setdefault(k, x)
        name = sd[1].decode()
        hits = []
        groups = []
        for k, e in st:
            if k != 9: continue
            ed = {}
            for a, b in fields(e): ed.setdefault(a, b)
            en, dis = [], []
            if 12 in ed:
                fl = fields(ed[12]); en = [x.decode() for a, x in fl if a == 1]; dis = [x.decode() for a, x in fl if a == 2]
            w = ed.get(2, 0)
            groups.append((ed[1].decode(), w))
            for feat in en + dis:
                pr = page_runtime(feat)
                if pr or feat.startswith('V8Flag_'):
                    hits.append((ed[1].decode(), w, '+' if feat in en else '-', feat, sorted(pr)))
        if not hits: continue
        filt = fields(sd[10]) if 10 in sd else []
        mn = [x.decode() for a, x in filt if a == 2]; mx = [x.decode() for a, x in filt if a == 3]
        plat = [x for a, x in filt if a == 5]
        total = sum(w for _, w in groups)
        cons = {0: 'session', 1: 'permanent'}.get(sd.get(7, 0), sd.get(7))
        layer = 'layer' if 16 in sd else ''
        print(f'{name} [{cons}{" " + layer if layer else ""}] min {mn} max {mx} platforms {plat} total weight {total}')
        for h in sorted(set((a,b,c,d,tuple(e)) for a,b,c,d,e in hits)): print('    ', h[0], f'{h[1]}/{total}', h[2] + h[3], list(h[4]))
