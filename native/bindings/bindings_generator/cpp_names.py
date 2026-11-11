"""How the generator names C++ files, classes, functions and members after Web IDL names."""

# Upstream: third_party/blink/renderer/bindings/scripts/bind_gen/name_style.py
# Upstream: third_party/blink/renderer/build/scripts/blinkbuild/name_style_converter.py

import re

__all__ = ["file_name", "function_name", "member_name"]


# Blink's tokens that its patterns alone would split, longest first, as name_style_converter.py lists them.
_SPECIAL_TOKENS = (
    "WebCodecs", "WebSocket", "String16", "Float32", "Float64", "Base64", "IFrame", "Latin1", "MathML", "PlugIn",
    "SQLite", "Uint16", "Uint32", "WebGL2", "webgl2", "WebGPU", "ASCII", "CSSOM", "CType", "DList", "Int16", "Int32",
    "MPath", "OList", "TSpan", "UList", "UTF16", "Uint8", "WebGL", "XPath", "ETC1", "etc1", "HTML", "Int8", "S3TC",
    "s3tc", "SPv2", "UTF8", "sRGB", "URLs", "API", "CSS", "DNS", "DOM", "EXT", "RTC", "SVG", "XSS", "2D", "AX", "FE",
    "JS", "V0", "V8", "v8", "XR",
)  # fmt: skip
_TOKEN_PATTERNS = ("[A-Z]?[a-z]+", "[A-Z]+(?![a-z])", "[0-9][Dd](?![a-z])", "[0-9]+")
_TOKEN = re.compile("(" + "|".join(_SPECIAL_TOKENS + _TOKEN_PATTERNS) + ")")
_LEADING_TOKEN_WITH_NUMBERS = re.compile(
    "^(" + "|".join(token for token in _SPECIAL_TOKENS if re.search("[0-9]", token)) + ")", re.IGNORECASE
)


def _words(name: str) -> list[str]:
    """Splits a Web IDL name into words as Blink's tokenize_name() does: "DOMException" into DOM and Exception,
    "hasUAVisualTransition" into has, UA, Visual and Transition, "HTMLOListElement" into HTML, OList and Element."""
    words: list[str] = []
    leading = _LEADING_TOKEN_WITH_NUMBERS.match(name)
    if leading:
        words.append(leading.group(0))
        name = name[leading.end() :]
    return words + _TOKEN.findall(name)


def file_name(name: str) -> str:
    """The base name of a file for a definition, such as "pop_state_event" for PopStateEvent."""
    return "_".join(word.lower() for word in _words(name))


def function_name(name: str) -> str:
    """The C++ name of a method for an operation or attribute, such as "InitEvent" or "HasUAVisualTransition"."""
    return "".join(word[0].upper() + word[1:] for word in _words(name))


def member_name(name: str) -> str:
    """The C++ name of a dictionary member, such as "has_ua_visual_transition"."""
    return "_".join(word.lower() for word in _words(name))
