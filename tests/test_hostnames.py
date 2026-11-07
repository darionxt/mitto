"""Tests that hostnames that are not ASCII come out as Chrome writes them.

//url turns such a name into ASCII by UTS 46, with ICU and its data. The library holds only the items of that data that
its code can read (native/make_icu_data.py), so these results were recorded from a library with all of it, and any item
that IDNA needs and the library lacks shows here. A cookie's domain goes through the same code as a URL's host, and needs
no server.
"""

import pytest

import mitto
from mitto import Cookie


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    ("hostname", "written"),
    [
        pytest.param("b\u00fccher.example", "xn--bcher-kva.example", id="punycode"),
        pytest.param("B\u00dcCHER.example", "xn--bcher-kva.example", id="lower-case"),
        pytest.param("bu\u0308cher.example", "xn--bcher-kva.example", id="composed"),
        pytest.param("fa\u00df.example", "xn--fa-hia.example", id="sharp-s-kept"),
        pytest.param("FASS.example", "fass.example", id="ascii"),
        pytest.param("\u1e9e.example", "xn--zca.example", id="capital-sharp-s"),
        pytest.param("\uff25\uff38\uff21\uff2d\uff30\uff2c\uff25.test", "example.test", id="full-width"),
        pytest.param("\ufb01.example", "fi.example", id="ligature"),
        pytest.param("\u24d0.example", "a.example", id="circled"),
        pytest.param("\u212b.example", "xn--5ca.example", id="angstrom-sign"),
        pytest.param("\u0130stanbul.example", "xn--istanbul-o0e.example", id="dotted-capital-i"),
        pytest.param("\u01c4.example", "xn--d-toa.example", id="digraph"),
        pytest.param("a\u00adb.example", "ab.example", id="soft-hyphen-dropped"),
        pytest.param("\u4f8b\u3048.jp", "xn--r8jz45g.jp", id="cjk"),
        pytest.param("\u4f8b\u3048\u3002jp", "xn--r8jz45g.jp", id="ideographic-full-stop"),
        pytest.param("\u4f8b\u3048\uff0ejp", "xn--r8jz45g.jp", id="full-width-full-stop"),
        pytest.param("\u03b5\u03bb\u03bb\u03ac\u03b4\u03b1.example", "xn--hxakic4aa.example", id="greek"),
        pytest.param("\u03b1\u03c2.example", "xn--mxa8a.example", id="final-sigma-kept"),
        pytest.param("\u0375\u03b1.example", "xn--wva4j.example", id="greek-numeral-sign"),
        pytest.param("\u0645\u062b\u0627\u0644.example", "xn--mgbh0fb.example", id="arabic"),
        pytest.param("\u05d0\u05d1.example", "xn--4dbc.example", id="hebrew"),
        pytest.param("\u05d0\u05d1.\u0627\u0628", "xn--4dbc.xn--mgbc", id="right-to-left-labels"),
        pytest.param("\u05d01.example", "xn--1-zhc.example", id="right-to-left-then-digit"),
        pytest.param("\u05f3.example", "xn--4eb.example", id="geresh"),
        pytest.param("\u0915\u094d\u200d\u0937.example", "xn--11b2ezcw70k.example", id="joiner-after-virama"),
        pytest.param("l\u00b7l.example", "xn--ll-0ea.example", id="middle-dot"),
        pytest.param("\u30fb.example", "xn--vek.example", id="katakana-middle-dot"),
        pytest.param("\u2135.example", "xn--4db.example", id="alef-symbol"),
        pytest.param("\u0345.example", "xn--uxa.example", id="ypogegrammeni"),
        pytest.param("\u2603.example", "xn--n3h.example", id="symbol"),
        pytest.param("\U0001f600.example", "xn--e28h.example", id="emoji"),
        pytest.param("xn--bcher-kva.example", "xn--bcher-kva.example", id="already-punycode"),
    ],
)
def test_a_hostname_is_written_in_ascii_as_chrome_writes_it(hostname: str, written: str) -> None:
    with mitto.Session() as session:
        session.cookies.set(Cookie("a", "1", domain=hostname))

        assert [cookie.domain for cookie in session.cookies] == [written]


@pytest.mark.usefixtures("native_library")
@pytest.mark.parametrize(
    "hostname",
    [
        pytest.param("a\u200db.example", id="joiner-between-letters"),
        pytest.param("a\u200cb.example", id="non-joiner-between-letters"),
        pytest.param("\u200d.example", id="joiner-alone"),
        pytest.param("\u0300a.example", id="combining-mark-first"),
        pytest.param("ab\ufffd.example", id="replacement-character"),
        pytest.param("\u2488.example", id="digit-full-stop"),
        pytest.param("\u0661\u0662.example", id="arabic-digits-beside-left-to-right"),
        pytest.param("\u0661a.example", id="arabic-digit-then-latin"),
        pytest.param("1\u05d0.example", id="digit-then-right-to-left"),
        pytest.param("\u0660\u06f0.example", id="two-kinds-of-arabic-digits"),
        pytest.param("\u00ad.example", id="soft-hyphen-alone"),
        pytest.param("\u3164.example", id="hangul-filler"),
        pytest.param("\u00e0" * 60 + ".example", id="label-too-long"),
        pytest.param("\u3002example", id="empty-label"),
    ],
)
def test_a_hostname_that_uts_46_refuses_is_refused(hostname: str) -> None:
    with mitto.Session() as session, pytest.raises(ValueError, match="refused the cookies"):
        session.cookies.set(Cookie("a", "1", domain=hostname))
