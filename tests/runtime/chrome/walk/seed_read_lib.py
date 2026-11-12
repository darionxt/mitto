"""A minimal reader of protocol buffers, for variations seeds. Throwaway, for darionxt/mitto#207, never merged."""
def varint(b, i):
    r = s = 0
    while True:
        c = b[i]; i += 1; r |= (c & 0x7f) << s; s += 7
        if c < 0x80: return r, i
def fields(b):
    i = 0; out = []
    while i < len(b):
        key, i = varint(b, i); f, t = key >> 3, key & 7
        if t == 0: v, i = varint(b, i)
        elif t == 2: n, i = varint(b, i); v = b[i:i+n]; i += n
        elif t == 1: v = b[i:i+8]; i += 8
        elif t == 5: v = b[i:i+4]; i += 4
        else: raise ValueError(t)
        out.append((f, v))
    return out
