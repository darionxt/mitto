"""Tests of the members that pages call most, in the Page program: docs/runtime.md, "The members pages call most".

Each case's expected outcome is what Chrome 157.0.8103.51 gave for the same script, headless on a file:// page, which is
a secure context as the Page program's about:blank is. These run where MITTO_PAGE_PROGRAM names the Page program, as
tests/test_page_program.py does.
"""

import os
import subprocess
from pathlib import Path

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60

_TYPE_MISMATCH = (
    "threw: TypeMismatchError: Failed to execute 'getRandomValues' on 'Crypto': The provided ArrayBufferView is of type "
    "'{}', which is not an integer array type."
)
_NOT_ENCODED = (
    "threw: InvalidCharacterError: Failed to execute 'atob' on 'Window': The string to be decoded is not correctly "
    "encoded."
)
_TOO_LONG = (
    "threw: QuotaExceededError: Failed to execute 'getRandomValues' on 'Crypto': The ArrayBufferView's byte length ({}) "
    "exceeds the number of bytes of entropy available via this API (65536)."
)

_CRYPTO_AS_CHROME = {
    "the same view comes back": ("const a = new Uint8Array(16); return crypto.getRandomValues(a) === a", "value: true"),
    "its bytes are filled": (
        "const a = new Uint32Array(64); crypto.getRandomValues(a); return a.some(x => x !== 0)",
        "value: true",
    ),
    "only the view's bytes": (
        "const b = new Uint8Array(32); const v = new Uint8Array(b.buffer, 8, 8); crypto.getRandomValues(v); "
        "return b.slice(0, 8).every(x => x === 0) && b.slice(16).every(x => x === 0)",
        "value: true",
    ),
    "Float32Array": ("return crypto.getRandomValues(new Float32Array(4))", _TYPE_MISMATCH.format("Float32")),
    "Float64Array": ("return crypto.getRandomValues(new Float64Array(4))", _TYPE_MISMATCH.format("Float64")),
    "Float16Array": ("return crypto.getRandomValues(new Float16Array(4))", _TYPE_MISMATCH.format("Float16")),
    "DataView": (
        "return crypto.getRandomValues(new DataView(new ArrayBuffer(4)))",
        _TYPE_MISMATCH.format("DataView"),
    ),
    "BigInt64Array": ("const a = new BigInt64Array(4); return crypto.getRandomValues(a) === a", "value: true"),
    "Uint8ClampedArray": ("const a = new Uint8ClampedArray(4); return crypto.getRandomValues(a) === a", "value: true"),
    "65536 bytes": ("const a = new Uint8Array(65536); return crypto.getRandomValues(a) === a", "value: true"),
    "65537 bytes": ("return crypto.getRandomValues(new Uint8Array(65537))", _TOO_LONG.format(65537)),
    "too many bytes of a wider type": (
        "return crypto.getRandomValues(new Uint32Array(16385))",
        _TOO_LONG.format(65540),
    ),
    "the QuotaExceededError thrown": (
        "try { crypto.getRandomValues(new Uint8Array(65537)); } catch (e) { return [e instanceof QuotaExceededError, "
        "e instanceof DOMException, e.name, e.code, e.quota, e.requested, Object.prototype.toString.call(e), "
        "typeof e.stack, String(e.stack).split('\\n')[0]].join('|'); }",
        "value: true|true|QuotaExceededError|22|||[object QuotaExceededError]|string|"
        + _TOO_LONG.format(65537).removeprefix("threw: "),
    ),
    "not a view": (
        "return crypto.getRandomValues([1, 2])",
        "threw: TypeError: Failed to execute 'getRandomValues' on 'Crypto': parameter 1 is not of type "
        "'ArrayBufferView'.",
    ),
    "an ArrayBuffer": (
        "return crypto.getRandomValues(new ArrayBuffer(4))",
        "threw: TypeError: Failed to execute 'getRandomValues' on 'Crypto': parameter 1 is not of type "
        "'ArrayBufferView'.",
    ),
    "no argument": (
        "return crypto.getRandomValues()",
        "threw: TypeError: Failed to execute 'getRandomValues' on 'Crypto': 1 argument required, but only 0 present.",
    ),
    "a view of a shared buffer": (
        "return crypto.getRandomValues(new Uint8Array(new WebAssembly.Memory({initial: 1, maximum: 1, shared: true})"
        ".buffer))",
        "threw: TypeError: Failed to execute 'getRandomValues' on 'Crypto': The provided ArrayBufferView value must not "
        "be shared.",
    ),
    "a view of a resizable buffer": (
        "return crypto.getRandomValues(new Uint8Array(new ArrayBuffer(8, {maxByteLength: 16})))",
        "threw: TypeError: Failed to execute 'getRandomValues' on 'Crypto': The provided ArrayBufferView value must not "
        "be resizable.",
    ),
    "a detached view": (
        "const b = new ArrayBuffer(8); const a = new Uint8Array(b); b.transfer(); "
        "return crypto.getRandomValues(a) === a && a.length === 0",
        "value: true",
    ),
    "another receiver": (
        "return Crypto.prototype.getRandomValues.call({}, new Uint8Array(1))",
        "threw: TypeError: Illegal invocation",
    ),
    "randomUUID's form": (
        "const u = crypto.randomUUID(); "
        "return /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/.test(u) "
        "&& u !== crypto.randomUUID()",
        "value: true",
    ),
    "randomUUID on another receiver": (
        "return Crypto.prototype.randomUUID.call({})",
        "threw: TypeError: Illegal invocation",
    ),
    "the same objects each time": (
        "return [crypto === window.crypto, crypto === self.crypto, crypto instanceof Crypto, "
        "Object.getPrototypeOf(crypto) === Crypto.prototype, crypto.subtle === crypto.subtle, "
        "crypto.subtle instanceof SubtleCrypto].join()",
        "value: true,true,true,true,true,true",
    ),
    "new Crypto": ("return new Crypto()", "threw: TypeError: Failed to construct 'Crypto': Illegal constructor"),
    "new QuotaExceededError": (
        "const e = new QuotaExceededError('m', {quota: 5, requested: 10}); return [e.name, e.message, e.code, e.quota, "
        "e.requested, e instanceof DOMException, Object.getPrototypeOf(QuotaExceededError) === DOMException, "
        "Object.prototype.toString.call(e), typeof e.stack].join('|')",
        "value: QuotaExceededError|m|22|5|10|true|true|[object QuotaExceededError]|undefined",
    ),
    "QuotaExceededError's defaults": (
        "const e = new QuotaExceededError(); "
        "return [e.name, JSON.stringify(e.message), e.code, e.quota, e.requested].join('|')",
        'value: QuotaExceededError|""|22||',
    ),
    "QuotaExceededError without new": (
        "return QuotaExceededError('m')",
        "threw: TypeError: Failed to construct 'QuotaExceededError': Please use the 'new' operator, this DOM object "
        "constructor cannot be called as a function.",
    ),
    "QuotaExceededError's options are doubles": (
        "return new QuotaExceededError('m', {quota: 'x'})",
        "threw: TypeError: Failed to construct 'QuotaExceededError': Failed to read the 'quota' property from "
        "'QuotaExceededErrorOptions': The provided double value is non-finite.",
    ),
    "QuotaExceededError checks no range": (
        "const e = new QuotaExceededError('m', {quota: -1, requested: 1}); return [e.quota, e.requested].join()",
        "value: -1,1",
    ),
}


# atob() and btoa() are not counted, but the owner added them on 2026-11-13: as Known Gaps they gave "" and broke
# base64 and the code that builds on it without an error.
_BASE64_AS_CHROME = {
    "btoa of ASCII": ("return btoa('hello')", "value: aGVsbG8="),
    "btoa of Latin-1": ("return btoa('\\u00ff\\u00fe\\u0080')", "value: //6A"),
    "btoa of the empty string": ("return JSON.stringify(btoa(''))", 'value: ""'),
    "btoa of what converts to a string": (
        "return [btoa(null), btoa(undefined), btoa(12)].join()",
        "value: bnVsbA==,dW5kZWZpbmVk,MTI=",
    ),
    "btoa beyond Latin-1": (
        "return btoa('\\u20ac')",
        "threw: InvalidCharacterError: Failed to execute 'btoa' on 'Window': The string to be encoded contains "
        "characters outside of the Latin1 range.",
    ),
    "btoa of a lone surrogate": (
        "return btoa('\\ud800')",
        "threw: InvalidCharacterError: Failed to execute 'btoa' on 'Window': The string to be encoded contains "
        "characters outside of the Latin1 range.",
    ),
    "btoa without an argument": (
        "return btoa()",
        "threw: TypeError: Failed to execute 'btoa' on 'Window': 1 argument required, but only 0 present.",
    ),
    "btoa on another receiver": ("return btoa.call({}, 'a')", "threw: TypeError: Illegal invocation"),
    "btoa on undefined": ("return btoa.call(undefined, 'a')", "value: YQ=="),
    "atob of padded input": ("return atob('aGVsbG8=')", "value: hello"),
    "atob without padding": ("return atob('aGVsbG8')", "value: hello"),
    "atob with ASCII whitespace": ("return atob(' aG\\tVs\\nbG\\f8=\\r ')", "value: hello"),
    "atob with a vertical tab": ("return atob('aGVs\\vbG8=')", _NOT_ENCODED),
    "atob with a no-break space": ("return atob('aGVs\\u00a0bG8=')", _NOT_ENCODED),
    "atob with too much padding": ("return atob('aGVsbG8==')", _NOT_ENCODED),
    "atob of one character": ("return atob('a')", _NOT_ENCODED),
    "atob with padding inside": ("return atob('ab=c')", _NOT_ENCODED),
    "atob of padding alone": ("return atob('==')", _NOT_ENCODED),
    "atob of a character that is not base64": ("return atob('aGVs*G8=')", _NOT_ENCODED),
    "atob of base64url": ("return atob('_-8=')", _NOT_ENCODED),
    "atob beyond Latin-1": (
        "return atob('\\u20ac')",
        "threw: InvalidCharacterError: Failed to execute 'atob' on 'Window': The string to be decoded contains "
        "characters outside of the Latin1 range.",
    ),
    "atob of a Latin-1 letter": ("return atob('\\u00e9')", _NOT_ENCODED),
    "atob gives Latin-1": ("return Array.from(atob('//4A'), c => c.charCodeAt(0)).join()", "value: 255,254,0"),
    "atob of the empty string": ("return JSON.stringify(atob(''))", 'value: ""'),
    "atob of whitespace alone": ("return JSON.stringify(atob(' \\n '))", 'value: ""'),
    "atob of null": ("return Array.from(atob(null), c => c.charCodeAt(0)).join()", "value: 158,233,101"),
    "atob of padding after three characters": ("return atob('YW=')", _NOT_ENCODED),
    "atob of bits left over": ("return Array.from(atob('YR'), c => c.charCodeAt(0)).join()", "value: 97"),
    "atob without an argument": (
        "return atob()",
        "threw: TypeError: Failed to execute 'atob' on 'Window': 1 argument required, but only 0 present.",
    ),
    "atob on another receiver": ("return atob.call({}, 'aGk=')", "threw: TypeError: Illegal invocation"),
    "atob of an object": ("return atob({toString() { return 'aGk='; }})", "value: hi"),
    "the error is a DOMException": (
        "try { atob('a'); } catch (e) { return [e instanceof DOMException, e.name, e.code].join(); }",
        "value: true,InvalidCharacterError,5",
    ),
}


# An event's constructor sets the attributes that its init dictionary names, given or by their defaults, as the DOM
# Standard's inner event creation steps do. Attributes that Chrome works out instead, such as MouseEvent.x or
# PointerEvent.altitudeAngle, keep their defaults until M9.
_EVENTS_AS_CHROME = {
    "MouseEvent from its init": (
        "const e = new MouseEvent('click', {screenX: 5, screenY: 6, clientX: 3, movementX: 2, button: 1, buttons: 4, ctrlKey: true, detail: 9}); return [e.screenX, e.screenY, e.clientX, e.movementX, e.button, e.buttons, e.ctrlKey, e.detail, e.type].join()",
        "value: 5,6,3,2,1,4,true,9,click",
    ),
    "MouseEvent's defaults": (
        "const e = new MouseEvent('click'); return [e.screenX, e.button, e.buttons, e.relatedTarget, e.ctrlKey, e.view].join()",
        "value: 0,0,0,,false,",
    ),
    "a related target and a view": (
        "const t = new EventTarget(); const e = new MouseEvent('mouseover', {relatedTarget: t, view: window}); return [e.relatedTarget === t, e.view === window].join()",
        "value: true,true",
    ),
    "PointerEvent's defaults": (
        "const e = new PointerEvent('pointerdown'); return [e.width, e.height, e.pressure, e.tangentialPressure, e.tiltX, e.twist, e.pointerId, JSON.stringify(e.pointerType), e.isPrimary].join()",
        'value: 1,1,0,0,0,0,0,"",false',
    ),
    "PointerEvent from its init": (
        "const e = new PointerEvent('pointerdown', {pointerId: 7, pointerType: 'pen', pressure: 0.5, width: 3, height: 4, isPrimary: true, tiltX: 10, screenX: 8}); return [e.pointerId, e.pointerType, e.pressure, e.width, e.height, e.isPrimary, e.tiltX, e.screenX].join()",
        "value: 7,pen,0.5,3,4,true,10,8",
    ),
    "WheelEvent": (
        "const e = new WheelEvent('wheel', {deltaMode: 1, deltaY: 3.5}); return [e.deltaMode, e.deltaY, e.deltaX].join()",
        "value: 1,3.5,0",
    ),
    "KeyboardEvent": (
        "const e = new KeyboardEvent('keydown', {key: 'a', code: 'KeyA', repeat: true, location: 1, shiftKey: true}); return [e.key, e.code, e.repeat, e.location, e.shiftKey, e.isComposing].join()",
        "value: a,KeyA,true,1,true,false",
    ),
    "ErrorEvent": (
        "const o = {}; const e = new ErrorEvent('error', {message: 'm', filename: 'f', lineno: 3, colno: 4, error: o}); return [e.message, e.filename, e.lineno, e.colno, e.error === o].join()",
        "value: m,f,3,4,true",
    ),
    "ErrorEvent's defaults": (
        "const e = new ErrorEvent('error'); return [JSON.stringify(e.message), e.lineno, e.error].join()",
        'value: "",0,',
    ),
    "MessageEvent": (
        "const o = {a: 1}; const e = new MessageEvent('message', {data: o, origin: 'https://a.test', lastEventId: '5'}); return [e.data === o, e.origin, e.lastEventId, Array.isArray(e.ports), Object.isFrozen(e.ports), e.ports.length, e.ports === e.ports, e.source].join()",
        "value: true,https://a.test,5,true,true,0,true,",
    ),
    "ProgressEvent": (
        "const e = new ProgressEvent('progress', {loaded: 5, total: 10, lengthComputable: true}); return [e.loaded, e.total, e.lengthComputable].join()",
        "value: 5,10,true",
    ),
    "CookieChangeEvent": (
        "const e = new CookieChangeEvent('change', {changed: [{name: 'a', value: 'b'}]}); const c = e.changed; return [Object.isFrozen(c), c === e.changed, c.length, Object.getPrototypeOf(c[0]) === Object.prototype, JSON.stringify(c), JSON.stringify(e.deleted), Object.isFrozen(e.deleted)].join('|')",
        'value: true|true|1|true|[{"name":"a","value":"b"}]|[]|true',
    ),
    "a CookieChangeEvent's item keeps its members": (
        "const e = new CookieChangeEvent('change', {changed: [{name: 'a', value: 'b', extra: 1}]}); return JSON.stringify(e.changed[0])",
        'value: {"name":"a","value":"b"}',
    ),
    "SecurityPolicyViolationEvent": (
        "const e = new SecurityPolicyViolationEvent('securitypolicyviolation', {blockedURI: 'https://b.test/', violatedDirective: 'v', effectiveDirective: 'e', originalPolicy: 'p', disposition: 'enforce', statusCode: 200, documentURI: 'https://a.test/'}); return [e.blockedURI, e.violatedDirective, e.effectiveDirective, e.originalPolicy, e.disposition, e.statusCode, e.documentURI, JSON.stringify(e.sample), e.lineNumber].join()",
        'value: https://b.test/,v,e,p,enforce,200,https://a.test/,"",0',
    ),
    "UIEvent": (
        "const e = new UIEvent('x', {view: window, detail: 4}); return [e.view === window, e.detail, e.which].join()",
        "value: true,4,0",
    ),
    "an attribute's value stays": (
        "const e = new MouseEvent('click', {screenX: 5}); return [e.screenX, e.screenX].join()",
        "value: 5,5",
    ),
    "CustomEvent's detail": (
        "const o = {}; const e = new CustomEvent('x', {detail: o, bubbles: true}); return [e.detail === o, e.bubbles, new CustomEvent('y').detail, new CustomEvent('y', {detail: undefined}).detail, new CustomEvent('y', {detail: 0}).detail].join()",
        "value: true,true,,,0",
    ),
    "initCustomEvent": (
        "const e = new CustomEvent('x'); e.initCustomEvent('y', true, true, 5); return [e.type, e.bubbles, e.cancelable, e.detail].join()",
        "value: y,true,true,5",
    ),
    "initCustomEvent's defaults": (
        "const e = new CustomEvent('x', {detail: 1}); e.initCustomEvent('z'); return [e.type, e.bubbles, e.cancelable, e.detail].join()",
        "value: z,false,false,",
    ),
    "initCustomEvent while it dispatches": (
        "const t = new EventTarget(); const e = new CustomEvent('x', {detail: 1}); t.addEventListener('x', () => e.initCustomEvent('y', true, true, 2)); t.dispatchEvent(e); return [e.type, e.detail].join()",
        "value: x,1",
    ),
    "initCustomEvent without arguments": (
        "return new CustomEvent('x').initCustomEvent()",
        "threw: TypeError: Failed to execute 'initCustomEvent' on 'CustomEvent': 1 argument required, but only 0 present.",
    ),
    "CustomEvent's own properties": (
        "return [Object.getOwnPropertyNames(CustomEvent.prototype).join(), CustomEvent.length, CustomEvent.prototype.initCustomEvent.length].join('|')",
        "value: detail,initCustomEvent,constructor|1|1",
    ),
}

# Reflected attributes that [ReflectOnly] limits to their keywords, and autocapitalize, which Blink writes by hand.
_REFLECTION_AS_CHROME = {
    "fetchPriority of a link": (
        "const l = document.createElement('link'); const r = [l.fetchPriority]; l.fetchPriority = 'HIGH'; r.push(l.fetchPriority, l.getAttribute('fetchpriority')); l.setAttribute('fetchpriority', 'bogus'); r.push(l.fetchPriority); l.setAttribute('fetchpriority', ''); r.push(l.fetchPriority); return r.join()",
        "value: auto,high,HIGH,auto,auto",
    ),
    "fetchPriority of a script": (
        "const s = document.createElement('script'); const r = [s.fetchPriority]; s.setAttribute('fetchpriority', 'Low'); r.push(s.fetchPriority); return r.join()",
        "value: auto,low",
    ),
    "crossOrigin of a link": (
        "const l = document.createElement('link'); const r = [l.crossOrigin]; l.crossOrigin = ''; r.push(JSON.stringify(l.crossOrigin), l.getAttribute('crossorigin') === ''); l.crossOrigin = 'USE-CREDENTIALS'; r.push(l.crossOrigin); l.crossOrigin = 'x'; r.push(l.crossOrigin); l.crossOrigin = null; r.push(l.crossOrigin, l.hasAttribute('crossorigin')); return r.join()",
        'value: ,"anonymous",true,use-credentials,anonymous,,false',
    ),
    "referrerPolicy of a script": (
        "const s = document.createElement('script'); const r = [JSON.stringify(s.referrerPolicy)]; s.referrerPolicy = 'ORIGIN'; r.push(s.referrerPolicy); s.referrerPolicy = 'x'; r.push(JSON.stringify(s.referrerPolicy)); s.referrerPolicy = null; r.push(JSON.stringify(s.referrerPolicy), s.getAttribute('referrerpolicy')); return r.join()",
        'value: "",origin,"","",',
    ),
    "as of a link": (
        "const l = document.createElement('link'); const r = [JSON.stringify(l.as)]; l.as = 'STYLE'; r.push(l.as); l.as = 'x'; r.push(JSON.stringify(l.as)); return r.join()",
        'value: "",style,""',
    ),
    "popover": (
        "const d = document.createElement('div'); const r = [d.popover]; d.popover = ''; r.push(d.popover); d.popover = 'HINT'; r.push(d.popover); d.popover = 'x'; r.push(d.popover); d.popover = null; r.push(d.popover, d.hasAttribute('popover')); return r.join()",
        "value: ,auto,hint,manual,,false",
    ),
    "enterKeyHint and inputMode": (
        "const d = document.createElement('div'); const r = [JSON.stringify(d.enterKeyHint), JSON.stringify(d.inputMode)]; d.enterKeyHint = 'GO'; d.inputMode = 'x'; r.push(d.enterKeyHint, JSON.stringify(d.inputMode), d.getAttribute('inputmode')); return r.join()",
        'value: "","",go,"",x',
    ),
    "virtualKeyboardPolicy": (
        "const d = document.createElement('div'); d.setAttribute('virtualkeyboardpolicy', 'MANUAL'); return d.virtualKeyboardPolicy",
        "value: manual",
    ),
    "autocapitalize": (
        "const d = document.createElement('div'); const r = [JSON.stringify(d.autocapitalize)]; for (const v of ['off', 'NONE', 'on', 'WORDS', 'characters', 'sentences', 'x', '']) { d.autocapitalize = v; r.push(JSON.stringify(d.autocapitalize)); } r.push(d.getAttribute('autocapitalize') === ''); d.setAttribute('autocapitalize', 'Off'); r.push(d.autocapitalize, d.getAttribute('autocapitalize')); return r.join()",
        'value: "","none","none","sentences","words","characters","sentences","sentences","",true,none,Off',
    ),
}

# Notification's permission on a secure page. What requestPermission() does there is not Chrome's headless answer, which
# denies with no prompt to show: test_request_permission_waits_at_the_prompt_on_a_secure_page holds it.
_NOTIFICATION_AS_CHROME = {
    "Notification.permission": ("return Notification.permission", "value: default"),
    "requestPermission with a number": (
        "return Notification.requestPermission(5)",
        "threw: TypeError: Failed to execute 'requestPermission' on 'Notification': parameter 1 is not of type 'Function'.",
    ),
    "requestPermission with null": (
        "return Promise.race([Notification.requestPermission(null), new Promise(r => setTimeout(() => r('pending'), 300))])",
        "value: pending",
    ),
    "requestPermission on another receiver": (
        "return Promise.race([Notification.requestPermission.call({}), new Promise(r => setTimeout(() => r('pending'), 300))])",
        "value: pending",
    ),
}


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _evaluate(program: Path, script: str) -> str:
    # The outcome of the script as a string, or what it threw, as tests/test_page_known_gaps.py writes one.
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


@pytest.mark.parametrize(("script", "expected"), list(_CRYPTO_AS_CHROME.values()), ids=list(_CRYPTO_AS_CHROME))
def test_crypto_does_what_chrome_does(program: Path, script: str, expected: str) -> None:
    assert _evaluate(program, script) == expected


def test_random_values_differ_from_call_to_call(program: Path) -> None:
    script = (
        "const seen = new Set(); for (let i = 0; i < 64; i++) { "
        "seen.add(crypto.getRandomValues(new Uint8Array(16)).join()); } return seen.size"
    )

    assert _evaluate(program, script) == "value: 64"


@pytest.mark.parametrize(("script", "expected"), list(_BASE64_AS_CHROME.values()), ids=list(_BASE64_AS_CHROME))
def test_base64_does_what_chrome_does(program: Path, script: str, expected: str) -> None:
    assert _evaluate(program, script) == expected


@pytest.mark.parametrize(("script", "expected"), list(_EVENTS_AS_CHROME.values()), ids=list(_EVENTS_AS_CHROME))
def test_events_take_their_init_dictionaries_as_chrome_does(program: Path, script: str, expected: str) -> None:
    assert _evaluate(program, script) == expected


@pytest.mark.parametrize(("script", "expected"), list(_REFLECTION_AS_CHROME.values()), ids=list(_REFLECTION_AS_CHROME))
def test_keyword_attributes_reflect_as_chrome_does(program: Path, script: str, expected: str) -> None:
    assert _evaluate(program, script) == expected


@pytest.mark.parametrize(
    ("script", "expected"), list(_NOTIFICATION_AS_CHROME.values()), ids=list(_NOTIFICATION_AS_CHROME)
)
def test_notification_does_what_chrome_does(program: Path, script: str, expected: str) -> None:
    assert _evaluate(program, script) == expected


def test_request_permission_waits_at_the_prompt_on_a_secure_page(program: Path) -> None:
    # Chrome shows its prompt for a profile that has granted nothing, which waits for a person, and the deprecated
    # callback gets nothing meanwhile (notification_permission_context.cc).
    script = """let called = "not called";
        const answer = await Promise.race([
            Notification.requestPermission((permission) => { called = permission; }),
            new Promise((resolve) => setTimeout(() => resolve("pending"), 300)),
        ]);
        return [answer, called].join()"""

    assert _evaluate(program, script) == "value: pending,not called"
