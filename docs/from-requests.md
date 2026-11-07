# From `requests` to mitto

A changed import is not enough. mitto looks familiar on purpose, but it is deliberately not compatible with `requests` ([ADR-0004](adr/0004-network-stack-owns-http-semantics.md)). Chrome's network stack decides what `requests` decides for itself, such as redirects, cookies and connection reuse, and a Session sends what its **Persona** would send. So some arguments are gone, and some defaults differ. Most code needs small changes, and this page has the common ones, not all of `requests`. The [API reference](api.md) has every name.

In the tables, `session`, `url` and `response` are these. `tests/test_docs.py` runs each mitto column and each example against local servers, in order. Nothing runs the `requests` column, so this repository does not depend on `requests`.

```python
import mitto

url = "https://example.test/echo"
session = mitto.Session()
response = session.get(url)
```

## What maps directly

| `requests` | mitto | What to know |
| --- | --- | --- |
| `requests.Session()` | `mitto.Session()` | What it keeps is set by [its arguments](api.md#session-1), when it is created. Close it, or use a `with` statement |
| `requests.get(url)` | `mitto.get(url)` | On one default Session that stays open, so its cookies carry over. No other method has a shortcut |
| `session.get(url)` | `session.get(url)` | So do `post`, `put`, `patch`, `delete`, `head` and `options`, and `request("GET", url)` |
| `session.get(url, params={"q": "a b"})` | `session.get(url, params={"q": "a b"})` | Encoded as `URLSearchParams` does. Values [have to be text](#what-is-spelled-differently) |
| `session.get(url, headers={"X-Trace": "1"})` | `session.get(url, headers={"X-Trace": "1"})` | [It changes the Persona's lines](#the-headers-come-from-the-persona) |
| `session.post(url, data={"a": "b"})` | `session.post(url, data={"a": "b"})` | Its `Content-Type` is [what Chrome sends](api.md#data) for that kind of request, with `;charset=UTF-8` on a fetch |
| `session.post(url, data=b"bytes")` | `session.post(url, data=b"bytes")` | With no `Content-Type` |
| `session.post(url, json={"a": 1})` | `session.post(url, json={"a": 1})` | With no spaces in the text, as `JSON.stringify` writes it |
| `session.post(url, data=open("report.csv", "rb"))` | `session.post(url, data=open("report.csv", "rb"))` | A stream, which [goes once](api.md#data) |
| `session.get(url, timeout=5)` | `session.get(url, timeout=5)` | The total time of the request and its redirects, where `requests` gives connecting and each read 5 seconds each |
| `response.content`, `response.text`, `response.json()`, `response.url` | `response.content`, `response.text`, `response.json()`, `response.url` | `text` decodes by `Content-Type` or as UTF-8, and guesses nothing |
| `response.headers["content-type"]` | `response.headers["content-type"]` | Lookup ignores case, and repeated headers join with `", "` |
| `response.raise_for_status()` | `response.raise_for_status()` | An [`HTTPStatusError`](api.md#httpstatuserror) |

## What is spelled differently

| `requests` | mitto | What to know |
| --- | --- | --- |
| `response.status_code` | `response.status` | |
| `response.ok` | `response.status < 400` | A `Response` is always true, so `if response:` says nothing |
| `response.history` | `response.history` | A tuple of [`Redirect`](api.md#redirect): `status`, `url` and `headers`, with no body |
| `response.headers.items()` | `response.headers.pairs` | Iterating `headers` also gives pairs, not names |
| `response.encoding = "latin-1"` | `response.content.decode("latin-1")` | A response cannot be told what its charset is |
| `session.get(url, allow_redirects=False)` | `session.get(url, follow_redirects=False)` | The response is the redirect, with empty content |
| `session.head(url)` | `session.head(url, follow_redirects=False)` | Every method follows redirects by default, where `requests` leaves `HEAD` alone |
| `session.get(url, params={"page": 2})` | `session.get(url, params={"page": "2"})` | A number or `None` as a value raises a `TypeError` that names the argument, and so does one in a mapping in `data` or a number in `headers` |
| `session.get(url, params={"a": ["1", "2"]})` | `session.get(url, params=[("a", "1"), ("a", "2")])` | Pairs repeat a name |
| `session.get(url, timeout=(3, 10))` | `session.get(url, timeout=mitto.Timeout(connect=3, read=10))` | A tuple raises a `TypeError` that names `timeout`. [`Timeout`](api.md#timeout-2) has a `total` too |
| `session.post(url, files={"f": open("report.csv", "rb")})` | `session.post(url, files={"f": ("report.csv", b"a,b\n")})` | A file's content is in bytes, so read it first. Its type is `application/octet-stream` unless a third item says otherwise, and is never guessed |
| `session.get(url, stream=True)` and `iter_content()` | `with session.stream("GET", url) as stream: parts = list(stream.iter_bytes())` | Parts come as they arrive, in no size of a caller's, and the request ends with the `with` statement. `stream.read()` gives the rest at once |
| `session.get(url, cookies={"b": "2"})` | `session.get(url, headers={"Cookie": "b=2"})` | It goes only if the store holds no cookie for the URL, because `//net` writes over it otherwise. A cookie meant to last belongs in the store |
| `response.cookies` | `session.cookies` | There is one store, in `//net`, and a response has no jar |
| `session.cookies.get("a")` | `session.cookies.get("a")` | It returns a [`Cookie`](api.md#cookie) or `None`, not the value, so read `.value` |
| `session.cookies.set("a", "1", domain="example.test")` | `session.cookies.set(mitto.Cookie("a", "1", domain="example.test"))` | `//net` checks it as if a server at that domain had sent it |

`response.reason`, `elapsed`, `raw`, `request`, `links` and `iter_lines()` have no equal.

Errors are spelled differently too, and they sort by what a caller does next:

| `requests.exceptions` | mitto | What to know |
| --- | --- | --- |
| `RequestException` | `mitto.MittoError` | `mitto.RequestError` leaves out `HTTPStatusError`, because that request did get a response |
| `ConnectionError` | `mitto.ConnectionFailedError` | This is no base for the next few, so catch `mitto.RequestError` to mean all of them |
| `Timeout`, `ConnectTimeout`, `ReadTimeout` | `mitto.RequestTimeoutError` | Also Python's `TimeoutError` |
| `SSLError` | `mitto.TLSError` | `mitto.CertificateError` is the part about a refused certificate |
| `ProxyError` | `mitto.ProxyError` | |
| `TooManyRedirects` | `mitto.TooManyRedirectsError` | |
| `HTTPError` | `mitto.HTTPStatusError` | With `status` and `url` as well as `response` |
| `ChunkedEncodingError`, `ContentDecodingError` | `mitto.ProtocolError` | |
| `InvalidURL`, `MissingSchema` | `mitto.RequestError` | With the `name` `ERR_INVALID_URL` |
| `JSONDecodeError` | `ValueError` | From `response.json()`. `requests`' is a `ValueError` too |

A name that does not resolve has a type of its own, `mitto.NameResolutionError`, and `requests` has it as a `ConnectionError`.

## What deliberately differs

### The headers come from the Persona

A Session sends what its Chrome Release sends for that kind of request, in Chrome's order. `requests` would send `python-requests` as a `User-Agent`, and a few lines beside it. So there is no `session.headers` to hold more of them, because each hop of each request has its lines worked out from the Persona and the page it comes from.

To send your own lines, give `headers=` on each request, through a function of your own if there are many. A name the Persona has keeps its place, and another name goes after the Persona's lines.

Removing a line needs `None`, and it does not always remove it. `User-Agent` comes back from `//net` with the Persona's value, later in the request, because a request always gets one from the context. To change the value, give the value.

```python
def api_get(path):
    return session.get(f"https://example.test{path}", headers={"Authorization": "Bearer abc"})


assert api_get("/echo").json()["headers"]["Authorization"] == "Bearer abc"

sent = session.get(url, headers={"User-Agent": None}).json()["headers"]
assert "Chrome/" in sent["User-Agent"]

sent = session.get(url, headers={"User-Agent": "my-tool/1.0"}).json()["headers"]
assert sent["User-Agent"] == "my-tool/1.0"
```

### A request has a kind

Chrome sends other lines for a navigation than for a page's `fetch()`, so a request says which it is. A `GET` is a navigation unless it says otherwise, and any other method is a fetch. `kind=` changes that, and `initiator=` names the page it comes from. A `GET` to an API from a page is a fetch, and `requests` has no word for it.

```python
sent = session.get(url, kind="fetch", initiator="https://example.test/page").json()["headers"]

assert sent["Sec-Fetch-Mode"] == "cors"
assert sent["Sec-Fetch-Site"] == "same-origin"
```

### A POST does not take cookies to another origin

A `POST` is a fetch by default, and a page's `fetch()` sends cookies only to its own origin. So once a redirect takes it to another origin, it sends no cookie and stores none for the rest of its chain, as in Chrome. With `requests` a login that ends on another name leaves a cookie in the session, and here it does not.

If the `POST` is what a form in a page would do, say so with `kind="navigation"`, and the cookie stays. Chrome does the same with a form.

```python
login = "https://login.example.test/redirect?to=https://app.example.test/set-cookie?seen=1"

with mitto.Session() as form_as_fetch:
    form_as_fetch.post(login, data={"user": "a"})
    assert form_as_fetch.cookies.get("seen") is None

with mitto.Session() as form_as_navigation:
    form_as_navigation.post(login, data={"user": "a"}, kind="navigation")
    assert form_as_navigation.cookies.get("seen").value == "1"
```

### A `GET` has no body

`session.get(url, json=...)` works in `requests`. Fetch refuses a body with `GET` or `HEAD`, so Chrome never sends one, and a Session raises a `ValueError`. Use a query, or another method.

```python
try:
    session.get(url, json={"q": "a"})
except ValueError:
    response = session.post(url, json={"q": "a"})
```

### There is no `verify=False`

A Session verifies each certificate as Chrome does, and nothing turns that off. Chrome goes past a certificate it refuses only when a person clicks through, and a Session has nobody to ask. A chain that does not verify ends with a `CertificateError`.

For a private root or a self-signed server certificate, add it with `additional_trust_anchors=`. That also takes the place of `verify="ca.pem"`, except that it adds to the Chrome Root Store and does not replace it. It holds a certificate's text, not a path, and `REQUESTS_CA_BUNDLE` means nothing here.

```python
with open("company-root.pem") as file:
    company_root = file.read()

with mitto.Session(additional_trust_anchors=[company_root]) as trusting:
    assert trusting.get("https://intranet.example.test/").status == 200
```

### There are no client certificates

`cert=` has no equal. Chrome takes a client certificate from the system's store once a person has chosen it, and a Session has neither. A server that asks for one ends the request with a `TLSError` named `ERR_SSL_CLIENT_AUTH_CERT_NEEDED`. To reach such a server, use another client, or a local proxy that holds the certificate and talks to it.

```python
try:
    session.get("https://mtls.example.test/")
except mitto.TLSError as error:
    assert error.name == "ERR_SSL_CLIENT_AUTH_CERT_NEEDED"
```

### There is no `auth=`

`//net` owns authentication challenges, and Chrome answers them by asking a person. For an origin that takes a fixed credential, send an `Authorization` header. As in `requests`, it does not follow a redirect to another origin. A server's `401` stays a response.

A username and password in a URL also work, as in Chrome. They go only once a server has answered with a `401`, where `requests` sends them at once, and `//net` keeps them for the rest of the Session. `.netrc` is not read.

```python
import base64

basic = base64.b64encode(b"user:password").decode()

with mitto.Session() as asking:
    assert asking.get("https://example.test/challenge").status == 401
    answered = asking.get("https://example.test/challenge", headers={"Authorization": f"Basic {basic}"})
    assert answered.status == 200

with mitto.Session() as asking:
    assert asking.get("https://user:password@example.test/challenge").status == 200
```

### A proxy belongs to a Session

`proxies=` has no equal, on a request or on a Session. A Session takes one proxy for all its requests, because its connections and what it has resolved go with it, as a profile's do in Chrome. For several proxies, use a Session for each. `HTTP_PROXY` and the other variables are ignored, and so are the system's settings, so a Session uses a proxy only if it is told to. A proxy that fails raises a `ProxyError`, and a Session never goes directly in its place.

```python
with mitto.Session(proxy="http://user:password@proxy.example.test:8080") as proxied:
    assert proxied.get(url).status == 200
```

### There are no adapters, retries or hooks

`session.mount()`, `HTTPAdapter`, `max_retries` and `hooks=` have no equal. [ADR-0004](adr/0004-network-stack-owns-http-semantics.md) rejected a transport adapter, because two layers would make the same decisions by different rules, and that goes for retries too. `//net` tries again on what Chrome tries again on, and has Chrome's limit of 20 redirects, so there is no `max_redirects` either. For more retries, write the loop, around the errors that say another try is worth it.

```python
attempts = 0

with mitto.Session() as retrying:
    while True:
        attempts += 1
        try:
            response = retrying.get("https://example.test/flaky")
            break
        except (mitto.ConnectionFailedError, mitto.RequestTimeoutError):
            continue

assert attempts == 2
session.close()
```
