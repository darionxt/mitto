# API reference

Every public name of `mitto`, in the order a caller uses them. Terms in **bold** are defined in [CONTEXT.md](../CONTEXT.md). [persona.md](persona.md) says what a request sends, and [parity.md](parity.md) how that compares with Chrome and what stays different. For code that uses `requests`, see [from-requests.md](from-requests.md).

Nothing a caller passes is converted, so a number that goes in a URL or a header is written as text. An argument of a type it does not take raises a `TypeError` that names it, says what it takes and the type it got, and leaves out the value, which may be a secret. A request raises it before anything goes on the wire.

`tests/test_docs.py` holds this page to the code. Each public name, method and argument has a heading here, each signature is the one in the code, and each example runs, in order, with every line reached. In that test every name in a URL lands on a local server, so no example needs a port, and a Session trusts the certificate it finds there. `/echo` answers with what it received, as JSON. A name such as `nowhere.example.test` does what it says.

```python
import mitto

with mitto.Session() as session:
    response = session.get("https://example.test/")

assert response.status == 200
assert response.text == "Hello\n"
```

## Session

### `Session`

```text
Session(
    *,
    persona: str = 'chrome_157_windows',
    machine: Machine | None = None,
    accept_language: str | None = None,
    additional_trust_anchors: Sequence[str] = (),
    timeout: float | Timeout | None = None,
    cookie_file: str | os.PathLike[str] | None = None,
    proxy: str | None = None,
    cache: str | os.PathLike[str] | None = None,
)
```

A long-lived client that holds one Chrome-like browsing state: its **Persona**, its **Machine**, cookies, cache and open connections, and [which client hints](persona.md#client-hints) each origin has asked for. Two Sessions share none of that, as two Chrome profiles do not.

One Session may run requests from several threads at once. Close it with `close()` or a `with` statement, which ends the requests still running, but not while another thread is starting one or using its cookies.

Creating one raises:

- `TypeError` if an argument is of a type it does not take, as each says, with a message that names it. One text is not a list of `additional_trust_anchors`, and [`timeout`](#timeout) says what counts as a number.
- `ValueError` if one of the arguments below cannot be used, as each says. The message leaves out `proxy`, which may hold a password.
- `OSError` if `cookie_file` exists and cannot be read, a cache cannot be kept in the directory of `cache`, or another Session has that directory open.
- `NativeLibraryError` if the native library cannot be used.

#### `persona=`

The **Persona** to present: `"chrome_155_windows"`, `"chrome_156_windows"` or `"chrome_157_windows"`. The default is the **Engine Release**'s. The others are Persona data on the same network code ([ADR-0005](adr/0005-older-releases-as-data.md)), and today they differ only in [`sec-ch-ua` and `User-Agent`](persona.md#what-differs-between-chrome-releases). Another name raises a `ValueError`, and anything but text a `TypeError`.

```python
with mitto.Session(persona="chrome_155_windows") as session:
    sent = session.get("https://example.test/echo").json()["headers"]

assert "Chrome/155" in sent["User-Agent"]
```

#### `machine=`

The **Machine** to claim: the computer whose memory, display, window, network, user settings and Windows version go in [the client hints that follow the machine](persona.md#what-they-say), once a server asks for them. The default draws one with [`Machine.draw()`](#machinedraw), so that each Session claims a machine of its own. `rtt` and `downlink` also get [Chrome's noise for each host](persona.md#the-noise-on-rtt-and-downlink), from salts that the Session draws when it opens, from the Machine's seed if it has one. A value that is neither a [`Machine`](#machine-1) nor `None` raises a `TypeError`.

```python
machine = mitto.Machine.draw(seed=7, memory=16)

with mitto.Session(machine=machine) as session:
    assert session.machine is machine
```

A Session whose [`cookie_file`](#cookie_file) holds a Machine claims that one in place of a draw, and a `machine=` that differs from it raises a `ValueError`.

#### `accept_language=`

The value of `Accept-Language` in place of the Persona's `en-US,en;q=0.9`, as a person's languages change it in Chrome. An empty string sends none. `//net` adds the line, in [the place Chrome's has](persona.md#what-comes-from-where). A value the native core refuses raises a `ValueError`, and one that is neither text nor `None` a `TypeError`.

```python
with mitto.Session(accept_language="de-DE,de;q=0.9") as session:
    sent = session.get("https://example.test/echo").json()["headers"]

assert sent["Accept-Language"] == "de-DE,de;q=0.9"
```

#### `additional_trust_anchors=`

Root certificates that this Session trusts beside the Chrome Root Store. Each entry is the text of a PEM file that holds at least one. A self-signed server certificate can be its own root, and an anchor's own dates and constraints are not enforced. A list the native core refuses raises a `ValueError`. Text on its own, which would make each of its characters an anchor, raises a `TypeError`, and so does an entry that is not text, so one PEM file goes in a list.

Chrome receives its additional certificates the same way. `//net` refuses QUIC to a root that is not known, as it does in Chrome, so a server whose chain ends at one of these is reached over HTTP/1.1 or HTTP/2, whatever its `Alt-Svc` says. On Linux a Session reads no roots from the system, and on macOS it also reads those a user has added to the Keychain ([certificate verification](parity.md#certificate-verification)).

```python
with open("company-root.pem") as file:
    company_root = file.read()

with mitto.Session(additional_trust_anchors=[company_root]) as session:
    assert session.get("https://intranet.example.test/").status == 200
```

#### `timeout=`

What a request's [`timeout=`](#timeout-1) defaults to: a number is the total time in seconds, and a [`Timeout`](#timeout-2) sets each timer. The default runs no timer, leaving what `//net` has of its own. A value that is neither a number nor a `Timeout` raises a `TypeError` that names `timeout`, and `True` and `False` do not count as numbers here. A number that is not positive, or is more than 2,000,000 seconds, raises a `ValueError`.

#### `cookie_file=`

A file to keep the cookies in, with the Session's [Machine](#machine-1). They load from it now, if it exists, and the Session claims the Machine it holds, so that a Session that comes back is the same machine. `close()` saves them to it, as [`Cookies.save()`](#cookiessave) does. A crash loses what came after the last save, which a caller can make more often with `session.cookies.save(path)`. Chrome writes its store every 30 seconds. The default keeps them only until the Session closes. What origins asked for in client hints is not in the file, so it starts over with each Session.

A file from mitto 0.1 holds no Machine, so a Session that loads it claims the one every Session of mitto 0.1 claimed, [`Machine.named("mitto-0.1")`](#machinenamed). `close()` saves it in the new version, which mitto 0.1 cannot read.

A file that does not hold cookies this Session can use, or holds another Machine than `machine=` gives, raises a `ValueError`, because closing the Session would otherwise overwrite it. A value that is neither text, a path nor `None` raises a `TypeError`.

```python
with mitto.Session(cookie_file="cookies.json") as session:
    session.get("https://example.test/set-cookie?seen=1")
    machine = session.machine

with mitto.Session(cookie_file="cookies.json") as session:
    assert session.cookies.get("seen").value == "1"
    assert session.machine == machine
```

#### `proxy=`

The proxy for every request of this Session, as a URL. There is no proxy per request, and the system's proxy settings and the environment are ignored, so with the default each server is reached directly.

| The URL | What it does |
| --- | --- |
| `http://user:password@host:port` | HTTPS goes through a tunnel, and plain HTTP goes to the proxy with the whole URL |
| `https://user:password@host:port` | The same inside TLS. The proxy's certificate is verified as a server's is, with `additional_trust_anchors` for a private one |
| `socks5://user:password@host:port` | The proxy gets the name and finds it |

A username and password are percent-encoded. They answer the proxy when it asks, as they do in Chrome once a person has typed them, and a proxy that does not ask never gets them. Real Chrome cannot answer a SOCKS5 proxy at all. A Session gives its credentials there by RFC 1929, each at most 255 bytes once decoded, so a target sees no difference but the proxy sees [a greeting Chrome never sends](parity.md#credentials-over-socks5). SOCKS5 keeps nothing, so each new connection gives them again. A URL with only a username or only a password sends the other empty.

As in Chrome, a request to `localhost`, a `*.localhost` name or a loopback address goes around the proxy, and a proxy that fails is a [`ProxyError`](#proxyerror) and not a reason to go directly. A request through a proxy stays on HTTP/1.1 or HTTP/2. A URL that cannot be used raises a `ValueError`, and a value that is neither text nor `None` a `TypeError`. Neither message holds the URL.

```python
with mitto.Session(proxy="http://user:password@proxy.example.test:8080") as session:
    assert session.get("https://example.test/").status == 200
```

#### `cache=`

Where to keep responses, which `//net` then answers from and revalidates as Chrome's HTTP cache does, split by the top-level site of the page a request comes from. The default keeps none, where a real Chrome always has a cache, so a server sees each request.

| The value | What it does |
| --- | --- |
| A directory, as text or a path | Made if it does not exist, and what Chrome has. Responses stay for later Sessions. `//net` only touches its subdirectory `Cache_Data`, and one Session at a time can have the directory, in any process |
| `"memory"` | Keeps them until the Session closes, as Chrome's Incognito mode does. A server can tell this from a directory by [the stream a 304 cancels](parity.md#which-backend) over HTTP/2. Write `"./memory"` for a directory of that name |

An empty string raises a `ValueError`, and a value that is neither text, a path nor `None` a `TypeError`. A directory that cannot hold a cache, or that another Session has open, raises an `OSError`. [`Response.from_cache`](#response) says whether a body came from here.

```python
with mitto.Session(cache="memory") as session:
    first = session.get("https://example.test/cached")
    second = session.get("https://example.test/cached")

assert not first.from_cache
assert second.from_cache
```

#### `Session.cookies`

```text
Session.cookies -> Cookies
```

The [cookies](#cookies-1), which `//net` sets from responses and sends with requests.

#### `Session.machine`

```text
Session.machine -> Machine
```

The [Machine](#machine-1) this Session claims, which `cookie_file` or `machine=` gave or which was drawn for it.

#### `Session.close()`

```text
Session.close() -> None
```

Save the cookies to `cookie_file`, if it has one, and close the Session's connections and any cache on disk. Requests still running end with a `RequestError`, and so do later ones. A response that arrives during this call may not reach the file. The connections close even if the save fails, and a second call does nothing. Another Session can open the same `cache` once this has returned.

It raises an `OSError` if `cookie_file` cannot be written.

#### `Session.request()`

```text
Session.request(method: str, url: str, **options: Unpack[RequestOptions]) -> Response
```

Fetch `url` and block until the whole [response](#response) has arrived, with the GIL released while it waits. Fetch's normalized methods, `DELETE`, `GET`, `HEAD`, `OPTIONS`, `POST` and `PUT`, are sent in upper case however they are given, as Chrome does. Any other is sent as it is written, and that includes `patch`. `options` are the [request arguments](#request-arguments).

| It raises | If |
| --- | --- |
| `TypeError` | An argument is not one of the request arguments, or it is of a type it does not take, as [each says](#request-arguments) and the message names. `method` and `url` are text, and so are the names and values in `params`, `headers` and a mapping in `data`. Also a file of another form in `files`, a value in `json` that JSON has no form of, or a stream that gives something other than bytes |
| `ValueError` | `url`, `initiator` or a `Referer` in `headers` holds a NUL character or cannot be encoded as UTF-8, which the message names, a header cannot be encoded as Latin-1, `kind` is not a Request Kind, two names in `headers` differ only in case or `timeout` cannot be used. Also if the arguments of a body do not go together, `json` holds a NaN or an infinity, a body goes with `GET` or `HEAD` or with a Request Kind that Chrome sends none with, or a stream does not keep to its `Content-Length` |
| [`RequestError`](#errors) | The request ended without a response. An invalid header or method, or an `initiator` that is not a valid URL, is `ERR_INVALID_ARGUMENT`. A URL that is not valid is `ERR_INVALID_URL` |
| Whatever a stream's file or iterable raises | That ends the request |

An HTTP error status such as 404 is a response, which [`raise_for_status()`](#responseraise_for_status) turns into an error.

```python
with mitto.Session() as session:
    response = session.request("GET", "https://example.test/echo")

assert response.json()["method"] == "GET"
```

#### `Session.get()`, `Session.post()`, `Session.put()`, `Session.patch()`, `Session.delete()`, `Session.head()`, `Session.options()`

```text
Session.get(url: str, **options: Unpack[RequestOptions]) -> Response
Session.post(url: str, **options: Unpack[RequestOptions]) -> Response
Session.put(url: str, **options: Unpack[RequestOptions]) -> Response
Session.patch(url: str, **options: Unpack[RequestOptions]) -> Response
Session.delete(url: str, **options: Unpack[RequestOptions]) -> Response
Session.head(url: str, **options: Unpack[RequestOptions]) -> Response
Session.options(url: str, **options: Unpack[RequestOptions]) -> Response
```

Send a request of that method, as `request()` does. Each takes the same arguments and raises the same errors.

### `get()`

```text
get(url: str, **options: Unpack[RequestOptions]) -> Response
```

Send a `GET` on a default Session, which presents the Engine Release's Persona, claims a Machine drawn for it and stays open until the process exits, so its cookies and connections carry over from one call to the next. It takes the arguments of `Session.get()`, with a navigation as the default kind. It raises what `Session.request()` does, and a `NativeLibraryError` if the native library cannot be used. No other method has a shortcut, and nothing else about this Session can be set.

```python
assert mitto.get("https://example.test/").status == 200
```

## Machine

### `Machine`

```text
Machine(
    *,
    memory: int,
    display_width: int,
    display_height: int,
    scale: float,
    viewport_width: int,
    viewport_height: int,
    round_trip_ms: int,
    throughput_kbps: int,
    color_scheme: ColorScheme,
    reduced_motion: ReducedMotion,
    reduced_transparency: ReducedTransparency,
    platform_version: str,
    seed: int | None = None,
)
```

The computer a Session claims to run on, as Chrome reads it. A Session works out each client hint that follows the machine from it, [as Chrome does](persona.md#what-decides-each-value). It is apart from the **Host** and from the **Persona**, and never read from the computer mitto runs on. [`Machine.draw()`](#machinedraw) gives one from a table that ships with mitto, and a caller can make one too.

A value of a type it does not take raises a `TypeError` that names it, and `True` and `False` are no number. A value that Chrome on Windows 10 or 11 cannot read raises a `ValueError`, and so does a viewport larger than the display.

| Attribute | Type | What it holds |
| --- | --- | --- |
| `memory` | `int` | The memory in GiB, as Chrome rounds what Windows reports: 2, 4, 8, 16 or 32. `device-memory` says it |
| `display_width` | `int` | The width of the primary display, in pixels |
| `display_height` | `int` | The height of the primary display, in pixels |
| `scale` | `float` | The scale in Windows' display settings, such as 1.25 for 125%: 1, 1.25, 1.5, 1.75, 2, 2.25, 2.5, 3, 3.5, 4, 4.5 or 5. `dpr` says it |
| `viewport_width` | `int` | The width of the window's page area, in device-independent pixels, which are the display's pixels over the scale. At most the display's |
| `viewport_height` | `int` | The height of the window's page area, in device-independent pixels. At most the display's |
| `round_trip_ms` | `int` | The round trip of HTTP that Chrome's network quality estimator has, in milliseconds. `rtt` is it with Chrome's noise for the host, at most 3000 and rounded to 50, and `ect` follows from it with no noise |
| `throughput_kbps` | `int` | The throughput that the estimator has, in kbps. `downlink` is it with the same noise, at most 10,000 and rounded to 50, in Mbps |
| `color_scheme` | `ColorScheme` | `"light"` or `"dark"` |
| `reduced_motion` | `ReducedMotion` | `"no-preference"`, or `"reduce"` with Windows' animation effects off |
| `reduced_transparency` | `ReducedTransparency` | `"no-preference"`, or `"reduce"` with Windows' transparency effects off |
| `platform_version` | `str` | What `sec-ch-ua-platform-version` says: the version of Windows' UniversalApiContract, such as `"19.0.0"` for Windows 11 from 24H2 and `"10.0.0"` for Windows 10 from 2004. [Which Windows](persona.md#which-windows) has each |
| `seed` | `int \| None` | What `draw()` drew it from, or `None` for one that a caller made |

#### `Machine.draw()`

```text
Machine.draw(
    *,
    seed: int | None = None,
    memory: int | None = None,
    display_width: int | None = None,
    display_height: int | None = None,
    scale: float | None = None,
    viewport_width: int | None = None,
    viewport_height: int | None = None,
    round_trip_ms: int | None = None,
    throughput_kbps: int | None = None,
    color_scheme: ColorScheme | None = None,
    reduced_motion: ReducedMotion | None = None,
    reduced_transparency: ReducedTransparency | None = None,
    platform_version: str | None = None,
) -> Self
```

Return a Machine drawn from [the table](persona.md#the-table) that ships with mitto, with each value given here and the rest drawn to fit them.

The display, its scale and the viewport are drawn together, as one row of the table, and so are the round trip and the throughput. Of each of these two groups, give every value, or only values that a row has, such as `scale=1.25` alone. The Windows version goes with the viewport too, since its taskbar takes some of the height. Each other value is drawn on its own.

The same seed and the same table give the same Machine on every Host, and a value given in one group leaves what the seed draws for the others as it was, unless they have to fit together. Without a seed, one is drawn, which the Machine keeps. Another version of mitto can have another table, which draws another Machine from the same seed.

It raises what `Machine` raises, a `TypeError` if `seed` is not a whole number, and a `ValueError` if no row of the table has the values given of a group whose values are not all given.

```python
machine = mitto.Machine.draw(seed=7, memory=16)

assert machine.memory == 16
assert machine.seed == 7
assert mitto.Machine.draw(seed=7, memory=16) == machine
```

#### `Machine.named()`

```text
Machine.named(name: str) -> Self
```

Return the Machine of a name. `"mitto-0.1"` is the one that every Session of mitto 0.1 claimed: 8 GiB, a 1920 by 1080 display at 100% with a window that fills it on Windows 11 24H2, a round trip of 50 ms and a throughput of 10,000 kbps, and the settings that Windows comes with. mitto 0.1 sent that network as an `rtt` of 50 and a `downlink` of 10 to every host, and with Chrome's noise the `downlink` is now from 9 to 10, by host. Another name raises a `ValueError`, and anything but text a `TypeError`.

```python
assert mitto.Machine.named("mitto-0.1").viewport_height == 945
```

## Request arguments

The keyword arguments that each method of a Session takes, and `get()` and `stream()` too. A value of a type that one does not take raises a `TypeError` that names it, before anything is sent.

| Argument | What it takes | The default |
| --- | --- | --- |
| [`params=`](#params) | Names and values as text, in a mapping or in pairs | No query |
| [`headers=`](#headers) | Names with text or `None`, in a mapping | The Persona's |
| [`kind=`](#kind) | A [`RequestKind`](#requestkind), or its value | `navigation` for `GET`, and `fetch` for other methods |
| [`initiator=`](#initiator) | The URL of a page | None |
| [`timeout=`](#timeout-1) | A number, a [`Timeout`](#timeout-2) or `None` | The Session's |
| [`follow_redirects=`](#follow_redirects) | `True` or `False` | `True` |
| [`data=`](#data) | Bytes, text, a mapping of text, a file or an iterable of bytes | No body |
| [`json=`](#json) | A value that JSON has a form of | No body |
| [`files=`](#files) | Files by field name, in a mapping or in pairs | No body |

### `params=`

The query to add to the URL, encoded as `URLSearchParams` does. A query the URL already has stays as it is and comes first, and pairs can repeat a name. A name or value that is not text raises a `TypeError` that names `params`, and so does anything but a mapping or pairs, so a number is written as text.

```python
with mitto.Session() as session:
    sent = session.get("https://example.test/echo?a=1", params=[("b", "2"), ("b", "3")]).json()

assert sent["path"] == "/echo?a=1&b=2&b=3"
```

### `headers=`

Changes the Persona's lines for this request, by name in any case. [persona.md](persona.md#what-a-caller-changes) has the whole table. Two names that differ only in case raise a `ValueError`, and so does a value that cannot be encoded as Latin-1. A name that is not text, a value that is neither text nor `None` and anything but a mapping raise a `TypeError` that names `headers`.

| In `headers` | On the wire |
| --- | --- |
| A name the Persona has | Takes the place of the Persona's line, which keeps the Persona's spelling of the name |
| Another name | After the Persona's lines, before what `//net` adds |
| A name the Persona has, with `None` | The line is gone |
| Another name, with `None` | Nothing changes, so this cannot remove what `//net` adds |
| `User-Agent`, with `None` | The line leaves its place, and `//net` adds the Persona's value after the other lines, because a request always gets one from the context |
| `Referer` | Goes as given to every hop or, with `None`, goes from every hop, in place of what `initiator=` would make of it. `//net` still decides where it goes. A NUL character or a lone surrogate in it raises a `ValueError` that names it |
| A line the native core sets, such as `Origin`, `Sec-Fetch-Site` or `sec-ch-ua` | Stays as given or, with `None`, stays gone on each hop. `//net` still changes an `Origin` on a redirect, as it does in Chrome |
| `Cookie` | Goes only if the Session holds no cookie for the URL, because `//net` writes over it otherwise |

`Authorization` and `Cookie` do not follow a redirect to another origin, and stay gone if a later hop comes back, as Fetch does with `Authorization`.

```python
with mitto.Session() as session:
    sent = session.get(
        "https://example.test/echo",
        headers={"accept": "text/plain", "Upgrade-Insecure-Requests": None, "X-Trace": "1"},
    ).json()["headers"]

assert sent["Accept"] == "text/plain"
assert "Upgrade-Insecure-Requests" not in sent
assert sent["X-Trace"] == "1"

with mitto.Session() as session:
    sent = session.get("https://example.test/echo", headers={"User-Agent": None}).json()["headers"]

assert "Chrome/" in sent["User-Agent"]
assert list(sent).index("User-Agent") > list(sent).index("Sec-Fetch-Dest")
```

### `kind=`

Which **Request Kind** this request reproduces, which decides what the Persona sends. Text that is not one raises a `ValueError`, and anything else but `None` a `TypeError`.

```python
with mitto.Session() as session:
    navigation = session.get("https://example.test/echo").json()["headers"]
    fetch = session.get("https://example.test/echo", kind="fetch").json()["headers"]

assert navigation["Sec-Fetch-Mode"] == "navigate"
assert fetch["Sec-Fetch-Mode"] == "cors"
```

### `RequestKind`

A string enum, so that `kind="fetch"` and `kind=mitto.RequestKind.FETCH` are the same.

| Member | Value | What it reproduces |
| --- | --- | --- |
| `RequestKind.NAVIGATION` | `"navigation"` | A top-level navigation, including a form's `POST` |
| `RequestKind.FETCH` | `"fetch"` | A page's `fetch()` that does not ask for a `credentials` mode, so `same-origin` |
| `RequestKind.SCRIPT` | `"script"` | A `<script>`, which has no body, and whose cookies go by `SameSite` alone |
| `RequestKind.IMAGE` | `"image"` | An `<img>`, which has no body, and whose cookies go by `SameSite` alone |

### `initiator=`

The URL of the page the request is made from, which counts as a top-level page. It decides `Referer`, `Origin` and `Sec-Fetch-Site` on each hop, which `SameSite` cookies go, which connections and partitioned cookies the request can use, and for a kind other than navigation, which client hints it sends. [persona.md](persona.md#the-initiator) has the rules, which are ported from Chrome.

Without it, a navigation counts as started by the browser, and a subresource as coming from the page at the root of the origin of its URL. A URL that is not valid ends the request with `ERR_INVALID_ARGUMENT`. One that holds a NUL character or a lone surrogate, which UTF-8 cannot encode, raises a `ValueError` that names `initiator`, and a value that is neither text nor `None` raises a `TypeError`.

A `fetch` to another origin than its page's sends and stores no cookie, for the rest of its chain, as a page's `fetch()` does. A `script` or `image` still sends those that `SameSite` allows.

```python
page = "https://example.test/page"

with mitto.Session() as session:
    session.cookies.set(mitto.Cookie("a", "1", domain="other.test", secure=True, same_site="none"))
    fetch = session.get("https://other.test/echo", kind="fetch", initiator=page).json()["headers"]
    image = session.get("https://other.test/echo", kind="image", initiator=page).json()["headers"]

assert fetch["Sec-Fetch-Site"] == "cross-site"
assert fetch["Origin"] == "https://example.test"
assert fetch["Referer"] == "https://example.test/"
assert "Cookie" not in fetch
assert image["Cookie"] == "a=1"
```

### `timeout=`

A number is the total time in seconds, and a [`Timeout`](#timeout-2) sets each timer. `None` runs no timer, in place of the Session's default. The default is the Session's. A value that is neither a number nor a `Timeout` raises a `TypeError` that names `timeout`, and `True` and `False` do not count as numbers here. A number that is not positive, or is more than 2,000,000 seconds, raises a `ValueError`. A timer that runs out raises a [`RequestTimeoutError`](#requesttimeouterror), and the Session carries on.

```python
with mitto.Session() as session:
    try:
        session.get("https://example.test/slow", timeout=0.1)
    except mitto.RequestTimeoutError as error:
        assert error.name == "ERR_TIMED_OUT"
```

### `Timeout`

```text
Timeout(*, connect: float | None = None, read: float | None = None, total: float | None = None)
```

A request's timers, in seconds. None runs no timer, leaving what `//net` has of its own. The native core runs them, so all a server sees is the request ending. A value that is neither a number nor `None` raises a `TypeError` that names the timer, and `True` and `False` do not count as numbers here. A number that is not positive, or is more than 2,000,000 seconds, raises a `ValueError`.

| Attribute | Type | What it times |
| --- | --- | --- |
| `connect` | `float \| None` | From the start of the request, or from a redirect, until a connection is ready for it: the name is resolved, a connection is free and any proxy tunnel and TLS handshake are done. In a Session with a cache, this also counts a wait for another request's response to the same URL, and a response from the cache stops it |
| `read` | `float \| None` | The longest wait for bytes from the server |
| `total` | `float \| None` | From the start of the request to its end, over all its redirects |

Three things about `read`. The response's headers count as one wait, since `//net` does not report them a byte at a time. It does not run out while the request's body is still moving, and a wait for the next part of a stream body is a wait for the caller, which does not count. And `//net` sees a body move only until the system's socket buffers take it, so the time that the last of it spends in those buffers does count.

```python
timeout = mitto.Timeout(connect=5, read=0.1, total=60)

with mitto.Session(timeout=timeout) as session:
    try:
        session.get("https://example.test/slow")
    except TimeoutError as error:
        assert isinstance(error, mitto.RequestTimeoutError)
```

### `follow_redirects=`

`//net` follows a redirect as it does in Chrome: the limit of 20, the method of the next hop and when a body goes. `False` makes a redirect the response, with empty content, because `//net` does not read the body of a redirect, as with `redirect: "manual"` in Fetch. A redirect to a URL that is not valid still ends the request with a `RequestError`. Anything but `True` or `False`, such as `0` or `"no"`, raises a `TypeError`.

```python
redirecting = "https://example.test/redirect?to=/echo"

with mitto.Session() as session:
    followed = session.get(redirecting)
    not_followed = session.get(redirecting, follow_redirects=False)

assert followed.url == "https://example.test/echo"
assert [hop.status for hop in followed.history] == [302]
assert not_followed.status == 302
assert not_followed.content == b""
```

### `data=`

What the body is, which with the Request Kind decides its `Content-Type`, [as Chrome's does](persona.md#what-a-body-adds). A navigation's says what a form would, and a fetch's what a page's script would. A body with `GET` or `HEAD` raises a `ValueError`, because Fetch refuses one, and so does one on a `script` or `image`, which Chrome sends none with. Any other type raises a `TypeError`, and so does a mapping's name or value that is not text, whose message names `data`.

| `data=` | The body | `Content-Type` of a fetch | `Content-Type` of a navigation |
| --- | --- | --- | --- |
| Bytes | As they are | None | None |
| Text | UTF-8 | `text/plain;charset=UTF-8` | `text/plain` |
| A mapping of text | As `URLSearchParams` writes it | `application/x-www-form-urlencoded;charset=UTF-8` | `application/x-www-form-urlencoded` |
| A file object in binary mode | A stream, from where it stands | None | None |
| An iterable of bytes | A stream, each part as it comes | None | None |

A stream is read only as `//net` asks for more, in the thread that waits for the response, so a body of any size takes the same memory ([streams](parity.md#streams)). What decides how it goes is whether its size is known:

| The stream | What it sends | Over HTTP/1.1 |
| --- | --- | --- |
| A file that can seek, or an iterable with a `Content-Length` in `headers` | `Content-Length`, in the place `//net` gives it | Sent |
| An iterable or a file that cannot seek, as Chrome sends a page's `ReadableStream` | No `Content-Length`, over HTTP/2 or HTTP/3 | Refused with a [`TLSError`](#tlserror) named `ERR_ALPN_NEGOTIATION_FAILED`, and nothing on the wire |

An iterable that gives more or less than its `Content-Length` raises a `ValueError`. A stream goes once, so any redirect but a 303 raises a `RequestError` named `ERR_UPLOAD_STREAM_REWIND_NOT_SUPPORTED`, where a body in memory goes again.

```python
with mitto.Session() as session:
    form = session.post("https://example.test/echo", data={"a": "b c"}).json()
    with open("report.csv", "rb") as file:
        from_file = session.post("https://example.test/echo", data=file).json()

assert form["body"] == "a=b+c"
assert form["headers"]["Content-Type"] == "application/x-www-form-urlencoded;charset=UTF-8"
assert from_file["headers"]["Content-Length"] == "8"


def parts():
    yield b"first "
    yield b"second"


with mitto.Session() as session:
    sized = session.post("https://example.test/echo", data=parts(), headers={"Content-Length": "12"})
    unsized = session.post("https://h2.example.test/", data=parts())
    try:
        session.post("https://example.test/echo", data=parts())
    except mitto.TLSError as error:
        assert error.name == "ERR_ALPN_NEGOTIATION_FAILED"

assert sized.json()["body"] == "first second"
assert unsized.protocol == "h2"
```

### `json=`

A value that goes as `JSON.stringify` writes it, with a `Content-Type` of `application/json`. A float keeps Python's text, so `1.0` stays `1.0`. `None` sends no JSON, and `data` can send a JSON `null`. A value that JSON has no form of raises a `TypeError` that names `json`. A NaN or an infinity raises a `ValueError`, where `JSON.stringify` would write `null`, and so does `json` beside `data` or `files`.

```python
with mitto.Session() as session:
    sent = session.post("https://example.test/echo", json={"a": [1, 2.0], "b": "é"}).json()

assert sent["body"] == '{"a":[1,2.0],"b":"é"}'
assert sent["headers"]["Content-Type"] == "application/json"
```

### `files=`

Files by field name, or pairs that can repeat a name, which make a multipart body as Blink's `FormData` does. A file is its content in bytes, `(file name, content)` or `(file name, content, media type)`. With no file name it is Blink's `blob`, and with no media type it is `application/octet-stream`, since a type is not guessed. Text fields in a mapping in `data` come before the files, and anything else in `data` raises a `ValueError`. Any other form of a file raises a `TypeError` that names `files` and its field, so read a file object first. So does a field name that is not text or anything but a mapping or pairs.

```python
with mitto.Session() as session:
    sent = session.post(
        "https://example.test/echo",
        data={"title": "Numbers"},
        files={"report": ("report.csv", b"a,b\n", "text/csv")},
    ).json()

assert sent["headers"]["Content-Type"].startswith("multipart/form-data; boundary=----WebKitFormBoundary")
assert 'name="title"' in sent["body"]
assert 'name="report"; filename="report.csv"' in sent["body"]
```

## Responses

### `Response`

A finished response, which `request()` and the other methods of a Session return.

| Attribute | Type | What it holds |
| --- | --- | --- |
| `status` | `int` | The HTTP status code of the final response, after redirects |
| `url` | `str` | The final URL, after redirects |
| `protocol` | `str` | The protocol the response arrived over, for example `"http/1.1"`, `"h2"` or `"h3"` |
| `headers` | `Headers` | The final response's headers |
| `content` | `bytes` | The final response's body, after any content decoding. It stays out of `repr()`, since it can be large |
| `history` | `tuple[Redirect, ...]` | The redirects that led here, the first one first |
| `from_cache` | `bool` | Whether the body comes from the Session's HTTP cache. That goes for a response the server only had to confirm with a 304 too, so `True` does not say that no request went out |

#### `Response.text`

```text
Response.text -> str
```

The body decoded with the charset in `Content-Type`, otherwise as UTF-8. What does not decode becomes U+FFFD. A charset that names no text encoding counts for nothing. mitto does not guess one, and neither `Session` nor `Response` has anywhere to set it, so for anything else use `content.decode()`.

```python
with mitto.Session() as session:
    response = session.get("https://example.test/latin-1")

assert response.content == b"caf\xe9"
assert response.text == "café"
```

#### `Response.json()`

```text
Response.json() -> Any
```

Return the body parsed as JSON, which says its own encoding, so `Content-Type` does not matter. It raises a `ValueError` if the body is not JSON, which `json.JSONDecodeError` and `UnicodeDecodeError` both are.

#### `Response.raise_for_status()`

```text
Response.raise_for_status() -> None
```

Do nothing, unless the status says that the request failed. It raises an [`HTTPStatusError`](#httpstatuserror) if the status is 400 or above.

```python
with mitto.Session() as session:
    response = session.get("https://example.test/missing")

assert response.status == 404
try:
    response.raise_for_status()
except mitto.HTTPStatusError as error:
    assert error.response is response
```

### `Headers`

A response's headers in the order received, with lookup that ignores the case of a name. Iterating gives each (name, value) pair, so a repeated header stays separate, and `len()` counts those pairs. `headers["name"]` is what `get()` gives, and raises a `KeyError` if there is no such header. `"name" in headers` says whether there is. Each of these raises a `TypeError` for a name that is not text.

Headers have no fixed encoding, so they are decoded as Latin-1, which keeps every byte: `value.encode("latin-1")` gives back what the server sent.

| Attribute | Type | What it holds |
| --- | --- | --- |
| `pairs` | `tuple[tuple[str, str], ...]` | The (name, value) pairs in the order received |

```python
with mitto.Session() as session:
    headers = session.get("https://example.test/set-cookie?a=1&b=2").headers

assert headers["content-length"] == "0"
assert "Set-Cookie" in headers
assert [name for name, value in headers].count("Set-Cookie") == 2
assert headers.get_all("set-cookie") == ["a=1; Secure; SameSite=Lax", "b=2; Secure; SameSite=Lax"]
```

#### `Headers.get()`

```text
Headers.get(name: str, default: str | None = None) -> str | None
```

Return the value of a header, or `default` if there is none. Repeated headers come back joined by `", "`, as `Headers.get()` does in Fetch. That does not suit `Set-Cookie`, whose values can hold commas, so use `get_all()` for it. A `name` that is not text raises a `TypeError`, and so does a `default` that is neither text nor `None`, whether or not there is a header.

#### `Headers.get_all()`

```text
Headers.get_all(name: str) -> list[str]
```

Return each value of a header in the order received, which is an empty list if there is none. A `name` that is not text raises a `TypeError`.

### `Redirect`

A redirect that a request followed on its way to the response, in `Response.history`. It has no body, because `//net` does not read one.

| Attribute | Type | What it holds |
| --- | --- | --- |
| `status` | `int` | The HTTP status code of the redirect |
| `url` | `str` | The URL that answered with the redirect |
| `headers` | `Headers` | The redirect's headers |

## Streaming

### `Session.stream()`

```text
Session.stream(method: str, url: str, **options: Unpack[RequestOptions]) -> Generator[ResponseStream]
```

Fetch `url` in a `with` statement, and read the response's body as it arrives. Entering the statement blocks until the response's headers have arrived, with any stream body gone by then and the GIL released while it waits. It gives a [`ResponseStream`](#responsestream). Leaving the statement ends the request if that is still running.

It takes what `request()` takes, and raises what `request()` raises, on entering the statement. An end before the end of the body is raised by what reads it.

```python
with mitto.Session() as session:
    with session.stream("GET", "https://example.test/large") as stream:
        assert stream.status == 200
        size = sum(len(part) for part in stream.iter_bytes())

assert size == 1024 * 1024

with mitto.Session() as session, session.stream("GET", "https://example.test/unfinished") as stream:
    try:
        stream.read()
    except mitto.ProtocolError as error:
        assert error.name == "ERR_CONTENT_LENGTH_MISMATCH"
```

### `ResponseStream`

A response whose body arrives as the caller reads it. Nothing more of the body comes off the network until it is asked for, so a slow reader makes the server wait, through flow control, and a body of any size takes no more memory than the part in hand. What has been read is gone, so the two ways of reading take turns on the same bytes. Once the `with` statement has ended, reading raises a `RequestError`.

| Attribute | Type | What it holds |
| --- | --- | --- |
| `status` | `int` | The HTTP status code of the final response, after redirects |
| `url` | `str` | The final URL, after redirects |
| `protocol` | `str` | The protocol the response arrived over |
| `headers` | `Headers` | The final response's headers |
| `history` | `tuple[Redirect, ...]` | The redirects that led here, the first one first |
| `from_cache` | `bool` | Whether the body comes from the Session's HTTP cache, as in `Response` |

#### `ResponseStream.iter_bytes()`

```text
ResponseStream.iter_bytes() -> Iterator[bytes]
```

Return an iterator over the rest of the body, after any content decoding, in parts as they arrive. Iterating raises a `RequestError` if the request ends before the end of the body, and that is the only way to learn that it did.

#### `ResponseStream.read()`

```text
ResponseStream.read() -> bytes
```

Return the rest of the body, after any content decoding, once all of it has arrived. It raises a `RequestError` if the request ends before the end of the body.

## Cookies

A Session has `CookieMonster`, the cookie store in `//net` that Chrome uses, so `//net` decides [which cookies a response sets](parity.md#cookies), which a request sends and in what order. These calls only read and change that store.

### `Cookies`

The cookies of one Session, at `session.cookies`. Iterating gives the cookies as they are now, with the longest path first, then the earliest created, and leaves out any that have expired. `len()` counts them.

Each call asks the network thread, and blocks only the calling thread for as long as that takes, with the GIL released. The calls may come from any thread, but not after the Session has closed, when they raise a `ValueError`.

```python
with mitto.Session() as session:
    session.get("https://example.test/set-cookie?a=1&b=2")
    assert len(session.cookies) == 2
    assert [cookie.name for cookie in session.cookies] == ["a", "b"]

    session.cookies.set(mitto.Cookie("c", "3", domain="example.test"))
    assert session.cookies.get("c").value == "3"
    sent = session.get("https://example.test/echo").json()["headers"]
    assert sent["Cookie"] == "a=1; b=2; c=3"

    assert session.cookies.delete("c") == 1
    assert session.cookies.clear() == 2
```

#### `Cookies.get()`

```text
Cookies.get(name: str, *, domain: str | None = None, path: str | None = None) -> Cookie | None
```

Return the cookie of this name, or `None` if there is none. `domain` is written as `Cookie.domain` is, and `path` as `Cookie.path` is, and `None` stands for any. If several match, this is the one that goes first in a `Cookie` header. A `name` that is not text, or a `domain` or `path` that is neither text nor `None`, raises a `TypeError`.

#### `Cookies.set()`

```text
Cookies.set(cookie: Cookie) -> None
```

Put a cookie in the store, as a response's `Set-Cookie` would. It replaces one of the same name, domain and path. A cookie with an expiry in the past is not stored, and what it would replace still goes. A dot before a public suffix gives a cookie for that one name, as a server's `Domain` attribute does, and not one for all of the suffix.

It raises a `ValueError` if the native core refuses the cookie, such as a `__Host-` cookie that is not `Secure` or a domain that holds a port or a path, and a `TypeError` if `cookie` is not a `Cookie`.

#### `Cookies.delete()`

```text
Cookies.delete(name: str, *, domain: str | None = None, path: str | None = None) -> int
```

Delete the cookies of this name, and return how many went. `name`, `domain` and `path` are as for `get()`, and so is what each raises.

#### `Cookies.clear()`

```text
Cookies.clear() -> int
```

Delete every cookie, and return how many went.

#### `Cookies.save()`

```text
Cookies.save(path: str | os.PathLike[str]) -> None
```

Write all the cookies, with the Session's Machine, to a JSON file that only its owner can read or write. The values are not encrypted. Session cookies are written too, as a browser does that restores its last session. A crash during this call leaves the file as it was. It raises an `OSError` if the file cannot be written, and a `TypeError` if `path` is neither text nor a path.

#### `Cookies.load()`

```text
Cookies.load(path: str | os.PathLike[str]) -> None
```

Add the cookies of a file from `save()`, each with the attributes it had there, replacing any of the same name.

The file has to hold the Machine that this Session claims, since a server that sees the same cookies from another machine can tell. A Session claims a file's Machine when it opens with [`cookie_file=`](#cookie_file). A file from mitto 0.1 holds `Machine.named("mitto-0.1")`. To move cookies to a Session of another Machine all the same, `set()` each of them there.

It raises an `OSError` if the file cannot be read, such as a `FileNotFoundError`, and a `ValueError` if the file holds another Machine or the native core refuses what it holds, in which case no cookie is added. A `path` that is neither text nor a path raises a `TypeError`.

### `Cookie`

```text
Cookie(
    name: str,
    value: str,
    *,
    domain: str,
    path: str = '/',
    secure: bool = False,
    http_only: bool = False,
    same_site: SameSite = 'unspecified',
    priority: CookiePriority = 'medium',
    expires: datetime | None = None,
    partition_key: CookiePartitionKey | None = None,
    created: datetime | None = None,
    last_accessed: datetime | None = None,
    last_updated: datetime | None = None,
    source_scheme: CookieSourceScheme | None = None,
    source_port: int | None = None,
    source_type: CookieSourceType | None = None,
)
```

One cookie, as the store holds it. A cookie has no fixed encoding, so `name`, `value` and `path` hold one character for each byte, as a response's headers do: `value.encode("latin-1")` gives back what the server sent. To set text in another encoding, encode it and decode that as Latin-1.

Without `created`, this is a new cookie to set, and `//net` checks it as if a server at its domain had sent it. The attributes below `created` then stay `None`. A cookie from `Cookies` has them all, and can be set again as it is.

An attribute of a type it does not take raises a `TypeError` that names it, so a time is a `datetime` and not a number of seconds, and `True` and `False` are no `source_port`. So does a time without a timezone, which could stand for any moment, when the cookie is built. Text outside the values of its type, such as a `same_site` of `"sometimes"`, raises a `ValueError` when [`Cookies.set()`](#cookiesset) puts it in the store.

| Attribute | Type | What it holds |
| --- | --- | --- |
| `name` | `str` | The name |
| `value` | `str` | The value. It stays out of `repr()`, since it can be a secret |
| `domain` | `str` | Where it goes. With a leading dot it is a domain cookie, which also goes to subdomains. Without one it goes to that one name |
| `path` | `str` | The path that it goes to, and below |
| `secure` | `bool` | Whether it goes only over HTTPS and other secure schemes |
| `http_only` | `bool` | Whether a page's script cannot see it. A Session has no scripts, so nothing differs |
| `same_site` | `SameSite` | `"unspecified"`, `"none"`, `"lax"` or `"strict"`. `"unspecified"` is a cookie without the attribute, which `//net` treats as `"lax"` |
| `priority` | `CookiePriority` | `"low"`, `"medium"` or `"high"`, which says what the store evicts first |
| `expires` | `datetime \| None` | When it ends, as a timezone-aware time, or `None` for a session cookie. `//net` brings a time more than 400 days after `created` down to that, as it does for a server's |
| `partition_key` | `CookiePartitionKey \| None` | Its partition, or `None` if it is not partitioned |
| `created` | `datetime \| None` | When it was created, as a timezone-aware time, which also decides its place in a `Cookie` header |
| `last_accessed` | `datetime \| None` | When a request last sent it |
| `last_updated` | `datetime \| None` | When it last changed |
| `source_scheme` | `CookieSourceScheme \| None` | `"unset"`, `"non_secure"` or `"secure"`: the kind of scheme it came from |
| `source_port` | `int \| None` | The port it came from, -1 for none, or -2 if that was not valid |
| `source_type` | `CookieSourceType \| None` | `"http"`, `"script"` or `"other"`: what it came from, where a caller of this API is `"other"` |

### `CookiePartitionKey`

```text
CookiePartitionKey(top_level_site: str, has_cross_site_ancestor: bool)
```

The partition of a cookie that a request sends only from one top-level site, which `initiator=` names. A page always counts as top-level. A `top_level_site` that is not text, or a `has_cross_site_ancestor` that is neither `True` nor `False`, raises a `TypeError`.

| Attribute | Type | What it holds |
| --- | --- | --- |
| `top_level_site` | `str` | The site, such as `"https://example.com"` |
| `has_cross_site_ancestor` | `bool` | Whether a frame between it and the cookie's frame is cross-site |

## Errors

What goes wrong with a request is a `MittoError`, and its type says what a caller does next. Here they go in order of how much another try is worth:

| Raised | What happened | What to do next |
| --- | --- | --- |
| [`ConnectionFailedError`](#connectionfailederror) | No connection could be made, or the one in use broke | Often worth another try |
| [`RequestTimeoutError`](#requesttimeouterror) | A timer ran out | Often worth another try, or a longer one |
| [`ProtocolError`](#protocolerror) | The server broke the rules of HTTP, or a response ended early | Another try may help |
| [`TLSError`](#tlserror) | TLS failed for a reason other than the server's certificate | Another try rarely helps |
| [`TooManyRedirectsError`](#toomanyredirectserror) | A chain of redirects went past the limit, which often means a loop | Another try rarely helps |
| [`CertificateError`](#certificateerror) | The server's certificate was refused | Another try does not help. See [`additional_trust_anchors=`](#additional_trust_anchors) |
| [`NameResolutionError`](#nameresolutionerror) | The name did not resolve | Check the name and the DNS settings |
| [`ProxyError`](#proxyerror) | The proxy failed or refused the request | Check the proxy |
| [`RequestError`](#requesterror) | Anything else, such as a cancelled request or a URL that is not valid | Read its `name` |
| [`HTTPStatusError`](#httpstatuserror) | `raise_for_status()` found an error status | Read its `response` |
| [`NativeLibraryError`](#nativelibraryerror) | The native library cannot be used | Check the installation |

```text
MittoError
    NativeLibraryError
    HTTPStatusError
    RequestError
        NameResolutionError
        ConnectionFailedError
        TLSError
            CertificateError
        ProxyError
        ProtocolError
        TooManyRedirectsError
        RequestTimeoutError
```

```python
attempts = 0

with mitto.Session() as session:
    while True:
        attempts += 1
        try:
            response = session.get("https://example.test/flaky")
            break
        except (mitto.ConnectionFailedError, mitto.RequestTimeoutError):
            continue

assert response.status == 200
assert attempts == 2
```

A mistake in what a caller passes is a `TypeError` or a `ValueError`, as [`Session.request()`](#sessionrequest) says, and not a `MittoError`.

### `MittoError`

The base of mitto's own errors. A mistake in what a caller passes is not one, as above.

### `NativeLibraryError`

The native library is missing, cannot be loaded or was built for another ABI version.

### `RequestError`

A request ended without a response. A subclass says what went wrong in terms of what a caller does next, and this class itself stands for anything else. Its `str()` holds the method, the URL and the net error.

| Attribute | Type | What it holds |
| --- | --- | --- |
| `url` | `str` | The URL that was requested |
| `net_error` | `int` | The negative Chromium net error code, such as -105 |
| `method` | `str` | The HTTP method of the request |
| `name` | `str` | The name of the net error in the Engine Release, such as `"ERR_NAME_NOT_RESOLVED"` |

```python
with mitto.Session() as session:
    try:
        session.get("not a url")
    except mitto.RequestError as error:
        assert type(error) is mitto.RequestError
        assert error.name == "ERR_INVALID_URL"
        assert str(error) == "GET not a url failed with ERR_INVALID_URL (-300)"
```

### `NameResolutionError`

The name did not resolve. Check the name and the DNS settings. `ERR_NAME_NOT_RESOLVED` is the usual one.

```python
with mitto.Session() as session:
    try:
        session.get("https://nowhere.example.test/")
    except mitto.NameResolutionError as error:
        assert error.net_error == -105
```

### `ConnectionFailedError`

No connection could be made, or the one in use broke, such as `ERR_CONNECTION_REFUSED`, `ERR_CONNECTION_RESET` or `ERR_EMPTY_RESPONSE`. This is often worth another try.

### `TLSError`

TLS failed for a reason other than the server's certificate, such as `ERR_SSL_VERSION_OR_CIPHER_MISMATCH`. Another try rarely helps. `ERR_ALPN_NEGOTIATION_FAILED` has this type wherever it comes up, so a stream of unknown size raises it over plain HTTP too, where no TLS is involved.

### `CertificateError`

The server's certificate was refused, such as `ERR_CERT_AUTHORITY_INVALID`, so its identity is not proven. Another try does not help. This is a `TLSError` as well.

```python
with mitto.Session() as session:
    try:
        session.get("https://intranet.example.test/")
    except mitto.CertificateError as error:
        assert error.name == "ERR_CERT_AUTHORITY_INVALID"
```

### `ProxyError`

The proxy failed or refused the request, which says nothing about the server behind it. Check the proxy.

| Net error | What it means |
| --- | --- |
| `ERR_PROXY_CONNECTION_FAILED` | No connection to the proxy |
| `ERR_PROXY_AUTH_REQUESTED` | The proxy asks for credentials this Session does not have, or refuses those it has |
| `ERR_PROXY_AUTH_UNSUPPORTED` | A SOCKS5 proxy wants neither of the methods a Session with credentials offers |
| `ERR_SOCKS_CONNECTION_FAILED` | A SOCKS5 proxy does not speak the protocol, closes during the exchange, or asks a Session with no credentials |
| `ERR_TUNNEL_CONNECTION_FAILED` | The proxy would not open a tunnel to the server |

A connection that resets during the SOCKS5 exchange keeps its own error, so it is a `ConnectionFailedError`.

### `ProtocolError`

The server broke the rules of HTTP, or a response ended early, such as `ERR_CONTENT_LENGTH_MISMATCH` or `ERR_HTTP2_PROTOCOL_ERROR`. Another try may help.

### `TooManyRedirectsError`

A chain of redirects went past the limit of 20 that `//net` has, which often means a loop. Another try rarely helps.

```python
with mitto.Session() as session:
    try:
        session.get("https://example.test/loop")
    except mitto.TooManyRedirectsError as error:
        assert error.name == "ERR_TOO_MANY_REDIRECTS"
```

### `RequestTimeoutError`

A timer of the request ran out, or one that `//net` has of its own, such as `ERR_TIMED_OUT` or `ERR_CONNECTION_TIMED_OUT`. This is often worth another try, or a longer one. It is also Python's `TimeoutError`, so `except TimeoutError` catches it.

### `HTTPStatusError`

A response's status says that the request failed, as raised by [`Response.raise_for_status()`](#responseraise_for_status). Nothing else raises it, and it is not a `RequestError`, because the request did get a response.

| Attribute | Type | What it holds |
| --- | --- | --- |
| `response` | `Response` | The response, whose status and URL are here too |
| `status` | `int` | The HTTP status code |
| `url` | `str` | The final URL, after redirects |

## asyncio

The same browsing state, the same arguments and the same errors, also on the wire. What differs is how a caller waits, and this section says only that. One file descriptor per Session tells the event loop when any of its requests has an event waiting, so 1000 requests need no thread each.

```python
import asyncio


async def main():
    async with mitto.AsyncSession() as session:
        return await asyncio.gather(
            session.get("https://example.test/"),
            session.post("https://example.test/echo", json={"a": 1}),
        )


hello, echo = asyncio.run(main())

assert hello.text == "Hello\n"
assert echo.json()["body"] == '{"a":1}'
```

### `AsyncSession`

```text
AsyncSession(
    *,
    persona: str = 'chrome_157_windows',
    machine: Machine | None = None,
    accept_language: str | None = None,
    additional_trust_anchors: Sequence[str] = (),
    timeout: float | Timeout | None = None,
    cookie_file: str | os.PathLike[str] | None = None,
    proxy: str | None = None,
    cache: str | os.PathLike[str] | None = None,
)
```

The arguments mean what they mean for [`Session`](#session-1). The constructor keeps them for when this opens, which is when they are checked, so what `Session()` raises comes from `open()`, an `async with` statement or a first request.

Open it with an `async with` statement. Without one, `await session.open()` opens it, `await session.close()` closes it, and a first request opens one that nothing has opened yet. Opening and closing go off the event loop for what can block. It stays on the event loop that opened it, and belongs to that loop's thread, so any other loop gets a `RuntimeError`.

Cancelling a task cancels the native request it is waiting for or reading. That also goes for a read cancelled by `asyncio.timeout()`, so a later read raises a `RequestError` named `ERR_ABORTED` and cannot pass for the end of the body. A request's `read` timer is what waits for a slow server and leaves the request running.

```python
async def main():
    async with mitto.AsyncSession() as session:
        try:
            async with asyncio.timeout(0.1):
                await session.get("https://example.test/slow")
        except TimeoutError:
            return await session.get("https://example.test/")


assert asyncio.run(main()).status == 200
```

#### `AsyncSession.cookies`

```text
AsyncSession.cookies -> AsyncCookies
```

The [cookies](#asynccookies). One that has not opened has none yet, which raises a `RuntimeError`.

#### `AsyncSession.machine`

```text
AsyncSession.machine -> Machine
```

The Machine, as [`Session.machine`](#sessionmachine) says. One that has not opened has none yet, which raises a `RuntimeError`, and it claims one when it opens, from `cookie_file` or `machine=`, or by a draw.

#### `AsyncSession.open()`

```text
async AsyncSession.open() -> Self
```

Open this AsyncSession on the running event loop, unless it has opened already, and return it. It goes on opening if the caller is cancelled, until `close()` closes it. It raises what `Session()` raises, a `ValueError` if it has been closed, and a `RuntimeError` if it opened on another event loop.

#### `AsyncSession.close()`

```text
async AsyncSession.close() -> None
```

Do as `Session.close()` does, off the event loop for what can block. It goes on if the caller is cancelled. A second call waits for the first and does nothing else. It raises an `OSError` if `cookie_file` cannot be written, and a `RuntimeError` if it opened on another event loop.

#### `AsyncSession.request()`

```text
async AsyncSession.request(method: str, url: str, **options: Unpack[AsyncRequestOptions]) -> Response
```

Fetch `url`, and return once the whole response has arrived. It takes and raises what `Session.request()` does, and a `RuntimeError` if this opened on another event loop. Two things differ about a body:

- `data=` also takes an async iterable of bytes, which is a stream with no size unless `headers` gives one. It is read on the loop, and takes up nothing while it waits.
- A file or an iterable is read on a thread of the event loop's default executor, as `//net` asks for more, so one that waits for long takes up a thread until it gives something. Bytes in memory are copied on the loop, once, which holds it up for tens of milliseconds at hundreds of MiB, so a body that large is better off as a stream.

```python
async def parts():
    yield b"first "
    yield b"second"


async def main():
    async with mitto.AsyncSession() as session:
        return await session.post("https://h2.example.test/", data=parts())


assert asyncio.run(main()).protocol == "h2"
```

#### `AsyncSession.stream()`

```text
AsyncSession.stream(method: str, url: str, **options: Unpack[AsyncRequestOptions]) -> AsyncGenerator[AsyncResponseStream]
```

Fetch `url` in an `async with` statement, and read the response's body as it arrives, as `Session.stream()` does. It gives an [`AsyncResponseStream`](#asyncresponsestream).

```python
async def main():
    async with (
        mitto.AsyncSession() as session,
        session.stream("GET", "https://example.test/large") as stream,
    ):
        return sum([len(part) async for part in stream.iter_bytes()])


assert asyncio.run(main()) == 1024 * 1024
```

#### `AsyncSession.get()`, `AsyncSession.post()`, `AsyncSession.put()`, `AsyncSession.patch()`, `AsyncSession.delete()`, `AsyncSession.head()`, `AsyncSession.options()`

```text
async AsyncSession.get(url: str, **options: Unpack[AsyncRequestOptions]) -> Response
async AsyncSession.post(url: str, **options: Unpack[AsyncRequestOptions]) -> Response
async AsyncSession.put(url: str, **options: Unpack[AsyncRequestOptions]) -> Response
async AsyncSession.patch(url: str, **options: Unpack[AsyncRequestOptions]) -> Response
async AsyncSession.delete(url: str, **options: Unpack[AsyncRequestOptions]) -> Response
async AsyncSession.head(url: str, **options: Unpack[AsyncRequestOptions]) -> Response
async AsyncSession.options(url: str, **options: Unpack[AsyncRequestOptions]) -> Response
```

Send a request of that method, as `request()` does.

### `AsyncResponseStream`

What [`ResponseStream`](#responsestream) holds, and reading follows the same rules, but for needing an `await` and what a cancel does, as [`AsyncSession`](#asyncsession) says.

| Attribute | Type | What it holds |
| --- | --- | --- |
| `status` | `int` | As in `ResponseStream` |
| `url` | `str` | As in `ResponseStream` |
| `protocol` | `str` | As in `ResponseStream` |
| `headers` | `Headers` | As in `ResponseStream` |
| `history` | `tuple[Redirect, ...]` | As in `ResponseStream` |
| `from_cache` | `bool` | As in `ResponseStream` |

#### `AsyncResponseStream.iter_bytes()`

```text
AsyncResponseStream.iter_bytes() -> AsyncIterator[bytes]
```

Return an async iterator over the rest of the body, as `ResponseStream.iter_bytes()` does.

#### `AsyncResponseStream.read()`

```text
async AsyncResponseStream.read() -> bytes
```

Return the rest of the body once all of it has arrived, as `ResponseStream.read()` does.

### `AsyncCookies`

What [`Cookies`](#cookies-1) has, with the file left to a thread of the event loop's. What the network thread answers is one short task on it, which never waits for Python, so the other calls stay plain ones, on the event loop's thread. They raise a `ValueError` once the AsyncSession has begun to close.

#### `AsyncCookies.get()`, `AsyncCookies.set()`, `AsyncCookies.delete()`, `AsyncCookies.clear()`

```text
AsyncCookies.get(name: str, *, domain: str | None = None, path: str | None = None) -> Cookie | None
AsyncCookies.set(cookie: Cookie) -> None
AsyncCookies.delete(name: str, *, domain: str | None = None, path: str | None = None) -> int
AsyncCookies.clear() -> int
```

As in `Cookies`, and with no `await`.

#### `AsyncCookies.save()`, `AsyncCookies.load()`

```text
async AsyncCookies.save(path: str | os.PathLike[str]) -> None
async AsyncCookies.load(path: str | os.PathLike[str]) -> None
```

As in `Cookies`, off the event loop.

```python
async def main():
    async with mitto.AsyncSession() as session:
        session.cookies.set(mitto.Cookie("a", "1", domain="example.test"))
        await session.cookies.save("saved.json")
        session.cookies.clear()
        await session.cookies.load("saved.json")
        return session.cookies.get("a").value


assert asyncio.run(main()) == "1"
```
