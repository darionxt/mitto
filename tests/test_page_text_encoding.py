"""Tests of TextEncoder and TextDecoder in the Page program: docs/runtime.md, "What a Known Gap does".

Both have behaviour, Blink's: native/encoding holds its codecs and the registry of the Encoding Standard's labels, and
native/dom its two classes. Each expected value is what Chrome 157 gave for the same script. The decodings of every byte
and every pair of bytes are compared by a checksum of the string, with its length. These run where MITTO_PAGE_PROGRAM
names the Page program, as tests/test_page_program.py does.
"""

import json
import os
import subprocess
from pathlib import Path

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 120

# Helpers that the scripts share: the code points of a string, a checksum of one (FNV-1a over its code units, and its
# length), and a Uint8Array of bytes.
_PRELUDE = """
const hex = (s) => Array.from(s, (c) => c.codePointAt(0).toString(16)).join(" ");
const sum = (s) => {
    let h = 0x811c9dc5;
    for (let i = 0; i < s.length; i++) {
        h = Math.imul(h ^ s.charCodeAt(i), 0x01000193) >>> 0;
    }
    return h.toString(16) + "/" + s.length;
};
const bytes = (...values) => new Uint8Array(values);
"""

# The Encoding Standard's labels of each encoding, from its encodings.json, which name it whatever their ASCII case and
# whitespace around them. TextDecoder refuses those of replacement.
_LABELS = {
    "UTF-8": "unicode-1-1-utf-8 unicode11utf8 unicode20utf8 utf-8 utf8 x-unicode20utf8",
    "IBM866": "866 cp866 csibm866 ibm866",
    "ISO-8859-2": "csisolatin2 iso-8859-2 iso-ir-101 iso8859-2 iso88592 iso_8859-2 iso_8859-2:1987 l2 latin2",
    "ISO-8859-3": "csisolatin3 iso-8859-3 iso-ir-109 iso8859-3 iso88593 iso_8859-3 iso_8859-3:1988 l3 latin3",
    "ISO-8859-4": "csisolatin4 iso-8859-4 iso-ir-110 iso8859-4 iso88594 iso_8859-4 iso_8859-4:1988 l4 latin4",
    "ISO-8859-5": "csisolatincyrillic cyrillic iso-8859-5 iso-ir-144 iso8859-5 iso88595 iso_8859-5 iso_8859-5:1988",
    "ISO-8859-6": (
        "arabic asmo-708 csiso88596e csiso88596i csisolatinarabic ecma-114 iso-8859-6 iso-8859-6-e iso-8859-6-i"
        " iso-ir-127 iso8859-6 iso88596 iso_8859-6 iso_8859-6:1987"
    ),
    "ISO-8859-7": (
        "csisolatingreek ecma-118 elot_928 greek greek8 iso-8859-7 iso-ir-126 iso8859-7 iso88597 iso_8859-7"
        " iso_8859-7:1987 sun_eu_greek"
    ),
    "ISO-8859-8": (
        "csiso88598e csisolatinhebrew hebrew iso-8859-8 iso-8859-8-e iso-ir-138 iso8859-8 iso88598 iso_8859-8"
        " iso_8859-8:1988 visual"
    ),
    "ISO-8859-8-I": "csiso88598i iso-8859-8-i logical",
    "ISO-8859-10": "csisolatin6 iso-8859-10 iso-ir-157 iso8859-10 iso885910 l6 latin6",
    "ISO-8859-13": "iso-8859-13 iso8859-13 iso885913",
    "ISO-8859-14": "iso-8859-14 iso8859-14 iso885914",
    "ISO-8859-15": "csisolatin9 iso-8859-15 iso8859-15 iso885915 iso_8859-15 l9",
    "ISO-8859-16": "iso-8859-16",
    "KOI8-R": "cskoi8r koi koi8 koi8-r koi8_r",
    "KOI8-U": "koi8-ru koi8-u",
    "macintosh": "csmacintosh mac macintosh x-mac-roman",
    "windows-874": "dos-874 iso-8859-11 iso8859-11 iso885911 tis-620 windows-874",
    "windows-1250": "cp1250 windows-1250 x-cp1250",
    "windows-1251": "cp1251 windows-1251 x-cp1251",
    "windows-1252": (
        "ansi_x3.4-1968 ascii cp1252 cp819 csisolatin1 ibm819 iso-8859-1 iso-ir-100 iso8859-1 iso88591 iso_8859-1"
        " iso_8859-1:1987 l1 latin1 us-ascii windows-1252 x-cp1252"
    ),
    "windows-1253": "cp1253 windows-1253 x-cp1253",
    "windows-1254": (
        "cp1254 csisolatin5 iso-8859-9 iso-ir-148 iso8859-9 iso88599 iso_8859-9 iso_8859-9:1989 l5 latin5"
        " windows-1254 x-cp1254"
    ),
    "windows-1255": "cp1255 windows-1255 x-cp1255",
    "windows-1256": "cp1256 windows-1256 x-cp1256",
    "windows-1257": "cp1257 windows-1257 x-cp1257",
    "windows-1258": "cp1258 windows-1258 x-cp1258",
    "x-mac-cyrillic": "x-mac-cyrillic x-mac-ukrainian",
    "GBK": "chinese csgb2312 csiso58gb231280 gb2312 gb_2312 gb_2312-80 gbk iso-ir-58 x-gbk",
    "gb18030": "gb18030",
    "Big5": "big5 big5-hkscs cn-big5 csbig5 x-x-big5",
    "EUC-JP": "cseucpkdfmtjapanese euc-jp x-euc-jp",
    "ISO-2022-JP": "csiso2022jp iso-2022-jp",
    "Shift_JIS": "csshiftjis ms932 ms_kanji shift-jis shift_jis sjis windows-31j x-sjis",
    "EUC-KR": (
        "cseuckr csksc56011987 euc-kr iso-ir-149 korean ks_c_5601-1987 ks_c_5601-1989 ksc5601 ksc_5601 windows-949"
    ),
    "replacement": "csiso2022kr hz-gb-2312 iso-2022-cn iso-2022-cn-ext iso-2022-kr replacement",
    "UTF-16BE": "unicodefffe utf-16be",
    "UTF-16LE": "csunicode iso-10646-ucs-2 ucs-2 unicode unicodefeff utf-16 utf-16le",
    "x-user-defined": "x-user-defined",
}

# The encodings of one byte for each character.
_SINGLE_BYTE = [name for name in _LABELS if name.startswith(("IBM", "ISO-8859", "KOI8", "mac", "windows", "x-"))]

# The encodings of more than one byte for some characters.
_CJK = ["GBK", "gb18030", "Big5", "EUC-JP", "ISO-2022-JP", "Shift_JIS", "EUC-KR"]


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _evaluate(program: Path, script: str) -> str:
    # The outcome of the script as a string, or what it threw, as tests/test_page_known_gaps.py writes it.
    wrapped = f"""(async () => {{
        try {{
            const value = await (async () => {{ {_PRELUDE} {script} }})();
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
_ENCODER = {
    "encoding": ("return new TextEncoder().encoding", "value: utf-8"),
    "encode": (
        "return Array.from(new TextEncoder().encode('a\\u00e9\\u20ac\\ud83d\\ude00')).join()",
        "value: 97,195,169,226,130,172,240,159,152,128",
    ),
    "encode a lone surrogate": (
        "return Array.from(new TextEncoder().encode('\\ud800x\\udc00')).join()",
        "value: 239,191,189,120,239,191,189",
    ),
    "encode nothing, undefined and null": (
        "const e = new TextEncoder(); return [e.encode().length, e.encode(undefined).length, e.encode(null)].join(' ')",
        "value: 0 0 110,117,108,108",
    ),
    "encode converts its argument": (
        "return new TextEncoder().encode({toString() { return 'xy'; }}).join()",
        "value: 120,121",
    ),
    "encode gives a Uint8Array of a buffer of its own": (
        "const a = new TextEncoder().encode('abc');"
        " return [Object.prototype.toString.call(a), a.byteOffset, a.length, a.buffer.byteLength, a.buffer.resizable]"
        ".join()",
        "value: [object Uint8Array],0,3,3,false",
    ),
    "encodeInto": (
        "const u = new Uint8Array(10); const r = new TextEncoder().encodeInto('a\\u20ac\\ud83d\\ude00b', u);"
        " return JSON.stringify([r, Array.from(u), Object.getPrototypeOf(r) === Object.prototype])",
        'value: [{"read":5,"written":9},[97,226,130,172,240,159,152,128,98,0],true]',
    ),
    "encodeInto stops before a code point that does not fit": (
        "const u = new Uint8Array(3); const r = new TextEncoder().encodeInto('a\\u20ac', u);"
        " return JSON.stringify([r, Array.from(u)])",
        'value: [{"read":1,"written":1},[97,0,0]]',
    ),
    "encodeInto a view into a larger buffer": (
        "const b = new ArrayBuffer(6); const r = new TextEncoder().encodeInto('xyz', new Uint8Array(b, 2, 2));"
        " return JSON.stringify([r, Array.from(new Uint8Array(b))])",
        'value: [{"read":2,"written":2},[0,0,120,121,0,0]]',
    ),
    "encodeInto a lone surrogate": (
        "const u = new Uint8Array(4); const r = new TextEncoder().encodeInto('\\ud800', u);"
        " return JSON.stringify([r, Array.from(u)])",
        'value: [{"read":1,"written":3},[239,191,189,0]]',
    ),
    "encodeInto a detached buffer": (
        "const b = new ArrayBuffer(4); const u = new Uint8Array(b); b.transfer();"
        " return JSON.stringify(new TextEncoder().encodeInto('ab', u))",
        'value: {"read":0,"written":0}',
    ),
    "encodeInto without a destination": (
        "return new TextEncoder().encodeInto('a')",
        "threw: TypeError: Failed to execute 'encodeInto' on 'TextEncoder': 2 arguments required, but only 1 present.",
    ),
    "encodeInto an array": (
        "return new TextEncoder().encodeInto('a', [])",
        "threw: TypeError: Failed to execute 'encodeInto' on 'TextEncoder': parameter 2 is not of type 'Uint8Array'.",
    ),
    "encodeInto an Int8Array": (
        "return new TextEncoder().encodeInto('a', new Int8Array(2))",
        "threw: TypeError: Failed to execute 'encodeInto' on 'TextEncoder': parameter 2 is not of type 'Uint8Array'.",
    ),
    "encodeInto a resizable buffer": (
        "return new TextEncoder().encodeInto('a', new Uint8Array(new ArrayBuffer(2, {maxByteLength: 4})))",
        "threw: TypeError: Failed to execute 'encodeInto' on 'TextEncoder': The provided Uint8Array value must not be "
        "resizable.",
    ),
    "encodeInto a shared buffer": (
        "const memory = new WebAssembly.Memory({initial: 1, maximum: 1, shared: true});"
        " return new TextEncoder().encodeInto('a', new Uint8Array(memory.buffer))",
        "threw: TypeError: Failed to execute 'encodeInto' on 'TextEncoder': The provided Uint8Array value must not be "
        "shared.",
    ),
    "the constructor without new": (
        "return TextEncoder()",
        "threw: TypeError: Failed to construct 'TextEncoder': Please use the 'new' operator, this DOM object "
        "constructor cannot be called as a function.",
    ),
    "encode on another object": (
        "return TextEncoder.prototype.encode.call({})",
        "threw: TypeError: Illegal invocation",
    ),
}


@pytest.mark.parametrize(("script", "chrome"), list(_ENCODER.values()), ids=list(_ENCODER))
def test_text_encoder_does_what_chrome_does(program: Path, script: str, chrome: str) -> None:
    assert _evaluate(program, script) == chrome


_LABEL_NAMES = {label: name for name, labels in _LABELS.items() for label in labels.split()}

# The encoding that each label names, or the error that it throws, and what any of its other spellings gives where that
# differs: in upper case, or with ASCII whitespace around it.
_LABELS_SCRIPT = f"""
    const result = {{}};
    for (const label of {list(_LABEL_NAMES)!r}) {{
        for (const spelling of [label, label.toUpperCase(), ' \\t\\n\\f\\r' + label + '\\r\\n\\f\\t ']) {{
            let outcome;
            try {{
                outcome = new TextDecoder(spelling).encoding;
            }} catch (error) {{
                outcome = error.name;
            }}
            if (spelling === label) {{
                result[label] = outcome;
            }} else if (outcome !== result[label]) {{
                result[label] = 'differs as ' + JSON.stringify(spelling);
            }}
        }}
    }}
    return JSON.stringify(result);
"""


def test_each_label_names_its_encoding(program: Path) -> None:
    expected = {label: "RangeError" if name == "replacement" else name.lower() for label, name in _LABEL_NAMES.items()}
    outcome = _evaluate(program, _LABELS_SCRIPT)
    assert outcome.startswith("value: ")
    assert json.loads(outcome.removeprefix("value: ")) == expected


# Labels beyond the Encoding Standard's that Chromium's ICU has, or that look like one.
_OTHER_LABELS = (
    "646 ascii7 cesu-8 cp1250-1 cp367 cp437 cp850 cp878 csASCII euc-tw gb_2312-1980 hz ibm-1208 ibm-5348_P100-1997"
    " ibm037 ibm437 iso-2022-jp-2 iso-8859-16 iso-ir-6 iso646-us iso8859_1 iso_8859-16 koi8u l10 latin10 mac-roman"
    " maccyrillic ms936 scsu shift_jis-2004 tis620 ucs-4 us utf-16le-bom utf-32 utf-32be utf-7 utf16 windows-1252-2000"
    " windows-936 windows-950 x-big5 x-euc x-mac-ce x-mac-greek x-mac-turkish x-utf-16le x-windows-949 8859_1"
    " ISO_2022,locale=ja,version=0 utf-8\u00a0 \u00a0utf-8 utf-8\u0000 \u212aoi8-r"
)

_DECODER = {
    "labels beyond the Encoding Standard's": (
        f"""const result = [];
        for (const label of {_OTHER_LABELS!r}.split(' ')) {{
            try {{
                result.push(label + '=' + new TextDecoder(label).encoding);
            }} catch (error) {{
                result.push(label + ':' + error.name);
            }}
        }}
        return result.join(' ')""",
        (
            "value: 646:RangeError ascii7:RangeError cesu-8:RangeError cp1250-1:RangeError cp367:RangeError "
            "cp437:RangeError cp850:RangeError cp878:RangeError csASCII:RangeError euc-tw:RangeError "
            "gb_2312-1980:RangeError hz:RangeError ibm-1208:RangeError ibm-5348_P100-1997:RangeError ibm037:RangeError "
            "ibm437:RangeError iso-2022-jp-2:RangeError iso-8859-16=iso-8859-16 iso-ir-6:RangeError "
            "iso646-us:RangeError iso8859_1:RangeError iso_8859-16:RangeError koi8u:RangeError l10:RangeError "
            "latin10:RangeError mac-roman:RangeError maccyrillic:RangeError ms936:RangeError scsu:RangeError "
            "shift_jis-2004:RangeError tis620:RangeError ucs-4:RangeError us:RangeError utf-16le-bom:RangeError "
            "utf-32:RangeError utf-32be:RangeError utf-7:RangeError utf16:RangeError windows-1252-2000:RangeError "
            "windows-936:RangeError windows-950:RangeError x-big5:RangeError x-euc:RangeError x-mac-ce:RangeError "
            "x-mac-greek:RangeError x-mac-turkish:RangeError x-utf-16le:RangeError x-windows-949:RangeError "
            "8859_1:RangeError ISO_2022,locale=ja,version=0:RangeError utf-8\u00a0:RangeError \u00a0utf-8:RangeError "
            "utf-8\u0000:RangeError \u212aoi8-r:RangeError"
        ),
    ),
    "an invalid label": (
        "return new TextDecoder(' foo ')",
        "threw: RangeError: Failed to construct 'TextDecoder': The encoding label provided (' foo ') is invalid.",
    ),
    "the replacement encoding": (
        "return new TextDecoder('iso-2022-kr')",
        "threw: RangeError: Failed to construct 'TextDecoder': The encoding label provided ('iso-2022-kr') is invalid.",
    ),
    "an empty label": (
        "return new TextDecoder('')",
        "threw: RangeError: Failed to construct 'TextDecoder': The encoding label provided ('') is invalid.",
    ),
    "a label of undefined and null": (
        "return [new TextDecoder(undefined).encoding, new TextDecoder(null).encoding].join()",
        "threw: RangeError: Failed to construct 'TextDecoder': The encoding label provided ('null') is invalid.",
    ),
    "options that are not a dictionary": (
        "return new TextDecoder('utf-8', 5)",
        "threw: TypeError: Failed to construct 'TextDecoder': The provided value is not of type 'TextDecoderOptions'.",
    ),
    "options read in order": (
        "const read = []; const options = {};"
        " for (const key of ['ignoreBOM', 'fatal', 'stream']) {"
        "   Object.defineProperty(options, key, {get() { read.push(key); return false; }}); }"
        " new TextDecoder('utf-8', options).decode(bytes(1), options); return read.join()",
        "value: fatal,ignoreBOM,stream",
    ),
    "every byte of each single-byte encoding": (
        f"""const all = new Uint8Array(256).map((_, i) => i);
        return {_SINGLE_BYTE!r}.map((name) => name + ' ' + sum(new TextDecoder(name).decode(all))).join(', ')""",
        (
            "value: IBM866 c94f511d/256, ISO-8859-2 70d570e/256, ISO-8859-3 daefe744/256, ISO-8859-4 a6feef21/256, "
            "ISO-8859-5 258d6261/256, ISO-8859-6 98e099f9/256, ISO-8859-7 a1869e7f/256, ISO-8859-8 f4599334/256, "
            "ISO-8859-8-I f4599334/256, ISO-8859-10 ab9bc02e/256, ISO-8859-13 d20e6e4c/256, ISO-8859-14 4529d464/256, "
            "ISO-8859-15 25cb4d27/256, ISO-8859-16 d9e65a4b/256, KOI8-R 415e04a9/256, KOI8-U 6f861d47/256, macintosh "
            "270d7788/256, windows-874 751240d1/256, windows-1250 46c01ba5/256, windows-1251 865aeb5d/256, "
            "windows-1252 1f0fb4af/256, windows-1253 3f2fcaeb/256, windows-1254 73ac6d07/256, windows-1255 "
            "8c95c90f/256, windows-1256 84f9ea20/256, windows-1257 7d2cd804/256, windows-1258 2b601276/256, "
            "x-mac-cyrillic bfedee2/256, x-user-defined c51d92c5/256"
        ),
    ),
    "the bytes that each single-byte encoding lacks": (
        f"""return {_SINGLE_BYTE!r}.map((name) => {{
            const lacking = [];
            for (let i = 128; i < 256; i++) {{
                try {{
                    new TextDecoder(name, {{fatal: true}}).decode(bytes(i));
                }} catch (error) {{
                    lacking.push(i.toString(16));
                }}
            }}
            return name + ' ' + lacking.join(' ');
        }}).join(', ')""",
        (
            "value: IBM866 , ISO-8859-2 , ISO-8859-3 a5 ae be c3 d0 e3 f0, ISO-8859-4 , ISO-8859-5 , ISO-8859-6 a1 a2 "
            "a3 a5 a6 a7 a8 a9 aa ab ae af b0 b1 b2 b3 b4 b5 b6 b7 b8 b9 ba bc bd be c0 db dc dd de df f3 f4 f5 f6 f7 "
            "f8 f9 fa fb fc fd fe ff, ISO-8859-7 ae d2 ff, ISO-8859-8 a1 bf c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 ca cb cc cd "
            "ce cf d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 da db dc dd de fb fc ff, ISO-8859-8-I a1 bf c0 c1 c2 c3 c4 c5 c6 c7 "
            "c8 c9 ca cb cc cd ce cf d0 d1 d2 d3 d4 d5 d6 d7 d8 d9 da db dc dd de fb fc ff, ISO-8859-10 , ISO-8859-13 "
            ", ISO-8859-14 , ISO-8859-15 , ISO-8859-16 , KOI8-R , KOI8-U , macintosh , windows-874 db dc dd de fc fd "
            "fe ff, windows-1250 , windows-1251 , windows-1252 , windows-1253 aa d2 ff, windows-1254 , windows-1255 d9 "
            "da db dc dd de df fb fc ff, windows-1256 , windows-1257 a1 a5, windows-1258 , x-mac-cyrillic , "
            "x-user-defined "
        ),
    ),
    "every pair of bytes of each CJK encoding": (
        f"""const pairs = new Uint8Array(2 * 128 * 256);
        for (let i = 0; i < 128 * 256; i++) {{
            pairs[2 * i] = 0x80 + (i >> 8);
            pairs[2 * i + 1] = i & 0xff;
        }}
        return {_CJK!r}.map((name) => name + ' ' + sum(new TextDecoder(name).decode(pairs))).join(', ')""",
        (
            "value: GBK 389fe4e8/39744, gb18030 389fe4e8/39744, Big5 6d9e8069/43331, EUC-JP a6a6b04b/50176, "
            "ISO-2022-JP 8bf8f1c5/65536, Shift_JIS 9c61c4a/50488, EUC-KR f80ad65/45550"
        ),
    ),
    "the pairs of bytes that each CJK encoding lacks": (
        f"""return {_CJK!r}.map((name) => {{
            const decoder = new TextDecoder(name, {{fatal: true}});
            let lacking = 0;
            let first = '';
            for (let lead = 0x80; lead < 0x100; lead++) {{
                for (let trail = 0x40; trail < 0x100; trail++) {{
                    try {{
                        decoder.decode(bytes(lead, trail));
                    }} catch (error) {{
                        lacking++;
                        first ||= lead.toString(16) + trail.toString(16);
                    }}
                }}
            }}
            return name + ' ' + lacking + ' from ' + first;
        }}).join(', ')""",
        (
            "value: GBK 571 from 8081, gb18030 571 from 8081, Big5 5982 from 8040, EUC-JP 17177 from 8040, ISO-2022-JP "
            "24576 from 8040, Shift_JIS 6780 from 8081, EUC-KR 7528 from 8040"
        ),
    ),
    "four-byte sequences of gb18030": (
        """const out = [];
        for (const first of [0x81, 0x82, 0x84, 0x85, 0x8f, 0x90, 0xe3, 0xe4, 0xfd, 0xfe]) {
            for (let second = 0x30; second <= 0x39; second++) {
                for (let third = 0x81; third <= 0xfe; third += 5) {
                    for (let fourth = 0x30; fourth <= 0x39; fourth += 3) {
                        out.push(first, second, third, fourth);
                    }
                }
            }
        }
        const input = new Uint8Array(out);
        return [sum(new TextDecoder('gb18030').decode(input)), sum(new TextDecoder('gbk').decode(input))].join()""",
        "value: bf02020d/11670,bf02020d/11670",
    ),
    "gb18030's errors give bytes back": (
        "const d = new TextDecoder('gb18030');"
        " return [bytes(0x81, 0x30, 0x41), bytes(0x81, 0x30, 0x81, 0x41), bytes(0x81, 0x7f), bytes(0x84, 0x31, "
        "0xa5, 0x30),"
        " bytes(0xe3, 0x32, 0x9a, 0x36), bytes(0x80, 0xff, 0xa2, 0xe3)].map((b) => hex(d.decode(b))).join(', ')",
        "value: fffd 30 41, fffd 30 4e04, fffd 7f, fffd, fffd, 20ac fffd 20ac",
    ),
    "ISO-2022-JP's escapes": (
        "const d = new TextDecoder('iso-2022-jp');"
        " return [[0x1b, 0x24, 0x42, 0x30, 0x21, 0x1b, 0x28, 0x42, 0x61], [0x1b, 0x28, 0x4a, 0x5c, 0x7e],"
        " [0x1b, 0x28, 0x49, 0x21, 0x5f], [0x1b, 0x24, 0x40, 0x1b, 0x28, 0x42], [0x1b, 0x24], [0x1b, 0x41],"
        " [0x1b, 0x28, 0x41], [0x0e, 0x0f, 0x61], [0x1b, 0x24, 0x42, 0x30], [0x1b, 0x24, 0x42, 0x30, 0x1b, 0x28, 0x42]]"
        ".map((b) => hex(d.decode(new Uint8Array(b)))).join(', ')",
        "value: 4e9c 61, a5 203e, ff61 ff9f, fffd, fffd 24, fffd 41, fffd 28 41, fffd fffd 61, fffd, fffd",
    ),
    "ISO-2022-JP across calls": (
        "const d = new TextDecoder('iso-2022-jp');"
        " return [d.decode(bytes(0x1b), {stream: true}), d.decode(bytes(0x24, 0x42), {stream: true}),"
        " d.decode(bytes(0x30), {stream: true}), d.decode(bytes(0x21, 0x1b), {stream: true}), d.decode(bytes(0x28)),"
        " d.decode(bytes(0x21))].map(hex).join(', ')",
        "value: , , , 4e9c, fffd 28, 21",
    ),
    "EUC-JP's JIS X 0212": (
        "const d = new TextDecoder('euc-jp');"
        " return [bytes(0x8f, 0xb0, 0xa1), bytes(0x8e, 0xb1), bytes(0x8f, 0xa2, 0xb7), bytes(0x8f, 0xfe, 0xfe),"
        " bytes(0xa1, 0x41)].map((b) => hex(d.decode(b))).join(', ')",
        "value: 4e02, ff71, ff5e, fffd, fffd 41",
    ),
    "Shift_JIS's user-defined area and katakana": (
        "const d = new TextDecoder('shift_jis');"
        " return [bytes(0xf0, 0x40), bytes(0xf9, 0xfc), bytes(0xa1, 0xdf), bytes(0x80, 0xa0, 0xfd), bytes(0x81, 0x7f)]"
        ".map((b) => hex(d.decode(b))).join(', ')",
        "value: e000, e757, ff61 ff9f, 80 fffd fffd, fffd 7f",
    ),
    "Big5 beyond the Basic Multilingual Plane": (
        "const d = new TextDecoder('big5');"
        " return [bytes(0x87, 0x40), bytes(0x88, 0x62), bytes(0x88, 0x64), bytes(0x88, 0xa3), bytes(0x88, 0xa5),"
        " bytes(0xfe, 0xfe), bytes(0x81, 0x40)].map((b) => hex(d.decode(b))).join(', ')",
        "value: 43f0, 93 df04, 93 df0c, b3 df04, b3 df0c, 79d4, fffd 40",
    ),
    "UTF-8's errors": (
        "const d = new TextDecoder();"
        " return [bytes(0xc0, 0x80), bytes(0xe0, 0x80, 0x80), bytes(0xed, 0xa0, 0x80), bytes(0xf4, 0x90, 0x80, 0x80),"
        " bytes(0xf0, 0x90, 0x80), bytes(0xe2, 0x82), bytes(0xff, 0x61), bytes(0xf1, 0x80, 0x41),"
        " bytes(0xc3)].map((b) => hex(d.decode(b))).join(', ')",
        "value: fffd fffd, fffd fffd fffd, fffd fffd fffd, fffd fffd fffd fffd, fffd, fffd, fffd 61, fffd 41, fffd",
    ),
    "UTF-8 across calls": (
        "const d = new TextDecoder();"
        " return [d.decode(bytes(0xe2), {stream: true}), d.decode(bytes(0x82), {stream: true}), d.decode(bytes(0xac)),"
        " d.decode(bytes(0xf0, 0x9f), {stream: true}), d.decode(bytes(0x98), {stream: true}), d.decode(),"
        " d.decode(bytes(0xe2), {stream: true}), d.decode(bytes(0x41))].map(hex).join(', ')",
        "value: , , 20ac, , , fffd, , fffd 41",
    ),
    "UTF-16 across calls": (
        "const le = new TextDecoder('utf-16le'); const be = new TextDecoder('utf-16be');"
        " return [le.decode(bytes(0x3d), {stream: true}), le.decode(bytes(0xd8, 0x00), {stream: true}),"
        " le.decode(bytes(0xde)), le.decode(bytes(0x00, 0xd8, 0x41)), le.decode(bytes(0x00, 0xdc)),"
        " be.decode(bytes(0xd8, 0x3d, 0xde, 0x00, 0x00)), be.decode(bytes(0x00, 0x41), {stream: true}), be.decode()]"
        ".map(hex).join(', ')",
        "value: , , 1f600, fffd, fffd, 1f600 fffd, 41, ",
    ),
    "a byte order mark": (
        "const decode = (label, b, options) => hex(new TextDecoder(label, options).decode(new Uint8Array(b)));"
        " return [decode('utf-8', [0xef, 0xbb, 0xbf, 0x61]), decode('utf-8', [0xef, 0xbb, 0xbf, 0x61], {ignoreBOM: "
        "true}),"
        " decode('utf-8', [0xef, 0xbb, 0xbf, 0xef, 0xbb, 0xbf]), decode('utf-16le', [0xff, 0xfe, 0x61, 0x00]),"
        " decode('utf-16be', [0xfe, 0xff, 0x00, 0x61]), decode('utf-16le', [0xfe, 0xff]), decode('gb18030', [0x84, "
        "0x31, 0x95, 0x33]),"
        " decode('windows-1252', [0xef, 0xbb, 0xbf])].join(', ')",
        "value: 61, feff 61, feff, 61, 61, fffe, feff, ef bb bf",
    ),
    "a byte order mark across calls": (
        "const d = new TextDecoder();"
        " return [d.decode(bytes(0xef, 0xbb), {stream: true}), d.decode(bytes(0xbf, 0x61), {stream: true}),"
        " d.decode(bytes(0xef, 0xbb, 0xbf)), d.decode(bytes(0xef, 0xbb, 0xbf, 0x62)), d.decode(bytes()),"
        " d.decode(bytes(0xef, 0xbb, 0xbf), {stream: true}), d.decode(bytes(0xef, 0xbb, 0xbf))].map(hex).join(', ')",
        "value: , 61, feff, 62, , , feff",
    ),
    "fatal": (
        "const d = new TextDecoder('utf-8', {fatal: true}); const out = [];"
        " for (const b of [bytes(0x61, 0xff), bytes(0x62), bytes(0xe2, 0x82), bytes(0x63)]) {"
        "   try { out.push(hex(d.decode(b))); } catch (error) { out.push(String(error)); } }"
        " return out.join(', ')",
        (
            "value: TypeError: Failed to execute 'decode' on 'TextDecoder': The encoded data was not valid., 62, "
            "TypeError: Failed to execute 'decode' on 'TextDecoder': The encoded data was not valid., 63"
        ),
    ),
    "fatal across calls": (
        "const d = new TextDecoder('utf-8', {fatal: true}); const out = [];"
        " for (const [b, stream] of [[bytes(0xe2), true], [bytes(0x41), true], [bytes(0x42), true], [bytes(0x43), "
        "false],"
        " [bytes(0xf0, 0x9f), true], [bytes(), false], [bytes(0x44), false]]) {"
        "   try { out.push(hex(d.decode(b, {stream}))); } catch (error) { out.push(error.name); } }"
        " return out.join(', ')",
        "value: , TypeError, TypeError, TypeError, , TypeError, 44",
    ),
    "fatal in other encodings": (
        "const out = [];"
        " for (const [label, b] of [['utf-16le', [0x00, 0xdc]], ['utf-16be', [0x00]], ['shift_jis', [0x81, 0x20]],"
        " ['gb18030', [0x81, 0x30, 0x81]], ['iso-2022-jp', [0x1b, 0x24]], ['windows-1253', [0xaa]],"
        " ['x-user-defined', [0xff]], ['big5', [0x80]]]) {"
        "   try { out.push(hex(new TextDecoder(label, {fatal: true}).decode(new Uint8Array(b)))); }"
        "   catch (error) { out.push(label + ' ' + error.name); } }"
        " return out.join(', ')",
        (
            "value: utf-16le TypeError, utf-16be TypeError, shift_jis TypeError, gb18030 TypeError, iso-2022-jp "
            "TypeError, windows-1253 TypeError, f7ff, big5 TypeError"
        ),
    ),
    "decode takes an ArrayBuffer, a view or a DataView": (
        "const d = new TextDecoder(); const buffer = new Uint8Array([0x61, 0x62, 0x63, 0x64]).buffer;"
        " return [d.decode(buffer), d.decode(new Uint8Array(buffer, 1, 2)), d.decode(new DataView(buffer, 3)),"
        " d.decode(new Uint16Array(buffer, 2)), d.decode(undefined), d.decode()].join()",
        "value: abcd,bc,d,cd,,",
    ),
    "decode of a detached buffer": (
        "const b = new Uint8Array([0x61]).buffer; const v = new Uint8Array(b); b.transfer();"
        " return [new TextDecoder().decode(b), new TextDecoder().decode(v)].join() + '|'",
        "value: ,|",
    ),
    "decode of a buffer that its options detach": (
        "const b = new TextEncoder().encode('abc').buffer;"
        " return JSON.stringify(new TextDecoder().decode(b, {get stream() { b.transfer(0); return false; }}))",
        'value: ""',
    ),
    "decode of a view whose buffer its options detach": (
        "const v = new TextEncoder().encode('abcdefghijklmnopqrstuvwxyz0123456789'.repeat(2));"
        " return JSON.stringify(new TextDecoder().decode(v, {get stream() { v.buffer.transfer(0); return false; }}))",
        'value: ""',
    ),
    "decode of a small view whose buffer its options detach": (
        "const v = new Uint8Array([97, 98, 99]);"
        " return JSON.stringify(new TextDecoder().decode(v, {get stream() { v.buffer.transfer(0); return false; }}))",
        'value: "abc"',
    ),
    "decode of something else": (
        "return new TextDecoder().decode('abc')",
        "threw: TypeError: Failed to execute 'decode' on 'TextDecoder': parameter 1 is not of type 'ArrayBuffer'.",
    ),
    "decode of null": (
        "return new TextDecoder().decode(null)",
        "threw: TypeError: Failed to execute 'decode' on 'TextDecoder': parameter 1 is not of type 'ArrayBuffer'.",
    ),
    "decode of a resizable buffer": (
        "return new TextDecoder().decode(new ArrayBuffer(2, {maxByteLength: 4}))",
        (
            "threw: TypeError: Failed to execute 'decode' on 'TextDecoder': The provided ArrayBuffer value must not be "
            "resizable"
        ),
    ),
    "decode of a view of a resizable buffer": (
        "return new TextDecoder().decode(new Uint8Array(new ArrayBuffer(2, {maxByteLength: 4})))",
        (
            "threw: TypeError: Failed to execute 'decode' on 'TextDecoder': The provided ArrayBuffer value must not be "
            "resizable"
        ),
    ),
    "decode of a shared buffer": (
        "return new TextDecoder().decode(new WebAssembly.Memory({initial: 1, maximum: 1, shared: true}).buffer)",
        "threw: TypeError: Failed to execute 'decode' on 'TextDecoder': parameter 1 is not of type 'ArrayBuffer'.",
    ),
    "decode of a view of a shared buffer": (
        "const memory = new WebAssembly.Memory({initial: 1, maximum: 1, shared: true});"
        " return new TextDecoder().decode(new Uint8Array(memory.buffer, 0, 2))",
        (
            "threw: TypeError: Failed to execute 'decode' on 'TextDecoder': The provided ArrayBufferView value must "
            "not be shared."
        ),
    ),
    "decode's options that are not a dictionary": (
        "return new TextDecoder().decode(bytes(0x61), 'stream')",
        (
            "threw: TypeError: Failed to execute 'decode' on 'TextDecoder': The provided value is not of type "
            "'TextDecodeOptions'."
        ),
    ),
    "decode on another object": (
        "return TextDecoder.prototype.decode.call({})",
        "threw: TypeError: Illegal invocation",
    ),
    "the default encoding and options": (
        "const d = new TextDecoder(); return [d.encoding, d.fatal, d.ignoreBOM].join()",
        "value: utf-8,false,false",
    ),
    "options": (
        "const d = new TextDecoder('utf-16be', {fatal: 1, ignoreBOM: 'yes'}); return [d.encoding, d.fatal, d.ignoreBOM]"
        ".join()",
        "value: utf-16be,true,true",
    ),
}


@pytest.mark.parametrize(("script", "chrome"), list(_DECODER.values()), ids=list(_DECODER))
def test_text_decoder_does_what_chrome_does(program: Path, script: str, chrome: str) -> None:
    assert _evaluate(program, script) == chrome
