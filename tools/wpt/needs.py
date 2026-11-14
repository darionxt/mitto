"""What a test needs that a Page cannot give in M8, found by reading the test and the scripts it includes.

This is darionxt/mitto#260's scan. It reads text, so it finds a need a little too often: a test that only mentions
`getComputedStyle()` in a branch it never takes counts as needing layout. It only labels the differences that a test
has, and decides nothing about whether the test runs, except for testdriver.js, which the manifest names.
"""

import re
from collections.abc import Iterable
from pathlib import Path

__all__ = ["TESTDRIVER", "needs_of"]

TESTDRIVER = "testdriver"
"""What a test needs when it drives the browser through testdriver.js: input and other automation, which no recording
of Chrome here gives either. Such a test does not run."""

_SCANNED = {
    "layout": re.compile(
        r"getBoundingClientRect|getClientRects|\.offset(?:Width|Height|Top|Left|Parent)\b|"
        r"\.client(?:Width|Height|Top|Left)\b|\.scroll(?:Width|Height|Top|Left)\b|scrollIntoView|"
        r"\.scrollTo\(|\.scrollBy\(|elementsFromPoint|elementFromPoint|getComputedStyle|computedStyleMap|"
        r"IntersectionObserver|ResizeObserver|caretPositionFromPoint|checkVisibility"
    ),
    "second window": re.compile(
        r"\bwindow\.open\(|\bself\.open\(|\bshowModalDialog\b|target\s*=\s*['\"]?_blank|\.target\s*=\s*['\"]_blank"
    ),
    "service worker": re.compile(r"serviceWorker\.register|navigator\.serviceWorker|service_worker_unregister"),
    "shared worker": re.compile(r"\bSharedWorker\b"),
    "websocket": re.compile(r"\bnew WebSocket\b|\bWebSocketStream\b|\bWebTransport\b"),
    "graphics": re.compile(r"getContext\(|OffscreenCanvas|createImageBitmap|WebGL"),
    "media": re.compile(
        r"<video|<audio|createElement\(\s*['\"](?:video|audio)|\.play\(\)|MediaSource\b|AudioContext|"
        r"getUserMedia|RTCPeerConnection|MediaRecorder"
    ),
    "webcrypto": re.compile(r"crypto\.subtle"),
}

_SCRIPT_SRC = re.compile(r"<script[^>]*\bsrc\s*=\s*['\"]?([^'\" >]+)", re.IGNORECASE)


def needs_of(root: Path, source: str, scripts: Iterable[str], testdriver: bool) -> list[str]:
    """What a test needs that a Page cannot give in M8, in a fixed order.

    Args:
        root: The root of web-platform-tests.
        source: The test's file, from the root, such as "dom/nodes/Node-textContent.html".
        scripts: The scripts that its `// META: script=` lines include.
        testdriver: Whether the manifest says that it uses testdriver.js.
    """
    text = _read(root / source)
    parts = [text]
    for src in (*scripts, *_SCRIPT_SRC.findall(text)):
        local = _local_script(src, source)
        if local is not None:
            parts.append(_read(root / local))
    whole = "\n".join(parts)
    found = [TESTDRIVER] if testdriver else []
    return found + [need for need, pattern in _SCANNED.items() if pattern.search(whole)]


def _local_script(src: str, source: str) -> str | None:
    src = src.replace("{{host}}", "").replace("{{domains[www]}}", "").split("?")[0].split("#")[0]
    if not src.endswith(".js") or src.startswith(("http:", "https:", "//", "data:", "blob:")):
        return None
    local = src.lstrip("/") if src.startswith("/") else str(Path(source).parent / src)
    if local.startswith(("resources/testharness", "resources/testdriver")):
        return None
    parts: list[str] = []
    for part in Path(local).parts:
        if part == "..":
            if not parts:
                return None
            parts.pop()
        elif part != ".":
            parts.append(part)
    return "/".join(parts)


def _read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""
