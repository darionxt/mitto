"""Tests of URL and URLSearchParams in the Page program: docs/runtime.md, "What the DOM has".

Both have behaviour, Blink's: native/dom/url.h parses as Blink's KURL does, with the parts of native/dom/url_utils.h
that links share, and native/dom/url_search_params.h keeps the parameters. Each expected value is what Chrome
157.0.8103.51 gave for the same script. Iterating over URLSearchParams is a Known Gap, which these leave out. These run
where MITTO_PAGE_PROGRAM names the Page program, as tests/test_page_program.py does.
"""

import os
import subprocess
from pathlib import Path

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 120


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _evaluate(program: Path, script: str) -> str:
    # The outcome of the script as a string, or what it threw, as tests/test_page_text_encoding.py writes it.
    wrapped = f"""(async () => {{
        try {{
            const value = await (async () => {{ {script} }})();
            return "value: " + (typeof value === "object" && value !== null
                ? Object.prototype.toString.call(value) : String(value));
        }} catch (error) {{
            return "threw: " + error;
        }}
    }})()"""
    result = subprocess.run(
        [program, "-"], input=wrapped, capture_output=True, encoding="utf-8", timeout=_TIMEOUT_SECONDS, check=False
    )
    assert (result.returncode, result.stderr) == (0, "")
    return result.stdout.removesuffix("\n")


# What Chrome 157.0.8103.51 gave for each script.
_URL = {
    "a relative URL against a base": (
        "return new URL('../c?d=1#e', 'http://a.test/b/x').href",
        "value: http://a.test/c?d=1#e",
    ),
    "each part": (
        "const u = new URL('https://user:pw@h.test:8443/p/q?x=1&y=%20#f');"
        " return [u.protocol, u.username, u.password, u.host, u.hostname, u.port, u.pathname, u.search, u.hash,"
        " u.origin].join('|')",
        "value: https:|user|pw|h.test:8443|h.test|8443|/p/q|?x=1&y=%20|#f|https://h.test:8443",
    ),
    "a default port and an IDN": (
        "const u = new URL('HTTP://Bücher.example:80/a b?c d#e f'); return [u.href, u.host, u.port].join('|')",
        "value: http://xn--bcher-kva.example/a%20b?c%20d#e%20f|xn--bcher-kva.example|",
    ),
    "a URL that does not parse": (
        "return new URL('x')",
        "threw: TypeError: Failed to construct 'URL': Invalid URL",
    ),
    "a base that does not parse": (
        "return new URL('x', 'y')",
        "threw: TypeError: Failed to construct 'URL': Invalid base URL",
    ),
    "href's setter": (
        "const u = new URL('http://a/b?c'); u.href = 'https://d/e?f=1'; return [u.href, u.searchParams.get('f')].join('|')",
        "value: https://d/e?f=1|1",
    ),
    "href's setter with a URL that does not parse": (
        "const u = new URL('http://a/'); u.href = 'x'",
        "threw: TypeError: Failed to set the 'href' property on 'URL': Invalid URL",
    ),
    "the other setters": (
        "const u = new URL('http://a/b'); u.protocol = 'https'; u.username = 'n'; u.password = 'p'; u.host = 'h:81';"
        " u.pathname = '/c d'; u.search = 'e=1'; u.hash = 'g'; const before = u.href; u.port = ''; u.hostname = 'i';"
        " u.hash = ''; u.search = '?'; return [before, u.href].join('|')",
        "value: https://n:p@h:81/c%20d?e=1#g|https://n:p@i/c%20d",
    ),
    "setters that change nothing": (
        "const u = new URL('mailto:x@y'); u.host = 'h'; u.pathname = '/p'; u.port = '8'; u.protocol = 'http';"
        " return u.href",
        "value: http://x@y/",
    ),
    "toString and toJSON": (
        "const u = new URL('http://a/b'); return [String(u), JSON.stringify(u), u.toJSON()].join('|')",
        'value: http://a/b|"http://a/b"|http://a/b',
    ),
    "parse and canParse": (
        "return [URL.parse('x'), URL.parse('/p', 'http://h/').href, URL.parse('/p', 'y'), URL.canParse('x'),"
        " URL.canParse('/p', 'http://h'), URL.canParse('/p', 'y')].join('|')",
        "value: |http://h/p||false|true|false",
    ),
    "webkitURL": ("return webkitURL === URL", "value: true"),
    "origins": (
        "return [new URL('blob:http://a:80/x').origin, new URL('file:///x').origin, new URL('data:,x').origin,"
        " new URL('https://a:443/').origin].join('|')",
        "value: http://a|file://|null|https://a",
    ),
    "searchParams follows search and writes it back": (
        "const u = new URL('http://a/?x=1'); const p = u.searchParams; p.append('y', '2 3'); const written = u.href;"
        " u.search = '?z=4'; return [written, u.href, p.toString(), p.size].join('|')",
        "value: http://a/?x=1&y=2+3|http://a/?z=4|z=4|1",
    ),
    "searchParams is one object": (
        "const u = new URL('http://a/'); return u.searchParams === u.searchParams",
        "value: true",
    ),
    "deleting the last parameter drops the query": (
        "const u = new URL('http://a/?x#h'); u.searchParams.delete('x'); return u.href",
        "value: http://a/#h",
    ),
}

_SEARCH_PARAMS = {
    "a query string": (
        "return new URLSearchParams('?a=1&b=%zz&c=%E2%82%AC+x&=&d&&e=%C3').toString()",
        "value: a=1&b=%25zz&c=%E2%82%AC+x&=&d=&e=%EF%BF%BD",
    ),
    "a query string read back": (
        "const p = new URLSearchParams('?a=1&b=%zz&c=%E2%82%AC+x&=&d&&e=%C3');"
        " return JSON.stringify([p.get('a'), p.get('b'), p.get('c'), p.get(''), p.get('d'), p.get('e'), p.get('z'),"
        " p.size])",
        'value: ["1","%zz","€ x","","","�",null,6]',
    ),
    "pairs": (
        "return new URLSearchParams([['a', '1'], ['b', 'x y'], ['a', '2']]).toString()",
        "value: a=1&b=x+y&a=2",
    ),
    # Blink's message names the constructor, and the bindings add it again.
    "a pair of one": (
        "return new URLSearchParams([['a']])",
        "threw: TypeError: Failed to construct 'URLSearchParams': Failed to construct 'URLSearchParams': Sequence "
        "initializer must only contain pair elements",
    ),
    "a record": (
        "return new URLSearchParams({a: '1 2', b: 'é*-._~!\\'()'}).toString()",
        "value: a=1+2&b=%C3%A9*-._%7E%21%27%28%29",
    ),
    "a page's object with an iterator": (
        "return new URLSearchParams(new Map([['m', 'n']])).toString()",
        "value: m=n",
    ),
    "nothing, undefined and null": (
        "return [new URLSearchParams().toString(), new URLSearchParams(undefined).toString(),"
        " new URLSearchParams(null).toString()].join('|')",
        "value: ||null=",
    ),
    "get, getAll and has": (
        "const p = new URLSearchParams('a=1&a=2&b=3'); return JSON.stringify([p.get('a'), p.getAll('a'), p.getAll('z'),"
        " p.has('a'), p.has('a', '2'), p.has('a', '5'), p.has('z')])",
        'value: ["1",["1","2"],[],true,true,false,false]',
    ),
    "delete": (
        "const p = new URLSearchParams('a=1&a=2&b=3'); p.delete('a', '2'); const one = p.toString(); p.delete('a');"
        " return [one, p.toString()].join('|')",
        "value: a=1&b=3|b=3",
    ),
    "set": (
        "const p = new URLSearchParams('a=1&b=2&a=3'); p.set('a', '4'); p.set('c', '5'); return p.toString()",
        "value: a=4&b=2&c=5",
    ),
    "sort by code units": (
        "const p = new URLSearchParams(); for (const name of ['b', '\\ufffd', 'a', '\\ud83d\\ude00', 'A', 'b'])"
        " p.append(name, String(p.size)); p.sort(); return p.toString()",
        "value: A=4&a=2&b=0&b=5&%F0%9F%98%80=3&%EF%BF%BD=1",
    ),
    "lone surrogates": (
        "const p = new URLSearchParams(); p.append('\\ud800', '\\udc00x'); return [p.toString(), p.get('\\ufffd')].join('|')",
        "value: %EF%BF%BD=%EF%BF%BDx|�x",
    ),
    "the constructor without new": (
        "return URLSearchParams()",
        "threw: TypeError: Failed to construct 'URLSearchParams': Please use the 'new' operator, this DOM object "
        "constructor cannot be called as a function.",
    ),
    "a method on another object": (
        "return URLSearchParams.prototype.get.call({}, 'a')",
        "threw: TypeError: Illegal invocation",
    ),
}


@pytest.mark.parametrize(("script", "chrome"), list(_URL.values()), ids=list(_URL))
def test_url_does_what_chrome_does(program: Path, script: str, chrome: str) -> None:
    assert _evaluate(program, script) == chrome


@pytest.mark.parametrize(("script", "chrome"), list(_SEARCH_PARAMS.values()), ids=list(_SEARCH_PARAMS))
def test_url_search_params_does_what_chrome_does(program: Path, script: str, chrome: str) -> None:
    assert _evaluate(program, script) == chrome
