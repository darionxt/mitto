"""Tests of the security fixes that Chrome's release notes list, from the Chrome Releases blog, with no network."""

import pytest

from tests.release.saved import saved
from tools.release.chrome_releases import FEED_URL, SecurityFix, security_fixes
from tools.release.web import WebError

_ATOM = '<feed xmlns="http://www.w3.org/2005/Atom">{}</feed>'


def post(published: str, content: str) -> str:
    escaped = content.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    return f"<entry><published>{published}</published><content type='html'>{escaped}</content></entry>"


def feed(*posts: str) -> bytes:
    return _ATOM.format("".join(posts)).encode()


def test_the_fixes_are_those_the_posts_of_the_day_list() -> None:
    fixes = security_fixes(saved)

    assert fixes["572055997"] == SecurityFix(
        bug="572055997",
        severity="High",
        cve="CVE-2026-76029",
        title="Improper input validation in Network",
        published="2026-10-29",
        in_the_wild=False,
    )
    assert fixes["568826881"].severity == "Critical"
    assert fixes["568826881"].title == "Use after free in Network"
    assert len([fix for fix in fixes.values() if fix.published == "2026-11-09"]) == 9


def test_a_fix_whose_exploit_google_knows_of_is_in_the_wild() -> None:
    fixes = security_fixes(saved)

    assert [fix.cve for fix in fixes.values() if fix.in_the_wild] == ["CVE-2026-125350"]
    assert fixes["582442907"].title == "Use after free in XML"


def test_the_question_asks_for_two_months_of_posts() -> None:
    asked: list[str] = []

    def fetch(url: str) -> bytes:
        asked.append(url)
        return feed(post("2026-11-09T12:00:00.000-08:00", "No fixes."))

    security_fixes(fetch)

    assert asked == [f"{FEED_URL}?max-results=60"]


def test_a_fix_that_two_posts_list_counts_from_the_older() -> None:
    line = (
        "[N/A][572055997] High CVE-2026-76029: Improper input validation in Network. Reported by someone on 2026-10-09"
    )

    fixes = security_fixes(
        lambda _url: feed(post("2026-11-03T12:00:00.000-08:00", line), post("2026-10-29T12:00:00.000-07:00", line))
    )

    assert fixes["572055997"].published == "2026-10-29"


def test_a_line_of_the_notes_is_read_through_its_markup() -> None:
    content = (
        '<p><span style="color: red;">[$10,000][</span><a href="https://issues.chromium.org/issues/567735302">'
        "567735302</a><span>] Critical CVE-2026-121156: Buffer overflow in&nbsp;Dawn. Reported by Zhenpeng (Leo) "
        "Lin on 2026-09-30</span></p><p>[TBD][572578603] High CVE-2026-125346: Use after free in XML. "
        "Reported by Arni Hardarson on 2026-10-12</p>"
    )

    fixes = security_fixes(lambda _url: feed(post("2026-11-03T12:00:00.000-08:00", content)))

    assert [(fix.bug, fix.severity, fix.title) for fix in fixes.values()] == [
        ("567735302", "Critical", "Buffer overflow in Dawn"),
        ("572578603", "High", "Use after free in XML"),
    ]


def test_a_line_without_its_reporter_takes_no_title_from_the_next() -> None:
    content = (
        "<p>[N/A][111111111] High CVE-2026-1: Use after free in Network.</p>"
        "<p>[N/A][222222222] Medium CVE-2026-2: Race in Cache. Reported by someone on 2026-10-01</p>"
    )

    fixes = security_fixes(lambda _url: feed(post("2026-11-03T12:00:00.000-08:00", content)))

    assert list(fixes) == ["222222222"]
    assert fixes["222222222"].title == "Race in Cache"


@pytest.mark.parametrize(
    "body",
    [
        b"not xml",
        b"<rss></rss>",
        _ATOM.format("").encode(),
        _ATOM.format("<entry><published>2026-11-09</published></entry>").encode(),
    ],
)
def test_an_answer_that_is_not_a_feed_of_posts_is_an_error(body: bytes) -> None:
    with pytest.raises(WebError, match=r"chromereleases\.googleblog\.com"):
        security_fixes(lambda _url: body)


@pytest.mark.parametrize(
    ("sentence", "wild"),
    [
        ("Google is aware that an exploit for CVE-2026-2 exists in the wild.", {"CVE-2026-2"}),
        (
            "Google is aware of reports that exploits for CVE-2026-1 and CVE-2026-2 exist in the wild.",
            {"CVE-2026-1", "CVE-2026-2"},
        ),
        (
            "Reported by someone on 2026-10-01 Google is aware that an exploit for CVE-2026-2 exists in the wild.",
            {"CVE-2026-2"},
        ),
        ("No exploit for CVE-2026-2 is known.", set[str]()),
    ],
)
def test_the_fixes_in_the_wild_are_those_a_sentence_names_between_exploit_and_wild(
    sentence: str, wild: set[str]
) -> None:
    content = (
        "<p>[N/A][111111111] High CVE-2026-1: Use after free in Network. Reported by someone on 2026-10-01</p>"
        f"<p>[N/A][222222222] High CVE-2026-2: Use after free in XML. Reported by someone on 2026-10-01</p><p>{sentence}</p>"
    )

    fixes = security_fixes(lambda _url: feed(post("2026-11-09T12:00:00.000-08:00", content)))

    assert {fix.cve for fix in fixes.values() if fix.in_the_wild} == wild
