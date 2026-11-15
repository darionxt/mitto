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


# Elements that the window's and the document's named properties find, as Chrome 157 found them in the same markup.
_NAMED = (
    "<div id=d1></div><img name=im1 id=imid><img id=imonly><form name=f1 id=fid></form><embed name=e1>"
    "<object name=o1 id=oid></object><div id=dup></div><span id=dup></span><p id=42></p><img name=title>"
    "<form name=location></form><object name=two></object><object id=two></object><svg><circle id=circ></circle></svg>"
    "<template><div id=intemplate></div></template><img name=addEventListener>"
)
_NAME_OF = """
    const nameOf = (v) => v === undefined ? 'undefined' : v === null ? 'null'
        : v.constructor.name + (v instanceof HTMLCollection ? `(${v.length})` : '') + (v.id ? `#${v.id}` : '');
"""


def test_the_window_finds_elements_by_id_and_some_by_name(program: Path) -> None:
    # Any element by ID, and images, forms, embeds and objects by name, through the named properties object behind
    # Window.prototype. The window's own properties and those of its prototypes come first.
    script = (
        _NAME_OF
        + """
        ['d1', 'im1', 'imonly', 'fid', 'e1', 'dup', '42', 'title', 'location', 'two', 'circ', 'intemplate',
         'addEventListener', 'nothing'].map((name) => `${name}=${nameOf(window[name])}`).join(' ')
    """
    )

    assert _evaluate(program, script, _NAMED) == (
        "d1=HTMLDivElement#d1 im1=HTMLImageElement#imid imonly=HTMLImageElement#imonly fid=HTMLFormElement#fid "
        "e1=HTMLEmbedElement dup=HTMLCollection(2) 42=HTMLParagraphElement#42 title=HTMLImageElement "
        "location=Location two=HTMLCollection(2) circ=SVGCircleElement#circ intemplate=undefined "
        "addEventListener=Function nothing=undefined"
    )


def test_the_named_properties_object_has_them_but_takes_no_change(program: Path) -> None:
    script = """
        const object = Object.getPrototypeOf(Window.prototype);
        const result = [];
        const run = (f) => { try { result.push(String(f())); } catch (e) { result.push(`${e.name}: ${e.message}`); } };
        run(() => Object.prototype.toString.call(object));
        run(() => ['d1' in window, window.hasOwnProperty('d1'), object.hasOwnProperty('d1')].join());
        run(() => JSON.stringify(Object.getOwnPropertyDescriptor(object, 'd1'), ['writable', 'enumerable']));
        run(() => Object.getOwnPropertyNames(object).length);
        run(() => Object.defineProperty(object, 'd1', { value: 1 }));
        run(() => { 'use strict'; object.x = 1; });
        run(() => { object.x = 1; return object.x; });
        run(() => delete object.d1);
        run(() => { 'use strict'; return delete object.nothing; });
        run(() => Object.preventExtensions(object));
        run(() => { window.d1 = 5; const shadowed = d1; delete window.d1; return `${shadowed} ${d1.id}`; });
        result.join('\\n');
    """

    assert _evaluate(program, script, _NAMED).split("\n") == [
        "[object WindowProperties]",
        "true,false,true",
        '{"writable":true,"enumerable":false}',
        "0",
        "TypeError: Failed to set a named property 'd1' on 'WindowProperties': Named property setter is not supported.",
        "TypeError: Failed to set a named property 'x' on 'WindowProperties': Named property setter is not supported.",
        "undefined",
        "false",
        "TypeError: Failed to delete a named property 'nothing' from 'WindowProperties': "
        "Named property deleter is not supported.",
        "TypeError: Cannot prevent extensions",
        "5 d1",
    ]


def test_the_documents_named_items_are_own_properties_in_the_order_they_came(program: Path) -> None:
    # Forms, iframes and embeds by name, objects by name or ID, and images by name, or by ID with a name. An img adds
    # its ID before its name, and location, which cannot be configured, stays the document's own.
    script = (
        _NAME_OF
        + """
        [Object.getOwnPropertyNames(document).join(),
         ['im1', 'imid', 'imonly', 'f1', 'fid', 'oid', 'dup', '42', 'title', 'location', 'two', 'addEventListener']
             .map((name) => `${name}=${nameOf(document[name])}`).join(' '),
         JSON.stringify(Object.getOwnPropertyDescriptor(document, 'f1'), ['writable', 'enumerable', 'configurable']),
         document.two === document.two,
        ].join('\\n')
    """
    )

    assert _evaluate(program, script, _NAMED).split("\n") == [
        "location,imid,im1,f1,e1,oid,o1,title,two,addEventListener",
        "im1=HTMLImageElement#imid imid=HTMLImageElement#imid imonly=undefined f1=HTMLFormElement#fid fid=undefined "
        "oid=HTMLObjectElement#oid dup=undefined 42=undefined title=HTMLImageElement location=Location "
        "two=HTMLCollection(2) addEventListener=HTMLImageElement",
        '{"writable":true,"enumerable":true,"configurable":true}',
        "true",
    ]


def test_the_documents_named_items_follow_the_tree_and_the_attributes(program: Path) -> None:
    # Setting one keeps the element, deleting one takes the property away until an element adds the name again, and a
    # name that comes later goes last.
    script = (
        _NAME_OF
        + """
        const steps = [];
        const image = document.createElement('img');
        image.name = 'a';
        steps.push(nameOf(document.a));
        document.body.append(image);
        steps.push(nameOf(document.a), nameOf(window.a));
        image.id = 'b';
        steps.push(nameOf(document.b));
        image.removeAttribute('name');
        steps.push(nameOf(document.b), nameOf(window.b), 'a' in document);
        image.remove();
        steps.push(nameOf(window.b), 'b' in document);
        document.f1 = 5;
        steps.push(nameOf(document.f1), delete document.f1, 'f1' in document);
        const form = document.createElement('form');
        form.name = 'f1';
        document.body.prepend(form);
        steps.push(Object.getOwnPropertyNames(document).slice(-2).join(), nameOf(document.f1));
        steps.join(' ');
    """
    )

    assert _evaluate(program, script, _NAMED) == (
        "undefined HTMLImageElement HTMLImageElement HTMLImageElement#b undefined HTMLImageElement#b false "
        "undefined false HTMLFormElement#fid true false addEventListener,f1 HTMLCollection(2)"
    )


def test_a_document_without_a_window_has_no_named_properties(program: Path) -> None:
    script = """
        const other = document.implementation.createHTMLDocument('');
        const form = other.createElement('form');
        form.name = 'f';
        other.body.append(form);
        [String(other.f), Object.getOwnPropertyNames(other).join()].join(' ');
    """

    assert _evaluate(program, script) == "undefined location"


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


def test_pseudo_classes_match_and_a_pseudo_element_matches_nothing(program: Path) -> None:
    # tests/test_page_queries.py holds the selectors to what Chrome matches; here, that they are no Known Gaps.
    script = "[document.querySelector('p:first-child').id, document.querySelector('p::before'), "
    script += "document.querySelectorAll(':is(p, b):not(#a)').length].join()"

    assert _evaluate(program, script, "<p id=a>x<b>y</b></p><p>z</p>") == "a,,2"


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


def test_a_cdata_section_splits_into_cdata_sections(program: Path) -> None:
    # Blink's CloneWithData(), which splitText() and cloning use, keeps the node's kind, as Chrome 157 gave.
    script = """
        const xml = new Document();
        const root = xml.appendChild(xml.createElement('r'));
        const cdata = root.appendChild(xml.createCDATASection('ab'));
        const split = cdata.splitText(1);
        [[...root.childNodes].map((node) => node.nodeType).join(), split.nodeName,
         xml.createCDATASection('cd').splitText(1).nodeType, cdata.cloneNode().nodeType].join(' ')
    """

    assert _evaluate(program, script) == "4,4 #cdata-section 4 4"


def test_a_processing_instruction_reads_its_attributes_again_after_any_change_of_its_data(program: Path) -> None:
    # Setting an attribute writes the data without reading it again, so "$" stays, as Chrome 157 gave. Setting the
    # data, to the same value too, reads it again: "$" is no XML name, so the data has no attributes.
    script = """
        const pi = document.createProcessingInstruction('t', 'a="1"');
        pi.setAttribute('$', 'x');
        const before = [pi.getAttribute('$'), pi.getAttribute('a'), pi.data].join('|');
        pi.data = pi.data;
        const after = [pi.getAttribute('$'), pi.getAttribute('a')].join('|');
        pi.setAttribute('b', '2');
        [before, after, pi.data].join(' ')
    """

    assert _evaluate(program, script) == 'x|1|a="1" $="x" | b="2"'


def test_a_cdata_section_serializes_as_text_does(program: Path) -> None:
    # HTML writes a CDATA section by the rules of text: as it is in script, style and the like, and in noscript where
    # the document has a window, and escaped elsewhere, as Chrome 157 gave.
    script = """
        const xml = new Document();
        const markup = (element) => (element.append(xml.createCDATASection('a<b&c')), element.innerHTML);
        [...['script', 'style', 'noscript', 'div', 'textarea'].map((tag) => markup(document.createElement(tag))),
         markup(document.createElementNS('http://www.w3.org/2000/svg', 'script'))].join(' ')
    """

    assert _evaluate(program, script) == "a<b&c a<b&c a<b&c a&lt;b&amp;c a&lt;b&amp;c a&lt;b&amp;c"


def test_a_processing_instruction_with_a_namespace_declared_twice_has_no_attributes(program: Path) -> None:
    # Declarations are no attributes, but libxml2 reads none from a list that declares a prefix or the default
    # namespace twice, as Chrome 157 gave.
    script = """
        const names = (data) => document.createProcessingInstruction('t', data).getAttributeNames();
        ['a="1" xmlns:x="urn:a" xmlns:x="urn:b"', 'a="1" xmlns="urn:a" xmlns="urn:b"',
         'a="1" xmlns:x="urn:a" xmlns:y="urn:b"'].map((data) => JSON.stringify(names(data))).join(' ')
    """

    assert _evaluate(program, script) == '[] [] ["a"]'


def test_a_processing_instruction_escapes_attributes_by_its_documents_markup(program: Path) -> None:
    # HTML escapes a no-break space and keeps tabs and line breaks, XML writes those as character references and keeps
    # the no-break space, so that an XML document reads the value back, as Chrome 157 gave.
    script = """
        const show = (data) => [...data].map((c) => {
          const code = c.charCodeAt(0);
          return code < 32 || code > 126 ? `U+${code.toString(16)}` : c;
        }).join('');
        const value = '\\u00a0<>&"\\'\\t\\n\\r x';
        [document, new Document()].map((doc) => {
          const pi = doc.createProcessingInstruction('t', '');
          pi.setAttribute('a', value);
          const data = show(pi.data);
          pi.data = pi.data;
          return `${data} ${pi.getAttribute('a') === value}`;
        }).join(' | ')
    """

    assert _evaluate(program, script) == (
        'a="&nbsp;&lt;&gt;&amp;&quot;\'U+9U+aU+d x" false | a="U+a0&lt;&gt;&amp;&quot;\'&#9;&#10;&#13; x" true'
    )


def test_a_processing_instruction_takes_xmlns_with_no_prefix_as_an_attribute(program: Path) -> None:
    # "xmlns:" declares nothing to libxml2, so it is an attribute like any other, twice too, as Chrome 157 gave.
    script = """
        ['a="1" xmlns:="u"', 'a="1" xmlns:="u" xmlns:="v"'].map((data) => {
          const pi = document.createProcessingInstruction('t', data);
          const read = `${JSON.stringify(pi.getAttributeNames())} ${pi.getAttribute('xmlns:')}`;
          pi.setAttribute('c', '3');
          return `${read} ${pi.data}`;
        }).join(' / ')
    """

    assert _evaluate(program, script) == '["a","xmlns:"] u a="1" xmlns:="u" c="3" / [] null c="3"'


def test_an_img_that_gets_its_id_in_the_tree_leaves_without_a_trace(program: Path) -> None:
    # Blink counts an img's ID once when the img gets it in the tree but twice when it comes in with both, and takes it
    # away twice as it leaves, which does nothing for a name no longer there. An img that loses its name keeps its
    # ID's property, which gives the prototype's value, as Chrome 157 gave.
    script = """
        const out = [];
        const a = document.createElement('img');
        a.name = 'n1';
        document.body.append(a);
        a.id = 'x1';
        out.push([document.x1 === a, window.x1 === a].join());
        a.remove();
        out.push(['x1' in document, 'n1' in document, String(window.x1)].join());
        const b = document.createElement('img');
        b.name = 'n2';
        b.id = 'x2';
        document.body.append(b);
        b.removeAttribute('name');
        out.push(['x2' in document, String(document.x2)].join());
        b.remove();
        out.push(['x2' in document, String(document.x2)].join());
        document.body.append(b);
        b.name = 'n3';
        out.push(['x2' in document, document.x2 === b].join());
        b.remove();
        out.push(['x2' in document, 'n3' in document].join());
        out.join(' | ')
    """

    assert _evaluate(program, script) == (
        "true,true | false,false,undefined | true,undefined | true,undefined | true,true | false,false"
    )


def test_the_body_setter_and_other_document_accessors_follow_blink(program: Path) -> None:
    # As Chrome 157 gave: the body setter's checks and their messages, an SVG document's title in its root's SVG title
    # child, a.text as textContent, document.all.item() with nothing given, a base element in a document with no
    # window, and an input's type from its type attribute of no namespace.
    script = """
        const out = [];
        const t = (f) => { try { out.push(f()); } catch (e) { out.push(e.name + ': ' + e.message); } };
        t(() => { document.body = null; });
        t(() => { document.body = document.createElement('div'); });
        t(() => {
          const doc = document.implementation.createHTMLDocument('');
          doc.removeChild(doc.documentElement);
          doc.body = doc.createElement('body');
        });
        t(() => {
          const doc = document.implementation.createHTMLDocument('');
          const body = doc.createElement('body');
          doc.body = body;
          return [doc.body === body, doc.documentElement.children.length].join();
        });
        t(() => {
          const doc = document.implementation.createDocument('http://www.w3.org/2000/svg', 'svg', null);
          const before = doc.title;
          doc.title = 'foo';
          const title = doc.documentElement.firstChild;
          return [JSON.stringify(before), title.localName, title.namespaceURI, doc.title].join();
        });
        t(() => {
          const a = document.createElement('a');
          a.innerHTML = 'a <b>b</b> <!--x-->c ';
          const before = a.text;
          a.text = 'x';
          return JSON.stringify([before, a.innerHTML, a.childNodes.length]);
        });
        t(() => {
          const d = document.createElement('div');
          d.id = 'undefined';
          document.body.append(d);
          const r = [document.all.item(), document.all.item(undefined), document.all(), document.all.item('undefined') === d];
          d.remove();
          return r.map(String).join();
        });
        t(() => {
          const doc = document.implementation.createHTMLDocument('');
          const base = doc.createElement('base');
          base.href = 'http://example.com/';
          doc.head.appendChild(base);
          return doc.baseURI;
        });
        t(() => {
          const input = document.createElement('input');
          input.setAttributeNS('urn:x', 'type', 'file');
          return input.type;
        });
        out.join('\\n')
    """

    assert _evaluate(program, script, "<!DOCTYPE html><body>").split("\n") == [
        "HierarchyRequestError: Failed to set the 'body' property on 'Document': The 1st argument provided is either "
        "null, or an invalid HTMLElement object.",
        "HierarchyRequestError: Failed to set the 'body' property on 'Document': The new body element is of type "
        "'DIV'. It must be either a 'BODY' or 'FRAMESET' element.",
        "HierarchyRequestError: Failed to set the 'body' property on 'Document': No document element exists.",
        "true,2",
        '"",title,http://www.w3.org/2000/svg,foo',
        '["a b c ","x",1]',
        "null,null,null,true",
        "http://example.com/",
        "text",
    ]


def test_aria_element_reflection_follows_blink(program: Path) -> None:
    # As Chrome 157 gave: the IDs of the content attribute name elements in the element's tree, connected or not; the
    # same frozen array while the elements stay the same; an element that a page set holds only while it is in the
    # element's tree, and a change of the attribute forgets it, but for aria-actions, which Blink's list of element
    # reflection attributes leaves out; an empty list counts as none set; and aria-labeledby counts for
    # aria-labelledby alone.
    script = """
        const out = [];
        const ids = (a) => a === null ? 'null' : a === undefined ? 'undef'
            : Array.isArray(a) ? '[' + a.map(e => e.id).join(',') + ']' : a.id;
        const mk = (id) => { const d = document.createElement('div'); d.id = id; document.body.append(d); return d; };
        const w = document.createElement('div');
        w.innerHTML = '<div id=x1></div><div id=x2></div>'
            + '<div id=e aria-labelledby="x1  x2 nope" aria-activedescendant=x2 aria-controls=""></div>';
        const e = w.lastChild;
        out.push([ids(e.ariaLabelledByElements), ids(e.ariaActiveDescendantElement), ids(e.ariaControlsElements),
                  ids(e.ariaDetailsElements), ids(e.ariaOwnsElements)].join(' '));
        const x = e.ariaLabelledByElements;
        out.push([x === e.ariaLabelledByElements, Object.isFrozen(x)].join());
        w.firstChild.id = 'x0';
        out.push([x === e.ariaLabelledByElements, ids(e.ariaLabelledByElements)].join());
        const a = mk('a'), b = mk('b'), f = mk('f');
        f.ariaFlowToElements = [b, a, b];
        out.push([JSON.stringify(f.getAttribute('aria-flowto')), ids(f.ariaFlowToElements)].join(' '));
        f.setAttribute('aria-flowto', 'a');
        out.push(ids(f.ariaFlowToElements));
        f.ariaFlowToElements = null;
        out.push([f.hasAttribute('aria-flowto'), ids(f.ariaFlowToElements)].join(' '));
        const d = document.createElement('div');
        f.ariaActiveDescendantElement = d;
        out.push(ids(f.ariaActiveDescendantElement));
        document.body.append(d);
        out.push(f.ariaActiveDescendantElement === d);
        f.ariaActionsElements = [a];
        f.setAttribute('aria-actions', 'zz');
        out.push(ids(f.ariaActionsElements));
        f.ariaActionsElements = [];
        f.removeAttribute('aria-actions');
        out.push(ids(f.ariaActionsElements));
        f.setAttribute('aria-labeledby', 'a');
        out.push(ids(f.ariaLabelledByElements));
        f.setAttribute('aria-labelledby', '');
        out.push(ids(f.ariaLabelledByElements));
        try { f.ariaControlsElements = [1]; } catch (error) { out.push(error.message); }
        out.join('\\n')
    """

    assert _evaluate(program, script, "<!DOCTYPE html><body>").split("\n") == [
        "[x1,x2] x2 [] null undef",
        "true,true",
        "false,[x2]",
        '"" [b,a]',
        "[a]",
        "false null",
        "null",
        "true",
        "[a]",
        "null",
        "[a]",
        "[]",
        "Failed to set the 'ariaControlsElements' property on 'Element': Failed to convert value to 'Element'.",
    ]


def test_a_buttons_command_gives_only_the_keywords_chrome_enables(program: Path) -> None:
    # As Chrome 157 gave: a keyword in lower case, a custom command as it is, and "" for another value, the
    # commands behind flags that Chrome leaves off, and a submit button of a form; commandForElement reflects the
    # commandfor element whatever the button is.
    script = """
        const out = [];
        const b = document.createElement('button');
        for (const v of ['show-modal', 'SHOW-modal', '--Foo', 'fooBar', 'toggle-overscroll', 'show-picker', 'Request-Close']) {
          b.setAttribute('command', v);
          out.push(JSON.stringify(b.command));
        }
        const form = document.createElement('form');
        form.append(b);
        document.body.append(form);
        const target = document.createElement('div');
        target.id = 't';
        document.body.append(target);
        b.setAttribute('command', 'close');
        b.setAttribute('commandfor', 't');
        out.push([b.type, JSON.stringify(b.command), b.commandForElement === target].join());
        b.type = 'submit';
        b.disabled = true;
        out.push([JSON.stringify(b.command), b.commandForElement === target].join());
        const other = document.createElement('div');
        b.commandForElement = other;
        out.push([JSON.stringify(b.getAttribute('commandfor')), String(b.commandForElement)].join());
        document.body.append(other);
        out.push(b.commandForElement === other);
        out.join(' ')
    """

    assert _evaluate(program, script, "<!DOCTYPE html><body>") == (
        '"show-modal" "show-modal" "--Foo" "" "" "" "request-close" button,"close",true "",true "",null true'
    )
