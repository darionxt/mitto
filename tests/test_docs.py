"""Tests that README.md, docs/api.md and docs/from-requests.md say what the code does, and CHANGELOG.md what is real.

The checks of names, signatures and links need nothing. Running the examples needs a native library, and every name in a URL
lands on a local server, so that a reader sees no port numbers. Nothing runs the `requests` column of the guide, so this
repository does not depend on `requests`.
"""

import base64
import dataclasses
import enum
import http.server
import inspect
import json
import re
import ssl
import sys
import threading
import tomllib
import types
from collections import Counter
from collections.abc import Callable, Iterator
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast
from urllib.parse import parse_qsl, urlsplit

import pytest

import mitto
from mitto import _api
from mitto._native import PrivateSettings
from mitto._persona import ENGINE_RELEASE_PERSONA, WINDOWS_PERSONAS
from mitto._request import RequestOptions
from tests.local_certificates import Issued, TlsServer, generate_leaf, generate_root, serving
from tests.local_proxy import LocalProxy
from tools.capture import HTTP2, CaptureServer

REPOSITORY = Path(__file__).parent.parent
API_PAGE = REPOSITORY / "docs" / "api.md"
GUIDE = REPOSITORY / "docs" / "from-requests.md"
README = REPOSITORY / "README.md"
CHANGELOG = REPOSITORY / "CHANGELOG.md"
PAGES = [API_PAGE, GUIDE, README]
# The changelog has no examples, and the checks of names and links read it as they read the pages.
PAGES_AND_CHANGELOG = [*PAGES, CHANGELOG]

PROJECT = tomllib.loads((REPOSITORY / "pyproject.toml").read_text(encoding="utf-8"))["project"]

# What each page may have blocks of: examples, signatures that the checks read, and the README's install command.
_BLOCK_LANGUAGES: dict[Path, set[str]] = {
    API_PAGE: {"python", "text"},
    GUIDE: {"python"},
    README: {"python", "bash"},
    CHANGELOG: set(),
}

_FENCE = "```"
_HEADING = re.compile(r"(#{1,6}) (.+)")
_CODE_SPAN = re.compile(r"`([^`]+)`")
_LINK = re.compile(r"\[[^\]]*\]\(([^)\s]+)\)")
_CELL_BORDER = re.compile(r"(?<!\\)\|")
_MEMBER_SPAN = re.compile(r"(?:mitto\.)?([A-Z]\w+)\.(\w+)(?:\(\))?")
_MODULE_PATH = re.compile(r"\b(?:mitto\.\w+|collections\.abc|contextlib|datetime|typing)\.")
# A signature that takes several lines has them indented, up to a line that closes its parenthesis.
_NEXT_SIGNATURE = re.compile(r"\n(?=[^\s)])")
# A link to a file or a directory of this repository on its main branch, which works on PyPI too.
_ON_GITHUB = re.compile(
    rf"{re.escape(PROJECT['urls']['Source'])}/(?:blob|tree)/main/(?P<path>[^#]+)(?:#(?P<anchor>.+))?"
)
# What names a Chrome Release, in text or as part of the name of a Persona.
_NAMES_A_CHROME_RELEASE = re.compile(r"\bChrome \d+|\bchrome_\d+_")
# The one line of the README that names Chrome Releases, so that the release routine changes only that one.
_SUPPORTED_WINDOW = re.compile(
    r"This version presents Chrome (?P<older>(?:\d+, )*\d+) and (?P<newest>\d+), "
    r"with Chrome (?P<engine>\d+) as its Engine Release\."
)

# The classes that a caller makes, so that their constructors are part of the API.
_MADE_BY_A_CALLER = (
    mitto.Session,
    mitto.AsyncSession,
    mitto.Machine,
    mitto.Timeout,
    mitto.Cookie,
    mitto.CookiePartitionKey,
)
# Session sets this one in its constructor, so its class does not show it.
_SESSION_ATTRIBUTES = {"Session.cookies": "Session.cookies -> Cookies"}
_ERROR_TREE_ROOT = "MittoError"

# Longer than any local request takes, even on a busy machine.
TIMEOUT_SECONDS = 30.0
LARGE_BODY_SIZE = 1024 * 1024

# The file that the examples upload, and how much of it `Content-Length` says there is.
REPORT = b"a,b\n1,2\n"

# What the URL of the proxy in the examples says.
PROXY_CREDENTIALS = "user:password"

# The names that the local servers' certificate covers, beyond those with a server of their own. The README's examples go
# to example.com, so that a reader who runs them reaches a real server.
EXAMPLE_NAMES = ("example.test", "*.example.test", "other.test", "*.other.test", "example.com")


@dataclass(frozen=True, slots=True)
class Heading:
    level: int
    text: str
    line: int

    @property
    def spans(self) -> list[str]:
        return _CODE_SPAN.findall(self.text)


@dataclass(frozen=True, slots=True)
class Block:
    """A fenced block, and the number of the line that holds the first line of its code."""

    language: str
    code: str
    line: int


@dataclass(frozen=True, slots=True)
class Page:
    """What the checks read in one page of Markdown.

    Attributes:
        blocks: The fenced blocks.
        prose: Every other line, with its number.
        headings: The headings among those.
    """

    blocks: tuple[Block, ...]
    prose: tuple[tuple[int, str], ...]
    headings: tuple[Heading, ...]


def parse_page(text: str) -> Page:
    blocks: list[Block] = []
    prose: list[tuple[int, str]] = []
    headings: list[Heading] = []
    open_block: Block | None = None
    for number, line in enumerate(text.splitlines(), start=1):
        if open_block is not None:
            if line == _FENCE:
                blocks.append(open_block)
                open_block = None
            else:
                open_block = dataclasses.replace(open_block, code=f"{open_block.code}{line}\n")
        elif line.startswith(_FENCE):
            open_block = Block(line.removeprefix(_FENCE), "", number + 1)
        else:
            prose.append((number, line))
            if match := _HEADING.fullmatch(line):
                headings.append(Heading(len(match[1]), match[2], number))
    return Page(tuple(blocks), tuple(prose), tuple(headings))


def _read_page(path: Path) -> Page:
    return parse_page(path.read_text(encoding="utf-8"))


# What the page has to name.


def _public_members(item: type) -> dict[str, Any]:
    """Return the methods and properties of a class, leaving out what it inherits from outside mitto, such as Exception.

    A class method, such as Machine.draw(), is as the class gives it, bound to the class.
    """
    members: dict[str, Any] = {}
    for base in reversed(item.__mro__):
        if base.__module__.startswith("mitto."):
            members |= {
                name: getattr(item, name) if isinstance(member, classmethod) else member
                for name, member in vars(base).items()
                if not name.startswith("_") and (inspect.isroutine(member) or isinstance(member, property))
            }
    return members


def _public_classes() -> dict[str, type]:
    return {name: item for name in mitto.__all__ if inspect.isclass(item := getattr(mitto, name))}


def _session_arguments() -> list[str]:
    return [name for name in inspect.signature(mitto.Session).parameters if not name.startswith("_")]


def expected_names() -> set[str]:
    """Return what the headings of the API page have to name, each once and nothing else, as named_by_headings() writes it."""
    names = {f"{name}()" for name in mitto.__all__ if not inspect.isclass(getattr(mitto, name))}
    names |= set(_SESSION_ATTRIBUTES)
    for name, item in _public_classes().items():
        names.add(name)
        names |= {
            f"{name}.{member_name}" if isinstance(member, property) else f"{name}.{member_name}()"
            for member_name, member in _public_members(item).items()
        }
    names |= {f"Session({argument}=)" for argument in _session_arguments()}
    names |= {
        f"request({argument}=)" for argument in RequestOptions.__optional_keys__ | RequestOptions.__required_keys__
    }
    return names


def named_by_headings(page: Page) -> list[str]:
    """Return what the headings below the second level name, which are the ones that do, by their code spans.

    `Session.get()` stands for itself. An argument like `timeout=` belongs to the class of the heading it is under, and it
    stands for a request's where there is none: Session(timeout=), or else request(timeout=). A heading with no span is
    returned as it stands, so that it counts as naming nothing real.
    """
    names: list[str] = []
    owner: Heading | None = None
    for heading in page.headings:
        if owner is not None and heading.level <= owner.level:
            owner = None
        if heading.level <= 2:
            continue
        if not heading.spans:
            names.append(heading.text)
        for span in heading.spans:
            if span.endswith("="):
                names.append(f"{owner.spans[0] if owner else 'request'}({span})")
            else:
                names.append(span)
                owner = owner or heading
    return names


def _section_of(page: Page, span: str) -> str:
    """Return the prose under the heading that holds a code span, down to the next heading at its level or above."""
    heading = next(heading for heading in page.headings if span in heading.spans)
    end = next(
        (later.line for later in page.headings if later.line > heading.line and later.level <= heading.level),
        sys.maxsize,
    )
    return "\n".join(text for number, text in page.prose if heading.line < number < end).replace("\\|", "|")


# What the signatures have to say.


def _without_module_paths(text: str) -> str:
    return _MODULE_PATH.sub("", text)


def _type_text(annotation: Any) -> str:
    return annotation.__name__ if isinstance(annotation, type) else _without_module_paths(repr(annotation))


def _signature_of(name: str, function: Callable[..., Any]) -> str:
    signature = inspect.signature(function)
    signature = signature.replace(
        parameters=[
            parameter
            for parameter in signature.parameters.values()
            if parameter.name != "self" and not parameter.name.startswith("_")
        ]
    )
    if inspect.isclass(function):
        signature = signature.replace(return_annotation=inspect.Signature.empty)
    prefix = "async " if inspect.iscoroutinefunction(function) else ""
    return f"{prefix}{name}{_without_module_paths(str(signature))}"


def expected_signatures() -> set[str]:
    """Return each signature that the API page has to hold, as a reader sees it, without `self` or private arguments."""
    signatures = set(_SESSION_ATTRIBUTES.values())
    signatures |= {
        _signature_of(name, item) for name in mitto.__all__ if not inspect.isclass(item := getattr(mitto, name))
    }
    signatures |= {_signature_of(item.__name__, item) for item in _MADE_BY_A_CALLER}
    for name, item in _public_classes().items():
        for member_name, member in _public_members(item).items():
            if isinstance(member, property):
                annotation = inspect.signature(cast(Callable[..., Any], member.fget)).return_annotation
                signatures.add(f"{name}.{member_name} -> {_type_text(annotation)}")
            else:
                signatures.add(_signature_of(f"{name}.{member_name}", member))
    return signatures


def _squeezed(signature: str) -> str:
    """Return a signature without its layout, so that the page can spread a long one over several lines."""
    return re.sub(r"\s+", "", signature).replace(",)", ")")


def signatures_on(page: Page) -> set[str]:
    """Return each signature in the text blocks of a page, without its layout. The tree of errors holds none."""
    return {
        _squeezed(signature)
        for block in page.blocks
        if block.language == "text" and not block.code.startswith(_ERROR_TREE_ROOT)
        for signature in _NEXT_SIGNATURE.split(block.code.strip())
    }


def known_members(name: str) -> set[str]:
    """Return every attribute of a public class that the text of a page can name."""
    item = _public_classes()[name]
    members = set(_public_members(item))
    if dataclasses.is_dataclass(item):
        members |= {field.name for field in dataclasses.fields(item)}
    if issubclass(item, enum.Enum):
        members |= set(item.__members__)
    if item in _ERROR_ATTRIBUTES:
        members |= set(_ERROR_ATTRIBUTES[item])
    members |= {key.removeprefix(f"{name}.") for key in _SESSION_ATTRIBUTES if key.startswith(f"{name}.")}
    return members


def _example_response() -> mitto.Response:
    return mitto.Response(status=404, url="", protocol="", headers=mitto.Headers(), content=b"")


# Errors set these in their constructors, so their classes do not show them.
_ERROR_ATTRIBUTES: dict[type, list[str]] = {
    mitto.RequestError: list(vars(mitto.RequestError("", 0, "", ""))),
    mitto.HTTPStatusError: list(vars(mitto.HTTPStatusError(_example_response()))),
}


def _error_tree_edges(tree: str) -> set[tuple[str, str]]:
    """Return each (parent, child) of a tree drawn by indentation."""
    edges: set[tuple[str, str]] = set()
    path: list[tuple[int, str]] = []
    for line in tree.splitlines():
        indent = len(line) - len(line.lstrip())
        while path and path[-1][0] >= indent:
            path.pop()
        if path:
            edges.add((path[-1][1], line.strip()))
        path.append((indent, line.strip()))
    return edges


# The checks of the API page, none of which needs a native library.


def test_each_public_name_method_and_argument_has_a_heading() -> None:
    assert sorted(expected_names() - set(named_by_headings(_read_page(API_PAGE)))) == []


def test_each_heading_names_something_real() -> None:
    assert sorted(set(named_by_headings(_read_page(API_PAGE))) - expected_names()) == []


def test_nothing_has_two_headings() -> None:
    counts = Counter(named_by_headings(_read_page(API_PAGE)))
    assert [name for name, count in counts.items() if count > 1] == []


def test_a_name_with_no_heading_is_noticed() -> None:
    text = API_PAGE.read_text(encoding="utf-8").replace("### `Timeout`\n", "")

    assert expected_names() - set(named_by_headings(parse_page(text))) == {"Timeout"}


def test_a_method_or_an_argument_with_no_heading_is_noticed() -> None:
    text = API_PAGE.read_text(encoding="utf-8")
    text = text.replace("#### `Headers.get_all()`\n", "").replace("### `follow_redirects=`\n", "")
    text = text.replace("#### `cache=`\n", "")

    assert expected_names() - set(named_by_headings(parse_page(text))) == {
        "Headers.get_all()",
        "request(follow_redirects=)",
        "Session(cache=)",
    }


def test_a_heading_that_names_nothing_real_is_noticed() -> None:
    text = "### `Session`\n\n#### `nothing=`\n\n#### `Session.nothing()`\n\n### Not a name\n"

    assert set(named_by_headings(parse_page(text))) - expected_names() == {
        "Session(nothing=)",
        "Session.nothing()",
        "Not a name",
    }


def test_a_comment_in_an_example_is_no_heading() -> None:
    page = parse_page(f"{_FENCE}python\n# Not a heading\nx = 1\n{_FENCE}\n# A heading\n")

    assert [heading.text for heading in page.headings] == ["A heading"]
    assert page.blocks == (Block("python", "# Not a heading\nx = 1\n", 2),)


def test_each_signature_on_the_page_is_the_one_in_the_code() -> None:
    expected = {_squeezed(signature): signature for signature in expected_signatures()}
    on_page = signatures_on(_read_page(API_PAGE))

    assert [expected[signature] for signature in sorted(expected.keys() - on_page)] == []
    assert sorted(on_page - expected.keys()) == []


def test_a_signature_that_has_changed_is_noticed() -> None:
    text = API_PAGE.read_text(encoding="utf-8").replace("timeout: float | Timeout | None = None,\n", "", 1)

    assert _squeezed(_signature_of("Session", mitto.Session)) not in signatures_on(parse_page(text))


def test_each_field_of_a_class_has_a_row_with_its_type() -> None:
    page = _read_page(API_PAGE)
    for name, item in _public_classes().items():
        if not dataclasses.is_dataclass(item):
            continue
        section = _section_of(page, name)
        for field in dataclasses.fields(item):
            if not field.name.startswith("_"):
                assert f"| `{field.name}` | `{_type_text(field.type)}` |" in section, f"{name}.{field.name}"


def test_each_attribute_of_an_error_has_a_row() -> None:
    page = _read_page(API_PAGE)
    for error_type, attributes in _ERROR_ATTRIBUTES.items():
        section = _section_of(page, error_type.__name__)
        for attribute in attributes:
            assert f"| `{attribute}` |" in section, f"{error_type.__name__}.{attribute}"


def test_each_request_kind_has_a_row() -> None:
    section = _section_of(_read_page(API_PAGE), "RequestKind")
    for kind in mitto.RequestKind:
        assert f'| `RequestKind.{kind.name}` | `"{kind.value}"` |' in section


def test_each_persona_is_named() -> None:
    section = _section_of(_read_page(API_PAGE), "persona=")
    for persona in WINDOWS_PERSONAS.values():
        assert f'`"{persona}"`' in section


def test_the_tree_of_errors_is_the_hierarchy() -> None:
    tree = next(block.code for block in _read_page(API_PAGE).blocks if block.code.startswith(_ERROR_TREE_ROOT))
    errors = [item for item in _public_classes().values() if issubclass(item, mitto.MittoError)]

    assert _error_tree_edges(tree) == {
        (item.__bases__[0].__name__, item.__name__) for item in errors if item is not mitto.MittoError
    }


def test_the_table_of_errors_says_what_to_do_about_each() -> None:
    text = API_PAGE.read_text(encoding="utf-8")
    for name, item in _public_classes().items():
        if issubclass(item, mitto.MittoError) and item is not mitto.MittoError:
            assert f"| [`{name}`](#{name.lower()}) |" in text


# The checks of every page, and of the changelog.


@pytest.mark.parametrize("path", PAGES_AND_CHANGELOG, ids=lambda path: path.stem)
def test_each_block_is_an_example_or_something_the_checks_read(path: Path) -> None:
    assert {block.language for block in _read_page(path).blocks} <= _BLOCK_LANGUAGES[path]


@pytest.mark.parametrize("path", PAGES_AND_CHANGELOG, ids=lambda path: path.stem)
def test_each_member_named_in_the_text_is_real(path: Path) -> None:
    for number, text in _read_page(path).prose:
        for span in _CODE_SPAN.findall(text):
            match = _MEMBER_SPAN.fullmatch(span)
            if match and match[1] in _public_classes():
                assert match[2] in known_members(match[1]), f"{path.name}:{number}: {span}"


def _anchors(path: Path) -> set[str]:
    """Return the anchors that GitHub gives the headings of a page."""
    seen: Counter[str] = Counter()
    anchors: set[str] = set()
    for heading in _read_page(path).headings:
        anchor = re.sub(r"[^\w\- ]", "", heading.text.lower()).replace(" ", "-")
        anchors.add(f"{anchor}-{seen[anchor]}" if seen[anchor] else anchor)
        seen[anchor] += 1
    return anchors


def link_target(page: Path, link: str) -> tuple[Path, str] | None:
    """Return the path in this repository that a link on a page leads to, and its anchor, or None if it leads elsewhere.

    A link leads into this repository if it is relative, or if it goes to this repository's main branch on GitHub.
    """
    if match := _ON_GITHUB.fullmatch(link):
        return REPOSITORY / match["path"], match["anchor"] or ""
    if "://" in link:
        return None
    target, _, anchor = link.partition("#")
    return (page.parent / target).resolve() if target else page, anchor


def test_a_link_into_this_repository_is_relative_or_on_its_main_branch() -> None:
    page = REPOSITORY / "docs" / "page.md"
    source = PROJECT["urls"]["Source"]

    assert link_target(page, "api.md#session") == (REPOSITORY / "docs" / "api.md", "session")
    assert link_target(page, "#errors") == (page, "errors")
    assert link_target(page, f"{source}/blob/main/docs/api.md#errors") == (REPOSITORY / "docs" / "api.md", "errors")
    assert link_target(page, f"{source}/tree/main/docs/adr") == (REPOSITORY / "docs" / "adr", "")
    assert link_target(page, f"{source}/issues") is None
    assert link_target(page, "https://example.com/") is None


@pytest.mark.parametrize("path", PAGES_AND_CHANGELOG, ids=lambda path: path.stem)
def test_each_link_leads_somewhere(path: Path) -> None:
    for number, text in _read_page(path).prose:
        for link in _LINK.findall(text):
            if (target := link_target(path, link)) is None:
                continue
            page, anchor = target
            assert page.exists(), f"{path.name}:{number}: {link}"
            if anchor:
                assert anchor in _anchors(page), f"{path.name}:{number}: {link}"


# The checks of the README, which PyPI shows too.


def test_each_link_of_the_readme_works_on_pypi() -> None:
    """PyPI shows the README on a page of its own, where a relative link leads nowhere."""
    for number, text in _read_page(README).prose:
        for link in _LINK.findall(text):
            assert link.startswith("https://"), f"{README.name}:{number}: {link}"


def test_pypi_shows_the_readme() -> None:
    assert PROJECT["readme"] == README.name


def test_each_url_of_the_project_into_this_repository_leads_somewhere() -> None:
    for label, url in PROJECT["urls"].items():
        if (target := link_target(README, url)) is not None:
            assert target[0].exists(), label


def test_the_readme_installs_this_project() -> None:
    commands = [block.code for block in _read_page(README).blocks if block.language == "bash"]

    assert commands == [f"pip install {PROJECT['name']}\n"]


def lines_naming_chrome_releases(text: str) -> list[str]:
    return [line for line in text.splitlines() if _NAMES_A_CHROME_RELEASE.search(line)]


def supported_window(line: str) -> tuple[list[str], str] | None:
    """Return the Chrome Releases that a line of the README presents, and its Engine Release, or None if it says neither."""
    if match := _SUPPORTED_WINDOW.fullmatch(line):
        return [*match["older"].split(", "), match["newest"]], match["engine"]
    return None


def test_the_readme_names_the_supported_window_on_one_line() -> None:
    lines = lines_naming_chrome_releases(README.read_text(encoding="utf-8"))
    engine_release = next(release for release, persona in WINDOWS_PERSONAS.items() if persona == ENGINE_RELEASE_PERSONA)

    assert len(lines) == 1, lines
    assert supported_window(lines[0]) == (list(WINDOWS_PERSONAS), engine_release)


def test_a_chrome_release_named_on_another_line_is_noticed() -> None:
    text = README.read_text(encoding="utf-8")
    text += '\n```python\nmitto.Session(persona="chrome_150_windows")\n```\nChrome 158 is next.\n'

    assert len(lines_naming_chrome_releases(text)) == 3


def test_a_supported_window_with_another_release_is_noticed() -> None:
    line = "This version presents Chrome 156, 157 and 158, with Chrome 158 as its Engine Release."

    assert supported_window(line) == (["156", "157", "158"], "158")
    assert supported_window("This version presents Chrome 157.") is None


# Running the examples.


@dataclass(frozen=True, slots=True)
class Example:
    """Code to run, and the number of the line of the page where it starts."""

    code: str
    line: int


def examples_on(page: Page) -> list[Example]:
    """Return what runs from a page, in the order it stands there: its Python blocks and the mitto columns of its tables."""
    examples = [Example(block.code, block.line) for block in page.blocks if block.language == "python"]
    column: int | None = None
    for number, line in page.prose:
        if not line.startswith("|"):
            column = None
            continue
        cells = [cell.strip() for cell in _CELL_BORDER.split(line)[1:-1]]
        if "mitto" in cells:
            column = cells.index("mitto")
        elif column is not None:
            examples += [Example(span.replace("\\|", "|"), number) for span in _CODE_SPAN.findall(cells[column])]
    return sorted(examples, key=lambda example: example.line)


def _lines_of(code: types.CodeType) -> set[int]:
    """Return the lines that a compiled example has something to run on, with the functions it defines."""
    lines = {line for _, _, line in code.co_lines() if line is not None}
    for constant in code.co_consts:
        if isinstance(constant, types.CodeType):
            lines |= _lines_of(constant)
    return lines


def run_examples(path: Path, examples: list[Example]) -> list[int]:
    """Run examples in order, sharing one namespace, and return the lines of the page that have code but never ran.

    An `except` that does not run means the example did not show what it says. That is why lines count, and not only whether
    an example ended well. An exception goes to the caller with its traceback on the lines of the page.
    """
    filename = str(path)
    reached: set[int] = set()

    def trace(frame: types.FrameType, event: str, arg: Any) -> Any:
        if frame.f_code.co_filename != filename:
            return None
        if event == "line":
            reached.add(frame.f_lineno)
        return trace

    namespace: dict[str, Any] = {"__name__": "__docs__"}
    has_code: set[int] = set()
    previous = sys.gettrace()
    previous_in_threads = threading.gettrace()
    threading.settrace(trace)
    try:
        for example in examples:
            code = compile("\n" * (example.line - 1) + example.code, filename, "exec")
            # What the compiler adds at the start of a module is on no line of the example.
            has_code |= {line for line in _lines_of(code) if line >= example.line}
            sys.settrace(trace)
            try:
                exec(code, namespace)
            finally:
                sys.settrace(previous)
    finally:
        threading.settrace(cast(Any, previous_in_threads))
    return sorted(has_code - reached)


def test_examples_are_blocks_and_the_mitto_column_of_a_table() -> None:
    page = parse_page(
        "| `requests` | mitto | About |\n"
        "| --- | --- | --- |\n"
        "| `a()` | `b()`, `c \\| d` | `e()` |\n"
        "\n"
        "| Other | mitto? |\n"
        "| --- | --- |\n"
        "| `f()` | `g()` |\n"
        "\n"
        f"{_FENCE}python\nh()\n{_FENCE}\n"
        f"{_FENCE}text\ni()\n{_FENCE}\n"
    )

    assert examples_on(page) == [Example("b()", 3), Example("c | d", 3), Example("h()\n", 10)]


def test_the_guide_runs_its_mitto_column() -> None:
    examples = examples_on(_read_page(GUIDE))

    assert any(example.code == "session.get(url, follow_redirects=False)" for example in examples)
    assert any(example.code == "mitto.RequestTimeoutError" for example in examples)


def test_an_example_that_fails_stops_the_run(tmp_path: Path) -> None:
    with pytest.raises(ZeroDivisionError):
        run_examples(tmp_path / "page.md", [Example("1 / 0\n", 1)])


def test_a_line_that_no_example_reaches_is_noticed(tmp_path: Path) -> None:
    code = "try:\n    x = 1\nexcept ValueError:\n    y = 2\nz = 3\n"

    assert run_examples(tmp_path / "page.md", [Example(code, 10)]) == [12, 13]


def test_lines_inside_a_function_count_as_reached_when_it_runs(tmp_path: Path) -> None:
    code = "def called():\n    return 1\n\ndef not_called():\n    return 2\n\ncalled()\n"

    assert run_examples(tmp_path / "page.md", [Example(code, 1)]) == [5]


class _ExampleHandler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self) -> None:
        server = cast("ExampleServer", self.server)
        url = urlsplit(self.path)
        query = parse_qsl(url.query)
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        match url.path:
            case "/":
                self._answer(200, b"Hello\n", ("Content-Type", "text/plain; charset=utf-8"))
            case "/echo":
                self._answer_with_what_came(body)
            case "/set-cookie":
                self._answer(
                    200, b"", *(("Set-Cookie", f"{name}={value}; Secure; SameSite=Lax") for name, value in query)
                )
            case "/redirect":
                self._answer(302, b"", ("Location", dict(query)["to"]))
            case "/loop":
                self._answer(302, b"", ("Location", "/loop"))
            case "/slow":
                server.finished.wait()
                self.close_connection = True
            case "/large":
                self._answer(200, bytes(LARGE_BODY_SIZE))
            case "/unfinished":
                self.send_response(200)
                self.send_header("Content-Length", "100")
                self.end_headers()
                self.wfile.write(b"not all of it")
                self.close_connection = True
            case "/cached":
                self._answer(200, b"Kept\n", ("Cache-Control", "max-age=3600"))
            case "/flaky":
                if server.flaky_has_failed:
                    self._answer(200, b"Fine now\n")
                else:
                    server.flaky_has_failed = True
                    self.close_connection = True
            case "/challenge":
                if "Authorization" in self.headers:
                    self._answer(200, b"Hello, you\n")
                else:
                    self._answer(401, b"Who are you?\n", ("WWW-Authenticate", 'Basic realm="example"'))
            case "/latin-1":
                self._answer(200, b"caf\xe9", ("Content-Type", "text/plain; charset=latin-1"))
            case _:
                self._answer(404, b"Not found\n", ("Content-Type", "text/plain; charset=utf-8"))

    do_POST = do_GET  # noqa: N815
    do_PUT = do_GET  # noqa: N815
    do_PATCH = do_GET  # noqa: N815
    do_DELETE = do_GET  # noqa: N815
    do_HEAD = do_GET  # noqa: N815
    do_OPTIONS = do_GET  # noqa: N815

    def _answer(self, status: int, body: bytes, *headers: tuple[str, str]) -> None:
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _answer_with_what_came(self, body: bytes) -> None:
        answer = {
            "method": self.command,
            "path": self.path,
            "headers": dict(self.headers.items()),
            "body": body.decode("utf-8", errors="replace"),
        }
        self._answer(200, json.dumps(answer).encode(), ("Content-Type", "application/json"))

    def log_message(self, format: str, *args: Any) -> None:
        pass


class ExampleServer(TlsServer):
    """The local HTTPS server that the names in the examples land on, with the paths that they use.

    Attributes:
        finished: Lets go of what /slow holds.
        flaky_has_failed: Whether /flaky has ended a connection, which it does once.
    """

    def __init__(self, directory: Path, leaf: Issued) -> None:
        super().__init__(directory, leaf)
        self.RequestHandlerClass = _ExampleHandler
        self.finished = threading.Event()
        self.flaky_has_failed = False


def _asking_for_a_client_certificate(directory: Path, leaf: Issued, root: Issued) -> TlsServer:
    server = TlsServer(directory, leaf)
    context = cast(ssl.SSLSocket, server.socket).context
    context.verify_mode = ssl.CERT_REQUIRED
    context.load_verify_locations(cadata=root.pem)
    return server


@pytest.fixture
def example_world(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> Iterator[None]:
    """Make each Session reach local servers under the names the examples use, in a directory of their own.

    Sessions trust the local servers. intranet.example.test is the one that has to be added, from company-root.pem.
    """
    directory = tmp_path / "work"
    directory.mkdir()
    root = generate_root()
    company_root = generate_root("mitto test company root")
    leaf = generate_leaf(root, EXAMPLE_NAMES[0], other_hosts=EXAMPLE_NAMES[1:])
    (directory / "company-root.pem").write_text(company_root.pem)
    (directory / "report.csv").write_bytes(REPORT)
    monkeypatch.chdir(directory)

    main = ExampleServer(tmp_path, leaf)
    intranet = TlsServer(tmp_path, generate_leaf(company_root, "intranet.example.test"))
    mtls = _asking_for_a_client_certificate(tmp_path, leaf, root)
    authorization = base64.b64encode(PROXY_CREDENTIALS.encode()).decode()
    proxy = LocalProxy(authorization=f"Basic {authorization}", tunnel_port=main.server_port)
    with CaptureServer() as capture, serving(main), serving(intranet), serving(mtls), serving(proxy):
        settings = PrivateSettings(
            ignore_certificate_errors_spki_list=[capture.spki_hash],
            host_resolver_rules=",".join(
                (
                    "MAP nowhere.example.test ~NOTFOUND",
                    f"MAP h2.example.test 127.0.0.1:{capture.port(HTTP2)}",
                    f"MAP intranet.example.test 127.0.0.1:{intranet.server_port}",
                    f"MAP mtls.example.test 127.0.0.1:{mtls.server_port}",
                    f"MAP proxy.example.test 127.0.0.1:{proxy.port}",
                    f"MAP * 127.0.0.1:{main.server_port}",
                )
            ),
        )
        session_type = mitto.Session
        async_session_type = mitto.AsyncSession
        sessions: list[mitto.Session] = []

        def with_local_settings(arguments: dict[str, Any]) -> dict[str, Any]:
            return {
                "additional_trust_anchors": [root.pem],
                "timeout": TIMEOUT_SECONDS,
                "_settings": settings,
                **arguments,
            }

        def new_session(**arguments: Any) -> mitto.Session:
            sessions.append(session_type(**with_local_settings(arguments)))
            return sessions[-1]

        def new_async_session(**arguments: Any) -> mitto.AsyncSession:
            return async_session_type(**with_local_settings(arguments))

        monkeypatch.setattr(mitto, "Session", new_session)
        monkeypatch.setattr(mitto, "AsyncSession", new_async_session)
        monkeypatch.setattr(_api, "Session", new_session)
        try:
            yield
        finally:
            for session in sessions:
                session.close()
            main.finished.set()


@pytest.mark.usefixtures("native_library", "example_world")
@pytest.mark.parametrize("path", PAGES, ids=lambda path: path.stem)
def test_each_example_runs_and_reaches_every_line(path: Path) -> None:
    examples = examples_on(_read_page(path))
    if sys.platform == "win32" and any("AsyncSession" in example.code for example in examples):
        pytest.skip("the page has examples of the asyncio API, which does not run on Windows yet")

    assert examples
    assert run_examples(path, examples) == []
