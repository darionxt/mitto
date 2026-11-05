# Parity Checks

A **Parity Check** compares what the native core sends with what real Chrome sent, as recorded in a **Baseline**. Everything the rest of milestone M1 does is measured with them. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

## Run them

The checks need a native library ([building.md](building.md)) and no network connection, because they only talk to a capture server on loopback:

```bash
export MITTO_NATIVE_LIBRARY="$MITTO_WORKSPACE/chromium-<tag>/src/out/mitto/libmitto_core.dylib"
uv run pytest tests/parity
```

On Linux the file is `libmitto_core.so`. Without a native library, the tests that make requests are skipped and the others still run.

## What a check does

For each Baseline, it:

1. starts a [capture server](capture.md);
2. creates a new native Session that accepts the server's private certificate and has the **Persona**'s settings. For an `h3` Baseline it also forces QUIC on the server's HTTP/3 origin, as the Chrome that recorded it was told to, because `//net` refuses QUIC to a private certificate otherwise;
3. requests the path of the Baseline's **Request Kind** on the port of the Baseline's protocol, with what the Persona sends for that kind ([persona.md](persona.md));
4. takes the normal form of what the server received, and compares it with the Baseline.

Each comparison covers what the connection looks like and the one request. So a difference in the connection, such as in the ClientHello, shows up once for each Request Kind.

Chrome 157 on Windows is the only Persona so far, so a check of any Baseline presents that one.

## Baselines

A Baseline file is the normal form the capture kit writes, at `tests/baselines/RELEASE/NAME.json`, where `RELEASE` is the **Chrome Release**. The file holds one Baseline for each Request Kind in it, and the protocol comes from the file's `protocol` key. [baselines.md](baselines.md) is how to record them.

Other files under `RELEASE` are left alone: `*.raw.json` and anything that does not end in `.json`, such as `chrome.txt`, which says which Chrome recorded the Baselines.

## Gaps

A gap is one field in which the two normal forms differ. A failure names the Chrome Release, Request Kind, protocol and field, with both values:

```text
Chrome 157, navigation, h2, request.headers.accept-encoding
  Baseline: "accept-encoding: gzip, deflate, br, zstd"
  mitto:    "accept-encoding: gzip, deflate"
```

`(nothing)` means that side does not have the field. The comparison looks only at the shape of the data, so new fields in the normal form need no change here:

| Data | Compares | Example field |
|---|---|---|
| An object | Key by key | `tls.legacy_version` |
| A list of objects with a `type` | Item by item, by `type` | `tls.extensions.key_share.shares` |
| A list of `name: value` lines | Line by line, by name, with the case sent | `request.headers.user-agent`, `http2.preface.SETTINGS.settings.ENABLE_PUSH` |
| Anything else, such as a list of plain values | As a whole | `tls.cipher_suites` |

The request's own fields come under `request`. If several items have the same `type` or name, the later ones get a number, such as `tls.extensions.GREASE#2`. If the items both sides have are in a different order, that is one more gap, in a field such as `request.headers.<order>`, whose values are the names in each order.

A request that gets no response has nothing to compare. It is one gap in the field `response`, whose value for mitto is the net error.

## The known-gaps list

`tests/baselines/known_gaps.json` holds every **Known Gap**, one per line, with the keys `release`, `kind`, `protocol`, `field`, `baseline` and `mitto`. Here `null` means that side does not have the field.

A run fails on a gap that is not in the list and on a gap in the list that no longer exists. A gap whose value has changed counts as both, so the list always says exactly how mitto differs from the Baselines.

There is one list for both **Hosts**, because a Wire Fingerprint must not depend on the Host. A gap on only one Host fails the run on the other.

To write the list again from what the native core does now:

```bash
uv run python -m tools.parity
```

This needs a native library, and it runs the same comparison as the tests. `--baselines DIR` uses another directory of Baselines, with its own list. Read the diff before you commit it. Fixing a gap only removes lines. An added line means mitto moved away from a Baseline, or a Baseline changed.

## Tests of the comparison

Real Baselines are recorded by hand on a Windows machine, so `tests/parity` tests the comparison itself with a sample: the real normal forms of Chrome 156 on macOS in `tests/capture/data`. It changes that sample on purpose and expects the run to fail, both through the native core and, where there is no native library, through a stand-in for it.
