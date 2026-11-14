"""Copy the web-platform-tests that the Runtime is held to, and the server's part of their tools, from the Engine
Release's checkout into the repository, and list their tests."""

import json
import re
import shutil
import subprocess
import sys
from collections.abc import Iterable, Iterator
from pathlib import Path

from tools.wpt.needs import needs_of
from tools.wpt.parts import DIRECTORIES
from tools.wpt.tests import ListedTest, dumps_tests

__all__ = ["WPT_IN_CHECKOUT", "copy_tests", "write_tests"]

WPT_IN_CHECKOUT = Path("third_party/blink/web_tests/external/wpt")
"""Where the checkout's `src` holds web-platform-tests, as Chromium imported them."""

_TOOLS_IN_CHECKOUT = Path("third_party/wpt_tools/wpt/tools")

_WHOLE = ("resources/", "common/", "interfaces/", *DIRECTORIES)
"""What the copy takes whole: the chosen directories, what nearly every test includes, and the Web IDL files that
idlharness.js fetches by the name of their spec."""

_ROOT_FILES = ("LICENSE.md",)

_TOOLS = (
    "__init__.py",
    "localpaths.py",
    "certs",
    "gitignore",
    "manifest",
    "metadata",
    "serve",
    "wptserve",
    "third_party/atomicwrites",
    "third_party/h2",
    "third_party/hpack",
    "third_party/html5lib",
    "third_party/hyperframe",
    "third_party/pywebsocket3",
    "third_party/six",
    "third_party/webencodings",
    "wpt/__init__.py",
    "wpt/utils.py",
)
"""What `wpt serve` and the manifest need of the tools, about 2.5 MB, and what some handlers import."""

_CHROMIUM_FILES = {"OWNERS", "DIR_METADATA", "META.yml", "WEB_FEATURES.yml", "PRESUBMIT.py"}
"""Files that no test reads: Chromium's own, and upstream's metadata."""

_BASELINE = re.compile(r"-expected\.(?:txt|png|wav)$")
"""Chromium's baselines, which sit beside the tests in its tree."""

_TEXT = (".html", ".htm", ".js", ".mjs", ".xhtml", ".xht", ".svg", ".py", ".headers", ".json", ".css", ".xml", ".asis")
_ABSOLUTE = re.compile(r"""["'(=\s,`}]/((?:[A-Za-z0-9_\-.]+/)*[A-Za-z0-9_\-.]+/?)""")
"""A path from the root, after a quote, a bracket, a space or the end of a template's `${self.origin}`."""
_RELATIVE = re.compile(r"""["'(=`](\.\.?/(?:[A-Za-z0-9_\-.]+/)*[A-Za-z0-9_\-.]*)""")
_QUOTED_NAME = re.compile(r"""["']([A-Za-z0-9_\-.]+\.[A-Za-z0-9]{1,5})["']""")
_MOST_FILES_OF_A_NAME = 3
"""A name that more files of the tree have, such as `blank.html`, names none of them in particular."""
_PYTHON_IMPORT = re.compile(
    r"""import_module\(\s*["']([A-Za-z0-9_.\-]+)["']|^from\s+((?:common|fetch|html|resources|service-workers"""
    r"""|IndexedDB|workers|cookies|images|media|webmessaging|xhr|dom|encoding)[A-Za-z0-9_.\-]*)\s+import""",
    re.MULTILINE,
)

_MANIFEST_SCRIPT = """
import json, sys
tools, root = sys.argv[1], sys.argv[2]
sys.path.insert(0, tools)
import localpaths
from manifest import manifest
built = manifest.load_and_update(root, root + "/MANIFEST.json", "/", rebuild=True, write_manifest=False, parallel=False)
tests = []
for _, path, items in built.itertypes("testharness"):
    for item in items:
        scripts = [value for key, value in (item.script_metadata or []) if key == "script"]
        tests.append([path, item.url, item.timeout or "normal", bool(item.testdriver), scripts])
json.dump(tests, sys.stdout)
"""
"""Lists the testharness.js tests of the copy with the copy's own manifest tool, which a run of the Engine Release has."""

_NOT_RUN_VARIANTS = (".sharedworker", ".serviceworker", "shadowrealm")
"""Variants of a `.any.js` test for scopes that M8 leaves out, which #260 chose not to run."""


def copy_tests(checkout: Path, destination: Path) -> list[ListedTest]:
    """Copy web-platform-tests from a checkout's `src`, and return their tests.

    `destination` becomes a small root of web-platform-tests: the chosen directories whole, `resources` and `common`
    whole, every file outside them that their files name, and the tools that serve them. What was there goes.

    Raises:
        FileNotFoundError: The checkout has no web-platform-tests or no tools.
        subprocess.CalledProcessError: The manifest tool failed.
    """
    source = checkout / WPT_IN_CHECKOUT
    tools = checkout / _TOOLS_IN_CHECKOUT
    for required in (source, tools):
        if not required.is_dir():
            raise FileNotFoundError(f"{required} is not a directory")
    files = _closure(source, set(_files_under(source, _WHOLE, ())) | set(_ROOT_FILES))
    if destination.exists():
        shutil.rmtree(destination)
    for name in sorted(files):
        _copy(source / name, destination / name)
    for name in _TOOLS:
        for path in _files_under(tools, (name,), ("tests",)):
            _copy(tools / path, destination / "tools" / path)
    return _list_tests(destination)


def _files_under(root: Path, names: Iterable[str], skipped: tuple[str, ...]) -> Iterator[str]:
    """Yield every file under the named files and directories of root, as paths from root, but Chromium's own files and
    those in a directory with a skipped name."""
    for name in names:
        top = root / name
        paths = [top] if top.is_file() else (path for path in top.rglob("*") if path.is_file())
        for path in paths:
            relative = path.relative_to(root)
            if _wanted(relative) and not any(part in skipped for part in relative.parts[:-1]):
                yield relative.as_posix()


def _wanted(relative: Path) -> bool:
    if relative.name in _CHROMIUM_FILES or _BASELINE.search(relative.name):
        return False
    return not any(part == "__pycache__" or part.startswith(".") for part in relative.parts[:-1])


def _closure(root: Path, files: set[str]) -> set[str]:
    """Add every file that the text files name, by an absolute or relative path or a Python import, until none is new,
    with the `.headers` files that wptserve sends with them.

    A handler often joins a file's name to a directory, as `os.path.join(..., "images", "green-96x96.svg")` does, so a
    name with an extension that a Python file quotes counts too, as the few files of the tree that have that name.
    """
    have = set(files)
    by_name: dict[str, list[str]] = {}
    for path in root.rglob("*"):
        if path.is_file():
            by_name.setdefault(path.name, []).append(path.relative_to(root).as_posix())
    todo = [name for name in have if name.endswith(_TEXT)]
    while todo:
        name = todo.pop()
        references = set(_references(root, name))
        if name.endswith(".py"):
            for quoted in _QUOTED_NAME.findall((root / name).read_text(encoding="utf-8", errors="replace")):
                candidates = by_name.get(quoted, [])
                if len(candidates) <= _MOST_FILES_OF_A_NAME:
                    references.update(candidates)
        for reference in references:
            candidates = [reference, f"{reference}.headers", str(Path(reference).parent / "__dir__.headers")]
            if not Path(reference).suffix and (root / reference).parent.is_dir():
                # A media file that a test names without its extension, which it picks by what the browser plays.
                stem = Path(reference).name
                candidates += [
                    path.relative_to(root).as_posix() for path in (root / reference).parent.glob(f"{stem}.*")
                ]
            for candidate in candidates:
                if candidate in have or not (root / candidate).is_file() or not _wanted(Path(candidate)):
                    continue
                have.add(candidate)
                if candidate.endswith(_TEXT):
                    todo.append(candidate)
    return have


def _references(root: Path, name: str) -> Iterator[str]:
    try:
        text = (root / name).read_text(encoding="utf-8", errors="replace")
    except OSError:
        return
    found = set(_ABSOLUTE.findall(text))
    found.update(str(Path(name).parent / relative) for relative in _RELATIVE.findall(text))
    for module, imported in _PYTHON_IMPORT.findall(text):
        path = (module or imported).replace(".", "/")
        found.update((f"{path}.py", path))
    for reference in found:
        cleaned = _normalized(reference.split("?")[0].split("#")[0].rstrip("/"))
        if cleaned:
            yield cleaned


def _normalized(path: str) -> str | None:
    parts: list[str] = []
    for part in path.split("/"):
        if part == "..":
            if not parts:
                return None
            parts.pop()
        elif part not in ("", "."):
            parts.append(part)
    return "/".join(parts) or None


def _copy(source: Path, target: Path) -> None:
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)


def _list_tests(root: Path) -> list[ListedTest]:
    output = subprocess.run(
        [sys.executable, "-I", "-c", _MANIFEST_SCRIPT, str(root / "tools"), str(root)],
        check=True,
        capture_output=True,
        text=True,
        encoding="utf-8",
    ).stdout
    tests: list[ListedTest] = []
    for source, url, timeout, testdriver, scripts in json.loads(output):
        test = url.removeprefix("/")
        if not test.startswith(DIRECTORIES) or any(variant in test for variant in _NOT_RUN_VARIANTS):
            continue
        tests.append(ListedTest(test, timeout == "long", needs_of(root, source, scripts, testdriver)))
    return sorted(tests, key=lambda entry: entry.test)


def write_tests(path: Path, tests: Iterable[ListedTest]) -> None:
    """Write the list of tests that copy_tests() returned."""
    path.write_text(dumps_tests(tests), encoding="utf-8")
