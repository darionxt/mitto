# The capture kit

The capture kit records what a client sends to a server: the raw TLS ClientHello, the decrypted HTTP/2 frames or HTTP/1.1 requests, and a **normal form** of them that compares equal between recordings of the same client. The normal form is also the **Baseline** file format, and **Parity Checks** compare in it. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md).

The kit lives in `tools/capture`. It needs nothing but [uv](https://docs.astral.sh/uv/) and runs on Windows, macOS and Linux. It does not need the native core, and its dependencies are in the development-only `dev` group.

HTTP/3 is not part of the kit yet.

## Record a client

From the repository root, start the server:

```bash
uv run python -m tools.capture --out recordings
```

It prints the hash of a certificate it just generated, one address per protocol, and the Chrome command to use:

```text
SPKI hash: s7oEk1pfWBlrJ4HC8N6EMoT0vFzSxCKHUgHGBCo1Ejc=
HTTP/1.1:  https://capture.localhost:8443/
HTTP/2:    https://capture.localhost:8444/

Start each recording from a new, empty user data directory:
  chrome --user-data-dir="NEW_EMPTY_DIRECTORY" --ignore-certificate-errors-spki-list=s7oEk1pfWBlrJ4HC8N6EMoT0vFzSxCKHUgHGBCo1Ejc= --no-first-run --no-default-browser-check URL
```

Open each address in a Chrome started that way. Chrome ignores `--ignore-certificate-errors-spki-list` unless `--user-data-dir` is there too. Each address loads a test page, which tells the server when it has finished. The server then writes that protocol's files and, once it has both, exits:

| File | Holds |
|---|---|
| `recordings/http1.json`, `recordings/http2.json` | The normal form |
| `recordings/http1.raw.json`, `recordings/http2.raw.json` | The raw recording: every connection, with its ClientHello bytes, every frame and every request |

To check that a client records the same twice, record it into two directories and compare the files.

`--host` sets another name ending in `.localhost`, and `--http1-port` and `--http2-port` set other ports. The normal form does not depend on any of them.

## What the server does

- It serves at a `*.localhost` name. Chrome and the network stack resolve these names to loopback themselves, with no DNS or hosts file entry, and unlike an IP address such a name gets SNI. The server listens only on loopback.
- It serves HTTP/1.1 and HTTP/2 on separate ports. Each port offers one protocol through ALPN. A client that does not take it is served HTTP/1.1, and the normal form shows that.
- It gives out no session tickets. Every connection is therefore a fresh one, with no `pre_shared_key`, including the later connections of one page load.
- Its private certificate is new on every start, so the hash changes too.

Each main **Request Kind** has a path, and the test page requests all of them from one same-origin **Initiator**:

| Request Kind | Path | Requested by |
|---|---|---|
| navigation | `/` | Opening the address |
| script | `/script.js` | A `<script>` element in the page |
| image | `/image.png` | An `<img>` element in the page |
| fetch | `/fetch` | `fetch()` in the page |

When those have finished, the page requests `/done`, which is no Request Kind. A request to any other path gets a 404, and it stays in the raw recording only.

## The normal form

A normal form is JSON. Its keys are always in the same order, and a Baseline file is exactly this text.

```json
{
  "format": 1,
  "protocol": "h2",
  "tls": {
    "record_version": "TLS 1.0",
    "legacy_version": "TLS 1.2",
    "session_id_length": 32,
    "cipher_suites": ["GREASE", "TLS_AES_128_GCM_SHA256", "..."],
    "compression_methods": ["null"],
    "extensions": [
      {"type": "GREASE", "length": 0},
      {"type": "supported_groups", "groups": ["GREASE", "X25519MLKEM768", "x25519", "..."]},
      {"type": "encrypted_client_hello", "client_hello": "outer", "kdf": "HKDF-SHA256", "aead": "AES-128-GCM"}
    ]
  },
  "http2": {
    "preface": [
      {"type": "SETTINGS", "settings": ["HEADER_TABLE_SIZE: 65536", "ENABLE_PUSH: 0", "..."]},
      {"type": "WINDOW_UPDATE", "stream": 0, "increment": 15663105}
    ]
  },
  "requests": {
    "navigation": {
      "flags": ["END_STREAM", "END_HEADERS", "PRIORITY"],
      "priority": {"exclusive": true, "weight": 256},
      "headers": [":method: GET", ":authority: <authority>", ":scheme: https", ":path: /", "..."]
    }
  }
}
```

(The server writes one array item per line. This example is shortened.)

| Key | Meaning |
|---|---|
| `format` | The version of this format. |
| `protocol` | The protocol the connection settled on: `http/1.1` or `h2`. |
| `tls` | The ClientHello. |
| `http2` | Only for `h2`. `preface` holds the frames the client sent before its first request, without acknowledgements of what the server sent. |
| `requests` | One entry per Request Kind recorded, always in the order navigation, fetch, script, image. If a Request Kind was requested twice, the first request counts. |

An HTTP/1.1 request has `request_line` and `headers`. An HTTP/2 request has the `flags` of its HEADERS frame, the frame's `priority` if it has any, `headers` and, if there were any, `later_frames`: what the client sent on the stream after the headers, without bodies or encoded header blocks. Headers are written `name: value`, in the order sent and with the case sent, and repeated headers stay separate.

### What stays, and what does not

Whatever a server can observe and a client decides stays as it was sent, in the order it was sent. Everything else follows these rules:

| Part | In the normal form | Why |
|---|---|---|
| Order of TLS extensions | Sorted: GREASE first, then by number | Chrome permutes them on every connection |
| GREASE values in cipher suites, extensions, groups, versions, signature algorithms and key shares | The placeholder `GREASE` | Chrome picks them at random |
| GREASE HTTP/2 settings and frames | The placeholder `GREASE`, with no value, flags or payload | The same |
| ClientHello random and session ID | Dropped, except the session ID's length | Random |
| Key shares | The group and the key's length | The key is random |
| Encrypted ClientHello (ECH) | The ClientHello type and the KDF and AEAD | The configuration ID, encapsulated key and payload are random, and so is the payload's size |
| `pre_shared_key`, `session_ticket`, `cookie`, `early_data`, `padding` | Presence only | Their content changes between connections, or follows from the rest of the ClientHello |
| Server name | The placeholder `<host>` | It says where the server is, not who the client is |
| The server's name and port in header values | The placeholders `<authority>` and, with `https://`, `<origin>` | The same |
| HTTP/2 stream IDs and stream dependencies of requests and `later_frames` | Dropped | They depend on which requests happened to be open. In `preface` they stay, because no request has started yet |

A number with a known name is written as that name, such as `TLS_AES_128_GCM_SHA256`. Any other number stays in hexadecimal, such as `0x1234`, and an extension with no known structure keeps its content in hexadecimal too. If a known extension is malformed, its content shows under `malformed`. Naming another number later changes the normal forms that hold it, so those need recording again.

Only connections that carried a request of a Request Kind count. Chrome also opens connections that never carry a request, and it can spread one page's requests over several connections. Those that count must all have the same ClientHello and preface, or the recording has no normal form and the kit says so.

### What it does not capture yet

- **ALPS.** Python's `ssl` cannot negotiate it, so the normal form has Chrome's offer but not the settings Chrome would then send.
- **Resumed connections.** The server gives out no tickets, so the normal form describes fresh connections only.
- **Frames that depend on the response or on timing.** The normal form leaves out frames sent after the first request on stream 0, such as a later `SETTINGS` or `PING`. They remain in the raw recording.
- **Later ClientHellos.** After a HelloRetryRequest, only the first ClientHello is recorded.

### What can differ between Chrome installations

A fresh user data directory has no variations seed, so it shows no field trials. Chrome's trust anchors, in the `trust_anchors` extension, start from its compiled-in root store, and a component update can change them without a new Chrome Release.

## From Python

A Parity Check can run the server itself. It picks free ports unless told otherwise, and it listens as long as the `with` block runs:

```python
from tools.capture import HTTP2, KIND_PATHS, CaptureServer, RequestKind, dumps_normal_form, normal_form

with CaptureServer() as server:
    url = f"https://{server.authority(HTTP2)}{KIND_PATHS[RequestKind.FETCH]}"
    ...  # Make the request to url, with server.spki_hash trusted.
    form = normal_form(server.recording(HTTP2))
print(dumps_normal_form(form))
```

`normal_form` includes only the Request Kinds in the recording, so a recording of one request gives a normal form with one entry in `requests`. It raises `RecordingError` when a recording has no normal form.

## Tests

`uv run pytest tests/capture` runs the kit's tests, including its fuzz targets on a fixed set of inputs. Files under `tests/capture/data` hold a real recording of Chrome, which the tests replay.

To run the same check against a real Chrome, headless, from fresh user data directories, name its executable in `MITTO_CHROME`. Without that, the test is skipped:

```bash
MITTO_CHROME="/path/to/chrome" uv run pytest tests/capture/test_chrome.py
```

Every parser we write has a fuzz target in `tests/capture/test_fuzz.py`. They take arbitrary bytes, built mostly from mutated real traffic and well-formed structures with random content, and may only fail in the way the parser documents. To look for new failures, with new inputs and many more of them:

```bash
uv run pytest tests/capture/test_fuzz.py --hypothesis-profile=fuzz
```
