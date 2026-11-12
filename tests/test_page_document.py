"""Tests of a Page's document: the DOM in native/dom and the tree builder of its HTML parser, in the Page program.

These run where MITTO_PAGE_PROGRAM names the Page program, as tests/test_page_bindings.py does, whose probe compares
the DOM's interfaces and much of its behaviour with real Chrome. Here a script gets a document that the program builds
from markup, with --html=, finds and changes its elements and reads the tree back. html5lib's tree construction tests
run against the tree builder in html_tree_builder_test.
"""

import os
import subprocess
from pathlib import Path

import pytest

_PROGRAM_ENV_VAR = "MITTO_PAGE_PROGRAM"
_TIMEOUT_SECONDS = 60
_SCRIPT_THREW = 1
_WRONG_ARGUMENTS = 2

# A script that writes the tree under the document as html5lib's tests write it, one node to a line.
_TREE = """
(() => {
  const lines = [];
  const walk = (node, depth) => {
    for (let child = node.firstChild; child; child = child.nextSibling) {
      const indent = "  ".repeat(depth);
      if (child.nodeType === Node.DOCUMENT_TYPE_NODE) {
        lines.push(`${indent}<!DOCTYPE ${child.name}>`);
      } else if (child.nodeType === Node.ELEMENT_NODE) {
        lines.push(`${indent}<${child.localName}>`);
        for (const name of child.getAttributeNames().sort()) {
          lines.push(`${indent}  ${name}="${child.getAttribute(name)}"`);
        }
      } else if (child.nodeType === Node.TEXT_NODE) {
        lines.push(`${indent}"${child.data}"`);
      } else if (child.nodeType === Node.COMMENT_NODE) {
        lines.push(`${indent}<!-- ${child.data} -->`);
      }
      walk(child, depth + 1);
    }
  };
  walk(document, 0);
  return lines.join("\\n");
})()
"""


@pytest.fixture(scope="module")
def program() -> Path:
    """Return the Page program, or skip if the environment names none."""
    path = os.environ.get(_PROGRAM_ENV_VAR)
    if not path:
        pytest.skip(f"no Page program; set {_PROGRAM_ENV_VAR} to run this test")
    return Path(path)


def _run(program: Path, *arguments: str, stdin: str | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [program, *arguments],
        input=stdin,
        capture_output=True,
        encoding="utf-8",
        timeout=_TIMEOUT_SECONDS,
        check=False,
    )


def _evaluate(program: Path, script: str, html: str | None = None) -> str:
    options = [] if html is None else [f"--html={html}"]
    result = _run(program, *options, script)
    assert (result.returncode, result.stderr) == (0, "")
    return result.stdout.removesuffix("\n")


def _tree(program: Path, html: str) -> list[str]:
    return _evaluate(program, _TREE, html).split("\n")


def test_a_page_starts_with_the_document_of_about_blank(program: Path) -> None:
    script = "[document.documentElement.outerHTML, document.compatMode, document.doctype, document.URL].join(' ')"

    assert _evaluate(program, script) == "<html><head></head><body></body></html> BackCompat  about:blank"


def test_the_parser_makes_html_head_and_body_where_the_markup_leaves_them_out(program: Path) -> None:
    html = "<!DOCTYPE html><!--first--><title>A page</title><p id=a class='x y'>One<p>Two <b>three</b><!--last-->"

    assert _tree(program, html) == [
        "<!DOCTYPE html>",
        "<!-- first -->",
        "<html>",
        "  <head>",
        "    <title>",
        '      "A page"',
        "  <body>",
        "    <p>",
        '      class="x y"',
        '      id="a"',
        '      "One"',
        "    <p>",
        '      "Two "',
        "      <b>",
        '        "three"',
        "      <!-- last -->",
    ]


def test_a_script_finds_and_changes_elements_and_the_tree_reads_back(program: Path) -> None:
    html = "<!DOCTYPE html><ul id=list><li>one<li class=done>two</ul><p>Total: <span id=total>2</span></p>"
    script = """
        const list = document.getElementById("list");
        const item = document.createElement("li");
        item.textContent = "three";
        item.setAttribute("data-new", "");
        list.appendChild(item);
        list.removeChild(list.querySelector(".done"));
        document.querySelector("#total").textContent = list.childElementCount;
        document.title = "Changed";
        document.body.innerHTML
    """

    assert _evaluate(program, script, html) == (
        '<ul id="list"><li>one</li><li data-new="">three</li></ul><p>Total: <span id="total">2</span></p>'
    )
    assert _evaluate(program, script + ";document.head.outerHTML", html) == "<head><title>Changed</title></head>"


@pytest.mark.parametrize(
    ("doctype", "mode"),
    [
        pytest.param("", "BackCompat", id="none"),
        pytest.param("<!DOCTYPE html>", "CSS1Compat", id="html"),
        pytest.param(
            '<!DOCTYPE html PUBLIC "-//W3C//DTD HTML 4.01 Transitional//EN">', "BackCompat", id="transitional"
        ),
        pytest.param(
            '<!DOCTYPE html PUBLIC "-//W3C//DTD HTML 4.01 Transitional//EN" "http://www.w3.org/TR/html4/loose.dtd">',
            "CSS1Compat",
            id="limited-quirks",
        ),
        pytest.param("<!DOCTYPE svg>", "BackCompat", id="other-name"),
    ],
)
def test_the_doctype_decides_the_documents_mode(program: Path, doctype: str, mode: str) -> None:
    assert _evaluate(program, "document.compatMode", doctype + "<p>x") == mode


def test_quirks_mode_matches_classes_and_ids_in_any_case(program: Path) -> None:
    script = "[!!document.querySelector('.ABC'), !!document.querySelector('#X'), !!document.getElementById('X')]"

    assert _evaluate(program, script, "<p class=abc id=x>") == "true,true,false"
    assert _evaluate(program, script, "<!DOCTYPE html><p class=abc id=x>") == "false,false,false"


def test_markup_in_utf8_reads_back_the_same(program: Path) -> None:
    html = "<p title='café'>日本 \U0001f600 &eacute;</p>"

    assert _evaluate(program, "document.body.innerHTML", html) == '<p title="café">日本 \U0001f600 é</p>'


def test_a_script_on_the_command_line_reads_in_utf8(program: Path) -> None:
    # On Windows the program takes its arguments from the command line in UTF-16, not in the ANSI code page.
    assert _evaluate(program, "'caf\u00e9 \u65e5\u672c \U0001f600'.length + ' caf\u00e9'") == "10 caf\u00e9"


def test_a_script_can_come_on_standard_input(program: Path) -> None:
    # For a script longer than a command line holds, such as the probe of the bindings' tests on Windows, which allows
    # 32,767 characters.
    script = "/* " + "x" * 40000 + " */ [document.body.innerHTML, '\u00e9t\u00e9'].join(' ')"
    result = _run(program, "--html=<p>caf\u00e9</p>", "-", stdin=script)

    assert (result.returncode, result.stdout, result.stderr) == (0, "<p>caf\u00e9</p> \u00e9t\u00e9\n", "")


def test_the_window_is_the_global_object(program: Path) -> None:
    script = """[
        window === globalThis, self === window, window.window === window, document.defaultView === window,
        Object.getPrototypeOf(window) === Window.prototype, window instanceof EventTarget,
        Object.prototype.toString.call(window), Object.prototype.toString.call(document),
    ].join()"""

    assert _evaluate(program, script) == "true,true,true,true,true,true,[object Window],[object HTMLDocument]"


def test_the_windows_other_members_are_known_gaps(program: Path) -> None:
    # A Known Gap gives its type's default, and its getter what its setter stored (docs/runtime.md).
    script = "[window.name === '', (window.name = 7, window.name)].join()"

    assert _evaluate(program, script) == "true,7"


def test_a_member_whose_behaviour_is_missing_gives_its_default(program: Path) -> None:
    # An object of an interface without a class, with nothing behind it, the same one each time.
    script = """
        const tokens = document.body.classList;
        [tokens instanceof DOMTokenList, tokens === document.body.classList, tokens.length].join()
    """

    assert _evaluate(program, script) == "true,true,0"


@pytest.mark.parametrize(
    ("selector", "found"),
    [
        pytest.param("body > /**/ p", "a", id="between-tokens"),
        pytest.param("/* first */p/* last */", "a", id="around"),
        pytest.param("p/**/.x/**/#b", "b", id="inside-a-compound"),
        pytest.param("p /* unclosed", "a", id="unclosed"),
        pytest.param("[class/**/=/**/x]", "b", id="in-an-attribute-selector"),
    ],
)
def test_selectors_may_hold_comments(program: Path, selector: str, found: str) -> None:
    # CSS's tokenizer drops comments between tokens.
    script = f"document.querySelector({selector!r}).id"

    assert _evaluate(program, script, "<p id=a><p id=b class=x>") == found


@pytest.mark.parametrize(
    ("selector", "found"),
    [
        pytest.param('[title="a\\\r\nb"]', "a", id="escaped-crlf"),
        pytest.param('[title="a\\\rb"]', "a", id="escaped-cr"),
        pytest.param('[title="a\\\fb"]', "a", id="escaped-ff"),
        pytest.param("p\r\n+\r\np", "b", id="crlf-around-a-combinator"),
        pytest.param("#\\61 \r\n.x", None, id="crlf-after-an-escape"),
    ],
)
def test_newlines_in_a_selector_are_preprocessed_as_css_says(program: Path, selector: str, found: str | None) -> None:
    # CSS Syntax turns CR LF, CR and FF into LF before it tokenizes, so an escaped one continues a string.
    script = f"(document.querySelector({selector!r}) || {{id: null}}).id"

    assert _evaluate(program, script, "<p id=a title=ab><p id=b class=x>") == ("null" if found is None else found)


def test_a_newline_ends_a_string_in_a_selector(program: Path) -> None:
    result = _run(program, "document.querySelector('[title=\"a\\r\\nb\"]')")

    assert result.stderr.startswith("Uncaught SyntaxError: Failed to execute 'querySelector' on 'Document': ")


@pytest.mark.parametrize("selector", ["div/**/p", "p >/**/", "#/**/a", "[a~/**/=b]"])
def test_a_comment_is_not_whitespace_in_a_selector(program: Path, selector: str) -> None:
    # A comment makes no token, so names on each side of it are not a descendant combinator.
    result = _run(program, f"document.querySelector({selector!r})")

    assert result.stderr == (
        f"Uncaught SyntaxError: Failed to execute 'querySelector' on 'Document': '{selector}' is not a valid selector.\n"
    )


def test_a_selector_that_needs_more_than_simple_selectors_is_a_known_gap(program: Path) -> None:
    result = _run(program, "document.querySelector('p:first-child')")

    assert result.stderr == ("Uncaught TypeError: Failed to execute 'querySelector' on 'Document': Not implemented.\n")


def test_the_documents_scripts_do_not_run_until_a_page_loads_documents(program: Path) -> None:
    # A Page runs a document's scripts once it loads documents through its Session (#183).
    script = "[typeof ran, document.querySelector('script').text].join()"

    assert _evaluate(program, script, "<script>var ran = 1</script>") == "undefined,var ran = 1"


def test_a_detached_node_keeps_its_tree_through_garbage_collection(program: Path) -> None:
    # Only the innermost node's wrapper holds the tree, through V8's C++ heap, while scripts allocate enough to
    # collect.
    script = """
        const leaves = [];
        for (let i = 0; i < 500; i++) {
          const root = document.createElement("div");
          root.innerHTML = `<p><b><i>${i}</i></b></p>`;
          leaves.push(root.querySelector("i"));
        }
        for (let i = 0; i < 200000; i++) new Array(64).fill(i);
        leaves.every((leaf, i) => leaf.parentNode.parentNode.parentNode.innerHTML === `<p><b><i>${i}</i></b></p>`)
    """

    assert _evaluate(program, script) == "true"


def test_a_nodes_wrapper_lives_as_long_as_the_node(program: Path) -> None:
    # A node in the document holds its wrapper, so what a script put on the wrapper is still there when it finds the
    # node again.
    script = """
        for (let i = 0; i < 500; i++) {
          const element = document.createElement("span");
          element.id = `s${i}`;
          element.mark = i;
          document.body.appendChild(element);
        }
        for (let i = 0; i < 200000; i++) new Array(64).fill(i);
        Array.from({ length: 500 }, (_, i) => document.getElementById(`s${i}`).mark === i).every(Boolean)
    """

    assert _evaluate(program, script) == "true"


def test_a_session_page_takes_no_markup_on_its_command_line(program: Path) -> None:
    result = _run(program, "--html=<p>", "--session=3,1")

    assert (result.returncode, result.stdout) == (_WRONG_ARGUMENTS, "")
