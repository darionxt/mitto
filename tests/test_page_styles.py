"""Tests of the style sheets that a Page's link and style elements load: their @import rules, what a sheet that loads holds
back, and the load and error events, through Session._open_page() and AsyncSession._open_page(), which stay private
until M10.

A local server stands for the page's origin and records each request. Where a caller can ask for the same request, the
lines that the Page sent are checked against the caller's, as the Session's Request Kinds make both. Each test needs a
native library with the Page program beside it.
"""

import threading
import time
from collections.abc import Callable, Iterator
from typing import Any

import pytest

import mitto
from mitto import _native
from mitto._page import Page
from tests.page_server import TIMEOUT_SECONDS, RecordingServer, without_favicon

pytestmark = pytest.mark.usefixtures("both_apis")

_CSS = "text/css"


@pytest.fixture
def server() -> Iterator[RecordingServer]:
    with RecordingServer() as page_server:
        yield page_server


@pytest.fixture
def session(native_library: Any) -> Iterator[mitto.Session]:
    """Return a Session through each API in turn, or skip if no Page program lies beside the library."""
    library_path = _native.find_library()
    assert library_path is not None
    if not library_path.with_name(_native.PAGE_PROGRAM_NAME).is_file():
        pytest.skip("no Page program beside the library")
    with mitto.Session() as session:
        yield session


def _open_page(session: mitto.Session, url: str) -> Page:
    return session._open_page(url, timeout=TIMEOUT_SECONDS)  # pyright: ignore[reportPrivateUsage]


def _value(lines: list[tuple[str, str]], name: str) -> str | None:
    return next((value for line_name, value in lines if line_name.lower() == name.lower()), None)


def _open_page_holding(
    session: mitto.Session, server: RecordingServer, url: str, held: str, check: Callable[[], Any]
) -> Any:
    """Open `url` while the server holds its answer for `held`, and return what `check` saw half a second after the
    Page asked for it, before the answer goes. Opening a Page waits for its load event, which the held sheet holds back,
    so the answer goes from a thread."""
    hold = threading.Event()
    server.routes[held].hold = hold
    seen: list[Any] = []

    def answer_later() -> None:
        server.wait_for_path(held)
        time.sleep(0.5)
        seen.append(check())
        hold.set()

    answerer = threading.Thread(target=answer_later)
    answerer.start()
    try:
        with _open_page(session, url):
            pass
    finally:
        hold.set()
        answerer.join()
    return seen[0]


def test_a_sheet_and_its_imports_go_as_a_callers_style_requests(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/linked.css", b'@charset "utf-8"; @layer base; @import url("nested.css") screen;', _CSS)
    server.route("/nested.css", b"@import 'deepest.css'; p { color: red }", _CSS)
    server.route("/deepest.css", b"p {}", _CSS)
    server.route("/inline.css", b"p {}", _CSS)
    url = server.page(
        "/page",
        '<link rel="stylesheet" href="linked.css"><style>@import "inline.css"; p { color: blue }</style>',
    )

    with _open_page(session, url):
        pass

    assert sorted(without_favicon(server.paths())) == [
        "/deepest.css",
        "/inline.css",
        "/linked.css",
        "/nested.css",
        "/page",
    ]
    # A sheet of the document goes from the document, and an import from the sheet that imports it, as Referer says.
    for path, initiator in (
        ("/linked.css", url),
        ("/inline.css", url),
        ("/nested.css", server.url("/linked.css")),
        ("/deepest.css", server.url("/nested.css")),
    ):
        session.get(server.url(path), kind="style", initiator=initiator)
        page_lines, caller_lines = server.headers_of(path)
        assert page_lines == caller_lines, path
        assert _value(page_lines, "Referer") == initiator, path


def test_an_import_takes_the_decoded_string_of_a_quoted_url(session: mitto.Session, server: RecordingServer) -> None:
    server.route("/escaped.css", b"p {}", _CSS)
    server.route("/say's.css", b"p {}", _CSS)
    url = server.page("/page", r"""<style>@import url("esc\61 ped.css"); @import url( 'say\'s.css' );</style>""")

    with _open_page(session, url):
        pass

    # The string inside url() decodes its escapes, as the tokenizer reads it, an escaped quote too.
    assert sorted(without_favicon(server.paths())) == ["/escaped.css", "/page", "/say's.css"]


def test_a_link_type_with_parameters_names_a_style_sheet(session: mitto.Session, server: RecordingServer) -> None:
    server.route("/parameters.css", b"p {}", _CSS)
    server.route("/spaced.css", b"p {}", _CSS)
    server.route("/other.css", b"p {}", _CSS)
    url = server.page(
        "/page",
        '<link rel="stylesheet" type="text/css; charset=utf-8" href="parameters.css">'
        '<link rel="stylesheet" type=" TEXT/CSS " href="spaced.css">'
        '<link rel="stylesheet" type="text/plain; x=text/css" href="other.css">',
    )

    with _open_page(session, url):
        pass

    # The type's MIME type, without white space or parameters, as Blink's ContentType::GetType() takes it.
    assert sorted(without_favicon(server.paths())) == ["/page", "/parameters.css", "/spaced.css"]


def test_an_import_takes_the_encoding_that_decoded_its_sheet(session: mitto.Session, server: RecordingServer) -> None:
    server.route("/declared.css", b'@import "undeclared.css";', "text/css; charset=windows-1251")
    server.route("/undeclared.css", '@import "\u0438\u043c\u044f.css";'.encode("cp1251"), _CSS)
    named = "/%D0%B8%D0%BC%D1%8F.css"
    server.route(named, b"p {}", _CSS)
    url = server.page("/page", '<meta charset="utf-8"><link rel="stylesheet" href="declared.css">')

    with _open_page(session, url):
        pass

    # A sheet that says nothing of its encoding decodes as the sheet that imports it was decoded, as Blink gives the
    # import the parent's CSSStyleSheetResource::Encoding().
    assert sorted(without_favicon(server.paths())) == [named, "/declared.css", "/page", "/undeclared.css"]


def test_a_style_element_reads_its_text_again_once_a_script_changes_it(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/first.css", b"p {}", _CSS)
    server.route("/next.css", b"p {}", _CSS)
    url = server.page("/page", "<style id=s>@import 'first.css';</style>")

    with _open_page(session, url) as page:
        page.evaluate("s.firstChild.data = \"@import 'next.css';\"; true")
        assert server.wait_for_path("/next.css")

    # CharacterData::DidModifyData() tells the parent, and the style element makes its sheet again.
    assert sorted(without_favicon(server.paths())) == ["/first.css", "/next.css", "/page"]


def test_an_import_goes_under_the_policy_that_its_sheets_response_names(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/private.css", b"@import 'from-private.css';", _CSS, [("Referrer-Policy", "no-referrer")])
    server.route("/from-private.css", b"p {}", _CSS)
    server.route("/plain.css", b"@import 'from-plain.css';", _CSS)
    server.route("/from-plain.css", b"p {}", _CSS)
    url = server.page(
        "/page",
        '<link rel="stylesheet" href="private.css"><link rel="stylesheet" referrerpolicy="no-referrer" href="plain.css">',
    )

    with _open_page(session, url):
        pass

    # CSSStyleSheetResource::GetReferrerPolicy(): the sheet's Referrer-Policy, else the document's, whatever policy
    # fetched the sheet.
    (private_lines,) = server.headers_of("/from-private.css")
    (plain_lines,) = server.headers_of("/from-plain.css")
    assert _value(private_lines, "Referer") is None
    assert _value(plain_lines, "Referer") == server.url("/plain.css")


def test_a_sheet_whose_response_says_nosniff_loads_only_as_text_css(
    session: mitto.Session, server: RecordingServer
) -> None:
    nosniff = [("X-Content-Type-Options", "nosniff")]
    server.route("/plain.css", b"p {}", "text/plain", nosniff)
    server.route("/css.css", b"p {}", _CSS, nosniff)
    url = server.page(
        "/page",
        "<script>window.events = [];</script>"
        "<link rel=stylesheet href=plain.css onload=\"events.push('plain load')\" onerror=\"events.push('plain error')\">"
        "<link rel=stylesheet href=css.css onload=\"events.push('css load')\" onerror=\"events.push('css error')\">",
    )

    with _open_page(session, url) as page:
        events = page.evaluate("events.slice().sort()", timeout=TIMEOUT_SECONDS)
        errors = [message.text for message in page.console_messages if message.level == "error"]

    # ResourceLoader::CheckResponseNosniff(): a style request's response that says nosniff fails unless it is text/css.
    assert events == ["css load", "plain error"]
    assert errors == [
        f"Refused to apply style from '{server.url('/plain.css')}' because its MIME type ('text/plain') is not a "
        "supported stylesheet MIME type, and strict MIME checking is enabled."
    ]


def test_an_import_of_a_sheet_of_another_origin_names_that_sheet_as_referrer(
    session: mitto.Session, server: RecordingServer
) -> None:
    with RecordingServer() as other:
        other.route("/sheet.css", b"@import 'imported.css';", _CSS)
        other.route("/imported.css", b"p {}", _CSS)
        url = server.page("/", f'<link rel="stylesheet" href="{other.url("/sheet.css")}">')

        with _open_page(session, url):
            pass

        # The sheet's own URL, a response that the document had whole, as Chrome sends it.
        [imported] = other.headers_of("/imported.css")
        assert _value(imported, "Referer") == other.url("/sheet.css")


def test_an_import_after_a_rule_or_of_its_own_sheet_loads_nothing(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/cycle.css", b"@import 'cycle.css'; @import url(cycle.css#again);", _CSS)
    url = server.page(
        "/",
        "<style>p {} @import 'after-rule.css';</style>"
        "<style>@media screen { @import 'in-block.css'; }</style>"
        '<style>nonsense!! {} @import "after-invalid.css";</style>'
        '<link rel="stylesheet" href="cycle.css">',
    )

    with _open_page(session, url):
        pass

    # A rule that does not parse ends no import section, as Blink drops it.
    assert sorted(without_favicon(server.paths())) == ["/", "/after-invalid.css", "/cycle.css"]


def test_a_parser_inserted_sheet_holds_back_the_scripts_after_it(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/held.css", b"p {}", _CSS)
    server.route("/after-script", b"", "text/plain")
    url = server.page("/", "<link rel=stylesheet href=held.css><script>fetch('after-script')</script>")

    asked_early = _open_page_holding(session, server, url, "/held.css", lambda: "/after-script" in server.paths())

    assert not asked_early
    assert without_favicon(server.paths())[-1] == "/after-script"


@pytest.mark.parametrize(
    "markup",
    [
        "<link rel=stylesheet media=print href=held.css>",
        '<link rel="alternate stylesheet" title=other href=held.css>',
        "<script>document.head.append(Object.assign(document.createElement('link'), "
        "{rel: 'stylesheet', href: 'held.css'}))</script>",
    ],
    ids=["other-media", "alternate", "script-inserted"],
)
def test_a_sheet_for_other_media_an_alternate_or_a_scripts_holds_no_script_back(
    session: mitto.Session, server: RecordingServer, markup: str
) -> None:
    server.route("/held.css", b"p {}", _CSS)
    server.route("/after-script", b"", "text/plain")
    url = server.page("/", f"{markup}<script>fetch('after-script')</script>")

    asked_early = _open_page_holding(session, server, url, "/held.css", lambda: "/after-script" in server.paths())

    assert asked_early


# At-rules with a block, each before an import of its own: those whose prelude or, for @property, whose descriptors
# Blink does not parse, which its parser drops, and those it makes rules of, which end the imports.
_AT_RULES = {
    "dropped": [
        "@supports foo",
        "@supports (a: b) and (c: d) or (e: f)",
        "@container none (width > 1px)",
        "@container foo bar",
        "@layer a, b",
        "@scope (> p)",
        "@keyframes none",
        "@font-face x",
        "@page :nope",
        "@counter-style decimal",
        "@position-try a",
        "@font-feature-values serif",
        "@function --f",
        "@function --f() returns ???",
        "@function --f(--x: ;)",
        "@property --a",
    ],
    "kept": [
        "@media !!!",
        "@supports not(a: b)",
        "@container foo",
        "@layer initial.a",
        "@scope (p) to (q)",
        '@keyframes "a"',
        "@page a:left",
        "@counter-style a",
        "@font-feature-values a serif",
        "@function --f(--a <length>) returns <length>",
    ],
}


def test_an_at_rule_whose_prelude_does_not_parse_ends_no_imports(
    session: mitto.Session, server: RecordingServer
) -> None:
    styles: list[str] = []
    for kind, rules in _AT_RULES.items():
        for i, rule in enumerate(rules):
            server.route(f"/{kind}-{i}.css", b"p {}", _CSS)
            styles.append(f"<style>{rule} {{}} @import '{kind}-{i}.css';</style>")
    server.route("/property-kept.css", b"p {}", _CSS)
    styles.append("<style>@property --a { syntax: '*'; inherits: false; } @import 'property-kept.css';</style>")
    # A space ends the escape of an f, so that the syntax is the ident foo.
    server.route("/property-escaped.css", b"p {}", _CSS)
    styles.append(
        "<style>@property --b { syntax: '\\66 oo'; inherits: false; initial-value: foo; }"
        " @import 'property-escaped.css';</style>"
    )
    # An escaped semicolon is an ident too, which ends neither the string nor the rule.
    server.route("/property-semicolon.css", b"p {}", _CSS)
    styles.append(
        "<style>@property --c { syntax: '\\\\;'; inherits: false; initial-value: \\; }"
        " @import 'property-semicolon.css';</style>"
    )
    url = server.page("/", "".join(styles))

    with _open_page(session, url):
        pass

    # What Chrome 157 fetched: the import after each rule that its parser dropped.
    fetched = sorted(path for path in without_favicon(server.paths()) if path != "/")
    assert fetched == sorted(f"/dropped-{i}.css" for i in range(len(_AT_RULES["dropped"])))


# Style elements in documents that no window shows, and in the page's own, each with what it imports, and the events
# they fire in turn.
_UNSHOWN_STYLES = """
new Promise(resolve => {
  const events = [];
  const style = (doc, name, text) => {
    const element = doc.createElement('style');
    element.onload = () => events.push(`${name} load`);
    element.onerror = () => events.push(`${name} error`);
    element.textContent = text;
    return element;
  };
  const made = document.implementation.createHTMLDocument();
  made.head.append(style(made, 'made relative', '@import url("/relative.css");'));
  made.head.append(style(made, 'made plain', 'p {}'));
  made.head.append(style(made, 'made data', '@import url("data:text/css,p{}");'));
  const parsed = new DOMParser().parseFromString('<style>@import url("/parsed.css");</style><p>after</p>', 'text/html');
  const imported = parsed.querySelector('style');
  imported.onload = () => events.push('parsed load');
  imported.onerror = () => events.push('parsed error');
  document.head.append(style(document, 'page invalid', '@import url("http://[");'));
  document.head.append(style(document, 'page itself', '@import url("");'));
  setTimeout(() => resolve({events, parsed: parsed.body.textContent}), 500);
})
"""


def test_a_style_element_of_a_document_with_no_window_fires_load_or_error(
    session: mitto.Session, server: RecordingServer
) -> None:
    url = server.page("/", "<p>page</p>")

    with _open_page(session, url) as page:
        result = page.evaluate(_UNSHOWN_STYLES, timeout=TIMEOUT_SECONDS)

    # What Chrome 157 gives. A document that no window shows fetches nothing: an import goes to its fetcher, which
    # blocks it, so the sheet errs, but a relative URL against the base of about:blank resolves to nothing, which the
    # check for a cycle takes as the inline sheet's own null URL, so that it imports nothing. The page's own sheet
    # errs for a URL that does not resolve, and imports nothing for its own URL.
    assert result == {
        "events": [
            "made relative load",
            "made plain load",
            "made data error",
            "parsed error",
            "page invalid error",
            "page itself load",
        ],
        "parsed": "after",
    }
    assert without_favicon(server.paths()) == ["/"]


def test_link_and_style_elements_fire_load_or_error_once_their_imports_end(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/fine.css", b"@import 'fine-import.css';", _CSS)
    server.route("/fine-import.css", b"p {}", _CSS)
    server.route("/broken-import.css", b"@import 'missing.css';", _CSS)
    server.route("/plain.css", b"@import 'never.css';", "text/plain")
    url = server.page(
        "/",
        "<!doctype html><script>window.events = [];"
        "function note(event) { events.push(event.target.getAttribute('data-name') + ':' + event.type); }</script>"
        "<link rel=stylesheet href=fine.css data-name=fine onload=note(event) onerror=note(event)>"
        "<link rel=stylesheet href=missing.css data-name=missing onload=note(event) onerror=note(event)>"
        "<link rel=stylesheet href=broken-import.css data-name=broken onload=note(event) onerror=note(event)>"
        "<link rel=stylesheet href=plain.css data-name=plain onload=note(event) onerror=note(event)>"
        "<style data-name=empty onload=note(event) onerror=note(event)></style>"
        "<style data-name=imports onload=note(event) onerror=note(event)>@import 'missing.css';</style>",
    )

    with _open_page(session, url) as page:
        events = page.evaluate("events.sort()", timeout=TIMEOUT_SECONDS)

    # A sheet of the wrong type is empty, so it imports nothing, but loads; one that fails, or whose import fails, errs.
    assert events == ["broken:error", "empty:load", "fine:load", "imports:error", "missing:error", "plain:load"]
    assert "/never.css" not in server.paths()


def test_a_quirks_document_takes_a_same_origin_sheet_of_any_type(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/plain.css", b"@import 'imported.css';", "text/plain")
    server.route("/imported.css", b"p {}", _CSS)
    url = server.page("/", "<link rel=stylesheet href=plain.css>")

    with _open_page(session, url):
        pass

    assert "/imported.css" in server.paths()


def test_a_disabled_link_loads_its_sheet_once_a_script_enables_it(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/later.css", b"p {}", _CSS)
    url = server.page(
        "/", "<link rel=stylesheet href=later.css disabled id=link><link rel=stylesheet type=text/plain href=x.css>"
    )

    with _open_page(session, url) as page:
        before = without_favicon(server.paths())
        loaded = page.evaluate(
            """new Promise(resolve => {
              const link = document.getElementById('link');
              link.onload = () => resolve(true);
              link.removeAttribute('disabled');
            })""",
            timeout=TIMEOUT_SECONDS,
        )

    assert before == ["/"]
    assert loaded is True
    assert without_favicon(server.paths()) == ["/", "/later.css"]


def test_the_window_load_event_waits_for_a_sheet_and_its_imports(
    session: mitto.Session, server: RecordingServer
) -> None:
    server.route("/outer.css", b"@import 'held.css';", _CSS)
    server.route("/held.css", b"p {}", _CSS)
    url = server.page(
        "/",
        "<script>window.order = []; addEventListener('load', () => order.push('window'));</script>"
        "<link rel=stylesheet href=outer.css onload=\"order.push('link')\">",
    )
    hold = threading.Event()
    server.routes["/held.css"].hold = hold

    def answer_later() -> None:
        server.wait_for_path("/held.css")
        time.sleep(0.5)
        hold.set()

    answerer = threading.Thread(target=answer_later)
    answerer.start()
    try:
        with _open_page(session, url) as page:
            order = page.evaluate("order", timeout=TIMEOUT_SECONDS)
    finally:
        hold.set()
        answerer.join()

    assert order == ["link", "window"]
