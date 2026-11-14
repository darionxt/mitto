"""Tests of how a Page decodes its document and its scripts, and encodes the query of a URL that its document resolves:
docs/runtime.md, "Loading a document" and "Text encodings".

Each case is a document's URL, its Content-Type and its body in the pieces that the Page gets it in, with the scripts it
asks for. The test plays the Session at the other end of the Page program's channel, as tests/test_page_program.py does,
so each case comes with exactly those lines and pieces. What the document reads of itself afterwards, and the URLs of
the scripts that it asks for, are compared with what Chrome 157 gave for the same case, which
tests/data/document_encodings.json keeps: Chrome 157.0.8103.51 on macOS recorded it with its user interface in en-US, the
locale of every Persona, from a local server that sent each piece of a body on its own and every host to itself. Decoded
text is compared by a checksum of its code units, with its length, as tests/test_page_text_encoding.py compares it.

These run on macOS and Linux where MITTO_PAGE_PROGRAM names the Page program, as tests/test_page_program.py does.
"""

import json
import os
import struct
import sys
from collections.abc import Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import pytest

from tests.test_page_program import (  # pyright: ignore[reportPrivateUsage]
    _CONSOLE,  # pyright: ignore[reportPrivateUsage]
    _CREDIT,  # pyright: ignore[reportPrivateUsage]
    _KNOWN_GAP,  # pyright: ignore[reportPrivateUsage]
    _LOADED,  # pyright: ignore[reportPrivateUsage]
    _PROGRAM_ENV_VAR,  # pyright: ignore[reportPrivateUsage]
    _REQUEST,  # pyright: ignore[reportPrivateUsage]
    _TIMEOUT_SECONDS,  # pyright: ignore[reportPrivateUsage]
    _UNCAUGHT_ERROR,  # pyright: ignore[reportPrivateUsage]
    _body,  # pyright: ignore[reportPrivateUsage]
    _end,  # pyright: ignore[reportPrivateUsage]
    _respond,  # pyright: ignore[reportPrivateUsage]
    _Session,  # pyright: ignore[reportPrivateUsage]
)

CHROME_RESULTS = Path(__file__).parent / "data" / "document_encodings.json"

# What a document reads of itself once it has loaded: its encoding by each of its three names, a checksum of the text of
# its element with ID t and of its title, and of each string that its scripts put in `r`.
PROBE = """
(() => {
  const sum = (s) => {
    let h = 0x811c9dc5;
    for (let i = 0; i < s.length; i++) {
      h = Math.imul(h ^ s.charCodeAt(i), 0x01000193) >>> 0;
    }
    return h.toString(16) + "/" + s.length;
  };
  const t = document.getElementById("t");
  const r = self.r || {};
  return {
    charset: [document.characterSet, document.charset, document.inputEncoding],
    text: t ? sum(t.textContent) : null,
    title: sum(document.title),
    scripts: Object.fromEntries(Object.keys(r).sort().map((key) => [key, sum(r[key])])),
  };
})()
"""


@dataclass(frozen=True)
class Script:
    """A script's response: its Content-Type and its body."""

    content_type: str
    body: bytes


@dataclass(frozen=True)
class Case:
    """A document's URL, its Content-Type and its body, in the pieces that it comes in, and the responses of the scripts
    it asks for, in the order it asks for them."""

    name: str
    content_type: str
    pieces: tuple[bytes, ...]
    scripts: tuple[Script, ...] = ()
    host: str = "www.example.test"

    @property
    def url(self) -> str:
        return f"http://{self.host}/case/{self.name}"

    @property
    def has_html_tree(self) -> bool:
        """Whether the Page builds the tree that Chrome does. Chrome makes another kind of document of other types, which
        the Page parses as HTML, a Known Gap, so only their encoding and requests compare."""
        return self.content_type.split(";")[0] == "text/html"


# The Encoding Standard's encodings, each with the label that the cases declare it by.
_LABELS = {
    "UTF-8": "utf-8",
    "IBM866": "ibm866",
    "ISO-8859-2": "iso-8859-2",
    "ISO-8859-3": "iso-8859-3",
    "ISO-8859-4": "iso-8859-4",
    "ISO-8859-5": "iso-8859-5",
    "ISO-8859-6": "iso-8859-6",
    "ISO-8859-7": "iso-8859-7",
    "ISO-8859-8": "iso-8859-8",
    "ISO-8859-8-I": "iso-8859-8-i",
    "ISO-8859-10": "iso-8859-10",
    "ISO-8859-13": "iso-8859-13",
    "ISO-8859-14": "iso-8859-14",
    "ISO-8859-15": "iso-8859-15",
    "ISO-8859-16": "iso-8859-16",
    "KOI8-R": "koi8-r",
    "KOI8-U": "koi8-u",
    "macintosh": "macintosh",
    "windows-874": "windows-874",
    "windows-1250": "windows-1250",
    "windows-1251": "windows-1251",
    "windows-1252": "windows-1252",
    "windows-1253": "windows-1253",
    "windows-1254": "windows-1254",
    "windows-1255": "windows-1255",
    "windows-1256": "windows-1256",
    "windows-1257": "windows-1257",
    "windows-1258": "windows-1258",
    "x-mac-cyrillic": "x-mac-cyrillic",
    "GBK": "gbk",
    "gb18030": "gb18030",
    "Big5": "big5",
    "EUC-JP": "euc-jp",
    "ISO-2022-JP": "iso-2022-jp",
    "Shift_JIS": "shift_jis",
    "EUC-KR": "euc-kr",
    "replacement": "iso-2022-kr",
    "UTF-16BE": "utf-16be",
    "UTF-16LE": "utf-16le",
    "x-user-defined": "x-user-defined",
}
_UTF16 = {"UTF-16BE": "utf-16-be", "UTF-16LE": "utf-16-le"}

# What each encoding decodes in a case: every byte from 0x80 for one of a byte for each character, and for the others
# sequences of each kind they have, and bytes that are errors. None of them decodes to "<" or "&".
_SINGLE_BYTE = bytes(range(0x80, 0x100))
_MULTI_BYTE = {
    "UTF-8": "é€😀".encode() + b"\xff\xc3x",
    "GBK": "中文".encode("gbk") + b"\x80" + "😀".encode("gb18030") + b"\xff",
    "gb18030": "中文".encode("gbk") + b"\x80" + "😀".encode("gb18030") + b"\x84\x31\xa4\x39\xff",
    "Big5": "中文".encode("big5") + b"\x88\x62\x88\x64\xa1\x40\x80\xff",
    "EUC-JP": "日本語".encode("euc_jp") + b"\x8e\xb1\x8f\xb0\xa1\xff",
    "ISO-2022-JP": "日本語".encode("iso2022_jp")[:-3] + b"\x1b(J\x5c\x7e\x1b(I\x31\x1b(B",
    "Shift_JIS": "日本語".encode("shift_jis") + b"\xb1\x80\xa0\xf0\x40\xff",
    "EUC-KR": "한국어".encode("euc_kr") + b"\x81\x41\xff",
    "replacement": "é".encode(),
}
# A string of UTF-16 code units: a character of the Basic Multilingual Plane, one beyond it, and a lone surrogate.
_UTF16_TEXT = "é€\U0001f600\ud800x"

# The characters that the query of each case's script holds, as character references, so that they are the same in a
# document of any encoding: one that most encodings have, others that only some have, and one beyond the Basic
# Multilingual Plane, which none but GB18030 has.
_QUERY = b"?q=&#xE9;&#x20AC;&#x65E5;&#xD55C;&#x1F600;x"


def _payload(name: str) -> bytes:
    if name in _MULTI_BYTE:
        return _MULTI_BYTE[name]
    return _SINGLE_BYTE


def _html(head: bytes, body: bytes) -> bytes:
    return b"<!doctype html><html><head>" + head + b"<title>t</title></head><body>" + body + b"</body></html>"


def _utf16_document(codec: str, script_markup: str = "") -> bytes:
    """A document of UTF-16 code units, markup and all."""
    markup = f"<!doctype html><html><head><title>t</title></head><body><p id=t>{_UTF16_TEXT}</p>{script_markup}</body></html>"
    return markup.encode(codec, "surrogatepass")


def _script_body(key: str, payload: bytes) -> bytes:
    return b'self.r = self.r || {}; self.r["' + key.encode() + b'"] = "' + payload + b'";'


def _utf16_script(key: str, codec: str) -> bytes:
    text = f'self.r = self.r || {{}}; self.r["{key}"] = "{_UTF16_TEXT}";'
    return text.encode(codec, "surrogatepass")


def _declared_by_header(name: str) -> Case:
    """A document in the encoding, which its Content-Type names, with a script that its document's encoding decodes, at a
    URL whose query the encoding encodes."""
    label = _LABELS[name]
    content_type = f"text/html; charset={label}"
    src = b"/script/header-" + name.encode() + b"/0" + _QUERY
    script_markup = b'<script src="' + src + b'"></script>'
    if name in _UTF16:
        codec = _UTF16[name]
        document = _utf16_document(codec, script_markup.decode())
        script = _utf16_script("doc", codec)
    else:
        document = _html(b"", b"<p id=t>" + _payload(name) + b"</p>" + script_markup)
        script = _script_body("doc", _payload(name))
    return Case(f"header-{name}", content_type, (document,), (Script("text/javascript", script),))


def _declared_by_meta(name: str, *, pragma: bool) -> Case:
    """A document that a <meta> declares the encoding of, which is the closest byte-based one."""
    label = _LABELS[name].encode()
    meta = (
        b'<meta http-equiv="Content-Type" content="text/html; charset=' + label + b'">'
        if pragma
        else b'<meta charset="' + label + b'">'
    )
    payload = "é€😀".encode() if name in _UTF16 else _payload(name)
    kind = "pragma" if pragma else "meta"
    return Case(f"{kind}-{name}", "text/html", (_html(meta, b"<p id=t>" + payload + b"</p>"),))


def _scripts_of_each_encoding(way: str) -> Case:
    """A UTF-8 document with a script in each encoding, which its charset attribute or its response names."""
    scripts: list[Script] = []
    markup = b""
    for index, (name, label) in enumerate(_LABELS.items()):
        body = _utf16_script(name, _UTF16[name]) if name in _UTF16 else _script_body(name, _payload(name))
        src = f"/script/scripts-{way}/{index}".encode()
        if way == "attribute":
            markup += b'<script src="' + src + b'" charset="' + label.encode() + b'"></script>'
            scripts.append(Script("text/javascript", body))
        else:
            markup += b'<script src="' + src + b'"></script>'
            scripts.append(Script(f"text/javascript; charset={label}", body))
    return Case(f"scripts-{way}", "text/html; charset=utf-8", (_html(b"", markup),), tuple(scripts))


# A paragraph in each of several languages, for the detector.
_JAPANESE = (
    "日本語のテキストです。これは文字コードの自動判別のための文章です。東京は日本の首都であり、多くの人々が暮らしています。"
    * 2
)
_SIMPLIFIED_CHINESE = (
    "这是一个用于测试字符编码自动检测的中文段落。北京是中华人民共和国的首都、有很多人在这里生活和工作。" * 2
)
_TRADITIONAL_CHINESE = (
    "這是一個用於測試字元編碼自動偵測的中文段落。臺北是一個美麗的城市、有很多人在這裡生活和工作。" * 2
)
_KOREAN = (
    "이것은 문자 인코딩 자동 감지를 시험하기 위한 한국어 문단입니다. 서울은 대한민국의 수도이며 많은 사람들이 살고 있습니다. "
    * 2
)
_RUSSIAN = (
    "Это русский текст для проверки автоматического определения кодировки. Москва является столицей России, "
    "и в ней живёт много людей. "
) * 2
_FRENCH = (
    "Ceci est un texte français pour vérifier la détection automatique de l'encodage. À Paris, les cafés sont très "
    "fréquentés été comme hiver. "
) * 2


def _undeclared(
    name: str, text: str, codec: str, *, host: str = "www.example.test", content_type: str = "text/html"
) -> Case:
    """A document that declares no encoding, in `codec`, which Chrome's detector or its default decodes."""
    return Case(
        f"undeclared-{name}", content_type, (_html(b"", b"<p id=t>" + text.encode(codec) + b"</p>"),), host=host
    )


_SJIS_TEXT = "日本語".encode("shift_jis")
_CYRILLIC = "Ж".encode("cp1251") + bytes(range(0xC0, 0x100))


def _special_cases() -> list[Case]:
    padding = b"<!-- " + b"x" * 1500 + b" -->"
    french = _FRENCH.encode("cp1252")
    ascii_document = _html(b"", b"<p id=t>plain ASCII text</p>")
    cases = [
        # A link's URL encodes its query in the document's encoding, as a script's does.
        Case(
            "link-query",
            "text/html; charset=windows-1252",
            (
                _html(
                    b"",
                    b'<a id=l href="http://www.example.test/x' + _QUERY + b'"></a>'
                    b'<script src="/script/link-query/0"></script>',
                ),
            ),
            (
                Script(
                    "text/javascript",
                    b'const l = document.getElementById("l"); self.r = {href: l.href, search: l.search};',
                ),
            ),
        ),
        # A byte order mark wins over what the response says.
        Case(
            "bom-UTF-8",
            "text/html; charset=windows-1251",
            (b"\xef\xbb\xbf" + _html(b"", b"<p id=t>" + "é€😀".encode() + b"</p>"),),
        ),
        Case("bom-UTF-16LE", "text/html; charset=iso-8859-2", (b"\xff\xfe" + _utf16_document("utf-16-le"),)),
        Case("bom-UTF-16BE", "text/html; charset=iso-8859-2", (b"\xfe\xff" + _utf16_document("utf-16-be"),)),
        # The response's charset wins over a <meta>, unless it names no encoding.
        Case(
            "header-over-meta",
            "text/html; charset=windows-1251",
            (_html(b'<meta charset="shift_jis">', b"<p id=t>" + _CYRILLIC + b"</p>"),),
        ),
        Case(
            "unknown-header-then-meta",
            "text/html; charset=bogus",
            (_html(b'<meta charset="shift_jis">', b"<p id=t>" + french + b"</p>"),),
        ),
        # How //net reads Content-Type, of a document in ASCII, which the detector takes as windows-1252.
        Case("quoted-header", 'text/html; charset="windows-1251"', (ascii_document,)),
        Case("header-with-spaces", "text/html; charset = windows-1251 ", (ascii_document,)),
        Case("two-content-types", "text/plain; charset=windows-1251, text/html", (ascii_document,)),
        Case("same-type-again", "text/html; charset=windows-1251, text/html", (ascii_document,)),
        Case("charset-of-star", "*/*; charset=windows-1251", (ascii_document,)),
        # The prescan reads the head for as long as it lasts, and 1,024 bytes whatever.
        Case(
            "meta-after-1024-bytes-in-head",
            "text/html",
            (_html(padding + b'<meta charset="shift_jis">', b"<p id=t>" + french + b"</p>"),),
        ),
        Case(
            "meta-after-1024-bytes-in-body",
            "text/html",
            (_html(b"", b"<p id=t>" + french + b"</p>" + padding + b'<meta charset="shift_jis">'),),
        ),
        Case(
            "meta-in-first-1024-bytes-in-body",
            "text/html",
            (_html(b"", b"<p id=t>" + french + b'</p><meta charset="shift_jis">'),),
        ),
        Case(
            "meta-in-script",
            "text/html",
            (_html(b'<script>"<meta charset=shift_jis>"</script>', b"<p id=t>" + french + b"</p>"),),
        ),
        Case(
            "meta-in-title",
            "text/html",
            (
                b"<!doctype html><html><head><title><meta charset=shift_jis></title></head><body><p id=t>"
                + french
                + b"</p></body></html>",
            ),
        ),
        Case(
            "meta-in-noscript",
            "text/html",
            (_html(b'<noscript><meta charset="shift_jis"></noscript>', b"<p id=t>" + french + b"</p>"),),
        ),
        Case(
            "meta-with-spaces", "text/html", (_html(b'<meta charset=" shift_jis ">', b"<p id=t>" + french + b"</p>"),)
        ),
        Case(
            "pragma-without-http-equiv",
            "text/html",
            (_html(b'<meta content="text/html; charset=shift_jis">', b"<p id=t>" + french + b"</p>"),),
        ),
        Case(
            "pragma-quoted",
            "text/html",
            (
                _html(
                    b"<meta http-equiv=content-type content=\"text/html; charset='euc-jp'\">",
                    b"<p id=t>" + "日本語".encode("euc_jp") + b"</p>",
                ),
            ),
        ),
        Case(
            "unknown-meta-then-meta",
            "text/html",
            (_html(b'<meta charset="bogus"><meta charset="shift_jis">', b"<p id=t>" + french + b"</p>"),),
        ),
        # A <meta> that comes in a later piece decodes what follows, and the title decodes again.
        Case(
            "meta-in-later-piece",
            "text/html",
            (
                b"<!doctype html><html><head>" + padding,
                b'<meta charset="windows-1251"><title>t</title></head><body><p id=t>'
                + _CYRILLIC
                + b"</p></body></html>",
            ),
        ),
        Case(
            "title-decodes-again",
            "text/html",
            (
                b"<!doctype html><html><head><title>" + "Привет".encode("cp1251") + b"</title>" + padding,
                b'<meta charset="windows-1251"></head><body><p id=t>' + _CYRILLIC + b"</p></body></html>",
            ),
        ),
        # An XML declaration at the very start names an encoding, which a <meta> overrides.
        Case(
            "xml-declaration-in-html",
            "text/html",
            (b'<?xml version="1.0" encoding="shift_jis"?>' + _html(b"", b"<p id=t>" + _SJIS_TEXT + b"</p>"),),
        ),
        Case(
            "xml-declaration-then-meta",
            "text/html",
            (
                b'<?xml version="1.0" encoding="shift_jis"?>'
                + _html(b'<meta charset="euc-kr">', b"<p id=t>" + "한국어".encode("euc_kr") + b"</p>"),
            ),
        ),
        Case(
            "xml-declaration-utf-16",
            "text/html",
            (b'<?xml version="1.0" encoding="utf-16"?>' + _html(b"", b"<p id=t>" + "é€".encode() + b"</p>"),),
        ),
        Case(
            "xml-declaration-x-user-defined",
            "text/html",
            (b'<?xml version="1.0" encoding="x-user-defined"?>' + _html(b"", b"<p id=t>" + _SINGLE_BYTE + b"</p>"),),
        ),
        Case(
            "utf-16le-without-bom",
            "text/html",
            (
                _html(b"", b"<p id=t>x</p>")
                .replace(b"<!doctype html>", b'<?xml version="1.0"?>')
                .decode()
                .encode("utf-16-le"),
            ),
        ),
        # Documents of other types.
        Case("xml-undeclared", "application/xml", (b'<root><p id="t">' + "é€".encode() + b"</p></root>",)),
        Case(
            "xml-declared",
            "application/xml",
            (b'<?xml version="1.0" encoding="Shift_JIS"?><root><p id="t">' + _SJIS_TEXT + b"</p></root>",),
        ),
        Case("json", "application/json", (b'{"a": "' + "é".encode() + b'"}',)),
        Case("css-charset", "text/css", (b'@charset "windows-1251"; p { content: "' + _CYRILLIC + b'"; }',)),
        Case("text-plain-meta", "text/plain", (_html(b'<meta charset="shift_jis">', b"<p id=t>" + french + b"</p>"),)),
        # Nothing declared: Chrome's detector, or its default for the top-level domain or the locale.
        Case("undeclared-empty", "text/html", ()),
        Case("undeclared-empty-ru", "text/html", (), host="www.example.ru"),
        _undeclared("ascii", "plain ASCII text", "ascii"),
        _undeclared("UTF-8", _FRENCH + _JAPANESE, "utf-8"),
        _undeclared("UTF-8-jp", _JAPANESE, "utf-8", host="www.example.jp"),
        _undeclared("windows-1252", _FRENCH, "cp1252"),
        _undeclared("Shift_JIS", _JAPANESE, "shift_jis"),
        _undeclared("EUC-JP", _JAPANESE, "euc_jp"),
        _undeclared("ISO-2022-JP", _JAPANESE, "iso2022_jp"),
        _undeclared("GBK", _SIMPLIFIED_CHINESE, "gbk"),
        _undeclared("Big5", _TRADITIONAL_CHINESE, "big5"),
        _undeclared("EUC-KR", _KOREAN, "euc_kr"),
        _undeclared("windows-1251", _RUSSIAN, "cp1251"),
        _undeclared("KOI8-R", _RUSSIAN, "koi8_r"),
        _undeclared("text-plain-Shift_JIS", _JAPANESE, "shift_jis", content_type="text/plain"),
        # The ASCII start comes in its own piece, which the detector reads first.
        Case(
            "undeclared-pieces",
            "text/html",
            (
                b"<!doctype html><html><head><title>t</title></head><body>",
                b"<p id=t>" + _JAPANESE.encode("shift_jis") + b"</p></body></html>",
            ),
        ),
    ]
    # A script's encoding: its response's charset, or else its charset attribute, or else its document's, but for a
    # byte order mark.
    precedence = [
        ("inherit", b"", "text/javascript", _CYRILLIC),
        ("attribute-empty", b' charset=""', "text/javascript", _CYRILLIC),
        ("attribute", b' charset="koi8-r"', "text/javascript", _CYRILLIC),
        ("header-over-attribute", b' charset="koi8-r"', "text/javascript; charset=iso-8859-5", _CYRILLIC),
        (
            "bom-over-header",
            b' charset="koi8-r"',
            "text/javascript; charset=iso-8859-5",
            b"\xef\xbb\xbf" + "Ж".encode(),
        ),
        ("attribute-unknown", b' charset="bogus"', "text/javascript", _CYRILLIC),
        ("attribute-with-spaces", b' charset=" koi8-r "', "text/javascript", _CYRILLIC),
        ("header-unknown", b' charset="koi8-r"', "text/javascript; charset=bogus", _CYRILLIC),
    ]
    markup = b""
    scripts: list[Script] = []
    for index, (key, attribute, content_type, payload) in enumerate(precedence):
        markup += b'<script src="/script/script-precedence/' + str(index).encode() + b'"' + attribute + b"></script>"
        if payload.startswith(b"\xef\xbb\xbf"):
            body = b"\xef\xbb\xbf" + _script_body(key, payload[3:])
        else:
            body = _script_body(key, payload)
        scripts.append(Script(content_type, body))
    utf16_key = "header-utf-16"
    markup += b'<script src="/script/script-precedence/' + str(len(precedence)).encode() + b'"></script>'
    scripts.append(Script("text/javascript; charset=utf-16", _utf16_script(utf16_key, "utf-16-le")))
    markup += (
        b'<script src="/script/script-precedence/' + str(len(precedence) + 1).encode() + b'" charset="koi8-r"></script>'
    )
    scripts.append(Script("text/javascript", b"\xff\xfe" + _utf16_script("bom-utf-16le", "utf-16-le")))
    cases.append(Case("script-precedence", "text/html; charset=windows-1251", (_html(b"", markup),), tuple(scripts)))
    # The path and the query of a script's URL: UTF-8, and the document's encoding.
    cases.append(
        Case(
            "url-parts",
            "text/html; charset=windows-1251",
            (_html(b"", b'<script src="/script/url-parts/&#x416;/0?&#x416;%E9 #&#x416;"></script>'),),
            (Script("text/javascript", b""),),
        )
    )
    # A script that a document detected as Shift_JIS asks for, and decodes with it.
    cases.append(
        Case(
            "undeclared-script",
            "text/html",
            (
                _html(
                    b"",
                    b"<p id=t>"
                    + _JAPANESE.encode("shift_jis")
                    + b'</p><script src="/script/undeclared-script/0?q=&#x65E5;"></script>',
                ),
            ),
            (Script("text/javascript", _script_body("doc", _SJIS_TEXT)),),
        )
    )
    return cases


def all_cases() -> list[Case]:
    """Every case, which the recording in Chrome loads too."""
    cases = [_declared_by_header(name) for name in _LABELS]
    cases += [_declared_by_meta(name, pragma=False) for name in _LABELS]
    cases += [_declared_by_meta(name, pragma=True) for name in _LABELS]
    cases += [_scripts_of_each_encoding("attribute"), _scripts_of_each_encoding("header")]
    cases += _special_cases()
    return cases


_CASES = all_cases()


@pytest.fixture
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


@pytest.fixture
def session(program: Path) -> Iterator[_Session]:
    """Start the Page program with a channel whose other end the test holds, on macOS and Linux."""
    if sys.platform == "win32":
        pytest.skip("the channel is a named pipe on Windows, which tests/test_page_process.py drives")
    page = _Session(program)
    yield page
    page.socket.close()
    if page.process.poll() is None:
        page.process.kill()
    page.process.wait(_TIMEOUT_SECONDS)


def script_index(url: str) -> int:
    """Which of its case's scripts a URL asks for: the last segment of its path."""
    return int(url.split("?", 1)[0].rsplit("/", 1)[1])


def _requested_url(payload: bytes) -> tuple[int, str]:
    """The number and the URL of a Request, after the ID of its document, without its fragment, which never reaches a
    server. The URL comes after the number, the kind, the flags, five bytes of modes, policy and priority, and the
    target."""
    number = struct.unpack("<Q", payload[8:16])[0]
    (size,) = struct.unpack("<I", payload[31:35])
    url = payload[35 : 35 + size].decode()
    return number, url.split("#", 1)[0]


def _load(session: _Session, case: Case) -> dict[str, Any]:
    """Give the Page the case's document and scripts, and return what it reads of itself and the URLs it asked for."""
    session.start()
    _respond(session, 0, case.url, case.content_type)
    for piece in case.pieces:
        _body(session, 0, piece)
    _end(session, 0)
    requests: list[str] = []
    while True:
        message_type, payload = session.receive()
        if message_type in {_CREDIT, _CONSOLE, _UNCAUGHT_ERROR, _KNOWN_GAP}:
            continue
        if message_type == _LOADED:
            break
        assert message_type == _REQUEST, payload
        number, url = _requested_url(payload)
        requests.append(url)
        script = case.scripts[script_index(url)]
        _respond(session, number, url, script.content_type)
        if script.body:
            _body(session, number, script.body)
        _end(session, number)
    failed, result = session.evaluate(PROBE)
    assert not failed, result
    return {**json.loads(result), "requests": requests}


def _chrome_results() -> dict[str, dict[str, Any]]:
    results: dict[str, dict[str, Any]] = json.loads(CHROME_RESULTS.read_text(encoding="utf-8"))
    return results


@pytest.mark.parametrize("case", _CASES, ids=[case.name for case in _CASES])
def test_a_document_and_its_scripts_decode_as_in_chrome(session: _Session, case: Case) -> None:
    result = _load(session, case)
    chrome = _chrome_results()[case.name]
    if case.has_html_tree:
        assert result == chrome
    else:
        assert (result["charset"], result["requests"]) == (chrome["charset"], chrome["requests"])


def test_chrome_gave_a_result_for_every_case_and_no_other() -> None:
    assert sorted(_chrome_results()) == sorted(case.name for case in _CASES)
