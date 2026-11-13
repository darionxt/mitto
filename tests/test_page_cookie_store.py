"""Tests of the Cookie Store API in a Page that a Session opened, on a local server.

It needs an origin that is not opaque, and so a Page that a Session opened, with a native library and the Page program
beside it. The expected values are what Chrome 157 gave for the same scripts on the same kind of page, headless on macOS
(157.0.8103.51), once the page had loaded. The cookie case keeps only whether an expiry is a time that Chrome would
give, since its times are of the clock: a cookie lasts at most 400 days.
"""

import http.server
import threading
from collections.abc import Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from tests.local_server import QuietServer

_TIMEOUT_SECONDS = 60
_BLANK = b"<!doctype html><title>c</title><body>"

_COOKIES = """
new Promise(async (resolve) => {
  const out = {};
  const day = 864e5;
  const when = (expires) => typeof expires !== 'number' ? expires
      : Math.abs(expires - (Date.now() + 400 * day)) < 6e4 ? 'in 400 days'
      : Math.abs(expires - (Date.now() + 100e3)) < 6e4 ? 'in 100 seconds' : 'another time';
  const item = (c) => c === null ? null : ('expires' in c ? {...c, expires: when(c.expires)} : c);
  const text = (c) => JSON.stringify(item(c));
  const error = async (f) => { try { const v = await f(); return ['ok', v === undefined ? 'undefined' : v]; } catch (e) { return `${e?.name}: ${e?.message}`; } };
  out.shape = [Object.prototype.toString.call(cookieStore), cookieStore === window.cookieStore,
    Object.getOwnPropertyNames(CookieStore.prototype).sort(), typeof CookieChangeEvent];
  out.errors = {
    getEmpty: await error(() => cookieStore.get()),
    getEmptyOptions: await error(() => cookieStore.get({})),
    getUrl: await error(() => cookieStore.get({url: '/other'})),
    eqEmptyName: await error(() => cookieStore.set('', 'a=b')),
    bothEmpty: await error(() => cookieStore.set('', '')),
    nameEq: await error(() => cookieStore.set('a=b', 'c')),
    hostDomain: await error(() => cookieStore.set({name: '__Host-a', value: '1', domain: '127.0.0.1'})),
    dotDomain: await error(() => cookieStore.set({name: 'a', value: '1', domain: '.127.0.0.1'})),
    otherDomain: await error(() => cookieStore.set({name: 'a', value: '1', domain: 'example.com'})),
    hostPath: await error(() => cookieStore.set({name: '__Host-a', value: '1', path: '/x'})),
    badPath: await error(() => cookieStore.set({name: 'a', value: '1', path: 'x'})),
    httpPrefix: await error(() => cookieStore.set('__Http-a', '1')),
    setOneArg: await error(() => cookieStore.set('a')),
    noName: await error(() => cookieStore.set({value: '1'})),
    deleteNone: await error(() => cookieStore.delete()),
    construct: await error(() => new CookieStore()),
    maxAgeAndExpires: await error(() => cookieStore.set({name: 'a', value: '1', expires: Date.now() + 1e6, maxAge: 5})),
  };
  const events = [];
  cookieStore.onchange = (e) => events.push([e.type, e.constructor.name, e.isTrusted, e.bubbles, e.cancelable,
    e.changed.map(text), e.deleted.map(text), e.changed === e.changed, Object.isFrozen(e.changed)]);
  document.cookie = 'plain=1';
  out.setResult = await error(() => cookieStore.set('a', '1'));
  await cookieStore.set({name: 'b', value: '2', path: '/', sameSite: 'lax', expires: 4102444800000});
  await cookieStore.set({name: 'c', value: '3', domain: '127.0.0.1', path: '/sub', sameSite: 'none'});
  await cookieStore.set({name: ' spaced ', value: ' v '});
  out.get = item(await cookieStore.get('a'));
  out.getMissing = await cookieStore.get('zzz');
  out.getAll = (await cookieStore.getAll()).map(item);
  out.getAllNamed = (await cookieStore.getAll('b')).map(item);
  out.getByUrl = item(await cookieStore.get({name: 'a', url: location.href + '#frag'}));
  out.documentCookie = document.cookie;
  await cookieStore.delete('a');
  await cookieStore.delete({name: 'b'});
  await cookieStore.set({name: 'm', value: '1', maxAge: 100});
  out.maxAge = (await cookieStore.get('m')).expires > Date.now();
  await cookieStore.set({name: 'm', value: '1', maxAge: 0});
  out.afterMaxAge0 = await cookieStore.get('m');
  out.afterDelete = (await cookieStore.getAll()).map(c => c.name);
  await new Promise(r => setTimeout(r, 200));
  out.events = events;
  const ev = new CookieChangeEvent('change', {changed: [{name: 'x', value: 'y', extra: 1}], deleted: [{name: 'z'}]});
  out.constructed = [ev.changed.map(c => JSON.stringify(c)), ev.deleted.map(c => JSON.stringify(c)), Object.isFrozen(ev.changed), ev.changed === ev.changed, ev.isTrusted];
  out.constructedEmpty = [new CookieChangeEvent('x').changed.length, Object.isFrozen(new CookieChangeEvent('x').deleted)];
  for (const c of await cookieStore.getAll()) await cookieStore.delete({name: c.name, path: c.path, domain: c.domain});
  document.cookie = 'plain=; max-age=0';
  resolve(out);
})
"""

_COOKIES_EXPECTED: dict[str, Any] = {
    "shape": ["[object CookieStore]", True, ["constructor", "delete", "get", "getAll", "onchange", "set"], "function"],
    "errors": {
        "getEmpty": "TypeError: Failed to execute 'get' on 'CookieStore': CookieStoreGetOptions must not be empty",
        "getEmptyOptions": "TypeError: Failed to execute 'get' on 'CookieStore': CookieStoreGetOptions "
        "must not be empty",
        "getUrl": "TypeError: Failed to execute 'get' on 'CookieStore': URL must match the document URL",
        "eqEmptyName": "TypeError: Failed to execute 'set' on 'CookieStore': Cookie value cannot contain "
        "'=' if the name is empty",
        "bothEmpty": "TypeError: Failed to execute 'set' on 'CookieStore': Cookie name and value both cannot be empty",
        "nameEq": "TypeError: Failed to execute 'set' on 'CookieStore': Cookie name cannot contain '='",
        "hostDomain": "TypeError: Failed to execute 'set' on 'CookieStore': Cookies with \"__Host-\" "
        "prefix cannot have a domain",
        "dotDomain": "TypeError: Failed to execute 'set' on 'CookieStore': Cookie domain cannot start with \".\"",
        "otherDomain": "TypeError: Failed to execute 'set' on 'CookieStore': Cookie domain must "
        "domain-match current host",
        "hostPath": "TypeError: Failed to execute 'set' on 'CookieStore': Cookies with \"__Host-\" "
        'prefix cannot have a non-"/" path',
        "badPath": "TypeError: Failed to execute 'set' on 'CookieStore': Cookie path must start with \"/\"",
        "httpPrefix": "TypeError: Failed to execute 'set' on 'CookieStore': Cookies with \"__Http-\" "
        "prefix cannot be set using the CookieStore API.",
        "setOneArg": "TypeError: Failed to execute 'set' on 'CookieStore': The provided value is not of "
        "type 'CookieInit'.",
        "noName": "TypeError: Failed to execute 'set' on 'CookieStore': Failed to read the 'name' "
        "property from 'CookieInit': Required member is undefined.",
        "deleteNone": "TypeError: Failed to execute 'delete' on 'CookieStore': 1 argument required, but "
        "only 0 present.",
        "construct": "TypeError: Failed to construct 'CookieStore': Illegal constructor",
        "maxAgeAndExpires": "TypeError: Failed to execute 'set' on 'CookieStore': Cookie expires and "
        "maxAge cannot both be specified",
    },
    "setResult": ["ok", "undefined"],
    "get": {
        "domain": None,
        "expires": None,
        "name": "a",
        "partitioned": False,
        "path": "/",
        "sameSite": "strict",
        "secure": True,
        "value": "1",
    },
    "getMissing": None,
    "getAll": [
        {
            "domain": None,
            "expires": None,
            "name": "plain",
            "partitioned": False,
            "path": "/",
            "sameSite": "lax",
            "secure": False,
            "value": "1",
        },
        {
            "domain": None,
            "expires": None,
            "name": "a",
            "partitioned": False,
            "path": "/",
            "sameSite": "strict",
            "secure": True,
            "value": "1",
        },
        {
            "domain": None,
            "expires": "in 400 days",
            "name": "b",
            "partitioned": False,
            "path": "/",
            "sameSite": "lax",
            "secure": True,
            "value": "2",
        },
        {
            "domain": None,
            "expires": None,
            "name": "spaced",
            "partitioned": False,
            "path": "/",
            "sameSite": "strict",
            "secure": True,
            "value": "v",
        },
    ],
    "getAllNamed": [
        {
            "domain": None,
            "expires": "in 400 days",
            "name": "b",
            "partitioned": False,
            "path": "/",
            "sameSite": "lax",
            "secure": True,
            "value": "2",
        }
    ],
    "getByUrl": {
        "domain": None,
        "expires": None,
        "name": "a",
        "partitioned": False,
        "path": "/",
        "sameSite": "strict",
        "secure": True,
        "value": "1",
    },
    "documentCookie": "plain=1; a=1; b=2; spaced=v",
    "maxAge": True,
    "afterMaxAge0": None,
    "afterDelete": ["plain", "spaced"],
    "events": [
        [
            "change",
            "CookieChangeEvent",
            True,
            False,
            False,
            [
                '{"domain":null,"expires":null,"name":"plain","partitioned":false,"path":"/","sameSite":"lax","secure":false,"value":"1"}'
            ],
            [],
            True,
            True,
        ],
        [
            "change",
            "CookieChangeEvent",
            True,
            False,
            False,
            [
                '{"domain":null,"expires":null,"name":"a","partitioned":false,"path":"/","sameSite":"strict","secure":true,"value":"1"}'
            ],
            [],
            True,
            True,
        ],
        [
            "change",
            "CookieChangeEvent",
            True,
            False,
            False,
            [
                '{"domain":null,"expires":"in 400 '
                'days","name":"b","partitioned":false,"path":"/","sameSite":"lax","secure":true,"value":"2"}'
            ],
            [],
            True,
            True,
        ],
        [
            "change",
            "CookieChangeEvent",
            True,
            False,
            False,
            [
                '{"domain":null,"expires":null,"name":"spaced","partitioned":false,"path":"/","sameSite":"strict","secure":true,"value":"v"}'
            ],
            [],
            True,
            True,
        ],
        [
            "change",
            "CookieChangeEvent",
            True,
            False,
            False,
            [],
            ['{"domain":null,"name":"a","partitioned":false,"path":"/","sameSite":"strict","secure":true}'],
            True,
            True,
        ],
        [
            "change",
            "CookieChangeEvent",
            True,
            False,
            False,
            [],
            ['{"domain":null,"name":"b","partitioned":false,"path":"/","sameSite":"lax","secure":true}'],
            True,
            True,
        ],
        [
            "change",
            "CookieChangeEvent",
            True,
            False,
            False,
            [
                '{"domain":null,"expires":"in 100 '
                'seconds","name":"m","partitioned":false,"path":"/","sameSite":"strict","secure":true,"value":"1"}'
            ],
            [],
            True,
            True,
        ],
        [
            "change",
            "CookieChangeEvent",
            True,
            False,
            False,
            [],
            ['{"domain":null,"name":"m","partitioned":false,"path":"/","sameSite":"strict","secure":true}'],
            True,
            True,
        ],
        [
            "change",
            "CookieChangeEvent",
            True,
            False,
            False,
            [],
            ['{"domain":null,"name":"plain","partitioned":false,"path":"/","secure":false}'],
            True,
            True,
        ],
        [
            "change",
            "CookieChangeEvent",
            True,
            False,
            False,
            [],
            ['{"domain":null,"name":"spaced","partitioned":false,"path":"/","sameSite":"strict","secure":true}'],
            True,
            True,
        ],
    ],
    "constructed": [['{"name":"x","value":"y"}'], ['{"name":"z"}'], True, True, False],
    "constructedEmpty": [0, True],
}


@pytest.fixture
def page_program(native_library: Any) -> None:
    """Skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")


@pytest.fixture
def blank_page() -> Iterator[str]:
    """The URL of a blank page on 127.0.0.1, which is a secure context."""

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self) -> None:
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(_BLANK)))
            self.end_headers()
            self.wfile.write(_BLANK)

        def log_message(self, format: str, *args: Any) -> None:
            pass

    server = QuietServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    yield f"http://127.0.0.1:{server.server_port}/"
    server.shutdown()
    server.server_close()


def _evaluate(url: str, script: str) -> Any:
    with mitto.Session() as session, session._open_page(url, timeout=_TIMEOUT_SECONDS) as page:  # pyright: ignore[reportPrivateUsage]
        return page.evaluate(f"Promise.resolve(({script}))")


@pytest.mark.usefixtures("page_program")
def test_the_cookie_store_reads_writes_and_tells_of_changes_as_chrome_s(blank_page: str) -> None:
    assert _evaluate(blank_page, _COOKIES) == _COOKIES_EXPECTED
